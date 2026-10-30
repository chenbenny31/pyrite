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

# hold allocates 1000000 bytes (2 MiB after rounding) on whatever it is given
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
    command: ["sh", "-c", "exec /hold /dev/pyrite* 1000000"]
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

k delete pod hold hold5 --wait > /dev/null
echo "$fail failure(s)"
exit "$fail"
