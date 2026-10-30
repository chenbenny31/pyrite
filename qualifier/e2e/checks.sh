#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Qualifier checks; e2e/run.sh feeds this to sh inside the k3s VM

set -u

NODE=pyrite-k3s
fail=0

k() {
    k3s kubectl "$@"
}

check() { # name, command...
    name=$1
    shift
    if "$@"; then
        echo "ok   $name"
    else
        echo "FAIL $name"
        fail=$((fail + 1))
    fi
}

# Retry a command once a second for up to $1 seconds
within() {
    n=$1
    shift
    until "$@" 2> /dev/null; do
        n=$((n - 1))
        if [ "$n" -le 0 ]; then
            return 1
        fi
        sleep 1
    done
}

allocatable_is() {
    [ "$(k get node $NODE -o jsonpath='{.status.allocatable.pyrite\.dev/gpu}')" = "$1" ]
}

inject() { # device index, command
    echo "$2" | sudo tee "/sys/kernel/debug/pyrite/pyrite$1/inject" > /dev/null
}

# The agent logged an event for a device since the mark, e.g.
# logged quarantine 2 'qualify: mismatch'
mark=0
logged() { # event, device index, text in the line
    k -n kube-system logs ds/pyrite-qualifier | tail -n +$((mark + 1)) |
        grep "\"msg\":\"$1\"" | grep "\"dev\":\"pyrite$2\"" | grep -qF "${3:-}"
}

set_mark() {
    mark=$(k -n kube-system logs ds/pyrite-qualifier | wc -l)
}

pod_gone() {
    ! k get pod "$1" > /dev/null 2>&1
}

hold_pod() { # name
    k apply -f - > /dev/null <<YAML
apiVersion: v1
kind: Pod
metadata:
  name: $1
spec:
  restartPolicy: Never
  terminationGracePeriodSeconds: 1
  containers:
  - name: hold
    image: localhost/pyrite-hold:dev
    imagePullPolicy: Never
    command: ["sh", "-c", "for d in /dev/pyrite*; do /hold \$d 1000000 & done; wait"]
    resources:
      limits:
        pyrite.dev/gpu: 1
YAML
}

now_ms() {
    date +%s%3N
}

k delete pod qhold --ignore-not-found --wait > /dev/null
check "agent rolled out" k -n kube-system rollout status ds/pyrite-qualifier --timeout=120s
check "every device passes its first qualification" within 60 sh -c '
    for i in 0 1 2 3; do
        k3s kubectl -n kube-system logs ds/pyrite-qualifier |
            grep "\"msg\":\"qualify\"" | grep "\"dev\":\"pyrite$i\"" | grep -q "\"result\":\"pass\"" || exit 1
    done'
check "nothing quarantined" allocatable_is 4

# A stuck silent bit under a running pod: two failed runs quarantine the
# device, the pod is evicted, and requalification keeps failing
hold_pod qhold
k wait --for=condition=Ready pod/qhold --timeout=60s > /dev/null
bad=$(k exec qhold -- sh -c 'ls /dev/pyrite*')
bad=${bad#/dev/pyrite}
set_mark
t0=$(now_ms)
inject "$bad" "stuck sdc 100 3 1"
check "silent error quarantines pyrite$bad" within 60 logged quarantine "$bad" "qualify: mismatch"
echo "    stuck sdc -> quarantine: $(($(now_ms) - t0)) ms"
check "allocatable drops to 3" within 30 allocatable_is 3
check "its pod is evicted" within 60 pod_gone qhold
check "requalification fails while the bit is stuck" within 60 logged requalify "$bad" mismatch
check "the qualifier saw what pyrite handed out" [ "$(sudo cat /sys/kernel/debug/pyrite/pyrite$bad/sdc_count)" -gt 0 ]
inject "$bad" clear
check "after clear it requalifies and comes back" within 60 logged restore "$bad"
check "allocatable back to 4" within 30 allocatable_is 4

# One failed run is not enough with k=2
other=$(((bad + 1) % 4))
set_mark
inject "$other" "rate sdc 1000000"
within 30 logged qualify "$other" mismatch
inject "$other" "rate sdc 0"
inject "$other" clear
sleep 25
check "a single transient failure does not quarantine" sh -c "! k3s kubectl -n kube-system logs ds/pyrite-qualifier | tail -n +$((mark + 1)) | grep '\"msg\":\"quarantine\"' | grep -q '\"dev\":\"pyrite$other\"'"

# A reported fault quarantines at once; the sysfs reset brings it back
set_mark
inject "$other" "xid 48"
check "xid quarantines pyrite$other at once" within 30 logged quarantine "$other" "health: xid"
check "reset and requalify bring it back" within 60 logged restore "$other"
check "its health is ok again" [ "$(cat /sys/class/pyrite/pyrite$other/health)" = ok ]

# An uncorrected bit fails qualification as a device error until cleared
set_mark
inject "$other" "stuck ue 100 3 1"
check "uncorrected error quarantines pyrite$other" within 60 logged quarantine "$other" "device error"
check "requalification fails while it is stuck" within 60 logged requalify "$other" "device error"
inject "$other" clear
check "after clear it comes back" within 60 logged restore "$other"
check "allocatable back to 4 again" within 30 allocatable_is 4

# Node scope: one bad device blocks all four
k -n kube-system patch ds pyrite-qualifier --type=json \
    -p '[{"op":"add","path":"/spec/template/spec/containers/0/args/-","value":"-scope=node"}]' > /dev/null
k -n kube-system rollout status ds/pyrite-qualifier --timeout=120s > /dev/null
set_mark
inject 0 "stuck sdc 100 3 1"
check "node scope: one bad device drops allocatable to 0" within 90 allocatable_is 0
inject 0 clear
check "node scope: all four come back" within 90 allocatable_is 4

for i in 0 1 2 3; do
    inject $i clear
    inject $i reset
done
echo "$fail failure(s)"
exit "$fail"
