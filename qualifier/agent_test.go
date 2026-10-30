// SPDX-License-Identifier: GPL-2.0-only

package main

import (
	"context"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

var (
	dev0 = Device{Name: "pyrite0", ID: "GPU-0e108210-008a-4001-ae40-008982159000"}
	dev1 = Device{Name: "pyrite1", ID: "GPU-c9a21ac0-8120-4d83-a878-00856245a060"}
	t0   = time.Date(2026, 10, 30, 12, 0, 0, 0, time.UTC)
)

// fakeHost acts like pyrite: reset clears health unless the device is busy.
type fakeHost struct {
	health  map[string]string   // by device name; missing means "ok"
	results map[string][]Result // queued qualify results; empty means Pass
	pods    map[string][]Pod    // pods holding each device
	busy    map[string]bool     // Reset returns ErrBusy
	calls   []string            // "qualify pyrite0", "reset pyrite1", "evict ns/p"
}

func newFakeHost() *fakeHost {
	return &fakeHost{health: map[string]string{}, results: map[string][]Result{},
		pods: map[string][]Pod{}, busy: map[string]bool{}}
}

func (f *fakeHost) Health(d Device) (string, error) {
	if h, ok := f.health[d.Name]; ok {
		return h, nil
	}
	return "ok", nil
}

func (f *fakeHost) Reset(d Device) error {
	f.calls = append(f.calls, "reset "+d.Name)
	if f.busy[d.Name] {
		return ErrBusy
	}
	delete(f.health, d.Name)
	return nil
}

func (f *fakeHost) Qualify(_ context.Context, d Device) (Result, string) {
	f.calls = append(f.calls, "qualify "+d.Name)
	q := f.results[d.Name]
	if len(q) == 0 {
		return Pass, ""
	}
	f.results[d.Name] = q[1:]
	return q[0], ""
}

func (f *fakeHost) PodsUsing(_ context.Context, d Device) ([]Pod, error) {
	return f.pods[d.Name], nil
}

// Evict removes the pod at once; the agent still waits a Retry before reset.
func (f *fakeHost) Evict(_ context.Context, p Pod) error {
	f.calls = append(f.calls, "evict "+p.Namespace+"/"+p.Name)
	for name, ps := range f.pods {
		var keep []Pod
		for _, q := range ps {
			if q != p {
				keep = append(keep, q)
			}
		}
		f.pods[name] = keep
	}
	return nil
}

func testConfig(t *testing.T) Config {
	return Config{
		Interval:       time.Minute,
		K:              2,
		Window:         10 * time.Minute,
		Retry:          30 * time.Second,
		MaxRequalify:   2,
		QuarantineFile: filepath.Join(t.TempDir(), "quarantine"),
	}
}

func newTestAgent(t *testing.T, cfg Config, f *fakeHost) *Agent {
	t.Helper()
	a, err := NewAgent(cfg, f, slog.New(slog.NewTextHandler(io.Discard, nil)), []Device{dev0, dev1})
	if err != nil {
		t.Fatal(err)
	}
	return a
}

func step(t *testing.T, a *Agent, now time.Time) {
	t.Helper()
	if err := a.Step(context.Background(), now); err != nil {
		t.Fatal(err)
	}
}

// quarantined returns the device names listed in the quarantine file.
func quarantined(t *testing.T, a *Agent) string {
	t.Helper()
	b, err := os.ReadFile(a.cfg.QuarantineFile)
	if os.IsNotExist(err) {
		return ""
	}
	if err != nil {
		t.Fatal(err)
	}
	var names []string
	for _, id := range strings.Fields(string(b)) {
		for _, d := range []Device{dev0, dev1} {
			if d.ID == id {
				names = append(names, d.Name)
			}
		}
	}
	return strings.Join(names, " ")
}

func TestOneMismatchIsNotEnough(t *testing.T) {
	f := newFakeHost()
	f.results["pyrite1"] = []Result{Mismatch}
	a := newTestAgent(t, testConfig(t), f)

	for i := 0; i < 5; i++ {
		step(t, a, t0.Add(time.Duration(i)*time.Minute))
	}
	if got := quarantined(t, a); got != "" {
		t.Fatalf("quarantined %q after a single mismatch with k=2", got)
	}
}

func TestKFailuresInWindowQuarantine(t *testing.T) {
	f := newFakeHost()
	f.results["pyrite1"] = []Result{Mismatch, Mismatch, Mismatch}
	f.pods["pyrite1"] = []Pod{{"default", "job"}}
	a := newTestAgent(t, testConfig(t), f)

	step(t, a, t0)
	if got := quarantined(t, a); got != "" {
		t.Fatalf("quarantined %q after one mismatch", got)
	}
	step(t, a, t0.Add(time.Minute))
	if got := quarantined(t, a); got != "pyrite1" {
		t.Fatalf("quarantined %q, want pyrite1", got)
	}
	if !strings.Contains(strings.Join(f.calls, ","), "evict default/job") {
		t.Fatalf("pod on the device not evicted: %v", f.calls)
	}
}

func TestOldFailuresExpire(t *testing.T) {
	f := newFakeHost()
	f.results["pyrite1"] = []Result{Mismatch, Mismatch}
	cfg := testConfig(t)
	cfg.Interval = 11 * time.Minute
	a := newTestAgent(t, cfg, f)

	step(t, a, t0)
	step(t, a, t0.Add(11*time.Minute))
	if got := quarantined(t, a); got != "" {
		t.Fatalf("quarantined %q from failures 11 min apart, window 10 min", got)
	}
}

// A device that keeps failing requalification stays out, and the agent
// stops trying after MaxRequalify attempts
func TestBadDeviceStaysOut(t *testing.T) {
	f := newFakeHost()
	f.results["pyrite0"] = []Result{DeviceError, DeviceError, Mismatch, Pass}
	a := newTestAgent(t, testConfig(t), f)

	step(t, a, t0) // device error quarantines at once; requalify fails (1)
	if got := quarantined(t, a); got != "pyrite0" {
		t.Fatalf("quarantined %q, want pyrite0", got)
	}
	step(t, a, t0.Add(30*time.Second)) // requalify fails (2): give up
	n := len(f.calls)
	step(t, a, t0.Add(time.Minute))
	step(t, a, t0.Add(2*time.Minute))
	for _, c := range f.calls[n:] {
		if strings.HasSuffix(c, "pyrite0") {
			t.Fatalf("kept working on a device it gave up on: %v", f.calls[n:])
		}
	}
	if got := quarantined(t, a); got != "pyrite0" {
		t.Fatalf("quarantined %q after giving up, want pyrite0", got)
	}
}

// A reported fault quarantines at once; once the pod is gone and the reset
// goes through, a passing qualification restores the device
func TestHealthFaultRequalifies(t *testing.T) {
	f := newFakeHost()
	f.health["pyrite1"] = "xid"
	f.pods["pyrite1"] = []Pod{{"default", "job"}}
	f.busy["pyrite1"] = true
	a := newTestAgent(t, testConfig(t), f)

	step(t, a, t0) // quarantine, evict
	if got := quarantined(t, a); got != "pyrite1" {
		t.Fatalf("quarantined %q, want pyrite1", got)
	}
	step(t, a, t0.Add(30*time.Second)) // reset is busy
	if got := quarantined(t, a); got != "pyrite1" {
		t.Fatalf("restored %q while reset was busy", got)
	}
	f.busy["pyrite1"] = false
	step(t, a, t0.Add(60*time.Second)) // reset, qualify, restore
	if got := quarantined(t, a); got != "" {
		t.Fatalf("quarantined %q after a passing requalification", got)
	}
	var calls []string
	for _, c := range f.calls {
		if !strings.HasSuffix(c, "pyrite0") {
			calls = append(calls, c)
		}
	}
	want := "evict default/job,reset pyrite1,reset pyrite1,qualify pyrite1"
	if got := strings.Join(calls, ","); got != want {
		t.Fatalf("calls %q, want %q", got, want)
	}
}

func TestToolErrorChangesNothing(t *testing.T) {
	f := newFakeHost()
	f.results["pyrite0"] = []Result{ToolError, ToolError, ToolError}
	a := newTestAgent(t, testConfig(t), f)

	for i := 0; i < 3; i++ {
		step(t, a, t0.Add(time.Duration(i)*time.Minute))
	}
	if got := quarantined(t, a); got != "" {
		t.Fatalf("quarantined %q because qualify itself failed", got)
	}
}

func TestNodeScopeBlocksEveryDevice(t *testing.T) {
	f := newFakeHost()
	f.health["pyrite0"] = "lost"
	f.busy["pyrite0"] = true
	cfg := testConfig(t)
	cfg.NodeScope = true
	a := newTestAgent(t, cfg, f)

	step(t, a, t0)
	if got := quarantined(t, a); got != "pyrite0 pyrite1" {
		t.Fatalf("quarantined %q, want both devices", got)
	}
	f.busy["pyrite0"] = false
	step(t, a, t0.Add(30*time.Second))
	if got := quarantined(t, a); got != "" {
		t.Fatalf("quarantined %q after the bad device requalified", got)
	}
}

// A restarted agent keeps devices from the existing file out until they
// requalify
func TestRestartKeepsQuarantine(t *testing.T) {
	cfg := testConfig(t)
	if err := os.WriteFile(cfg.QuarantineFile, []byte(dev1.ID+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	f := newFakeHost()
	f.busy["pyrite1"] = true
	a := newTestAgent(t, cfg, f)

	step(t, a, t0)
	if got := quarantined(t, a); got != "pyrite1" {
		t.Fatalf("quarantined %q after restart, want pyrite1", got)
	}
	f.busy["pyrite1"] = false
	step(t, a, t0.Add(30*time.Second))
	if got := quarantined(t, a); got != "" {
		t.Fatalf("quarantined %q after requalifying", got)
	}
}
