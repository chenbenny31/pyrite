// SPDX-License-Identifier: GPL-2.0-only

package main

import (
	"context"
	"errors"
	"io/fs"
	"log"
	"net"
	"os"
	"path/filepath"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	pluginapi "k8s.io/kubelet/pkg/apis/deviceplugin/v1beta1"
)

const (
	resourceName = "pyrite.dev/gpu"
	socketName   = "pyrite.sock"
)

// Plugin serves kubelet's device plugin API for a fixed set of devices.
// Its fields never change after New, so RPCs need no locking.
type Plugin struct {
	pluginapi.UnimplementedDevicePluginServer

	devs      []Device
	byID      map[string]Device
	pluginDir string
}

func New(devs []Device, pluginDir string) *Plugin {
	byID := make(map[string]Device, len(devs))
	for _, d := range devs {
		byID[d.ID] = d
	}
	return &Plugin{devs: devs, byID: byID, pluginDir: pluginDir}
}

// Run serves on <pluginDir>/pyrite.sock and registers with kubelet. kubelet
// deletes plugin sockets when it restarts, so Run checks for its socket every
// second and serves and registers again once it is gone. Run returns nil when
// ctx is done, and an error if serving or registering fails.
func (p *Plugin) Run(ctx context.Context) error {
	sock := filepath.Join(p.pluginDir, socketName)
	for {
		srv, err := p.serve(sock)
		if err != nil {
			return err
		}
		if err := p.register(ctx); err != nil {
			srv.Stop()
			return err
		}
		log.Printf("registered %d device(s) as %s", len(p.devs), resourceName)

		gone := waitGone(ctx, sock)
		srv.Stop()
		if !gone {
			return nil
		}
		log.Printf("%s was removed; registering again", sock)
	}
}

func (p *Plugin) serve(sock string) (*grpc.Server, error) {
	if err := os.Remove(sock); err != nil && !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	lis, err := net.Listen("unix", sock)
	if err != nil {
		return nil, err
	}

	srv := grpc.NewServer()
	pluginapi.RegisterDevicePluginServer(srv, p)
	go srv.Serve(lis)
	return srv, nil
}

// register waits up to 10 s for kubelet.sock, which kubelet recreates
// shortly after it deletes the plugin sockets.
func (p *Plugin) register(ctx context.Context) error {
	conn, err := grpc.NewClient("unix://"+filepath.Join(p.pluginDir, "kubelet.sock"),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return err
	}
	defer conn.Close()

	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	_, err = pluginapi.NewRegistrationClient(conn).Register(ctx, &pluginapi.RegisterRequest{
		Version:      pluginapi.Version,
		Endpoint:     socketName,
		ResourceName: resourceName,
		Options:      &pluginapi.DevicePluginOptions{},
	}, grpc.WaitForReady(true))
	return err
}

// waitGone returns true once sock no longer exists, or false when ctx is done.
func waitGone(ctx context.Context, sock string) bool {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return false
		case <-tick.C:
			if _, err := os.Stat(sock); errors.Is(err, fs.ErrNotExist) {
				return true
			}
		}
	}
}

func (p *Plugin) GetDevicePluginOptions(context.Context, *pluginapi.Empty) (*pluginapi.DevicePluginOptions, error) {
	return &pluginapi.DevicePluginOptions{}, nil
}

// ListAndWatch sends every device as Healthy once, then keeps the stream open
// until kubelet or Run closes it.
func (p *Plugin) ListAndWatch(_ *pluginapi.Empty, stream grpc.ServerStreamingServer[pluginapi.ListAndWatchResponse]) error {
	resp := &pluginapi.ListAndWatchResponse{}
	for _, d := range p.devs {
		resp.Devices = append(resp.Devices, &pluginapi.Device{ID: d.ID, Health: pluginapi.Healthy})
	}
	if err := stream.Send(resp); err != nil {
		return err
	}

	<-stream.Context().Done()
	return nil
}

// Allocate gives each container the device nodes it asked for, at the same
// path as on the host.
func (p *Plugin) Allocate(_ context.Context, req *pluginapi.AllocateRequest) (*pluginapi.AllocateResponse, error) {
	resp := &pluginapi.AllocateResponse{}
	for _, creq := range req.GetContainerRequests() {
		cresp := &pluginapi.ContainerAllocateResponse{}
		for _, id := range creq.GetDevicesIds() {
			d, ok := p.byID[id]
			if !ok {
				return nil, status.Errorf(codes.InvalidArgument, "unknown device %q", id)
			}
			cresp.Devices = append(cresp.Devices, &pluginapi.DeviceSpec{
				HostPath:      d.Path,
				ContainerPath: d.Path,
				Permissions:   "rw",
			})
		}
		resp.ContainerResponses = append(resp.ContainerResponses, cresp)
	}
	return resp, nil
}
