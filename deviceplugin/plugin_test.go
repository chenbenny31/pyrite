// SPDX-License-Identifier: GPL-2.0-only

package main

import (
	"context"
	"errors"
	"fmt"
	"io/fs"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	pluginapi "k8s.io/kubelet/pkg/apis/deviceplugin/v1beta1"
)

const (
	uuid0 = "GPU-0e108210-008a-4001-ae40-008982159000"
	uuid1 = "GPU-c9a21ac0-8120-4d83-a878-00856245a060"
)

// fakeSysfs makes <tmp>/pyriteN/uuid for each uuid, with sysfs's trailing newline.
func fakeSysfs(t *testing.T, uuids ...string) string {
	t.Helper()
	dir := t.TempDir()
	for i, u := range uuids {
		d := filepath.Join(dir, fmt.Sprintf("pyrite%d", i))
		if err := os.Mkdir(d, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(d, "uuid"), []byte(u+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestDiscover(t *testing.T) {
	devs, err := Discover(fakeSysfs(t, uuid0, uuid1))
	if err != nil {
		t.Fatal(err)
	}
	want := []Device{{uuid0, "/dev/pyrite0"}, {uuid1, "/dev/pyrite1"}}
	if fmt.Sprint(devs) != fmt.Sprint(want) {
		t.Fatalf("got %v, want %v", devs, want)
	}
}

func TestDiscoverNone(t *testing.T) {
	for _, dir := range []string{fakeSysfs(t), filepath.Join(t.TempDir(), "missing")} {
		if _, err := Discover(dir); err == nil {
			t.Errorf("Discover(%s): no error for no devices", dir)
		}
	}
}

// fakeKubelet answers Register on <dir>/kubelet.sock and passes each request on.
type fakeKubelet struct {
	pluginapi.UnimplementedRegistrationServer
	reqs chan *pluginapi.RegisterRequest
}

func (k *fakeKubelet) Register(_ context.Context, req *pluginapi.RegisterRequest) (*pluginapi.Empty, error) {
	k.reqs <- req
	return &pluginapi.Empty{}, nil
}

func startKubelet(t *testing.T, dir string) *fakeKubelet {
	t.Helper()
	lis, err := net.Listen("unix", filepath.Join(dir, "kubelet.sock"))
	if err != nil {
		t.Fatal(err)
	}
	k := &fakeKubelet{reqs: make(chan *pluginapi.RegisterRequest, 4)}
	srv := grpc.NewServer()
	pluginapi.RegisterRegistrationServer(srv, k)
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)
	return k
}

func (k *fakeKubelet) wait(t *testing.T) *pluginapi.RegisterRequest {
	t.Helper()
	select {
	case req := <-k.reqs:
		return req
	case <-time.After(5 * time.Second):
		t.Fatal("no Register within 5 s")
		return nil
	}
}

func dialPlugin(t *testing.T, dir string) pluginapi.DevicePluginClient {
	t.Helper()
	conn, err := grpc.NewClient("unix://"+filepath.Join(dir, socketName),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return pluginapi.NewDevicePluginClient(conn)
}

// startPlugin runs a plugin for uuid0 and uuid1 in dir. The returned function
// stops it and fails the test unless Run returns nil within 5 s.
func startPlugin(t *testing.T, dir string) func() {
	t.Helper()
	devs, err := Discover(fakeSysfs(t, uuid0, uuid1))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- New(devs, dir).Run(ctx) }()

	return func() {
		cancel()
		select {
		case err := <-done:
			if err != nil {
				t.Errorf("Run: %v", err)
			}
		case <-time.After(5 * time.Second):
			t.Error("Run did not return within 5 s of cancel")
		}
	}
}

func TestPlugin(t *testing.T) {
	dir := t.TempDir()
	k := startKubelet(t, dir)
	stop := startPlugin(t, dir)

	req := k.wait(t)
	if req.Version != pluginapi.Version || req.Endpoint != socketName || req.ResourceName != resourceName {
		t.Fatalf("Register: got %v", req)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	c := dialPlugin(t, dir)

	opts, err := c.GetDevicePluginOptions(ctx, &pluginapi.Empty{})
	if err != nil || opts.PreStartRequired || opts.GetPreferredAllocationAvailable {
		t.Fatalf("GetDevicePluginOptions: %v, %v", opts, err)
	}

	stream, err := c.ListAndWatch(ctx, &pluginapi.Empty{})
	if err != nil {
		t.Fatal(err)
	}
	lw, err := stream.Recv()
	if err != nil {
		t.Fatal(err)
	}
	if len(lw.Devices) != 2 || lw.Devices[0].ID != uuid0 || lw.Devices[1].ID != uuid1 ||
		lw.Devices[0].Health != pluginapi.Healthy || lw.Devices[1].Health != pluginapi.Healthy {
		t.Fatalf("ListAndWatch: got %v", lw.Devices)
	}

	resp, err := c.Allocate(ctx, &pluginapi.AllocateRequest{
		ContainerRequests: []*pluginapi.ContainerAllocateRequest{
			{DevicesIds: []string{uuid1}},
			{DevicesIds: []string{uuid0, uuid1}},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, cr := range resp.ContainerResponses {
		s := ""
		for _, d := range cr.Devices {
			s += d.HostPath + ">" + d.ContainerPath + ":" + d.Permissions + " "
		}
		got = append(got, s)
	}
	want := []string{
		"/dev/pyrite1>/dev/pyrite1:rw ",
		"/dev/pyrite0>/dev/pyrite0:rw /dev/pyrite1>/dev/pyrite1:rw ",
	}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Fatalf("Allocate: got %q, want %q", got, want)
	}

	_, err = c.Allocate(ctx, &pluginapi.AllocateRequest{
		ContainerRequests: []*pluginapi.ContainerAllocateRequest{{DevicesIds: []string{"GPU-bogus"}}},
	})
	if status.Code(err) != codes.InvalidArgument {
		t.Fatalf("Allocate of unknown ID: got %v, want InvalidArgument", err)
	}

	stop()
	if _, err := os.Stat(filepath.Join(dir, socketName)); !errors.Is(err, fs.ErrNotExist) {
		t.Errorf("socket still there after stop: %v", err)
	}
}

// kubelet deletes plugin sockets when it restarts; the plugin must notice
// and register again on a new socket
func TestReregister(t *testing.T) {
	dir := t.TempDir()
	k := startKubelet(t, dir)
	stop := startPlugin(t, dir)
	defer stop()

	k.wait(t)
	if err := os.Remove(filepath.Join(dir, socketName)); err != nil {
		t.Fatal(err)
	}
	k.wait(t)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	stream, err := dialPlugin(t, dir).ListAndWatch(ctx, &pluginapi.Empty{})
	if err != nil {
		t.Fatal(err)
	}
	if lw, err := stream.Recv(); err != nil || len(lw.Devices) != 2 {
		t.Fatalf("ListAndWatch after re-register: %v, %v", lw, err)
	}
}
