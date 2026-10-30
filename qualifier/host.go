// SPDX-License-Identifier: GPL-2.0-only

package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	podresources "k8s.io/kubelet/pkg/apis/podresources/v1"
)

const (
	resourceName = "pyrite.dev/gpu"
	saDir        = "/var/run/secrets/kubernetes.io/serviceaccount/"
)

// discover reads <sysClass>/pyriteN/uuid for N = 0, 1, ... until one is
// missing, as the device plugin does.
func discover(sysClass string) ([]Device, error) {
	var devs []Device
	for i := 0; ; i++ {
		name := fmt.Sprintf("pyrite%d", i)
		uuid, err := os.ReadFile(filepath.Join(sysClass, name, "uuid"))
		if errors.Is(err, fs.ErrNotExist) {
			break
		}
		if err != nil {
			return nil, err
		}
		devs = append(devs, Device{Name: name, ID: strings.TrimSpace(string(uuid))})
	}

	if len(devs) == 0 {
		return nil, fmt.Errorf("no pyrite devices with a uuid under %s; is the module loaded?", sysClass)
	}
	return devs, nil
}

// sysHost is the real Host: pyrite's sysfs, the qualify binary, kubelet's
// PodResources socket and the API server.
type sysHost struct {
	sysClass     string
	qualify      string
	passes       int
	podResources string
	apiServer    string
	http         *http.Client
}

// newSysHost reads the API server's address and CA from the pod's
// service-account setup; the token is read again for every request, since
// kubelet rotates it.
func newSysHost(sysClass, qualify string, passes int, podResources string) (*sysHost, error) {
	host, port := os.Getenv("KUBERNETES_SERVICE_HOST"), os.Getenv("KUBERNETES_SERVICE_PORT")
	if host == "" || port == "" {
		return nil, errors.New("not in a cluster: KUBERNETES_SERVICE_HOST is unset")
	}
	ca, err := os.ReadFile(saDir + "ca.crt")
	if err != nil {
		return nil, err
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(ca) {
		return nil, errors.New("no certificates in " + saDir + "ca.crt")
	}

	return &sysHost{
		sysClass:     sysClass,
		qualify:      qualify,
		passes:       passes,
		podResources: podResources,
		apiServer:    "https://" + net.JoinHostPort(host, port),
		http: &http.Client{
			Timeout:   10 * time.Second,
			Transport: &http.Transport{TLSClientConfig: &tls.Config{RootCAs: pool}},
		},
	}, nil
}

func (h *sysHost) Health(d Device) (string, error) {
	b, err := os.ReadFile(filepath.Join(h.sysClass, d.Name, "health"))
	return strings.TrimSpace(string(b)), err
}

func (h *sysHost) Reset(d Device) error {
	f, err := os.OpenFile(filepath.Join(h.sysClass, d.Name, "reset"), os.O_WRONLY, 0)
	if err != nil {
		return err
	}
	defer f.Close()

	if _, err := f.WriteString("1"); errors.Is(err, syscall.EBUSY) {
		return ErrBusy
	} else if err != nil {
		return err
	}
	return nil
}

// Qualify runs tools/qualify on one device and maps its exit status.
func (h *sysHost) Qualify(ctx context.Context, d Device) (Result, string) {
	ctx, cancel := context.WithTimeout(ctx, 5*time.Minute)
	defer cancel()

	out, err := exec.CommandContext(ctx, h.qualify, "-n", strconv.Itoa(h.passes),
		"/dev/"+d.Name).CombinedOutput()
	detail := strings.TrimSpace(string(out))

	var exit *exec.ExitError
	switch {
	case err == nil:
		return Pass, detail
	case errors.As(err, &exit) && exit.ExitCode() == 1:
		return Mismatch, detail
	case errors.As(err, &exit) && exit.ExitCode() == 2:
		return DeviceError, detail
	default:
		return ToolError, strings.TrimSpace(detail + " " + err.Error())
	}
}

// PodsUsing asks kubelet which pods hold the device.
func (h *sysHost) PodsUsing(ctx context.Context, d Device) ([]Pod, error) {
	conn, err := grpc.NewClient("unix://"+h.podResources,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	resp, err := podresources.NewPodResourcesListerClient(conn).List(ctx,
		&podresources.ListPodResourcesRequest{})
	if err != nil {
		return nil, err
	}

	var pods []Pod
	for _, p := range resp.GetPodResources() {
		if usesDevice(p, d.ID) {
			pods = append(pods, Pod{Namespace: p.GetNamespace(), Name: p.GetName()})
		}
	}
	return pods, nil
}

func usesDevice(p *podresources.PodResources, id string) bool {
	for _, c := range p.GetContainers() {
		for _, dev := range c.GetDevices() {
			if dev.GetResourceName() == resourceName && slices.Contains(dev.GetDeviceIds(), id) {
				return true
			}
		}
	}
	return false
}

// Evict asks the API server to evict a pod, which respects its
// PodDisruptionBudget and grace period. A pod already gone counts as evicted.
func (h *sysHost) Evict(ctx context.Context, p Pod) error {
	token, err := os.ReadFile(saDir + "token")
	if err != nil {
		return err
	}
	body, err := json.Marshal(map[string]any{
		"apiVersion": "policy/v1",
		"kind":       "Eviction",
		"metadata":   map[string]string{"name": p.Name, "namespace": p.Namespace},
	})
	if err != nil {
		return err
	}

	url := fmt.Sprintf("%s/api/v1/namespaces/%s/pods/%s/eviction", h.apiServer, p.Namespace, p.Name)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+strings.TrimSpace(string(token)))
	req.Header.Set("Content-Type", "application/json")

	resp, err := h.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode/100 == 2 || resp.StatusCode == http.StatusNotFound {
		return nil
	}
	msg, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
	return fmt.Errorf("%s: %s", resp.Status, bytes.TrimSpace(msg))
}
