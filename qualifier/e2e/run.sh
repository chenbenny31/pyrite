#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Qualifier end-to-end test in the k3s VM. Run deviceplugin/e2e/run.sh first:
# it deploys the plugin and the hold image this test uses. The agent is
# removed again at the end, because the plugin's own test injects faults the
# agent would act on.

set -eu

R=$(cd "$(dirname "$0")/../.." && pwd)
VM=$R/vm/vm.sh

make -C "$R/qualifier" image
podman save localhost/pyrite-qualifier:dev |
    "$VM" ssh sudo k3s ctr -n k8s.io images import - > /dev/null

"$VM" ssh k3s kubectl apply -f - < "$R/qualifier/deploy/qualifier.yaml"
"$VM" ssh k3s kubectl -n kube-system rollout restart ds/pyrite-qualifier

rc=0
"$VM" ssh sh -s < "$R/qualifier/e2e/checks.sh" || rc=$?

# The test's pod may still wait for a device if a check failed early
"$VM" ssh k3s kubectl delete pod qhold --ignore-not-found --wait > /dev/null
"$VM" ssh k3s kubectl delete -f - < "$R/qualifier/deploy/qualifier.yaml" > /dev/null
"$VM" ssh sudo rm -f /var/lib/pyrite/quarantine
exit "$rc"
