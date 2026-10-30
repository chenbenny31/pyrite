// SPDX-License-Identifier: GPL-2.0-only

// Command pyrite-qualifier runs on every node next to the device plugin. It
// tests each pyrite device with tools/qualify, quarantines bad ones through
// a file the plugin reads, and brings them back once they requalify.
package main

import (
	"context"
	"flag"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	sysClass := flag.String("sysfs", "/sys/class/pyrite", "pyrite's sysfs class directory")
	qualify := flag.String("qualify", "/usr/local/bin/qualify", "the qualify binary")
	passes := flag.Int("passes", 1, "qualify passes per run")
	interval := flag.Duration("interval", time.Minute, "between qualifications of a device in service")
	k := flag.Int("k", 2, "failed qualifications within -window that quarantine a device")
	window := flag.Duration("window", 10*time.Minute, "window for -k")
	scope := flag.String("scope", "device", "quarantine one device (device) or all on the node (node)")
	retry := flag.Duration("retry", 30*time.Second, "between requalification steps")
	maxRequalify := flag.Int("max-requalify", 3, "failed requalifications before giving up")
	quarantine := flag.String("quarantine", "/var/lib/pyrite/quarantine", "file of quarantined device UUIDs")
	podResources := flag.String("pod-resources", "/var/lib/kubelet/pod-resources/kubelet.sock", "kubelet's PodResources socket")
	flag.Parse()

	log := slog.New(slog.NewJSONHandler(os.Stdout, nil))
	fail := func(msg string, err error) {
		log.Error(msg, "err", err.Error())
		os.Exit(1)
	}

	if *passes < 1 || *k < 1 || *maxRequalify < 1 || (*scope != "device" && *scope != "node") {
		log.Error("bad flags: -passes, -k and -max-requalify must be at least 1; -scope is device or node")
		os.Exit(2)
	}

	devs, err := discover(*sysClass)
	if err != nil {
		fail("discover", err)
	}
	host, err := newSysHost(*sysClass, *qualify, *passes, *podResources)
	if err != nil {
		fail("setup", err)
	}
	agent, err := NewAgent(Config{
		Interval:       *interval,
		K:              *k,
		Window:         *window,
		NodeScope:      *scope == "node",
		Retry:          *retry,
		MaxRequalify:   *maxRequalify,
		QuarantineFile: *quarantine,
	}, host, log, devs)
	if err != nil {
		fail("quarantine file", err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	log.Info("start", "devices", len(devs), "scope", *scope, "interval", interval.String(),
		"k", *k, "window", window.String())
	if err := agent.Run(ctx); err != nil {
		fail("run", err)
	}
}
