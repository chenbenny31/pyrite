#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Qualification experiment in the k3s VM. Each scenario starts the qualifier
# agent on clean devices, gives pyrite3 a stuck silent bit and the other three
# a background rate of transient silent errors, runs for DURATION seconds and
# saves the agent's event log. experiment/analyze.py turns the logs into
# results.csv and results.md.
#
# Needs the plugin deployed (deviceplugin/e2e/run.sh) and nothing else
# injecting faults; the agent's image is built and imported here.

set -eu

R=$(cd "$(dirname "$0")/.." && pwd)
VM=$R/vm/vm.sh
OUT=$R/experiment/out
DURATION=120
INTERVAL=5
BAD=3

# name scope k rate (ppm per page read on the good devices)
SCENARIOS="
d-k1-r0    device 1 0
d-k1-r200  device 1 200
d-k1-r1000 device 1 1000
d-k2-r0    device 2 0
d-k2-r200  device 2 200
d-k2-r1000 device 2 1000
n-k2-r200  node   2 200
"

vm() {
    "$VM" ssh "$@"
}

# Stop the agent and leave every device clean and in service
clean() {
    vm "k3s kubectl -n kube-system delete ds pyrite-qualifier --ignore-not-found --wait > /dev/null
        for i in 0 1 2 3; do
            echo clear | sudo tee /sys/kernel/debug/pyrite/pyrite\$i/inject > /dev/null
            echo reset | sudo tee /sys/kernel/debug/pyrite/pyrite\$i/inject > /dev/null
        done
        sudo rm -f /var/lib/pyrite/quarantine"
}

make -C "$R/qualifier" image
podman save localhost/pyrite-qualifier:dev |
    vm sudo k3s ctr -n k8s.io images import - > /dev/null
mkdir -p "$OUT"

# The list comes in on fd 3: ssh in the loop reads stdin and would eat it
while read -r name scope k rate <&3; do
    [ -n "$name" ] || continue
    echo "$name: scope=$scope k=$k rate=$rate ppm, ${DURATION}s"
    clean

    args="[\"-interval=${INTERVAL}s\",\"-retry=3s\",\"-window=2m\",\"-k=$k\",\"-scope=$scope\"]"
    vm k3s kubectl apply -f - < "$R/qualifier/deploy/qualifier.yaml" > /dev/null
    vm k3s kubectl -n kube-system patch ds pyrite-qualifier --type=json \
        -p "'[{\"op\":\"replace\",\"path\":\"/spec/template/spec/containers/0/args\",\"value\":$args}]'" > /dev/null
    vm k3s kubectl -n kube-system rollout status ds/pyrite-qualifier --timeout=120s > /dev/null
    sleep 2

    # Faults go in once the agent runs, so detection latency starts here
    t0=$(vm "date -u +%Y-%m-%dT%H:%M:%S.%NZ
        if [ $rate -gt 0 ]; then
            for i in 0 1 2 3; do
                [ \$i = $BAD ] && continue
                echo 'rate sdc $rate' | sudo tee /sys/kernel/debug/pyrite/pyrite\$i/inject > /dev/null
            done
        fi
        n=\$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')
        echo \"stuck sdc \$((n % 1048576)) \$((n % 8)) \$((n / 8 % 2))\" |
            sudo tee /sys/kernel/debug/pyrite/pyrite$BAD/inject > /dev/null")

    sleep "$DURATION"
    vm k3s kubectl -n kube-system logs ds/pyrite-qualifier > "$OUT/$name.jsonl"
    printf '{"scenario":"%s","scope":"%s","k":%s,"rate_ppm":%s,"bad":"pyrite%s","devices":4,"t0":"%s","duration_s":%s,"interval_s":%s}\n' \
        "$name" "$scope" "$k" "$rate" "$BAD" "$t0" "$DURATION" "$INTERVAL" > "$OUT/$name.meta.json"
done 3<<EOF
$SCENARIOS
EOF

clean
python3 "$R/experiment/analyze.py" "$OUT" "$R/experiment"
