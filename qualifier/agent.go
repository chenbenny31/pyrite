// SPDX-License-Identifier: GPL-2.0-only

package main

import (
	"context"
	"errors"
	"io/fs"
	"log/slog"
	"os"
	"strings"
	"time"
)

// Result of one run of tools/qualify on one device
type Result int

const (
	Pass        Result = iota
	Mismatch           // data came back wrong: silent corruption
	DeviceError        // the device failed an open, read or write
	ToolError          // qualify could not run; says nothing about the device
)

func (r Result) String() string {
	return [...]string{"pass", "mismatch", "device error", "tool error"}[r]
}

// ErrBusy is what Host.Reset returns while the device has allocations.
var ErrBusy = errors.New("device busy")

// Device is one pyrite device on this node.
type Device struct {
	Name string // "pyrite2"
	ID   string // sysfs uuid
}

// Pod is a pod holding a device.
type Pod struct {
	Namespace, Name string
}

// Host is everything the agent touches outside its own state: sysfs, the
// qualify tool, kubelet's PodResources API and the API server's evictions.
type Host interface {
	Health(d Device) (string, error)
	Reset(d Device) error
	Qualify(ctx context.Context, d Device) (Result, string)
	PodsUsing(ctx context.Context, d Device) ([]Pod, error)
	Evict(ctx context.Context, p Pod) error
}

type Config struct {
	Interval       time.Duration // between qualifications of a device in service
	K              int           // failed qualifications within Window that quarantine a device
	Window         time.Duration
	NodeScope      bool          // one quarantined device blocks every device on the node
	Retry          time.Duration // between requalification steps
	MaxRequalify   int           // failed requalifications before giving up
	QuarantineFile string
}

type devState struct {
	Device
	failures    []time.Time // failed qualifications within the window
	lastQualify time.Time

	// Set while the device itself is quarantined
	bad      bool
	reason   string
	since    time.Time
	lastTry  time.Time
	attempts int
	gaveUp   bool
}

type Agent struct {
	cfg     Config
	host    Host
	log     *slog.Logger
	devs    []*devState
	written string // quarantine file contents as last written
}

// NewAgent starts with the devices already in the quarantine file marked
// bad, so a restarted agent keeps them out and requalifies them.
func NewAgent(cfg Config, host Host, log *slog.Logger, devs []Device) (*Agent, error) {
	a := &Agent{cfg: cfg, host: host, log: log}

	b, err := os.ReadFile(cfg.QuarantineFile)
	if err != nil && !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	a.written = string(b)
	listed := make(map[string]bool)
	for _, id := range strings.Fields(a.written) {
		listed[id] = true
	}

	for _, d := range devs {
		s := &devState{Device: d}
		if listed[d.ID] {
			s.bad, s.reason = true, "listed before agent start"
			log.Info("quarantine", "dev", d.Name, "uuid", d.ID, "reason", s.reason)
		}
		a.devs = append(a.devs, s)
	}
	return a, nil
}

// Run calls Step once a second until ctx is done. It returns an error only
// if the quarantine file can't be written.
func (a *Agent) Run(ctx context.Context) error {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		if err := a.Step(ctx, time.Now()); err != nil {
			return err
		}
		select {
		case <-ctx.Done():
			return nil
		case <-tick.C:
		}
	}
}

// Step does one round at time now. Devices in service get their health
// checked and, every Interval, a qualification. Quarantined devices move one
// step toward requalification every Retry: evict their pods, reset, qualify.
func (a *Agent) Step(ctx context.Context, now time.Time) error {
	for _, d := range a.devs {
		if d.bad {
			continue
		}
		if h, err := a.host.Health(d.Device); err != nil {
			a.quarantine(d, now, "health: "+err.Error())
			continue
		} else if h != "ok" {
			a.quarantine(d, now, "health: "+h)
			continue
		}
		if now.Sub(d.lastQualify) >= a.cfg.Interval {
			d.lastQualify = now
			a.qualify(ctx, d, now)
		}
	}

	for _, d := range a.devs {
		if d.bad && !d.gaveUp && now.Sub(d.lastTry) >= a.cfg.Retry {
			d.lastTry = now
			a.requalify(ctx, d, now)
		}
	}

	return a.writeQuarantine()
}

func (a *Agent) qualify(ctx context.Context, d *devState, now time.Time) {
	r, detail := a.host.Qualify(ctx, d.Device)
	a.log.Info("qualify", "dev", d.Name, "result", r.String(), "detail", detail)

	switch r {
	case Mismatch:
		d.failures = append(recent(d.failures, now, a.cfg.Window), now)
		if len(d.failures) >= a.cfg.K {
			a.quarantine(d, now, "qualify: mismatch")
		}
	case DeviceError:
		a.quarantine(d, now, "qualify: device error")
	}
}

// recent keeps the times less than window before now.
func recent(ts []time.Time, now time.Time, window time.Duration) []time.Time {
	keep := ts[:0]
	for _, t := range ts {
		if now.Sub(t) < window {
			keep = append(keep, t)
		}
	}
	return keep
}

func (a *Agent) quarantine(d *devState, now time.Time, reason string) {
	d.bad, d.reason, d.since = true, reason, now
	d.lastTry, d.attempts, d.gaveUp = time.Time{}, 0, false
	a.log.Info("quarantine", "dev", d.Name, "uuid", d.ID, "reason", reason)
}

// requalify takes one step: evict the device's pods and come back later, or,
// once none are left, reset the device and qualify it.
func (a *Agent) requalify(ctx context.Context, d *devState, now time.Time) {
	pods, err := a.host.PodsUsing(ctx, d.Device)
	if err != nil {
		a.log.Warn("pods", "dev", d.Name, "err", err.Error())
		return
	}
	if len(pods) > 0 {
		for _, p := range pods {
			err := a.host.Evict(ctx, p)
			a.log.Info("evict", "dev", d.Name, "pod", p.Namespace+"/"+p.Name, "err", errText(err))
		}
		return
	}

	if err := a.host.Reset(d.Device); err != nil {
		a.log.Info("reset", "dev", d.Name, "err", err.Error())
		return
	}

	r, detail := a.host.Qualify(ctx, d.Device)
	switch r {
	case ToolError:
		a.log.Warn("requalify", "dev", d.Name, "result", r.String(), "detail", detail)
	case Pass:
		a.log.Info("restore", "dev", d.Name, "uuid", d.ID, "reason", d.reason,
			"quarantined_for", now.Sub(d.since).String())
		d.bad, d.failures, d.lastQualify = false, nil, now
	default:
		d.attempts++
		a.log.Info("requalify", "dev", d.Name, "result", r.String(), "detail", detail,
			"attempt", d.attempts)
		if d.attempts >= a.cfg.MaxRequalify {
			d.gaveUp = true
			a.log.Warn("give_up", "dev", d.Name, "uuid", d.ID,
				"reason", "requalification keeps failing; replace the device")
		}
	}
}

func errText(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

// writeQuarantine writes the UUIDs to keep out of service, one per line, when
// they change. It writes a temporary file and renames it, so the plugin never
// reads half a list.
func (a *Agent) writeQuarantine() error {
	anyBad := false
	for _, d := range a.devs {
		anyBad = anyBad || d.bad
	}

	var text string
	for _, d := range a.devs {
		if d.bad || (a.cfg.NodeScope && anyBad) {
			text += d.ID + "\n"
		}
	}
	if text == a.written {
		return nil
	}

	tmp := a.cfg.QuarantineFile + ".tmp"
	if err := os.WriteFile(tmp, []byte(text), 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, a.cfg.QuarantineFile); err != nil {
		return err
	}
	a.written = text
	a.log.Info("quarantine_file", "devices", strings.Count(text, "\n"))
	return nil
}
