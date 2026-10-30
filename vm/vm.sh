#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Single-node k3s VM for pyrite's Kubernetes device plugin
#
#   vm/vm.sh up          boot the VM, creating it on first use
#   vm/vm.sh provision   build, test and load pyrite in the VM; install k3s
#   vm/vm.sh ssh [cmd]   shell or command in the VM
#   vm/vm.sh down        shut the VM down
#   vm/vm.sh destroy     shut down and delete the VM disk (keeps the image)
#
# Everything lives in ./.vm, which git ignores

set -eu

R=$(cd "$(dirname "$0")/.." && pwd)
D=$R/.vm

IMG=Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2
SUMS=Fedora-Cloud-44-1.7-x86_64-CHECKSUM
URL=https://download.fedoraproject.org/pub/fedora/linux/releases/44/Cloud/x86_64/images
KEY=/etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-44-primary
K3S_VERSION=v1.36.5+k3s1
NODE=pyrite-k3s
PORT=2222
UUID_SEED=1

vssh() {
    ssh -i "$D/id_ed25519" -p "$PORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -o ConnectTimeout=5 fedora@127.0.0.1 "$@"
}

running() {
    [ -f "$D/qemu.pid" ] && kill -0 "$(cat "$D/qemu.pid")" 2>/dev/null
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
            echo "vm.sh: no SSH after 3 minutes; see $D/serial.log" >&2
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
    mkdir -p "$D/seed"
    cat > "$D/seed/meta-data" <<META
instance-id: $NODE
local-hostname: $NODE
META
    cat > "$D/seed/user-data" <<USER
#cloud-config
ssh_authorized_keys:
  - $(cat "$D/id_ed25519.pub")
ssh_pwauth: false
USER
    xorriso -as mkisofs -quiet -output "$D/seed.iso" -volid cidata -joliet -rock \
        "$D/seed/user-data" "$D/seed/meta-data"
}

up() {
    mkdir -p "$D"
    if running; then
        echo "vm.sh: already running"
        return
    fi

    fetch
    [ -f "$D/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -f "$D/id_ed25519"
    if [ ! -f "$D/disk.qcow2" ]; then
        qemu-img create -q -f qcow2 -F qcow2 -b "$D/$IMG" "$D/disk.qcow2" 20G
        seed
    fi

    qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 \
        -drive file="$D/disk.qcow2",if=virtio \
        -drive file="$D/seed.iso",if=virtio,format=raw,readonly=on \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
        -device virtio-net-pci,netdev=n0 \
        -display none -serial file:"$D/serial.log" \
        -pidfile "$D/qemu.pid" -daemonize

    wait_ssh
    vssh cloud-init status --wait > /dev/null || true
    echo "vm.sh: up; vm/vm.sh ssh for a shell"
}

provision() {
    if ! running; then
        echo "vm.sh: not running; run vm/vm.sh up first" >&2
        exit 1
    fi

    # Newest kernel first, so kernel-devel for the running kernel exists
    vssh sudo dnf -y -q upgrade kernel-core
    newest=$(vssh "rpm -q kernel-core --qf '%{VERSION}-%{RELEASE}.%{ARCH}\n' | sort -V | tail -1")
    if [ "$newest" != "$(vssh uname -r)" ]; then
        echo "vm.sh: rebooting into $newest"
        boot=$(vssh cat /proc/sys/kernel/random/boot_id)
        vssh sudo systemctl reboot || true
        wait_ssh "$boot"
    fi
    vssh sudo dnf -y -q install "kernel-devel-$newest" gcc make

    # k3s on Fedora otherwise needs Rancher's k3s-selinux policy
    vssh "sudo setenforce 0; sudo sed -i 's/^SELINUX=.*/SELINUX=permissive/' /etc/selinux/config"

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

    # Load at every boot from now on
    vssh "sudo install -D -m 644 pyrite/pyrite.ko /lib/modules/\$(uname -r)/extra/pyrite.ko &&
        sudo depmod -a &&
        echo pyrite | sudo tee /etc/modules-load.d/pyrite.conf > /dev/null &&
        echo 'options pyrite ndev=4 uuid_seed=$UUID_SEED' | sudo tee /etc/modprobe.d/pyrite.conf > /dev/null &&
        sudo modprobe pyrite"

    if ! vssh "k3s --version 2>/dev/null | grep -qF '$K3S_VERSION'"; then
        vssh "curl -sfL https://get.k3s.io | sudo INSTALL_K3S_VERSION='$K3S_VERSION' \
            INSTALL_K3S_SKIP_SELINUX_RPM=true INSTALL_K3S_SELINUX_WARN=true sh -s - \
            --disable traefik --disable servicelb --disable metrics-server \
            --write-kubeconfig-mode 644"
    fi
    vssh "timeout 180 sh -c 'until k3s kubectl get node $NODE > /dev/null 2>&1; do sleep 2; done' &&
        k3s kubectl wait --for=condition=Ready node/$NODE --timeout=180s"
    echo "vm.sh: provisioned; pyrite loaded, k3s $K3S_VERSION ready"
}

down() {
    if ! running; then
        echo "vm.sh: not running"
        return
    fi

    vssh sudo systemctl poweroff 2>/dev/null || true
    i=0
    while running && [ "$i" -lt 30 ]; do
        i=$((i + 1))
        sleep 2
    done
    if running; then
        kill "$(cat "$D/qemu.pid")"
    fi
    rm -f "$D/qemu.pid"
}

destroy() {
    down
    rm -rf "$D/disk.qcow2" "$D/seed.iso" "$D/seed" "$D/serial.log"
}

case "${1:-}" in
up) up ;;
provision) provision ;;
ssh) shift; vssh "$@" ;;
down) down ;;
destroy) destroy ;;
*) echo "usage: vm/vm.sh up | provision | ssh [cmd] | down | destroy" >&2; exit 1 ;;
esac
