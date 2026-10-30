# pyrite

pyrite lets you test a GPU scheduler on a machine with no GPUs. Each
`/dev/pyriteN` is a mock GPU with a fixed VRAM capacity. It accepts or refuses
allocations in 2 MiB units and reports usage per process (tgid) through
`ioctl` and sysfs. When a client dies, even by `SIGKILL`, the kernel frees
everything it allocated. A test can also make a device fail on cue, from an Xid
error to memory that silently returns wrong data, and check that the scheduler
and its health checks catch it.

In a guest, with the module and `tests/hold` built:

```sh
insmod pyrite.ko ndev=4 capacity_mb=8192,4096,2048,1024
./tests/hold /dev/pyrite0 1000000 &      # prints alloc_id, rounded size, pid
cat /sys/class/pyrite/pyrite0/used_mb    # 2
kill -9 %1
cat /sys/class/pyrite/pyrite0/used_mb    # 0
```

**Status:** the module, its UAPI header, a 16-test harness and `tests/run.sh`
are done, including device health, scratch memory and fault injection. The
tests pass on the plain kernel and on a debug kernel with KASAN, lockdep and
kmemleak. A [Kubernetes device plugin](#kubernetes) hands the devices to pods
in a k3s VM and stops handing out a device once it fails. A [node
agent](#qualification) finds bad devices, silent memory errors included, with a
known-answer test and keeps them out of service until they pass again. An
[experiment](#experiment) measures how fast it finds a bad device and what
false alarms cost. [Planned](#planned) lists what comes next.

## How it differs

pyrite's devices keep state that real processes change. Most other ways to test
a GPU scheduler without GPUs fake only what the scheduler reads, such as the GPU
count. No process can hold memory on those GPUs, so a job that dies while
holding some, or a GPU that fails under a running job, can't be tested.

| Core decision | What a test gets | What the alternatives do |
| --- | --- | --- |
| The ledger is in a kernel module | one view of usage per device and per process, shared by every process on the node; the kernel frees a client's memory when its last descriptor closes, even after `SIGKILL` | fake-GPU device plugins and KWOK keep no per-process state; NVML mocks keep it inside one test process |
| Faults change what calls return | after an injected Xid, ALLOC fails with `EIO`; a lost GPU fails every ioctl with `ENODEV`; a stuck memory bit returns wrong data that only a known-answer test catches | a fake plugin can report a device Unhealthy, but a job already on it never sees an error, and there is no memory to corrupt |
| It runs under a real kubelet | pods are real processes that open `/dev/pyriteN`, handed out by a device plugin that keys devices by UUID | KWOK simulates nodes and pods without running them |

pyrite stops at accounting. It reserves no memory and runs no GPU code, and
NVML, CUDA and `nvidia-smi` can't see its devices, so a scheduler needs a small
backend for pyrite's ioctls and sysfs. It also enforces nothing on real GPUs;
the `dmem` cgroup controller and CUDA interception do that.
[Related work](#related-work) compares these tools in more detail, and
[Limits](#limits) lists what pyrite leaves out.

## Why a kernel module

Cleanup runs in the kernel, so it does not depend on the client behaving.
However a process dies, the kernel closes its file descriptors (`do_exit` →
`exit_files` → `fput`). When the last reference to a file goes away, the
driver's `->release` runs, and pyrite frees that file's allocations there.
Nothing has to notice the death first. A userspace daemon can also detect a
dead client, through socket EOF or a pidfd, but its cleanup is a separate step
that runs later and can fail on its own.

pyrite frees in `->release` rather than `->flush`. `->flush` runs on every
`close()`, so it would free memory that a `dup`'d descriptor, a forked child or
an `SCM_RIGHTS` receiver still holds. `->release` runs once, after the last
reference is gone. So a scheduler can test a job whose shell exits while a
forked child still has the descriptor open: the memory stays in use until the
child exits too.

## NVML and CUDA

The queries follow NVML's shape, so a backend written for pyrite has the same
structure as an NVML one. `QUERY_DEVICE` matches `nvmlDeviceGetMemoryInfo`
plus a utilization figure, and `QUERY_PROCS` matches
`nvmlDeviceGetComputeRunningProcesses`.

An allocation is only an entry in a ledger, so no memory is reserved and no
hardware is touched. pyrite does not implement CUDA, and NVML and `nvidia-smi`
cannot see its devices.

## Devices and parameters

`insmod` takes four parameters:

| Parameter | Meaning |
| --- | --- |
| `ndev` | number of devices, 1..16, default 4 |
| `capacity_mb` | capacity per device in MiB; `0` or unset means 8192 |
| `uuid_seed` | seed for device UUIDs, default 0 |
| `scratch_kb` | [scratch memory](#scratch-memory) per device in KiB, 4..16384, default 1024 |

Each device gets these attributes under `/sys/class/pyrite/pyriteN/`, all
read-only except `reset`:

| Attribute | Content |
| --- | --- |
| `capacity_mb` | set at load time |
| `used_mb` | sum of live allocations |
| `util` | synthetic, 0..100, decays over time |
| `numa_node` | always `0` |
| `pcie_addr` | `0000:NN:00.0`, from the device index |
| `uuid` | `GPU-` plus a version 4 UUID |
| `health` | `ok`, `xid` or `lost`; see [Faults](#faults) |
| `last_xid` | last injected Xid code; `0` if none since load or reset |
| `scratch_kb` | scratch memory size in KiB |
| `ecc_ce_count` | corrected memory errors since load or `clear` |
| `ecc_ue_count` | uncorrected memory errors since load or `clear` |
| `reset` | write-only, root: `1` resets the device; see [Faults](#faults) |

`numa_node` and `pcie_addr` are fake. They are there because discovery code
expects them.

UUIDs come from the kernel's seeded PRNG, 16 bytes per device in index order.
The same seed gives the same UUIDs on every load, the way a real GPU keeps its
UUID across driver reloads. Kubernetes records allocated devices by ID, so a
reload does not invalidate them. Give each node in a cluster its own seed.

## ioctl interface

`uapi/pyrite.h` defines the ABI. Every struct holds only `__u64` fields with no
padding, so `compat_ptr_ioctl` covers 32-bit callers.

| Command | Struct (bytes) | Cmd word | Errors |
| --- | --- | --- | --- |
| `PYRITE_IOC_ALLOC` | `pyrite_alloc_req` (16) | `0xC010BE01` | `EINVAL`, `ENOMEM`, `EFAULT`, `EIO`, `ENODEV` |
| `PYRITE_IOC_FREE` | `pyrite_free_req` (8) | `0x4008BE02` | `EINVAL`, `EFAULT`, `ENODEV` |
| `PYRITE_IOC_QUERY_DEVICE` | `pyrite_device_info` (32) | `0x8020BE03` | `EFAULT`, `ENODEV` |
| `PYRITE_IOC_QUERY_PROCS` | `pyrite_procs_query` (16) | `0xC010BE04` | `ERANGE`, `ENOMEM`, `EFAULT`, `ENODEV` |

`EIO` and `ENODEV` come from device health; see [Faults](#faults). Other
commands return `ENOTTY`. The magic number is `0xBE`. The first version
used `0xB7`, but `nsfs.h` and `remoteproc_cdev.h` already use that one.

- **ALLOC.** A size of 0 returns `EINVAL`. Sizes are rounded up to a multiple
  of 2 MiB, and the rounded size is written back. A request that does not fit
  in the remaining capacity, before or after rounding, returns `ENOMEM`. IDs
  start at 1, so 0 is never a valid ID.
- **Ownership.** An allocation belongs to the open file that made it. Another
  descriptor cannot free it, even one in the same process. An unknown ID and
  an ID owned by another file both return `EINVAL`, so FREE cannot be used to
  probe for IDs.
- **Accounting.** Usage is charged to the tgid that called ALLOC. Passing the
  descriptor to another process with `SCM_RIGHTS` does not move the charge.
- **Reclaim.** When a file's last reference drops, all of its allocations are
  freed, including after `SIGKILL`. If ALLOC returns `EFAULT` while copying the
  result back, the allocation already exists. It stays with the file and is
  freed on release.
- **QUERY_PROCS sizing.** Call with `buf_ptr == 0`, or with a `count` that is
  too small, to get `ERANGE` and the needed `count`. A device with no
  allocations returns 0 with `count = 0`.
- **ABI changes.** The ABI is not stable. The struct size is part of the cmd
  word, so after a layout change old callers get `ENOTTY` instead of corrupt
  data.

## Scratch memory

Each device also has a block of memory, `scratch_kb` in size, that a test reads
and writes with `read`, `write`, `pread` and `pwrite` on `/dev/pyriteN`. It is
there so memory errors have somewhere to show up. It is a plain kernel buffer,
separate from the ALLOC ledger and never mapped. Everyone who opens the device
shares it, so only one tester at a time should use it.

A read at or past the end returns 0, and a write there returns `ENOSPC`.
`lseek` with `SEEK_END` gives the size. A `lost` device fails reads and writes
with `ENODEV`; an `xid` device still allows them, so it can be tested after a
fault.

## Faults

A real GPU can't be made to fail at a chosen point in a test. With pyrite, root
writes a command to `/sys/kernel/debug/pyrite/pyriteN/inject`, and the device
acts as if the fault happened:

```sh
echo "xid 48" > /sys/kernel/debug/pyrite/pyrite1/inject
cat /sys/class/pyrite/pyrite1/health     # xid
echo reset > /sys/kernel/debug/pyrite/pyrite1/inject
```

| Command | Effect | dmesg |
| --- | --- | --- |
| `xid <code>`, code > 0 | health `xid`, `last_xid` = code | `pyrite pyriteN: Xid <code>` |
| `lost` | health `lost` | `pyrite pyriteN: fell off the bus` |
| `reset` | health `ok`, `last_xid` 0 | `pyrite pyriteN: reset` |

Anything else returns `EINVAL`. Each command works from any state, and `reset`
leaves allocations alone. Health changes what the ioctls return:

| Call | `ok` | `xid` | `lost` |
| --- | --- | --- | --- |
| ALLOC | normal | `EIO` | `ENODEV` |
| FREE | normal | normal | `ENODEV` |
| QUERY_DEVICE, QUERY_PROCS | normal | normal | `ENODEV` |

`xid` is a GPU that reported a fatal error. Work already on it can still free
memory and monitors can still query it, but nothing new can be allocated.
`lost` is a GPU that fell off the bus, so every ioctl fails. In every state,
open, close and the sysfs files work as usual, and release still frees a dead
client's memory. Argument errors (`EFAULT`, a zero-byte or oversize ALLOC) are
reported before health.

An operator resets a device by writing `1` to `/sys/class/pyrite/pyriteN/reset`.
Like `nvidia-smi -r`, it refuses with `EBUSY` while the device has allocations,
so the work on it has to leave first. It sets health back to `ok` and leaves
memory errors in place. The `reset` command in debugfs is a test knob and skips
the allocation check.

The `inject` file is in debugfs because it is a test knob and stays out of the
ABI. Without debugfs, pyrite works the same but can't fake faults.

### Memory errors

The same `inject` file plants errors in scratch memory. Each error has a class,
which decides what a reader sees:

| Class | What a read returns | What pyrite reports |
| --- | --- | --- |
| `ce` (corrected) | the right data | `ecc_ce_count` goes up |
| `ue` (uncorrected) | `EIO` | `ecc_ue_count` goes up; health `xid` with `last_xid` 48, NVIDIA's Xid for an uncorrectable ECC error; `Xid 48` in dmesg |
| `sdc` (silent) | wrong data | nothing |

| Command | Effect |
| --- | --- |
| `stuck <class> <offset> <bit> <0\|1>` | that bit always reads as the given value; up to 64 cells per device |
| `rate <class> <ppm>` | each 4 KiB page read has a ppm/1,000,000 chance of one random bit error; `0` turns it off |
| `clear` | removes all memory errors and zeroes the counters |

A write stores what it was given, and errors apply only to what a read returns.
A stuck cell shows only when the stored bit differs from its stuck value, so a
stuck-at-1 bit hides under `0xFF`. A memory test therefore needs a pattern and
its inverse, as real ones do. `reset` leaves memory errors in place, because a
GPU reset doesn't fix bad memory; `clear` stands for replacing the card.

A known-answer test has to find silent errors on its own, since pyrite reports
nothing. To check such a test, `/sys/kernel/debug/pyrite/pyriteN/sdc_count`
counts the silent bit errors pyrite has handed to readers. It lives in debugfs,
out of reach of the plugin and other monitors.

Random errors come from a stream seeded from the device's UUID, and `clear`
restarts it, so the same reads in the same order give the same errors.

## Design notes

- One `cdev` covers all `ndev` minors. Each device has its own ledger and one
  mutex.
- `capacity`, `index` and `uuid` are set before `cdev_add` and never change, so
  they are read without the lock.
- `health` and `last_xid` are under the device mutex. Each ioctl checks health
  inside the section it already locks, so an ALLOC either finishes before an
  injected fault or fails after it.
- `copy_from_user` and `copy_to_user` never run under the mutex. Data is
  copied into a local snapshot under the lock and copied out after unlocking.
- ALLOC allocates its list node before locking, because its size is known.
  QUERY_PROCS allocates its buffer under the lock, because the size depends on
  the ledger. The 2 MiB unit caps that buffer at `capacity / 2 MiB` entries,
  which is 64 KiB for an 8 GiB device.
- `bytes > capacity` is checked before rounding, because
  `ALIGN(U64_MAX, SZ_2M)` wraps to 0.
- Utilization is synthetic. Each ALLOC or FREE adds 25 to the decayed value,
  capped at 100. A read halves it for every full second since the last ALLOC
  or FREE, and returns 0 after 32 seconds so the shift stays within a `u32`.
  Release does not change it.
- debugfs is created last in init and removed first in exit. `debugfs_remove`
  waits for writes in progress, so no `inject` write can touch a freed device.
- Scratch reads and writes go one 4 KiB page at a time through a bounce page,
  so the copy rule above holds for them too. Memory errors are applied to the
  bounce copy under the lock, and the buffer keeps what was written.

## Build and run

You need a kernel tree to build against and
[virtme-ng](https://github.com/arighi/virtme-ng) to boot it. The tree defaults
to `./linux` in this repo, which git ignores. Override it with `KDIR=`. pyrite
was developed against v6.19.14. Keep `virtme` in the kernel's version suffix
(`./linux` uses `-virtme`), because `run.sh` checks `uname -r` for it.

| Step | Where | Command |
| --- | --- | --- |
| Build module | host | `make && make check` |
| Build harness | host | `gcc -Wall -Wextra -O2 -o tests/harness tests/harness.c -lpthread` |
| Build qualify | host | `gcc -Wall -Wextra -O2 -o tools/qualify tools/qualify.c` |
| Build reclaim demo | host | `gcc -Wall -Wextra -O2 -o tests/hold tests/hold.c` |
| Boot guest | host, repo root | `vng -r ./linux --user root` |
| Run tests | guest | `./tests/run.sh` |

`make check` runs `checkpatch.pl` on the module and the UAPI header. Boot with
`--user root`, or `insmod` fails with `EPERM`.

### Debug kernel

`./linux-debug` is the same kernel with KASAN, lockdep and kmemleak on. It is a
git worktree of `./linux`, so it shares the git objects but keeps its own
build, and git ignores it too. Its config is `./linux/.config` plus the debug
options:

```sh
git -C linux worktree add --detach ../linux-debug
cd linux-debug
cp ../linux/.config .config
./scripts/config --set-str LOCALVERSION -virtme-debug \
    -e KASAN -e KASAN_GENERIC -e KASAN_INLINE \
    -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP -e DEBUG_LIST \
    -e DEBUG_FS -e DEBUG_KMEMLEAK -d DEBUG_KMEMLEAK_DEFAULT_OFF
make olddefconfig
make -j"$(nproc)"
```

The module has to be rebuilt for whichever kernel you boot:

| Step | Where | Command |
| --- | --- | --- |
| Build module | host | `make clean && make KDIR=$PWD/linux-debug` |
| Boot guest | host, repo root | `vng -r ./linux-debug --user root --memory 2G` |
| Run tests | guest | `PYRITE_DEBUG=1 ./tests/run.sh` |
| Back to the plain kernel | host | `make clean && make` |

## Tests

`tests/run.sh` loads four devices (8192, 4096, 2048 and 1024 MiB), runs the
harness and `tests/qualify.sh`, and unloads. It only runs as root inside a
virtme-ng guest, with `pyrite.ko`, `tests/harness` and `tools/qualify` already
built. The run fails when:

- a harness or `qualify` test fails
- `rmmod` fails (a leaked file blocks unload)
- a new `dmesg` line matches `WARN|BUG|KASAN|leak|general protection`
  (saved to `/tmp/pyrite-dmesg.txt`)
- lockdep is off before or after the test. lockdep turns itself off after its
  first report.
- kmemleak reports a new leak (saved to `/tmp/pyrite-kmemleak.txt`). kmemleak
  is scanned and cleared before `insmod`, then scanned again 6 s after `rmmod`.

On a kernel without lockdep or kmemleak, those two checks are skipped.
`PYRITE_DEBUG=1` makes a missing KASAN, lockdep or kmemleak fail the run, so a
run on the wrong kernel cannot pass.

| Test | Device | Checks |
| --- | --- | --- |
| `t_rounding` | 0 | ALLOC(1) returns 2 MiB, and `used_mb` reads 2 |
| `t_einval` | 0 | a zero-byte ALLOC, a FREE of an unknown ID, and a FREE from a second descriptor in the same process all return `EINVAL` |
| `t_enomem` | 1 | ALLOC(cap+1) is refused, ALLOC(cap) succeeds, then ALLOC(1) is refused; `used_mb` is 0 after close |
| `t_procs_shape` | 2 | two forked children hold 4 MiB each; the probe and a one-entry buffer both return `ERANGE` with count 2; the full read returns each child's tgid with 4 MiB |
| `t_kill_cleanup` | 0, 3 | a forked child allocates 4 MiB on each device and is killed with `SIGKILL`; both `used_mb` must reach 0 within about 2 s |
| `t_hammer` | 0 | two threads, each with its own descriptor, run 10k random 4 MiB ALLOC/FREE calls; the device ends with nothing used or allocated |
| `t_uuid` | 0–3 | every `uuid` is well-formed, the four differ, and a second read matches |
| `t_xid` | 1 | after `xid 48`, `health` and `last_xid` read back, ALLOC returns `EIO`, QUERY_DEVICE and FREE work, and `xid`, `xid 0` and `bogus` return `EINVAL`; after `reset`, ALLOC works |
| `t_lost` | 2 | after `lost`, all four ioctls return `ENODEV`; closing the descriptor still brings `used_mb` to 0 |
| `t_scratch_rw` | 3 | data reads back across a page boundary and at the last byte; EOF at the end and `ENOSPC` past it; a `lost` device fails reads and writes with `ENODEV` |
| `t_sdc` | 3 | a stuck-at-1 bit turns `0x00` into `0x08` with no counter or health change while `sdc_count` reads 1; under `0xFF` it shows nothing; `clear` removes it |
| `t_ce` | 3 | a stuck corrected bit reads back right, and `ecc_ce_count` goes up once per read |
| `t_ue` | 3 | a matching bit reads fine; a mismatched one fails with `EIO`, raises `ecc_ue_count` and sets health `xid`/48; the next page still reads |
| `t_rate` | 3 | at 1,000,000 ppm a page of zeros reads back with exactly one bit set; at 0 it reads back clean |
| `t_fault_cmds` | 3 | malformed `stuck` and `rate` commands return `EINVAL`, and a 65th stuck cell returns `ENOSPC` |
| `t_reset` | 1 | sysfs `reset` returns `EBUSY` while an allocation exists and works once it is freed, even with the descriptor still open; values other than `1` return `EINVAL`; a stuck memory bit survives it |

The harness needs a fresh load with at least four devices, because it opens
`/dev/pyrite0` to `/dev/pyrite3` directly and checks exact `used_mb` values.
The fault tests also need debugfs, which `run.sh` mounts. Each one resets or
clears its device on every exit path.

`tests/qualify.sh` runs `tools/qualify` on device 3. A clean device passes. A
stuck silent bit fails whichever way it is stuck, and the bit errors `qualify`
reports match `sdc_count`. A corrected bit passes. An uncorrected bit, a `lost`
device and a missing device are device errors, and bad usage exits 3.

`t_kill_cleanup` also prints the reclaim latency. In recorded runs it stayed
under 0.2 ms on the plain kernel and under 1.2 ms on the debug kernel. The
timer (`CLOCK_MONOTONIC`) starts just before `kill()` and stops at the first
sysfs read where both devices show 0, polling every 1 ms after `waitpid()`. So
the number is an upper bound on when reclaim became visible. It does not time
reclaim itself.

To check the gate itself, one bug at a time was planted in a copy of the module
and run on the debug kernel. Every run failed:

| Planted bug | Caught by |
| --- | --- |
| FREE reads an allocation after `kfree` | KASAN use-after-free |
| QUERY_PROCS buffer is one entry short | KASAN out-of-bounds |
| release uncharges its allocations but skips `kfree` | kmemleak only; every harness test passed |
| QUERY_DEVICE takes the mutex with preemption off | `DEBUG_ATOMIC_SLEEP` |
| ALLOC takes the next device's lock inside its own | lockdep |

The memory-error and reset tests were checked the same way on the plain
kernel. Each of nine planted bugs in the module, such as an SDC that never
flips its bit or a reset that ignores allocations, failed the test written for
it. So did two planted bugs in `qualify`: a missing comparison and ignored read
errors.

## Kubernetes

`deviceplugin/` is a Kubernetes device plugin, written in Go, that advertises
each pyrite device as `pyrite.dev/gpu`. A pod that asks for one gets a single
`/dev/pyriteN`, at the same path as on the host:

```yaml
resources:
  limits:
    pyrite.dev/gpu: 1
```

The plugin uses each device's `uuid` as its device ID, the way NVIDIA's plugin
uses GPU UUIDs. kubelet records the IDs it hands out, and the seeded UUIDs keep
them valid after a module reload. kubelet deletes plugin sockets when it
restarts, so the plugin checks for its socket every second and registers again
once it is gone. The plugin never opens a device, so its DaemonSet is not
privileged and mounts only kubelet's device plugin directory.

The plugin reads each device's `health` file once a second (`-interval`) and
tells kubelet when it changes. Only `ok` counts as Healthy; `xid`, `lost` and a
failed read, as while the module is unloaded, are Unhealthy. So is a device
listed in `/var/lib/pyrite/quarantine`, the file the
[qualifier](#qualification) writes. kubelet then stops counting the device as
allocatable, so no new pod gets it. A pod already on it keeps running, and
kubelet marks the device Unhealthy in the pod's
`status.containerStatuses[].allocatedResourcesStatus`. That pod still holds its
request, and the scheduler counts it against the lower allocatable, so a node
with a failed GPU under a running pod has two GPUs to give out, not three,
until the pod goes away. kubelet posts node status every 10 s, so the scheduler
can see a fault up to about 11 s after it happens, while the plugin sees it
within a second. Reloading the module with the same `uuid_seed` keeps the
device IDs; a different seed needs a plugin restart.

`vm/vm.sh` runs a single-node k3s cluster in a QEMU VM with Fedora 44 Cloud.
kind would use the host kernel, and virtme-ng shares the host's root
filesystem, so neither can host the cluster. `provision` builds pyrite in the
VM against Fedora's kernel, runs the harness there, loads the module at boot
with `uuid_seed=1`, and installs k3s. SELinux is permissive in the VM, because
k3s on Fedora otherwise needs Rancher's policy package. The host needs QEMU
with KVM, xorriso, gpg, podman and Go.

| Step | Command |
| --- | --- |
| Boot the VM (downloads the image the first time) | `vm/vm.sh up` |
| Build and test pyrite in the VM, load it, install k3s | `vm/vm.sh provision` |
| Plugin unit tests | `make -C deviceplugin test` |
| End-to-end test | `deviceplugin/e2e/run.sh` |
| Shell in the VM | `vm/vm.sh ssh` |
| Stop the VM, or delete its disk | `vm/vm.sh down`, `vm/vm.sh destroy` |

All commands run on the host. VM files go in `./.vm` and Go caches in
`./.cache/go`; git ignores both.

`e2e/run.sh` builds the plugin image and an image with `tests/hold`, copies
both into k3s's containerd (there is no registry), deploys the DaemonSet and
checks that:

- allocatable `pyrite.dev/gpu` reaches 4
- a 1-GPU pod sees exactly one `/dev/pyrite*`, and one device shows 2 MiB used
- a 5-GPU pod stays Pending
- deleting the 1-GPU pod brings every device back to 0 MiB
- after `systemctl restart k3s`, the plugin registers again without being
  restarted, allocatable returns to 4, and a new pod gets a device
- after `xid 48` on the 1-GPU pod's device, allocatable drops to 3, the pod
  keeps running and its status shows the device Unhealthy; a 2-GPU pod runs on
  healthy devices, and a 3-GPU pod stays Pending because the pod on the failed
  device still counts
- `reset` brings allocatable back to 4, and `lost` on another device drops it
  to 3 until the next `reset`
- a UUID written to the quarantine file drops allocatable to 3 until it is
  removed
- unloading the module drops allocatable to 0, and loading it again brings all
  four back without restarting the plugin

The test also prints how long the plugin and the node took to notice the Xid.

## Qualification

Silent errors are the faults `health` can't show, so something has to test the
memory. `tools/qualify` writes four patterns over a device's scratch memory and
reads each back: all zeros, all ones, seeded random data and its inverse. A
stuck bit hides under one value, so the two solid patterns catch it whichever
way it is stuck. It exits 0 when every device passes, 1 when data came back
wrong, 2 on a device error and 3 on bad usage.

`qualifier/` is an agent that runs on every node as a privileged DaemonSet,
next to the plugin. It uses only what a real driver offers: sysfs health, the
sysfs `reset` and the device files.

- Every `-interval` it runs `qualify` on each device in service. `-k` failed
  runs within `-window` quarantine the device, so one transient error doesn't.
  A device error, or health other than `ok`, quarantines it at once.
- Quarantine means writing the device's UUID to `/var/lib/pyrite/quarantine`
  through a temporary file and a rename. The plugin reads the file and reports
  the device Unhealthy within a second.
- It then evicts the pods on the device, resets the device through sysfs once
  they are gone, and runs `qualify` again. A pass puts the device back. After
  `-max-requalify` failures the agent gives up and logs that the device needs
  replacing.
- With `-scope node`, one bad device takes every device on the node out of
  service, as a `NoSchedule` taint would for GPU pods.
- Every decision is a JSON log line with a timestamp, device, event and reason.

The agent keeps everything on the node, so it needs no controller, no Node
patches and no client-go. kubelet's PodResources socket names the pods on a
device, and one REST call with the pod's service-account token evicts each one.
Qualification runs next to working pods because scratch memory is separate
from the ALLOC ledger; a real GPU would have to be idle for a full diagnostic.

| Step | Command |
| --- | --- |
| Agent unit tests | `make -C qualifier test` |
| End-to-end test, after the plugin's | `qualifier/e2e/run.sh` |

`qualifier/e2e/run.sh` deploys the agent with short intervals and checks that:

- every device passes its first qualification
- a stuck silent bit under a running pod gets the device quarantined and the pod
  evicted, and requalification fails until the bit is cleared
- `sdc_count` shows that pyrite really handed out the bits the agent caught
- one failed run does not quarantine a device when `-k` is 2
- an `xid` quarantines a device at once, and the sysfs reset brings it back
- a stuck uncorrected bit keeps a device out until it is cleared
- with `-scope node`, one bad device drops allocatable to 0 until it is cleared

The test removes the agent when it ends, because the plugin's test injects
faults that the agent would act on.

## Experiment

`experiment/run.sh` measures the qualifier on the k3s VM. Each scenario starts
the agent on clean devices, gives `pyrite3` a stuck silent bit at a random
spot, and gives the three good devices a background rate of transient silent
errors. It runs for 2 minutes, with qualification every 5 s and
requalification every 3 s, and keeps the agent's event log.
`experiment/analyze.py` turns the logs into `experiment/results.csv`. One
`qualify` run reads 1,024 pages, so at a per-page error rate r a good device
fails a run with probability 1 − (1 − r)^1024.

| Scope | k | Error rate | Detect | Good runs failed (expected) | False quarantines | Good devices given up | Capacity lost | Wasted |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| device | 1 | 0 | 2.8 s | 0% (0%) | 0 | 0 | 24% | 0% |
| device | 1 | 200 ppm | 2.8 s | 26% (19%) | 16 | 0 | 27% | 2% |
| device | 1 | 1000 ppm | 2.8 s | 65% (64%) | 15 | 2 | 74% | 49% |
| device | 2 | 0 | 7.8 s | 0% (0%) | 0 | 0 | 23% | 0% |
| device | 2 | 200 ppm | 8.8 s | 25% (19%) | 7 | 0 | 23% | 0% |
| device | 2 | 1000 ppm | 8.8 s | 74% (64%) | 18 | 1 | 43% | 20% |
| node | 2 | 200 ppm | 7.8 s | 24% (19%) | 7 | 0 | 94% | 70% |

Capacity lost is the GPU time the quarantine file blocked, out of 4 GPUs for 2
minutes; the bad device alone accounts for about 23%. Wasted is the part spent
on good devices.

The stuck bit was found in every run: 2.8 s after it went in at k=1, 8 to 9 s
at k=2. Each extra failure that k asks for costs one more interval, so
detection takes about U(0, interval) + (k − 1) × interval.

At 200 ppm a good device fails about one run in four. k=2 halved the false
quarantines, and because the agent retests at once and most retests pass, those
devices were back before the plugin saw them: no capacity wasted at k=2, 2% at
k=1. At 1000 ppm two runs in three fail and the noise wins. Retests fail too,
good devices run out of retries and are given up on, and k=1 wasted half the
node.

Node scope turns one bad device into a whole-node outage: 94% of the capacity
lost, against 23% at device scope.

The measured failure rate matches 1 − (1 − r)^1024 at 1000 ppm and runs above
it at 200 ppm, within the noise of about 60 runs per scenario. These are single
2-minute runs on one node, with a 5 s interval picked for speed, so they show
the trade-offs rather than tight estimates. The error streams are seeded, so
scenarios at the same rate see the same errors, which makes k=1 and k=2 a
paired comparison. Capacity is counted at the agent; the scheduler sees each
change up to about 10 s later, when kubelet next posts node status.

## Planned

Still to come are the ioctl latency benchmark, an ABI reference, and two
features from [Roster](https://github.com/chenbenny31/roster)'s design:
exclusive-process mode (a second process's ALLOC gets `EBUSY`) and delayed
reclaim (freed memory stays counted for a set time after release). Integration
with Roster comes last. MIG/MPS, memory mapping, thermals and topology are not
planned.

## Related work

| Project | What it does | How pyrite differs |
| --- | --- | --- |
| `vgem`, `vkms` | in-tree DRM drivers with no hardware: GEM buffers and a virtual display | not a DRM driver; tracks scheduler-level accounting, not buffer objects |
| `dmem` cgroup controller | limits and accounts device memory for DRM drivers through cgroups | enforces nothing outside its own devices; it only keeps a ledger |
| CUDA interception (e.g. HAMi-core) | limits VRAM by hooking the CUDA driver API with `LD_PRELOAD`, inside each client | accounting and reclaim live in the kernel, and CUDA is not involved |
| Kubernetes fake-GPU device plugins | advertise synthetic GPUs to the scheduler | keeps allocation state that survives a client's death; these plugins usually fake inventory only |
| KWOK | simulates nodes and pods for the control plane; no kubelet runs them | pods are real processes holding real device files |
| NVML mocks | replace NVML calls inside one test process | one ledger in the kernel, shared by every process on the node |

## Limits

pyrite only does GPU-shaped accounting. It has none of a real GPU driver's
DRM/TTM, GEM objects, DMA, interrupts or submission ring, and it maps no
memory. Scratch memory is a small kernel buffer for memory tests, not VRAM.
`util` is a synthetic number with no fairness or isolation behind it.
The UAPI can change at any time. Load it only in a disposable VM.

## Repository layout

| Path | Content |
| --- | --- |
| `pyrite.c` | the module |
| `uapi/pyrite.h` | ioctl structs and commands, shared with userspace |
| `tests/harness.c` | 16-test harness |
| `tests/run.sh` | in-guest gate: load, test, unload, check `dmesg`, lockdep and kmemleak |
| `tests/hold.c` | reclaim demo from the top of this README |
| `tests/qualify.sh` | tests of `qualify` against injected memory errors |
| `tools/qualify.c` | known-answer test of scratch memory |
| `deviceplugin/` | Kubernetes device plugin (Go), its image, DaemonSet and end-to-end test |
| `qualifier/` | node agent (Go) that qualifies, quarantines and requalifies devices, with its image, DaemonSet and end-to-end test |
| `experiment/` | qualification experiment: `run.sh`, `analyze.py` and the results |
| `vm/vm.sh` | single-node k3s VM for the plugin and the agent |
| `Kbuild`, `Makefile` | out-of-tree build and `checkpatch` |

## License

GPL-2.0-only. See [COPYING](COPYING).
