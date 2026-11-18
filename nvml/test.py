#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Test an NVML library through its C ABI: pyrite's stand-in, or NVIDIA's own.

    nvml/test.py [path/to/libnvidia-ml.so.1]

Against NVIDIA's library it checks the answers the stand-in copies, so the two
agree. Against the stand-in it also compares each card with sysfs, holds memory
from a child process, and, as root, injects faults through debugfs and checks
the Xid events they cause; the injected cards are reset at the end.
"""
import ctypes as C
import fcntl
import os
import signal
import struct
import sys
import time

SUCCESS, UNINITIALIZED, INVALID_ARGUMENT, NOT_SUPPORTED = 0, 1, 2, 3
INSUFFICIENT_SIZE, TIMEOUT, FUNCTION_NOT_FOUND, GPU_IS_LOST = 7, 10, 13, 15
XID_EVENT = 0x8
NO_INSTANCE = 0xFFFFFFFF
STAND_IN = "NVIDIA pyrite"
SYSFS = "/sys/class/pyrite"
DEBUGFS = "/sys/kernel/debug/pyrite"
PYRITE_IOC_ALLOC = 0xC010BE01  # _IOWR(0xBE, 0x01, struct pyrite_alloc_req)


class Memory(C.Structure):
    _fields_ = [("version", C.c_uint), ("total", C.c_ulonglong), ("reserved", C.c_ulonglong),
                ("free", C.c_ulonglong), ("used", C.c_ulonglong)]


MEMORY_V2 = C.sizeof(Memory) | 2 << 24


class Utilization(C.Structure):
    _fields_ = [("gpu", C.c_uint), ("memory", C.c_uint)]


class ProcessInfo(C.Structure):
    _fields_ = [("pid", C.c_uint), ("usedGpuMemory", C.c_ulonglong),
                ("gpuInstanceId", C.c_uint), ("computeInstanceId", C.c_uint)]


class EventData(C.Structure):
    _fields_ = [("device", C.c_void_p), ("eventType", C.c_ulonglong), ("eventData", C.c_ulonglong),
                ("gpuInstanceId", C.c_uint), ("computeInstanceId", C.c_uint)]


failures = 0


def report(name, ok, detail=""):
    global failures
    if ok:
        print(f"ok  {name}")
    else:
        print(f"FAIL {name}: {detail}")
        failures += 1


def expect(name, got, want):
    report(name, got == want, f"got {got}, want {want}")


def read_sysfs(minor, name):
    with open(f"{SYSFS}/pyrite{minor}/{name}") as f:
        return f.read().strip()


def memory(nvml, device):
    info = Memory(version=MEMORY_V2)
    return nvml.nvmlDeviceGetMemoryInfo_v2(device, C.byref(info)), info


def processes(nvml, device):
    """(code, {pid: bytes}), asked as nvml-wrapper asks: how many first, then the list"""
    count = C.c_uint(0)
    rc = nvml.nvmlDeviceGetComputeRunningProcesses_v3(device, C.byref(count), None)
    if rc != INSUFFICIENT_SIZE:
        return rc, {}
    count.value += 5
    infos = (ProcessInfo * count.value)()
    rc = nvml.nvmlDeviceGetComputeRunningProcesses_v3(device, C.byref(count), infos)
    found = infos[:count.value]
    report("processes have no MIG instance", all(p.gpuInstanceId == p.computeInstanceId == NO_INSTANCE
                                                 for p in found), "instance ids set")
    return rc, {p.pid: p.usedGpuMemory for p in found}


def wait(nvml, events, timeout_ms):
    """(code, (handle, type, data), seconds taken)"""
    data = EventData()
    start = time.monotonic()
    rc = nvml.nvmlEventSetWait_v2(events, C.byref(data), timeout_ms)
    return rc, (data.device, data.eventType, data.eventData), time.monotonic() - start


def check_card(nvml, index):
    """What any NVML card answers; its handle, name and UUID"""
    device = C.c_void_p()
    expect(f"card {index}: handle", nvml.nvmlDeviceGetHandleByIndex_v2(index, C.byref(device)), SUCCESS)
    buf = C.create_string_buffer(96)
    expect(f"card {index}: name", nvml.nvmlDeviceGetName(device, buf, len(buf)), SUCCESS)
    name = buf.value.decode()
    expect(f"card {index}: name in a short buffer",
           nvml.nvmlDeviceGetName(device, C.create_string_buffer(4), 4), INSUFFICIENT_SIZE)
    expect(f"card {index}: uuid", nvml.nvmlDeviceGetUUID(device, buf, len(buf)), SUCCESS)
    uuid = buf.value.decode()
    report(f"card {index}: uuid is GPU-<uuid>", uuid.startswith("GPU-") and len(uuid) == 40, uuid)

    rc, info = memory(nvml, device)
    expect(f"card {index}: memory", rc, SUCCESS)
    report(f"card {index}: free + used + reserved = total",
           info.free + info.used + info.reserved == info.total, f"{info.free}+{info.used}+{info.reserved}")
    wrong = Memory(version=1)
    expect(f"card {index}: memory, wrong version",
           nvml.nvmlDeviceGetMemoryInfo_v2(device, C.byref(wrong)), FUNCTION_NOT_FOUND)
    util = Utilization()
    expect(f"card {index}: utilization", nvml.nvmlDeviceGetUtilizationRates(device, C.byref(util)), SUCCESS)
    report(f"card {index}: utilization is a percentage", util.gpu <= 100, util.gpu)
    expect(f"card {index}: processes", processes(nvml, device)[0], SUCCESS)

    types = C.c_ulonglong()
    expect(f"card {index}: event types", nvml.nvmlDeviceGetSupportedEventTypes(device, C.byref(types)), SUCCESS)
    report(f"card {index}: critical Xid events supported", types.value & XID_EVENT, hex(types.value))
    return device, name, uuid


def check_quiet_wait(nvml, device):
    events = C.c_void_p()
    expect("event set", nvml.nvmlEventSetCreate(C.byref(events)), SUCCESS)
    expect("register an unsupported event type",
           nvml.nvmlDeviceRegisterEvents(device, C.c_ulonglong(1 << 40), events), NOT_SUPPORTED)
    expect("register critical Xid", nvml.nvmlDeviceRegisterEvents(device, C.c_ulonglong(XID_EVENT), events), SUCCESS)
    rc, _, took = wait(nvml, events, 300)
    expect("wait with no fault times out", rc, TIMEOUT)
    report("wait takes its timeout", 0.25 <= took < 1.0, f"{took:.3f} s")
    expect("free the event set", nvml.nvmlEventSetFree(events), SUCCESS)


def check_stand_in(nvml, cards):
    """Each card against sysfs, and memory held by another process"""
    minors = {}
    for entry in os.listdir(SYSFS):
        minor = int(entry.removeprefix("pyrite"))
        if os.access(f"/dev/{entry}", os.R_OK | os.W_OK):
            minors[read_sysfs(minor, "uuid")] = minor
    expect("cards are the pyrite devices this process can open", sorted(uuid for _, _, uuid in cards), sorted(minors))
    order = [minors.get(uuid, -1) for _, _, uuid in cards]
    expect("cards in minor order", order, sorted(order))

    for index, (device, _, uuid) in enumerate(cards):
        minor = minors.get(uuid, -1)
        _, info = memory(nvml, device)
        expect(f"card {index}: total is capacity_mb", info.total >> 20, int(read_sysfs(minor, "capacity_mb")))
        expect(f"card {index}: used is used_mb", info.used >> 20, int(read_sysfs(minor, "used_mb")))
        for call, arg in (("nvmlDeviceGetTemperature", (0, C.byref(C.c_uint()))),
                          ("nvmlDeviceGetPowerUsage", (C.byref(C.c_uint()),)),
                          ("nvmlDeviceGetEnforcedPowerLimit", (C.byref(C.c_uint()),)),
                          ("nvmlDeviceGetCurrentClocksThrottleReasons", (C.byref(C.c_ulonglong()),)),
                          ("nvmlDeviceGetRetiredPagesPendingStatus", (C.byref(C.c_int()),))):
            expect(f"card {index}: {call} not supported", getattr(nvml, call)(device, *arg), NOT_SUPPORTED)

    device, _, uuid = cards[0]
    _, before = memory(nvml, device)
    pid = hold(f"/dev/pyrite{minors[uuid]}", 64 << 20)
    rc, held = processes(nvml, device)
    expect("process holding 64 MiB is listed", (rc, held.get(pid)), (SUCCESS, 64 << 20))
    _, during = memory(nvml, device)
    expect("used grows by 64 MiB", during.used - before.used, 64 << 20)
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    expect("its memory goes with it", processes(nvml, device), (SUCCESS, {}))
    return minors


def hold(path, size):
    """A child process holding size bytes on the card until killed"""
    ready, go = os.pipe()
    pid = os.fork()
    if pid == 0:
        fd = os.open(path, os.O_RDWR)
        fcntl.ioctl(fd, PYRITE_IOC_ALLOC, bytearray(struct.pack("QQ", size, 0)))
        os.write(go, b"x")
        signal.pause()
    os.read(ready, 1)
    return pid


def inject(minor, command):
    with open(f"{DEBUGFS}/pyrite{minor}/inject", "w") as f:
        f.write(command)


def check_faults(nvml, cards, minors):
    """Xid events from debugfs faults on the last two cards"""
    (a, _, uuid_a), (b, _, uuid_b) = cards[-2], cards[-1]
    events = C.c_void_p()
    nvml.nvmlEventSetCreate(C.byref(events))
    for device, _, _ in cards:
        nvml.nvmlDeviceRegisterEvents(device, C.c_ulonglong(XID_EVENT), events)
    try:
        inject(minors[uuid_a], "xid 48")
        rc, event, took = wait(nvml, events, 3000)
        expect("xid 48 is an Xid 48 event on its card", (rc, event), (SUCCESS, (a.value, XID_EVENT, 48)))
        print(f"    event after {took * 1000:.0f} ms")
        expect("a card in xid still reads", memory(nvml, a)[0], SUCCESS)

        inject(minors[uuid_b], "lost")
        rc, event, _ = wait(nvml, events, 3000)
        expect("lost is an Xid 79 event", (rc, event), (SUCCESS, (b.value, XID_EVENT, 79)))
        expect("a lost card's readings fail", (memory(nvml, b)[0], processes(nvml, b)[0]), (GPU_IS_LOST, GPU_IS_LOST))
        buf = C.create_string_buffer(96)
        expect("a lost card still has its uuid", (nvml.nvmlDeviceGetUUID(b, buf, 96), buf.value.decode()),
               (SUCCESS, uuid_b))

        inject(minors[uuid_a], "reset")
        inject(minors[uuid_b], "reset")
        expect("a reset is no event", wait(nvml, events, 300)[0], TIMEOUT)

        inject(minors[uuid_a], "xid 62")
        inject(minors[uuid_b], "xid 63")
        got = {wait(nvml, events, 3000)[1] for _ in range(2)}
        expect("two faults at once are two events", got, {(a.value, XID_EVENT, 62), (b.value, XID_EVENT, 63)})
        expect("and then none", wait(nvml, events, 300)[0], TIMEOUT)

        late = C.c_void_p()
        nvml.nvmlEventSetCreate(C.byref(late))
        nvml.nvmlDeviceRegisterEvents(b, C.c_ulonglong(XID_EVENT), late)
        expect("a card faulty before registering is no event", wait(nvml, late, 300)[0], TIMEOUT)
        nvml.nvmlEventSetFree(late)
    finally:
        inject(minors[uuid_a], "reset")
        inject(minors[uuid_b], "reset")
        nvml.nvmlEventSetFree(events)


def main():
    nvml = C.CDLL(sys.argv[1] if len(sys.argv) > 1 else "libnvidia-ml.so.1")
    count = C.c_uint()
    expect("count before init", nvml.nvmlDeviceGetCount_v2(C.byref(count)), UNINITIALIZED)
    expect("init", nvml.nvmlInit_v2(), SUCCESS)
    expect("nested init", nvml.nvmlInit_v2(), SUCCESS)
    expect("nested shutdown", nvml.nvmlShutdown(), SUCCESS)
    expect("count", nvml.nvmlDeviceGetCount_v2(C.byref(count)), SUCCESS)
    if count.value == 0:
        report("cards found", False, "none")
        return 1
    expect("handle past the last card",
           nvml.nvmlDeviceGetHandleByIndex_v2(count.value, C.byref(C.c_void_p())), INVALID_ARGUMENT)

    cards = [check_card(nvml, index) for index in range(count.value)]
    check_quiet_wait(nvml, cards[0][0])
    if cards[0][1] == STAND_IN:
        minors = check_stand_in(nvml, cards)
        if os.geteuid() == 0 and len(cards) >= 2:
            check_faults(nvml, cards, minors)
        else:
            print("skip faults: they need root and two cards")
    else:
        print(f"skip pyrite checks: {cards[0][1]} is not the stand-in")

    expect("shutdown", nvml.nvmlShutdown(), SUCCESS)
    expect("count after shutdown", nvml.nvmlDeviceGetCount_v2(C.byref(count)), UNINITIALIZED)
    expect("shutdown again", nvml.nvmlShutdown(), SUCCESS)
    print(f"{failures} failed" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
