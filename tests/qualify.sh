#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# tools/qualify against injected memory errors on pyrite3
# run.sh calls this with pyrite loaded and debugfs mounted

set -u

Q=./tools/qualify
DEV=/dev/pyrite3
DBG=/sys/kernel/debug/pyrite/pyrite3
fail=0

inject() {
    echo "$1" > "$DBG/inject"
}

# name, expected exit status, then the command; leaves its output in $out
expect() {
    name=$1
    want=$2
    shift 2
    out=$("$@" 2>&1)
    got=$?
    if [ "$got" = "$want" ]; then
        echo "ok  qualify: $name"
    else
        echo "FAIL qualify: $name: exit $got, want $want: $out"
        fail=$((fail + 1))
    fi
}

# qualify's bit error count must match what pyrite handed out silently
counts_match() {
    n=$(echo "$out" | sed -n 's/.*fail: \([0-9]*\) bit errors.*/\1/p')
    if [ -n "$n" ] && [ "$n" = "$(cat "$DBG/sdc_count")" ]; then
        echo "ok  qualify: $1"
    else
        echo "FAIL qualify: $1: reported '$n', sdc_count $(cat "$DBG/sdc_count")"
        fail=$((fail + 1))
    fi
}

expect "clean device passes" 0 $Q -s 1 $DEV

inject "stuck sdc 100 3 1"
expect "stuck-at-1 silent bit fails" 1 $Q -s 1 $DEV
counts_match "reported errors match sdc_count"
inject clear

inject "stuck sdc 100 3 0"
expect "stuck-at-0 silent bit fails" 1 $Q -s 1 $DEV
inject clear

inject "stuck ce 100 3 1"
expect "corrected bit passes" 0 $Q -s 1 $DEV
inject clear

inject "stuck ue 100 3 1"
expect "uncorrected bit is a device error" 2 $Q -s 1 $DEV
inject clear
inject reset

inject "rate sdc 1000000"
expect "transient silent errors fail" 1 $Q -s 1 $DEV
inject clear

inject lost
expect "lost device is a device error" 2 $Q -s 1 $DEV
expect "worst result wins" 2 $Q -s 1 /dev/pyrite2 $DEV
inject reset

expect "missing device is a device error" 2 $Q /dev/pyrite99
expect "-n 0 is a usage error" 3 $Q -n 0 $DEV
expect "no device is a usage error" 3 $Q

inject clear
inject reset
echo "$fail qualify failure(s)"
exit "$fail"
