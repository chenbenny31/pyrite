#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Load pyrite, run the harness, unload, and fail on any kernel complaint
# Run inside a virtme-ng guest as root
# With PYRITE_DEBUG=1, also require KASAN, lockdep and kmemleak to be active

set -u

if [ "$(id -u)" -ne 0 ]; then
    echo "run.sh: must run as root inside a virtme-ng guest" >&2
    exit 1
fi

case "$(uname -r)" in
*virtme*) ;;
*) echo "run.sh: refusing to run outside a virtme-ng guest" >& 2; exit 1 ;;
esac

MOD=./pyrite.ko
HARNESS=./tests/harness
QUALIFY=./tools/qualify
LOCKDEP=/proc/lockdep_stats
KMEMLEAK=/sys/kernel/debug/kmemleak

if [ ! -f "$MOD" ] || [ ! -x "$HARNESS" ] || [ ! -x "$QUALIFY" ]; then
    echo "run.sh: build $MOD, $HARNESS and $QUALIFY first" >&2
    exit 1
fi

lockdep_on() {
    grep -Eq 'debug_locks: +1$' "$LOCKDEP"
}

# A new leak shows up only on the second scan: the first one skips
# objects whose contents changed since they were allocated
kmemleak_scan() {
    echo scan > "$KMEMLEAK"
    echo scan > "$KMEMLEAK"
}

mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug

if [ "${PYRITE_DEBUG:-0}" = 1 ]; then
    if ! dmesg | grep -q 'KernelAddressSanitizer initialized'; then
        echo "run.sh: KASAN is not active" >&2
        exit 1
    fi
    if [ ! -r "$LOCKDEP" ]; then
        echo "run.sh: lockdep is not built in" >&2
        exit 1
    fi
    if [ ! -w "$KMEMLEAK" ]; then
        echo "run.sh: kmemleak is not active" >&2
        exit 1
    fi
fi

if [ -r "$LOCKDEP" ] && ! lockdep_on; then
    echo "run.sh: lockdep was already off before the test" >&2
    exit 1
fi

# Report and forget leaks from before the test
if [ -w "$KMEMLEAK" ]; then
    kmemleak_scan
    echo clear > "$KMEMLEAK"
fi

mark=$(dmesg | wc -l)

insmod "$MOD" ndev=4 capacity_mb=8192,4096,2048,1024 || exit 1

"$HARNESS"
rc=$?

./tests/qualify.sh
rc=$((rc + $?))

rmmod pyrite || rc=$((rc + 1))

if [ -w "$KMEMLEAK" ]; then
    # kmemleak never reports objects younger than 5 s
    sleep 6
    kmemleak_scan
    cat "$KMEMLEAK" > /tmp/pyrite-kmemleak.txt
    if [ -s /tmp/pyrite-kmemleak.txt ]; then
        echo "run.sh: kmemleak reports:" >&2
        cat /tmp/pyrite-kmemleak.txt >&2
        rc=$((rc + 1))
    fi
fi

if [ -r "$LOCKDEP" ] && ! lockdep_on; then
    echo "run.sh: lockdep turned itself off during the test" >&2
    rc=$((rc + 1))
fi

dmesg | tail -n +$((mark + 1)) > /tmp/pyrite-dmesg.txt

if grep -Eq 'WARN|BUG|KASAN|leak|general protection' /tmp/pyrite-dmesg.txt; then
    echo "run.sh: kernel complaint in dmesg:" >&2
    cat /tmp/pyrite-dmesg.txt >&2
    rc=$((rc + 1))
fi

if [ "$rc" -eq 0 ]; then
    echo "run.sh: PASS"
else
    echo "run.sh: FAIL (rc=$rc)"
fi

exit $rc
