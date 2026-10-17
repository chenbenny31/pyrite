#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Load pyrite, run the harness, unload, and fail on any kernel complaint
# Run inside a virtme-ng guest as root

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

if [ ! -f "$MOD" ] || [ ! -x "$HARNESS" ]; then
    echo "run.sh: build $MOD and $HARNESS first" >&2
    exit 1
fi

mark=$(dmesg | wc -l)

insmod "$MOD" ndev=4 capacity_mb=8192,4096,2048,1024 || exit 1

"$HARNESS"
rc=$?

rmmod pyrite || rc=$((rc + 1))

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
