#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Device plugin end-to-end test in the k3s VM
# Run vm/vm.sh up and vm/vm.sh provision first

set -eu

R=$(cd "$(dirname "$0")/../.." && pwd)
VM=$R/vm/vm.sh
PLUGIN_IMAGE=localhost/pyrite-device-plugin:dev
HOLD_IMAGE=localhost/pyrite-hold:dev

make -C "$R/deviceplugin" image

# hold's build context is just its two files, not the repo with ./linux in it
ctx=$(mktemp -d)
trap 'rm -rf "$ctx"' EXIT
mkdir "$ctx/tests" "$ctx/uapi"
cp "$R/tests/hold.c" "$ctx/tests/"
cp "$R/uapi/pyrite.h" "$ctx/uapi/"
podman build -q -t "$HOLD_IMAGE" -f "$R/deviceplugin/e2e/hold.Containerfile" "$ctx" > /dev/null

# No registry: copy both images straight into k3s's containerd
for img in "$PLUGIN_IMAGE" "$HOLD_IMAGE"; do
    podman save "$img" | "$VM" ssh sudo k3s ctr -n k8s.io images import - > /dev/null
done

"$VM" ssh k3s kubectl apply -f - < "$R/deviceplugin/deploy/daemonset.yaml"
# Same tag after a rebuild, so restart to pick up the new image
"$VM" ssh k3s kubectl -n kube-system rollout restart ds/pyrite-device-plugin

"$VM" ssh sh -s < "$R/deviceplugin/e2e/checks.sh"
