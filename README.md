# pyrite

pyrite lets you test a GPU scheduler on a machine with no GPUs. Each
`/dev/pyriteN` is a mock GPU with a fixed VRAM capacity. It accepts or refuses
allocations in 2 MiB units and reports usage per process (tgid) through
`ioctl` and sysfs. When a client dies, even by `SIGKILL`, the kernel frees
everything it allocated. A test can also make a device fail on cue, with an Xid
error or by falling off the bus, and check that the scheduler stops using it.

In a guest, with the module and `tests/hold` built:

```sh
insmod pyrite.ko ndev=4 capacity_mb=8192,4096,2048,1024
./tests/hold /dev/pyrite0 1000000 &      # prints alloc_id, rounded size, pid
cat /sys/class/pyrite/pyrite0/used_mb    # 2
kill -9 %1
cat /sys/class/pyrite/pyrite0/used_mb    # 0
```

**Status:** the module, its UAPI header, a nine-test harness and `tests/run.sh`
are done, including device health and fault injection. The tests pass on the
plain kernel and on a debug kernel with KASAN, lockdep and kmemleak.
[Planned](#planned) lists what comes next.

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

`insmod` takes three parameters:

| Parameter | Meaning |
| --- | --- |
| `ndev` | number of devices, 1..16, default 4 |
| `capacity_mb` | capacity per device in MiB; `0` or unset means 8192 |
| `uuid_seed` | seed for device UUIDs, default 0 |

Each device gets these read-only attributes under `/sys/class/pyrite/pyriteN/`:

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

The `inject` file is in debugfs because it is a test knob and stays out of the
ABI. Without debugfs, pyrite works the same but can't fake faults.

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
harness and unloads. It only runs as root inside a virtme-ng guest, with
`pyrite.ko` and `tests/harness` already built. The run fails when:

- a harness test fails
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

The harness needs a fresh load with at least four devices, because it opens
`/dev/pyrite0` to `/dev/pyrite3` directly and checks exact `used_mb` values.
`t_xid` and `t_lost` also need debugfs, which `run.sh` mounts. Both close their
descriptor and reset the device on every exit path.

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

## Planned

The next steps make Kubernetes pyrite's first user. None of them is written
yet. In order:

| Step | What it adds |
| --- | --- |
| Device plugin | a Go DaemonSet in a k3s VM that advertises each device as `pyrite.dev/gpu`, keyed by UUID, and mounts `/dev/pyriteN` into the pod |
| Health in the plugin | the plugin polls `health` and reports a failed device as Unhealthy, so kubelet stops giving it to new pods |
| Silent data corruption | a small scratch buffer per device that tests write and read back, with injected corrected, uncorrected and silent bit flips, and ECC counters in sysfs |
| Qualification | `tools/qualify` writes known patterns and checks them; a controller taints a failing node, runs the check as a Job, and removes the taint when it passes. `reset` will refuse a device that still has allocations, like `nvidia-smi -r` |
| Experiment | inject faults at set rates and measure detection latency, false positives, missed faults and capacity lost to quarantine |

Silent corruption is the fault `health` can't show. The device still reads
`ok`, so only a test with a known answer finds it, and the experiment needs
qualification in place before it can count missed faults.

After that come the ioctl latency benchmark, an ABI reference, and two features
from [Roster](https://github.com/chenbenny31/roster)'s design: exclusive-process
mode (a second process's ALLOC gets `EBUSY`) and delayed reclaim (freed memory
stays counted for a set time after release). Integration with Roster comes
last. MIG/MPS, memory mapping, thermals and topology are not planned.

## Related work

| Project | What it does | How pyrite differs |
| --- | --- | --- |
| `vgem`, `vkms` | in-tree DRM drivers with no hardware: GEM buffers and a virtual display | not a DRM driver; tracks scheduler-level accounting, not buffer objects |
| `dmem` cgroup controller | limits and accounts device memory for DRM drivers through cgroups | enforces nothing outside its own devices; it only keeps a ledger |
| CUDA interception (e.g. HAMi-core) | limits VRAM by hooking the CUDA driver API with `LD_PRELOAD`, inside each client | accounting and reclaim live in the kernel, and CUDA is not involved |
| Kubernetes fake-GPU device plugins | advertise synthetic GPUs to the scheduler | keeps allocation state that survives a client's death; these plugins usually fake inventory only |

## Limits

pyrite only does GPU-shaped accounting. It has none of a real GPU driver's
DRM/TTM, GEM objects, DMA, interrupts or submission ring, and it maps no
memory. `util` is a synthetic number with no fairness or isolation behind it.
The UAPI can change at any time. Load it only in a disposable VM.

## Repository layout

| Path | Content |
| --- | --- |
| `pyrite.c` | the module |
| `uapi/pyrite.h` | ioctl structs and commands, shared with userspace |
| `tests/harness.c` | nine-test harness |
| `tests/run.sh` | in-guest gate: load, test, unload, check `dmesg`, lockdep and kmemleak |
| `tests/hold.c` | reclaim demo from the top of this README |
| `Kbuild`, `Makefile` | out-of-tree build and `checkpatch` |

## License

GPL-2.0-only. See [COPYING](COPYING).
