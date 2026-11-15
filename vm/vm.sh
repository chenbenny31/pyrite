#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# QEMU VMs with pyrite loaded: VM 1 is a single-node k3s cluster for pyrite's
# Kubernetes device plugin; VMs 2..N add plain nodes for multi-node schedulers
#
#   vm/vm.sh up [N]            boot VMs 1..N (default 1), creating them on first use
#   vm/vm.sh provision         build, test and load pyrite in every running VM at once;
#                              install k3s in VM 1
#   vm/vm.sh ssh [-n I] [cmd]  shell or command in VM I (default 1)
#   vm/vm.sh down              shut every VM down
#   vm/vm.sh destroy           shut down and delete the VM disks (keeps the image)
#
# VM I listens for SSH on 127.0.0.1 port 2221+I and loads pyrite with uuid_seed=I.
# Everything lives in ./.vm, which git ignores

set -eu

R=$(cd "$(dirname "$0")/.." && pwd)
D=$R/.vm

IMG=Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2
SUMS=Fedora-Cloud-44-1.7-x86_64-CHECKSUM
URL=https://download.fedoraproject.org/pub/fedora/linux/releases/44/Cloud/x86_64/images
KEY=/etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-44-primary
K3S_VERSION=v1.36.5+k3s1

# Select VM $1: VM 1 is the k3s server and keeps the original paths; VMs 2..N
# live in .vm/vmI with less memory, since they run no k3s
node() {
    PORT=$((2221 + $1))
    UUID_SEED=$1
    if [ "$1" -eq 1 ]; then
        NODE=pyrite-k3s V=$D MEM=4096 CPUS=4 K3S=yes
    else
        NODE=pyrite-$1 V=$D/vm$1 MEM=2048 CPUS=2 K3S=
    fi
}

vssh() {
    ssh -i "$D/id_ed25519" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -o ConnectTimeout=5 fedora@127.0.0.1 "$@"
}

running() {
    [ -f "$V/qemu.pid" ] && kill -0 "$(cat "$V/qemu.pid")" 2>/dev/null
}

# Run a command once per VM that has a disk, VM 1 first; VMs are numbered without gaps.
# The callee may read n and use i, but must not change n
each() {
    n=1
    while node "$n" && [ -f "$V/disk.qcow2" ]; do
        "$@"
        n=$((n + 1))
    done
}

# Wait until SSH answers, from a boot other than $1 if given
wait_ssh() {
    i=0
    while :; do
        id=$(vssh cat /proc/sys/kernel/random/boot_id 2>/dev/null) || id=
        if [ -n "$id" ] && [ "$id" != "${1:-}" ]; then
            return
        fi
        i=$((i + 1))
        if [ "$i" -ge 90 ]; then
            echo "vm.sh: no SSH from $NODE after 3 minutes; see $V/serial.log" >&2
            exit 1
        fi
        sleep 2
    done
}

# Download the cloud image once; trust only checksums signed by Fedora's key
fetch() {
    [ -f "$D/$IMG" ] && return

    curl -fL -o "$D/$SUMS" "$URL/$SUMS"
    gpg --dearmor < "$KEY" > "$D/fedora.gpg"
    rm -f "$D/sums.txt"
    gpgv --keyring "$D/fedora.gpg" --output "$D/sums.txt" "$D/$SUMS"

    curl -fL -o "$D/$IMG.part" "$URL/$IMG"
    want=$(sed -n "s/^SHA256 ($IMG) = //p" "$D/sums.txt")
    got=$(sha256sum "$D/$IMG.part" | cut -d' ' -f1)
    if [ -z "$want" ] || [ "$want" != "$got" ]; then
        echo "vm.sh: $IMG checksum mismatch" >&2
        exit 1
    fi
    mv "$D/$IMG.part" "$D/$IMG"
}

# cloud-init NoCloud seed: hostname and our SSH key for the fedora user
seed() {
    mkdir -p "$V/seed"
    cat > "$V/seed/meta-data" <<META
instance-id: $NODE
local-hostname: $NODE
META
    cat > "$V/seed/user-data" <<USER
#cloud-config
ssh_authorized_keys:
  - $(cat "$D/id_ed25519.pub")
ssh_pwauth: false
USER
    xorriso -as mkisofs -quiet -output "$V/seed.iso" -volid cidata -joliet -rock \
        "$V/seed/user-data" "$V/seed/meta-data"
}

# Start the selected VM in the background, creating its disk on first use
start() {
    if running; then
        echo "vm.sh: $NODE already running"
        return
    fi
    mkdir -p "$V"
    if [ ! -f "$V/disk.qcow2" ]; then
        qemu-img create -q -f qcow2 -F qcow2 -b "$D/$IMG" "$V/disk.qcow2" 20G
        seed
    fi

    qemu-system-x86_64 -enable-kvm -cpu host -smp "$CPUS" -m "$MEM" \
        -drive file="$V/disk.qcow2",if=virtio \
        -drive file="$V/seed.iso",if=virtio,format=raw,readonly=on \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
        -device virtio-net-pci,netdev=n0 \
        -display none -serial file:"$V/serial.log" \
        -pidfile "$V/qemu.pid" -daemonize
}

# Boot VMs 1..$1 together, then wait for each one's SSH and cloud-init, and name
# the ones without pyrite: new VMs, or VMs never provisioned
up() {
    mkdir -p "$D"
    fetch
    [ -f "$D/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -f "$D/id_ed25519"

    n=1
    while [ "$n" -le "$1" ]; do
        node "$n"
        start
        n=$((n + 1))
    done
    bare=
    n=1
    while [ "$n" -le "$1" ]; do
        node "$n"
        wait_ssh
        vssh cloud-init status --wait > /dev/null || true
        vssh test -d /sys/class/pyrite || bare="$bare $NODE"
        n=$((n + 1))
    done
    echo "vm.sh: $1 VM(s) up; vm/vm.sh ssh [-n I] for a shell"
    if [ -n "$bare" ]; then
        echo "vm.sh: no pyrite yet in$bare; vm/vm.sh provision builds and loads it"
    fi
}

# Provision the selected VM; runs in the background with output in its provision.log
provision_vm() {
    # Newest kernel first, so kernel-devel for the running kernel exists
    vssh sudo dnf -y -q upgrade kernel-core
    newest=$(vssh "rpm -q kernel-core --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | sort -V | tail -1")
    if [ "$newest" != "$(vssh uname -r)" ]; then
        echo "vm.sh: rebooting $NODE into $newest"
        boot=$(vssh cat /proc/sys/kernel/random/boot_id)
        vssh sudo systemctl reboot || true
        wait_ssh "$boot"
    fi
    vssh sudo dnf -y -q install "kernel-devel-$newest" gcc make

    # k3s on Fedora otherwise needs Rancher's k3s-selinux policy
    if [ -n "$K3S" ]; then
        vssh "sudo setenforce 0; sudo sed -i 's/^SELINUX=.*/SELINUX=permissive/' /etc/selinux/config"
    fi

    # Working tree: tracked and untracked files, minus ignored ones
    git -C "$R" ls-files -z --cached --others --exclude-standard |
        tar -C "$R" --null -T - -czf - |
        vssh 'rm -rf pyrite && mkdir pyrite && tar -C pyrite -xzf -'
    vssh 'make -C pyrite KDIR=/lib/modules/$(uname -r)/build &&
        gcc -Wall -Wextra -O2 -o pyrite/tests/harness pyrite/tests/harness.c -lpthread'

    # Harness on the Fedora kernel, with run.sh's load parameters
    vssh 'sudo modprobe -r pyrite 2>/dev/null || true
        sudo insmod pyrite/pyrite.ko ndev=4 capacity_mb=8192,4096,2048,1024 || exit 1
        sudo pyrite/tests/harness; rc=$?
        sudo rmmod pyrite; exit $rc'

    # Any user may open the devices, as with /dev/nvidia*, so a scheduler and its jobs
    # need no root
    vssh "echo 'SUBSYSTEM==\"pyrite\", MODE=\"0666\"' | sudo tee /etc/udev/rules.d/90-pyrite.rules > /dev/null &&
        sudo udevadm control --reload"

    # Load at every boot from now on
    vssh "sudo install -D -m 644 pyrite/pyrite.ko /lib/modules/\$(uname -r)/extra/pyrite.ko &&
        sudo depmod -a &&
        echo pyrite | sudo tee /etc/modules-load.d/pyrite.conf > /dev/null &&
        echo 'options pyrite ndev=4 uuid_seed=$UUID_SEED' | sudo tee /etc/modprobe.d/pyrite.conf > /dev/null &&
        sudo modprobe pyrite"

    if [ -z "$K3S" ]; then
        echo "vm.sh: $NODE provisioned; pyrite loaded"
        return
    fi
    if ! vssh "k3s --version 2>/dev/null | grep -qF '$K3S_VERSION'"; then
        vssh "curl -sfL https://get.k3s.io | sudo INSTALL_K3S_VERSION='$K3S_VERSION' \
            INSTALL_K3S_SKIP_SELINUX_RPM=true INSTALL_K3S_SELINUX_WARN=true sh -s - \
            --disable traefik --disable servicelb --disable metrics-server \
            --write-kubeconfig-mode 644"
    fi
    vssh "timeout 180 sh -c 'until k3s kubectl get node $NODE > /dev/null 2>&1; do sleep 2; done' &&
        k3s kubectl wait --for=condition=Ready node/$NODE --timeout=180s"
    echo "vm.sh: $NODE provisioned; pyrite loaded, k3s $K3S_VERSION ready"
}

# Start provisioning the selected VM in the background, or skip it when it is not running
provision_start() {
    if ! running; then
        echo "vm.sh: $NODE not running; skipped"
        return
    fi
    provision_vm < /dev/null > "$V/provision.log" 2>&1 &
    started="$started $n:$!"
    echo "vm.sh: provisioning $NODE; log in $V/provision.log"
}

# Provision every running VM at once, since the time goes to dnf and a reboot inside
# each VM, then report each one. Job control (set -m) puts each VM's job in its own
# process group, so Ctrl-C stops all of them, ssh included
provision() {
    set -m
    started=
    trap 'for s in $started; do kill -- "-${s#*:}"; done 2>/dev/null; exit 130' INT TERM
    each provision_start
    if [ -z "$started" ]; then
        echo "vm.sh: no VM running; run vm/vm.sh up first" >&2
        exit 1
    fi

    failed=0
    for s in $started; do
        node "${s%:*}"
        if wait "${s#*:}"; then
            echo "vm.sh: $NODE provisioned"
        else
            echo "vm.sh: $NODE failed; end of $V/provision.log:" >&2
            tail -n 20 "$V/provision.log" >&2
            failed=1
        fi
    done
    exit "$failed"
}

down_vm() {
    if ! running; then
        echo "vm.sh: $NODE not running"
        return
    fi

    vssh sudo systemctl poweroff 2>/dev/null || true
    i=0
    while running && [ "$i" -lt 30 ]; do
        i=$((i + 1))
        sleep 2
    done
    if running; then
        kill "$(cat "$V/qemu.pid")"
    fi
    rm -f "$V/qemu.pid"
}

destroy_vm() {
    down_vm
    rm -rf "$V/disk.qcow2" "$V/seed.iso" "$V/seed" "$V/serial.log" "$V/provision.log"
}

# A VM count or index: a positive number
number() {
    case $1 in
    '' | *[!0-9]* | 0) echo "vm.sh: $2 must be a positive number, not '$1'" >&2; exit 1 ;;
    esac
}

case "${1:-}" in
up)
    number "${2:-1}" "the VM count"
    up "${2:-1}"
    ;;
provision) provision ;;
ssh)
    shift
    vm=1
    if [ "${1:-}" = -n ]; then
        vm=${2:-}
        if [ $# -ge 2 ]; then shift 2; else shift; fi
    fi
    number "$vm" "the VM index"
    node "$vm"
    vssh "$@"
    ;;
down) each down_vm ;;
destroy) each destroy_vm ;;
*) echo "usage: vm/vm.sh up [N] | provision | ssh [-n I] [cmd] | down | destroy" >&2; exit 1 ;;
esac
