# pyrite

pyrite is a Linux kernel module that creates mock GPUs as character devices.
Each `/dev/pyriteN` has a fixed VRAM capacity, accepts or refuses allocations
in 2 MiB units, and reports per-process (tgid) usage through `ioctl` and sysfs.
When a client exits, even by `SIGKILL`, the kernel frees everything it
allocated. It is meant for testing a GPU scheduler on a machine with no GPUs.

**Status:** the module, its UAPI header, a six-test harness and `tests/run.sh`
are done. Still to do: an ioctl latency benchmark, a standalone ABI reference,
a test run on a KASAN + lockdep kernel, and integration with
[roster](https://github.com/chenbenny31/roster).

## Why a kernel module

Cleanup runs in the kernel, so it does not depend on the client behaving.
However a process dies, the kernel closes its file descriptors (`do_exit` →
`exit_files` → `fput`). When the last reference to a file goes away, the
driver's `->release` runs, and pyrite frees that file's allocations there.
Nothing has to notice the death first.

A userspace daemon can also detect a dead client, through socket EOF or a
pidfd. Its cleanup is then a separate step that runs later and can fail on its
own. pyrite puts the cleanup in the file's destructor instead.

pyrite frees in `->release` rather than `->flush`. `->flush` runs on every
`close()`, so it would free memory that a `dup`'d descriptor, a forked child or
an `SCM_RIGHTS` receiver still holds. `->release` runs once, after the last
reference is gone.

## NVML and CUDA

pyrite tracks what a scheduler needs: capacity, per-process usage and
utilization. An allocation is only an entry in a ledger, so no memory is
reserved and no hardware is touched. pyrite does not implement CUDA, and NVML
and `nvidia-smi` cannot see its devices.

The queries follow NVML's shape, so a backend written for pyrite has the same
structure as an NVML one. `QUERY_DEVICE` matches `nvmlDeviceGetMemoryInfo`
plus a utilization figure, and `QUERY_PROCS` matches
`nvmlDeviceGetComputeRunningProcesses`.

## Devices and parameters

```sh
insmod pyrite.ko ndev=4 capacity_mb=8192,4096,2048,1024
```

| Parameter | Meaning |
| --- | --- |
| `ndev` | number of devices, 1..16, default 4 |
| `capacity_mb` | capacity per device in MiB; `0` or unset means 8192 |

Each device gets these read-only attributes under `/sys/class/pyrite/pyriteN/`:

| Attribute | Content |
| --- | --- |
| `capacity_mb` | set at load time |
| `used_mb` | sum of live allocations |
| `util` | synthetic, 0..100, decays over time |
| `numa_node` | always `0` |
| `pcie_addr` | `0000:NN:00.0`, from the device index |

`numa_node` and `pcie_addr` are fake. They are there because discovery code
expects them.

## ioctl interface

`uapi/pyrite.h` defines the ABI. Every struct holds only `__u64` fields with no
padding, so `compat_ptr_ioctl` covers 32-bit callers.

| Command | Struct (bytes) | Cmd word | Errors |
| --- | --- | --- | --- |
| `PYRITE_IOC_ALLOC` | `pyrite_alloc_req` (16) | `0xC010BE01` | `EINVAL`, `ENOMEM`, `EFAULT` |
| `PYRITE_IOC_FREE` | `pyrite_free_req` (8) | `0x4008BE02` | `EINVAL`, `EFAULT` |
| `PYRITE_IOC_QUERY_DEVICE` | `pyrite_device_info` (32) | `0x8020BE03` | `EFAULT` |
| `PYRITE_IOC_QUERY_PROCS` | `pyrite_procs_query` (16) | `0xC010BE04` | `ERANGE`, `ENOMEM`, `EFAULT` |

Other commands return `ENOTTY`. The magic number is `0xBE`. The first version
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

## Design notes

- One `cdev` covers all `ndev` minors. Each device has its own ledger and one
  mutex.
- `capacity` and `index` are set before `cdev_add` and never change, so they
  are read without the lock.
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

## Build and run

You need a kernel tree to build against and
[virtme-ng](https://github.com/arighi/virtme-ng) to boot it. The tree defaults
to `./linux` in this repo, which git ignores. Override it with `KDIR=`. pyrite
was developed against v6.19.14.

| Step | Where | Command |
| --- | --- | --- |
| Build module | host | `make && make check` |
| Build harness | host | `gcc -Wall -Wextra -O2 -o tests/harness tests/harness.c -lpthread` |
| Boot guest | host, repo root | `vng -r ./linux --user root` |
| Run tests | guest | `./tests/run.sh` |

`make check` runs `checkpatch.pl` on the module and the UAPI header. Boot with
`--user root`, or `insmod` fails with `EPERM`.

To watch reclaim by hand, use `tests/hold.c`:

```sh
gcc -Wall -Wextra -O2 -o tests/hold tests/hold.c
./tests/hold /dev/pyrite0 1000000 &      # prints alloc_id, rounded size, pid
cat /sys/class/pyrite/pyrite0/used_mb    # 2
kill -9 %1
cat /sys/class/pyrite/pyrite0/used_mb    # 0
```

## Tests

`tests/run.sh` loads four devices (8192, 4096, 2048 and 1024 MiB), runs the
harness and unloads. It only runs as root inside a virtme-ng guest, with
`pyrite.ko` and `tests/harness` already built. It fails if the harness fails,
if `rmmod` fails (a leaked file blocks unload), or if a new `dmesg` line
matches `WARN|BUG|KASAN|leak|general protection`. Those `dmesg` lines are
saved to `/tmp/pyrite-dmesg.txt`.

| Test | Device | Checks |
| --- | --- | --- |
| `t_rounding` | 0 | ALLOC(1) returns 2 MiB, and `used_mb` reads 2 |
| `t_einval` | 0 | a zero-byte ALLOC, a FREE of an unknown ID, and a FREE from a second descriptor in the same process all return `EINVAL` |
| `t_enomem` | 1 | ALLOC(cap+1) is refused, ALLOC(cap) succeeds, then ALLOC(1) is refused; `used_mb` is 0 after close |
| `t_procs_shape` | 2 | two forked children hold 4 MiB each; the probe and a one-entry buffer both return `ERANGE` with count 2; the full read returns each child's tgid with 4 MiB |
| `t_kill_cleanup` | 0, 3 | a forked child allocates 4 MiB on each device and is killed with `SIGKILL`; both `used_mb` must reach 0 within about 2 s |
| `t_hammer` | 0 | two threads, each with its own descriptor, run 10k random 4 MiB ALLOC/FREE calls; the device ends with nothing used or allocated |

The harness needs a fresh load with at least four devices, because it opens
`/dev/pyrite0` to `/dev/pyrite3` directly and checks exact `used_mb` values.

`t_kill_cleanup` also prints the reclaim latency, which stayed under 0.2 ms in
recorded runs. The timer (`CLOCK_MONOTONIC`) starts just before `kill()` and
stops at the first sysfs read where both devices show 0, polling every 1 ms
after `waitpid()`. So the number is an upper bound on when reclaim became
visible, not how long reclaim itself took.

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
| `tests/harness.c` | six-test harness |
| `tests/run.sh` | in-guest gate: load, test, unload, scan `dmesg` |
| `tests/hold.c` | manual reclaim demo |
| `Kbuild`, `Makefile` | out-of-tree build and `checkpatch` |

## License

GPL-2.0-only. See [COPYING](COPYING).
