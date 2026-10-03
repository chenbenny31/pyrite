# pyrite

**A Linux kernel module that exposes N mock GPUs as character devices, with
per-tgid VRAM accounting and cleanup tied to `release()`.**

pyrite gives a GPU scheduler something real to schedule against when the
machine has no GPUs. Each `/dev/pyriteN` has a fixed VRAM capacity, admits or
refuses allocations in 2 MiB units, and reports per-tgid usage through an
NVML-shaped `ioctl` interface and sysfs. When the last reference to a file
closes, the kernel frees everything that file allocated. That includes a
client killed with `SIGKILL` while it held memory.

**Status:** the repository contains the module, its UAPI header, a
six-scenario test harness, and `tests/run.sh`. That script loads the module in
a virtme-ng guest, runs the harness, unloads it, and fails on any new kernel
complaint in `dmesg`. Not in the repository yet: an ioctl latency benchmark, a
standalone ABI reference, a run on a KASAN + lockdep kernel, and integration
with [roster](https://github.com/chenbenny31/roster).

## Why a kernel module

The point of the project is to make reclaim depend on the kernel instead of
on the client behaving well.

However a process dies, the kernel runs `do_exit` → `exit_files` → `fput` on
each of its file descriptors. When a `struct file`'s refcount reaches zero, the
driver's `->release` runs. pyrite frees allocations in `->release`, so cleanup
happens as part of destroying the file. No separate mechanism has to notice
the death first. The ledger is kernel state, so it outlives every client.

A userspace daemon can detect a client's death too, for example through socket
EOF or a pidfd. Its cleanup is then a second code path that runs after the
death and can fail on its own. pyrite exists to show the other design: the
destructor itself does the cleanup.

`->release` is used rather than `->flush` on purpose. `->flush` runs on every
`close()`, so it would free allocations that a `dup`'d descriptor, a forked
child, or an `SCM_RIGHTS` recipient still holds. `->release` runs once, after
the last of those references is gone.

## Relationship to CUDA and NVML

pyrite models the bookkeeping a GPU scheduler cares about, which is capacity,
usage per tgid, and utilization, **without implementing any CUDA API**.
Allocations are entries in a ledger: no memory is reserved and no hardware is
touched. A scheduler or test harness talks to pyrite directly through its
UAPI.

The query shapes copy NVML on purpose. `QUERY_DEVICE` corresponds to
`nvmlDeviceGetMemoryInfo` plus a utilization figure, and `QUERY_PROCS`
corresponds to `nvmlDeviceGetComputeRunningProcesses`. This means a backend
written against pyrite has the same structure as one written against NVML.
pyrite is NVML-*shaped* and never NVML-*detectable*: `nvidia-smi` and NVML
will not see these devices, and they are not meant to.

## Devices and parameters

```sh
insmod pyrite.ko ndev=4 capacity_mb=8192,4096,2048,1024
```

| Parameter | Meaning |
| --- | --- |
| `ndev` | number of devices, 1..16, default 4 |
| `capacity_mb` | per-device capacity in MiB; `0` or unset means 8192 |

Each device appears as `/dev/pyriteN` with these read-only attributes under
`/sys/class/pyrite/pyriteN/`:

| Attribute | Content |
| --- | --- |
| `capacity_mb` | fixed at load time |
| `used_mb` | sum of live allocations |
| `util` | synthetic 0..100, decayed on read |
| `numa_node` | constant `0` |
| `pcie_addr` | `0000:NN:00.0`, derived from the device index |

`numa_node` and `pcie_addr` are fake topology. They exist so discovery code
has the fields it expects.

## ioctl interface

`uapi/pyrite.h` is the reference. All of its structs contain only `__u64`
fields and have no padding. Because of that, `compat_ptr_ioctl` is enough for
32-bit callers.

| Command | Struct (bytes) | Cmd word | Errors |
| --- | --- | --- | --- |
| `PYRITE_IOC_ALLOC` | `pyrite_alloc_req` (16) | `0xC010BE01` | `EINVAL`, `ENOMEM`, `EFAULT` |
| `PYRITE_IOC_FREE` | `pyrite_free_req` (8) | `0x4008BE02` | `EINVAL`, `EFAULT` |
| `PYRITE_IOC_QUERY_DEVICE` | `pyrite_device_info` (32) | `0x8020BE03` | `EFAULT` |
| `PYRITE_IOC_QUERY_PROCS` | `pyrite_procs_query` (16) | `0xC010BE04` | `ERANGE`, `ENOMEM`, `EFAULT` |

Any other command returns `ENOTTY`. The magic number is `0xBE`. An earlier
revision used `0xB7`, which `nsfs.h` and `remoteproc_cdev.h` already claim
in-tree.

Semantics:

- **Admission.** ALLOC rejects a size of 0. Requests are rounded up to a
  multiple of 2 MiB, and the rounded size is written back. A request that
  exceeds remaining capacity, before or after rounding, fails with `ENOMEM`.
  Allocation IDs start at 1, so 0 is never a valid ID.
- **Ownership is per open file.** A second descriptor from the same process
  cannot FREE the first descriptor's allocation. An unknown ID and an ID owned
  by another file both return the same `EINVAL`, so FREE cannot be used to test
  whether an ID exists.
- **Accounting is per tgid.** The tgid is recorded from `current` at ALLOC
  time. If the descriptor is later passed with `SCM_RIGHTS`, the usage stays
  charged to the process that allocated.
- **Reclaim.** All of a file's allocations are freed when its last reference
  drops, including on `SIGKILL`. If ALLOC returns `EFAULT` while writing its
  result back, the allocation has already been committed. It stays bound to
  the file and is freed when the file is released.
- **QUERY_PROCS sizing.** Pass `buf_ptr == 0`, or a `count` that is too
  small, and the call returns `ERANGE` with `count` set to the number of
  entries needed. A device with no allocations returns 0 with `count = 0`.
- **No stability promise.** The struct size is encoded in the cmd word, so a
  layout change makes old callers get `ENOTTY` instead of silently corrupting
  data.

## Design notes

- One `cdev` covers `ndev` minors. Each device has one ledger, protected by
  one mutex.
- `capacity` and `index` are written before `cdev_add` and never change
  afterward, so they are read without the lock.
- `copy_from_user` and `copy_to_user` never run while the mutex is held. Data
  is copied into a local snapshot under the lock and copied out after
  unlocking.
- ALLOC allocates its list node before taking the lock, because the size is
  known up front. QUERY_PROCS allocates its scratch buffer under the lock,
  because the size depends on locked state. The 2 MiB granularity bounds that
  buffer: at most `capacity / 2 MiB` entries, which is 64 KiB at 8 GiB.
- `bytes > capacity` is checked before rounding. Otherwise
  `ALIGN(U64_MAX, SZ_2M)` would wrap to 0.
- Utilization is synthetic. The stored value means "utilization as of the
  last ALLOC or FREE". A read halves it once for every full second since then,
  and returns 0 after 32 seconds so the shift stays within a `u32`. Each ALLOC
  or FREE sets it to the decayed value plus 25, capped at 100. Release does not
  change it.

## Build and run

You need a kernel tree to build against (default `~/Code/linux`; override with
`KDIR=`) and [virtme-ng](https://github.com/arighi/virtme-ng) to boot it.
pyrite was developed against v6.19.14.

| Step | Where | Command |
| --- | --- | --- |
| Build module | host | `make && make check` |
| Build harness | host | `gcc -Wall -Wextra -O2 -o tests/harness tests/harness.c -lpthread` |
| Boot guest | host, from the repo root | `vng -r ~/Code/linux --user root` |
| Run tests | guest | `./tests/run.sh` |

`make check` runs `checkpatch.pl` on the module and the UAPI header. Without
`--user root` the guest shell is unprivileged and `insmod` fails with `EPERM`.

To watch reclaim by hand, build `tests/hold.c`:

```sh
gcc -Wall -Wextra -O2 -o tests/hold tests/hold.c
./tests/hold /dev/pyrite0 1000000 &      # prints alloc_id, rounded size, pid
cat /sys/class/pyrite/pyrite0/used_mb    # 2
kill -9 %1
cat /sys/class/pyrite/pyrite0/used_mb    # 0
```

## Tests

`tests/run.sh` checks that it is running as root inside a virtme-ng guest, and
that `pyrite.ko` and `tests/harness` exist. It then records the current `dmesg`
position, loads four devices of 8192, 4096, 2048 and 1024 MiB, runs the
harness, and unloads. The run fails if the harness fails, if `rmmod` fails (a
leaked file blocks unload), or if any new `dmesg` line matches
`WARN|BUG|KASAN|leak|general protection`. The scanned lines are saved to
`/tmp/pyrite-dmesg.txt`.

| Test | Device | Checks |
| --- | --- | --- |
| `t_rounding` | 0 | ALLOC(1) returns 2 MiB; `used_mb` reads 2 |
| `t_einval` | 0 | zero-byte ALLOC, unknown-ID FREE, and a FREE from a second descriptor in the same process all return `EINVAL` |
| `t_enomem` | 1 | ALLOC(cap+1) is refused, ALLOC(cap) is admitted, a further ALLOC(1) is refused; `used_mb` returns to 0 on close |
| `t_procs_shape` | 2 | two forked children hold 4 MiB each; the probe and a one-entry buffer both return `ERANGE` with count 2; the full read returns two entries, each a child's tgid with 4 MiB |
| `t_kill_cleanup` | 0, 3 | a child opens both devices after fork and allocates 4 MiB on each, then receives `SIGKILL`; both `used_mb` must reach 0 within about 2 s |
| `t_hammer` | 0 | 2 threads, each with its own descriptor, run 10k random 4 MiB ALLOC/FREE operations; the device ends with zero bytes used and zero allocations |

The harness expects a fresh load with at least four devices. It opens
`/dev/pyrite0`–`3` directly and checks absolute `used_mb` values.

`t_kill_cleanup` prints a reclaim latency. The timer (`CLOCK_MONOTONIC`) starts
just before `kill()` and stops at the first read where both devices show 0,
polling sysfs every 1 ms after `waitpid()`. In recorded runs it stayed under
0.2 ms. It is an upper bound on when the reclaim became visible, not a
measurement of how long reclaim itself takes.

## Related work

- **`vgem` and `vkms`.** In-tree DRM drivers with no hardware behind them:
  `vgem` provides GEM buffers and `vkms` is a virtual display. They let DRM
  code run without a GPU. pyrite is not a DRM driver and models scheduler-level
  accounting, not buffer objects.
- **The `dmem` cgroup controller.** Limits and accounts device memory for DRM
  drivers through cgroups. It is the in-kernel route to real VRAM policy.
  pyrite only keeps a ledger and enforces nothing outside its own devices.
- **CUDA interception, such as HAMi-core.** These libraries limit VRAM by
  intercepting the CUDA driver API inside each process using `LD_PRELOAD`.
  That puts enforcement inside the client. pyrite moves accounting and
  reclaim into the kernel, at the cost of not touching CUDA at all.
- **Fake-GPU device plugins for Kubernetes.** These advertise synthetic GPUs to
  the scheduler. They usually fake inventory without keeping any allocation
  state that survives a client's death.

## Non-goals

- **Being a GPU driver.** A real one uses DRM/TTM with GEM objects, DMA,
  interrupts and a submission ring. pyrite is a character device with
  GPU-shaped accounting.
- **Backing memory.** Nothing is mapped, so nothing can be read or written.
- **Showing up in NVML or `nvidia-smi`.**
- **Implementing or intercepting CUDA.**
- **Realistic utilization, compute fairness, or isolation.** `util` is a
  synthetic signal.
- **A stable UAPI.**
- **Running on a host you care about.** Load it in a disposable VM.

## Repository layout

| Path | Content |
| --- | --- |
| `pyrite.c` | the module |
| `uapi/pyrite.h` | ioctl structs and commands, shared with userspace |
| `tests/harness.c` | six-scenario test harness |
| `tests/run.sh` | in-guest gate: load, test, unload, scan `dmesg` |
| `tests/hold.c` | manual reclaim demo |
| `Kbuild`, `Makefile` | out-of-tree build and `checkpatch` |

## License

GPL-2.0-only. See [COPYING](COPYING).
