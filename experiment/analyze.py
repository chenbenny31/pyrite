#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Turn the qualification experiment's agent logs into results.csv and results.md.

usage: analyze.py OUT_DIR RESULTS_DIR

OUT_DIR holds <scenario>.jsonl (the agent's JSON event log) and
<scenario>.meta.json (scope, k, rate, bad device, t0, duration) from run.sh.
"""

import csv
import glob
import json
import os
import re
import sys
from datetime import datetime

# Page reads in one qualify run: 4 patterns over 1 MiB of scratch in 4 KiB pages
PAGE_READS_PER_RUN = 4 * 256


def parse_time(s):
    # Go writes nanoseconds; datetime keeps microseconds
    return datetime.fromisoformat(re.sub(r"(\.\d{6})\d*", r"\1", s).replace("Z", "+00:00"))


def analyze(meta, events):
    t0 = parse_time(meta["t0"])
    end = t0.timestamp() + meta["duration_s"]
    bad = meta["bad"]
    after = [e for e in events if parse_time(e["time"]).timestamp() >= t0.timestamp()]

    def at(e):
        return parse_time(e["time"]).timestamp() - t0.timestamp()

    # Detection: the bad device's first quarantine after the faults went in
    detect = next((at(e) for e in after if e["msg"] == "quarantine" and e.get("dev") == bad), None)

    # Good devices: how often a scheduled run failed, against 1 - (1 - r)^N
    runs = [e for e in after if e["msg"] == "qualify" and e.get("dev") != bad
            and e.get("result") in ("pass", "mismatch")]
    fails = sum(e["result"] == "mismatch" for e in runs)
    expected = 1 - (1 - meta["rate_ppm"] / 1e6) ** PAGE_READS_PER_RUN

    false_q = [e for e in after if e["msg"] == "quarantine" and e.get("dev") != bad]
    given_up = [e for e in after if e["msg"] == "give_up" and e.get("dev") != bad]

    # Time a falsely quarantined device stayed out, quarantine to restore
    out_since, restores = {}, []
    for e in after:
        dev = e.get("dev")
        if dev == bad:
            continue
        if e["msg"] == "quarantine":
            out_since[dev] = at(e)
        elif e["msg"] == "restore" and dev in out_since:
            restores.append(at(e) - out_since.pop(dev))

    # Capacity: devices listed in the quarantine file over time
    blocked, last_t, last_n = 0.0, 0.0, 0
    for e in after:
        if e["msg"] == "quarantine_file":
            t = min(at(e), meta["duration_s"])
            blocked += last_n * (t - last_t)
            last_t, last_n = t, e["devices"]
    blocked += last_n * (meta["duration_s"] - last_t)
    total = meta["devices"] * meta["duration_s"]
    needed = meta["duration_s"] - detect if detect is not None else 0.0

    good_hours = (meta["devices"] - 1) * meta["duration_s"] / 3600
    return {
        "scenario": meta["scenario"],
        "scope": meta["scope"],
        "k": meta["k"],
        "rate_ppm": meta["rate_ppm"],
        "detect_s": round(detect, 1) if detect is not None else "",
        "missed": int(detect is None),
        "good_runs": len(runs),
        "good_fail_rate": round(fails / len(runs), 3) if runs else "",
        "expected_fail_rate": round(expected, 3),
        "false_quarantines": len(false_q),
        "false_per_device_hour": round(len(false_q) / good_hours, 1),
        "good_given_up": len(given_up),
        "mean_restore_s": round(sum(restores) / len(restores), 1) if restores else "",
        "capacity_lost_pct": round(100 * blocked / total, 1),
        "wasted_capacity_pct": round(100 * max(blocked - needed, 0) / total, 1),
    }


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out_dir, results_dir = sys.argv[1], sys.argv[2]

    rows = []
    for meta_path in sorted(glob.glob(os.path.join(out_dir, "*.meta.json"))):
        with open(meta_path) as f:
            meta = json.load(f)
        with open(os.path.join(out_dir, meta["scenario"] + ".jsonl")) as f:
            events = [json.loads(line) for line in f if line.startswith("{")]
        rows.append(analyze(meta, events))

    with open(os.path.join(results_dir, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    cols = [("scenario", "Scenario"), ("detect_s", "Detect (s)"), ("missed", "Missed"),
            ("good_fail_rate", "Good-run fail rate"), ("expected_fail_rate", "Expected"),
            ("false_quarantines", "False quarantines"), ("good_given_up", "Good given up"),
            ("mean_restore_s", "Restore (s)"), ("capacity_lost_pct", "Capacity lost %"),
            ("wasted_capacity_pct", "Wasted %")]
    with open(os.path.join(results_dir, "results.md"), "w") as f:
        f.write("| " + " | ".join(h for _, h in cols) + " |\n")
        f.write("|" + " --- |" * len(cols) + "\n")
        for r in rows:
            f.write("| " + " | ".join(str(r[c]) for c, _ in cols) + " |\n")
    print(open(os.path.join(results_dir, "results.md")).read())


if __name__ == "__main__":
    main()
