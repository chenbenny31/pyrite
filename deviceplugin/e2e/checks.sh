#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Device plugin checks; e2e/run.sh feeds this to sh inside the k3s VM

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

# used_mb of every device, sorted, e.g. "0 0 0 2"
used_is() {
    [ "$(cat /sys/class/pyrite/pyrite*/used_mb | sort -n | tr '\n' ' ')" = "$1 " ]
}

node_ready() {
    [ "$(k get node $NODE -o jsonpath='{.status.conditions[?(@.type=="Ready")].status}')" = True ]
}

pod_ready() {
    k wait --for=condition=Ready "pod/$1" --timeout=60s > /dev/null
}

unschedulable() {
    [ "$(k get pod "$1" -o jsonpath='{.status.conditions[?(@.type=="PodScheduled")].reason}')" = Unschedulable ]
}

sees_one_device() {
    [ "$(k exec "$1" -- sh -c 'ls /dev/pyrite*' | wc -l)" = 1 ]
}

reregistered() {
    k -n kube-system logs ds/pyrite-device-plugin | grep -q 'registering again'
}

plugin_restarts_are() {
    [ "$(k -n kube-system get pods -l app=pyrite-device-plugin \
        -o jsonpath='{.items[0].status.containerStatuses[0].restartCount}')" = "$1" ]
}

# The pod runs hold on each device it is given; hold allocates 1000000 bytes,
# 2 MiB after rounding
hold_pod() { # name, GPU count
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
        pyrite.dev/gpu: $2
YAML
}

k delete pod hold hold5 --ignore-not-found --wait > /dev/null

check "plugin rolled out" k -n kube-system rollout status ds/pyrite-device-plugin --timeout=120s
check "allocatable reaches 4" within 60 allocatable_is 4

hold_pod hold 1
check "1-GPU pod runs" pod_ready hold
check "pod sees one /dev/pyrite*" sees_one_device hold
check "one device holds 2 MiB" within 10 used_is "0 0 0 2"

hold_pod hold5 5
check "5-GPU pod stays Pending" within 30 unschedulable hold5

start=$(date +%s)
k delete pod hold --wait > /dev/null
check "deleting the pod frees the memory" within 30 used_is "0 0 0 0"
echo "    reclaim after delete: $(($(date +%s) - start)) s"

# kubelet deletes plugin sockets on start; the plugin must register again
sudo systemctl restart k3s
check "node Ready after k3s restart" within 120 node_ready
check "plugin registered again" within 60 reregistered
check "plugin was not restarted" plugin_restarts_are 0
check "allocatable back to 4" within 60 allocatable_is 4
hold_pod hold 1
check "new pod gets a device" pod_ready hold
check "one device holds 2 MiB again" within 10 used_is "0 0 0 2"

# Health: the plugin reads sysfs health, and kubelet stops counting a failed
# device but leaves the pod on it running

now_ms() {
    date +%s%3N
}

# Retry a command every 0.1 s for up to $1 seconds; on success, set took to
# the milliseconds since $2
timed() {
    end=$(($(now_ms) + $1 * 1000))
    from=$2
    shift 2
    until "$@" 2> /dev/null; do
        if [ "$(now_ms)" -ge "$end" ]; then
            return 1
        fi
        sleep 0.1
    done
    took=$(($(now_ms) - from))
}

inject() { # device index, command
    echo "$2" | sudo tee "/sys/kernel/debug/pyrite/pyrite$1/inject" > /dev/null
}

plugin_logged() { # device index, kubelet health, health file reading
    k -n kube-system logs ds/pyrite-device-plugin | grep -q "/dev/pyrite$1 GPU-[^:]*: $2 ($3)"
}

pod_running() {
    [ "$(k get pod "$1" -o jsonpath='{.status.phase}')" = Running ]
}

# kubelet's per-device health in the pod's status (ResourceHealthStatus)
pod_device_health_is() { # pod, health
    [ "$(k get pod "$1" -o jsonpath='{.status.containerStatuses[0].allocatedResourcesStatus[0].resources[0].health}')" = "$2" ]
}

devices_of() {
    k exec "$1" -- sh -c 'ls /dev/pyrite*'
}

# The pod has $2 devices, and pyrite$3 is not one of them
has_devices_without() { # pod, count, device index
    devs=$(devices_of "$1")
    [ "$(echo "$devs" | wc -l)" = "$2" ] && ! echo "$devs" | grep -qx "/dev/pyrite$3"
}

k delete pod hold5 --wait > /dev/null
bad=$(devices_of hold)
bad=${bad#/dev/pyrite}

t0=$(now_ms)
inject "$bad" "xid 48"
check "plugin reports pyrite$bad Unhealthy" timed 10 "$t0" plugin_logged "$bad" Unhealthy xid
detect=$took
check "allocatable drops to 3" timed 30 "$t0" allocatable_is 3
echo "    xid 48 on pyrite$bad -> plugin: $detect ms, node allocatable: $took ms"
check "pod on the failed device keeps running" pod_running hold
check "pod status shows its device Unhealthy" within 30 pod_device_health_is hold Unhealthy

# The pod on the failed device still counts against the lower allocatable,
# so 3 - 1 = 2 GPUs are free, not 3
hold_pod hold2 2
check "2-GPU pod runs" pod_ready hold2
check "2-GPU pod did not get pyrite$bad" has_devices_without hold2 2 "$bad"
check "three devices hold 2 MiB" within 10 used_is "0 2 2 2"
hold_pod hold3 3
check "3-GPU pod stays Pending" within 30 unschedulable hold3
k delete pod hold2 hold3 --wait > /dev/null

t0=$(now_ms)
inject "$bad" reset
check "reset brings allocatable back to 4" timed 30 "$t0" allocatable_is 4
echo "    reset -> node allocatable: $took ms"
check "pod status shows its device Healthy again" within 30 pod_device_health_is hold Healthy

lost=$(((bad + 1) % 4))
inject "$lost" lost
check "lost on pyrite$lost drops allocatable to 3" within 30 allocatable_is 3
inject "$lost" reset
check "reset brings it back to 4" within 30 allocatable_is 4

# A UUID in the qualifier's quarantine file reads as Unhealthy
echo "$(cat /sys/class/pyrite/pyrite0/uuid)" | sudo tee /var/lib/pyrite/quarantine > /dev/null
check "quarantined pyrite0 drops allocatable to 3" within 30 allocatable_is 3
sudo rm /var/lib/pyrite/quarantine
check "unquarantined, allocatable is back to 4" within 30 allocatable_is 4

# Unloaded, every health read fails; the plugin keeps running through it
k delete pod hold --wait > /dev/null
sudo modprobe -r pyrite
check "unloading pyrite drops allocatable to 0" within 30 allocatable_is 0
sudo modprobe pyrite
check "reloading brings all four back" within 30 allocatable_is 4
check "plugin was not restarted by the reload" plugin_restarts_are 0

echo "$fail failure(s)"
exit "$fail"
