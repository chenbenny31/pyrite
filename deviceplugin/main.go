// SPDX-License-Identifier: GPL-2.0-only

// Command pyrite-device-plugin advertises pyrite's mock GPUs to kubelet as
// the extended resource pyrite.dev/gpu.
package main

import (
	"context"
	"flag"
	"log"
	"os/signal"
	"syscall"
	"time"

	pluginapi "k8s.io/kubelet/pkg/apis/deviceplugin/v1beta1"
)

func main() {
	sysClass := flag.String("sysfs", "/sys/class/pyrite", "pyrite's sysfs class directory")
	pluginDir := flag.String("plugin-dir", pluginapi.DevicePluginPath, "kubelet's device plugin directory")
	interval := flag.Duration("interval", time.Second, "how often to read device health and check the plugin socket")
	quarantine := flag.String("quarantine", "/var/lib/pyrite/quarantine", "file of quarantined device UUIDs, one per line")
	flag.Parse()

	devs, err := Discover(*sysClass)
	if err != nil {
		log.Fatal(err)
	}
	for _, d := range devs {
		log.Printf("found %s at %s", d.ID, d.Path)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	if err := New(devs, *pluginDir, *interval, *quarantine).Run(ctx); err != nil {
		log.Fatal(err)
	}
}
