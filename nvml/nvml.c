// SPDX-License-Identifier: GPL-2.0-only
/*
 * libnvidia-ml.so.1 stand-in - pyrite's mock GPUs seen through NVML
 *
 * Answers the NVML calls a GPU scheduler makes from /sys/class/pyrite and
 * /dev/pyriteN, so a program written for NVML runs unchanged on a machine with
 * pyrite and no NVIDIA GPU. It would shadow the real library, so install it
 * only where there is no NVIDIA driver.
 *
 * The devices are the pyrite cards this process can open, in minor order.
 * Memory, utilization and processes come from pyrite's ioctls; temperature,
 * power, throttling and page retirement are not supported. A card that fell
 * off the bus fails its readings with GPU_IS_LOST, while its handle, name and
 * UUID keep working, so its Xid 79 can still be matched to the card.
 *
 * Health turning xid is a critical Xid event carrying last_xid, and turning
 * lost is Xid 79. pyrite wakes poll() on a card's health file when its health
 * changes; a wait also rereads every card before it returns.
 *
 * The declarations are this file's own, written to NVML's documented ABI.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../uapi/pyrite.h"

#define PYRITE_SYSFS	"/sys/class/pyrite"
#define DEVICE_NAME	"NVIDIA pyrite"
#define MAX_DEVICES	64
#define XID_LOST	79		/* fell off the bus */
#define NO_INSTANCE	0xFFFFFFFFu	/* no MIG instance */

typedef enum {
	NVML_SUCCESS = 0,
	NVML_ERROR_UNINITIALIZED = 1,
	NVML_ERROR_INVALID_ARGUMENT = 2,
	NVML_ERROR_NOT_SUPPORTED = 3,
	NVML_ERROR_NO_PERMISSION = 4,
	NVML_ERROR_INSUFFICIENT_SIZE = 7,
	NVML_ERROR_DRIVER_NOT_LOADED = 9,
	NVML_ERROR_TIMEOUT = 10,
	NVML_ERROR_FUNCTION_NOT_FOUND = 13,	/* also a wrong struct version */
	NVML_ERROR_GPU_IS_LOST = 15,
	NVML_ERROR_MEMORY = 20,
	NVML_ERROR_UNKNOWN = 999,
} nvmlReturn_t;

#define NVML_EVENT_TYPE_XID_CRITICAL_ERROR	0x8ULL

struct nvmlDevice_st {
	int minor;
	int fd;			/* /dev/pyriteN, open from init to shutdown */
	char uuid[48];		/* GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
};
typedef struct nvmlDevice_st *nvmlDevice_t;

typedef struct {
	unsigned long long total, free, used;
} nvmlMemory_t;

typedef struct {
	unsigned int version;
	unsigned long long total, reserved, free, used;
} nvmlMemory_v2_t;

#define NVML_MEMORY_V2	((unsigned int)(sizeof(nvmlMemory_v2_t) | 2 << 24))

typedef struct {
	unsigned int gpu, memory;
} nvmlUtilization_t;

typedef struct {
	unsigned int pid;
	unsigned long long usedGpuMemory;
	unsigned int gpuInstanceId, computeInstanceId;
} nvmlProcessInfo_t;

typedef struct {
	nvmlDevice_t device;
	unsigned long long eventType, eventData;
	unsigned int gpuInstanceId, computeInstanceId;
} nvmlEventData_t;

/* A registered card: its health file, which poll() watches, and its last reading */
struct watch {
	nvmlDevice_t device;
	int fd;
	char health[8];		/* ok, xid or lost */
	unsigned int last_xid;
};

struct nvmlEventSet_st {
	struct watch watches[MAX_DEVICES];
	int count;
};
typedef struct nvmlEventSet_st *nvmlEventSet_t;

/* Set by init, fixed until the matching shutdown */
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static int init_count;
static struct nvmlDevice_st devices[MAX_DEVICES];
static unsigned int device_count;

static int initialized(void)
{
	return __atomic_load_n(&init_count, __ATOMIC_ACQUIRE) > 0;
}

static nvmlReturn_t check(nvmlDevice_t device)
{
	if (!initialized())
		return NVML_ERROR_UNINITIALIZED;
	if (!device)
		return NVML_ERROR_INVALID_ARGUMENT;
	return NVML_SUCCESS;
}

static nvmlReturn_t from_errno(int err)
{
	switch (err) {
	case ENODEV:
		return NVML_ERROR_GPU_IS_LOST;
	case EACCES:
	case EPERM:
		return NVML_ERROR_NO_PERMISSION;
	case ERANGE:
		return NVML_ERROR_INSUFFICIENT_SIZE;
	default:
		return NVML_ERROR_UNKNOWN;
	}
}

/* Read fd from the start into buf as one line without its newline; -1 on error */
static int read_line(int fd, char *buf, size_t size)
{
	ssize_t n;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return -1;
	n = read(fd, buf, size - 1);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

/* Card minor's sysfs attribute name; -1 on error */
static int read_attr(int minor, const char *name, char *buf, size_t size)
{
	char path[64];
	int fd, ret;

	snprintf(path, sizeof(path), PYRITE_SYSFS "/pyrite%d/%s", minor, name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ret = read_line(fd, buf, size);
	close(fd);
	return ret;
}

/* Copy s whole or not at all, as NVML does */
static nvmlReturn_t copy_string(char *buf, unsigned int length, const char *s)
{
	if (!buf)
		return NVML_ERROR_INVALID_ARGUMENT;
	if (strlen(s) >= length)
		return NVML_ERROR_INSUFFICIENT_SIZE;
	strcpy(buf, s);
	return NVML_SUCCESS;
}

static int by_minor(const void *a, const void *b)
{
	return ((const struct nvmlDevice_st *)a)->minor -
		((const struct nvmlDevice_st *)b)->minor;
}

/* Open every pyrite card this process may use; init_lock held */
static nvmlReturn_t scan(void)
{
	struct nvmlDevice_st *device;
	struct dirent *entry;
	char path[32];
	DIR *dir;
	int minor;

	dir = opendir(PYRITE_SYSFS);
	if (!dir)
		return NVML_ERROR_DRIVER_NOT_LOADED;

	device_count = 0;
	while ((entry = readdir(dir)) && device_count < MAX_DEVICES) {
		if (sscanf(entry->d_name, "pyrite%d", &minor) != 1)
			continue;
		device = &devices[device_count];
		snprintf(path, sizeof(path), "/dev/pyrite%d", minor);
		device->fd = open(path, O_RDWR | O_CLOEXEC);
		if (device->fd < 0)
			continue;	/* not this process's card */
		device->minor = minor;
		if (read_attr(minor, "uuid", device->uuid, sizeof(device->uuid)))
			device->uuid[0] = '\0';
		device_count++;
	}
	closedir(dir);

	qsort(devices, device_count, sizeof(devices[0]), by_minor);
	return NVML_SUCCESS;
}

nvmlReturn_t nvmlInit_v2(void)
{
	nvmlReturn_t ret = NVML_SUCCESS;

	pthread_mutex_lock(&init_lock);
	if (init_count == 0)
		ret = scan();
	if (ret == NVML_SUCCESS)
		__atomic_store_n(&init_count, init_count + 1, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&init_lock);
	return ret;
}

nvmlReturn_t nvmlInitWithFlags(unsigned int flags)
{
	(void)flags;	/* a machine without cards already initializes */
	return nvmlInit_v2();
}

/* Succeeds even when not initialized, as NVML does */
nvmlReturn_t nvmlShutdown(void)
{
	unsigned int i;

	pthread_mutex_lock(&init_lock);
	if (init_count > 0)
		__atomic_store_n(&init_count, init_count - 1, __ATOMIC_RELEASE);
	if (init_count == 0) {
		for (i = 0; i < device_count; i++)
			close(devices[i].fd);
		device_count = 0;
	}
	pthread_mutex_unlock(&init_lock);
	return NVML_SUCCESS;
}

const char *nvmlErrorString(nvmlReturn_t result)
{
	switch (result) {
	case NVML_SUCCESS:
		return "Success";
	case NVML_ERROR_UNINITIALIZED:
		return "Uninitialized";
	case NVML_ERROR_INVALID_ARGUMENT:
		return "Invalid Argument";
	case NVML_ERROR_NOT_SUPPORTED:
		return "Not Supported";
	case NVML_ERROR_NO_PERMISSION:
		return "Insufficient Permissions";
	case NVML_ERROR_INSUFFICIENT_SIZE:
		return "Insufficient Size";
	case NVML_ERROR_DRIVER_NOT_LOADED:
		return "Driver Not Loaded";
	case NVML_ERROR_TIMEOUT:
		return "Timeout";
	case NVML_ERROR_FUNCTION_NOT_FOUND:
		return "Function Not Found";
	case NVML_ERROR_GPU_IS_LOST:
		return "GPU is lost";
	case NVML_ERROR_MEMORY:
		return "Insufficient Memory";
	default:
		return "Unknown Error";
	}
}

nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *count)
{
	if (!initialized())
		return NVML_ERROR_UNINITIALIZED;
	if (!count)
		return NVML_ERROR_INVALID_ARGUMENT;
	*count = device_count;
	return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t *device)
{
	if (!initialized())
		return NVML_ERROR_UNINITIALIZED;
	if (index >= device_count || !device)
		return NVML_ERROR_INVALID_ARGUMENT;
	*device = &devices[index];
	return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t device, char *name, unsigned int length)
{
	nvmlReturn_t ret = check(device);

	return ret ? ret : copy_string(name, length, DEVICE_NAME);
}

nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char *uuid, unsigned int length)
{
	nvmlReturn_t ret = check(device);

	return ret ? ret : copy_string(uuid, length, device->uuid);
}

/* The card's memory and utilization; the caller has checked the handle */
static nvmlReturn_t query_device(nvmlDevice_t device, struct pyrite_device_info *info)
{
	if (ioctl(device->fd, PYRITE_IOC_QUERY_DEVICE, info) < 0)
		return from_errno(errno);
	return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t *memory)
{
	struct pyrite_device_info info;
	nvmlReturn_t ret = check(device);

	if (ret)
		return ret;
	if (!memory)
		return NVML_ERROR_INVALID_ARGUMENT;
	ret = query_device(device, &info);
	if (ret)
		return ret;
	memory->total = info.capacity_bytes;
	memory->used = info.used_bytes;
	memory->free = info.capacity_bytes - info.used_bytes;
	return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_v2_t *memory)
{
	struct pyrite_device_info info;
	nvmlReturn_t ret = check(device);

	if (ret)
		return ret;
	if (!memory)
		return NVML_ERROR_INVALID_ARGUMENT;
	if (memory->version != NVML_MEMORY_V2)
		return NVML_ERROR_FUNCTION_NOT_FOUND;
	ret = query_device(device, &info);
	if (ret)
		return ret;
	memory->total = info.capacity_bytes;
	memory->reserved = 0;
	memory->used = info.used_bytes;
	memory->free = info.capacity_bytes - info.used_bytes;
	return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t *utilization)
{
	struct pyrite_device_info info;
	nvmlReturn_t ret = check(device);

	if (ret)
		return ret;
	if (!utilization)
		return NVML_ERROR_INVALID_ARGUMENT;
	ret = query_device(device, &info);
	if (ret)
		return ret;
	utilization->gpu = info.util_pct;
	utilization->memory = 0;
	return NVML_SUCCESS;
}

/*
 * Every process holding memory on the card. With *infoCount too small for
 * them, as with 0 to ask how many, INSUFFICIENT_SIZE sets *infoCount to the
 * number there are.
 */
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t device,
		unsigned int *infoCount, nvmlProcessInfo_t *infos)
{
	struct pyrite_proc_entry *entries = NULL;
	struct pyrite_procs_query query;
	nvmlReturn_t ret = check(device);
	unsigned int i;

	if (ret)
		return ret;
	if (!infoCount || (*infoCount && !infos))
		return NVML_ERROR_INVALID_ARGUMENT;
	if (*infoCount) {
		entries = calloc(*infoCount, sizeof(*entries));
		if (!entries)
			return NVML_ERROR_MEMORY;
	}

	query.count = *infoCount;
	query.buf_ptr = (uintptr_t)entries;
	if (ioctl(device->fd, PYRITE_IOC_QUERY_PROCS, &query) < 0) {
		ret = from_errno(errno);
	} else {
		for (i = 0; i < query.count; i++) {
			infos[i].pid = entries[i].tgid;
			infos[i].usedGpuMemory = entries[i].used_bytes;
			infos[i].gpuInstanceId = NO_INSTANCE;
			infos[i].computeInstanceId = NO_INSTANCE;
		}
	}
	if (ret == NVML_SUCCESS || ret == NVML_ERROR_INSUFFICIENT_SIZE)
		*infoCount = query.count;
	free(entries);
	return ret;
}

static nvmlReturn_t not_supported(nvmlDevice_t device)
{
	nvmlReturn_t ret = check(device);

	return ret ? ret : NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, int sensorType, unsigned int *temp)
{
	(void)sensorType;
	(void)temp;
	return not_supported(device);
}

nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t device, unsigned int *power)
{
	(void)power;
	return not_supported(device);
}

nvmlReturn_t nvmlDeviceGetEnforcedPowerLimit(nvmlDevice_t device, unsigned int *limit)
{
	(void)limit;
	return not_supported(device);
}

nvmlReturn_t nvmlDeviceGetCurrentClocksThrottleReasons(nvmlDevice_t device,
		unsigned long long *reasons)
{
	(void)reasons;
	return not_supported(device);
}

nvmlReturn_t nvmlDeviceGetRetiredPagesPendingStatus(nvmlDevice_t device, int *isPending)
{
	(void)isPending;
	return not_supported(device);
}

nvmlReturn_t nvmlDeviceGetSupportedEventTypes(nvmlDevice_t device,
		unsigned long long *eventTypes)
{
	nvmlReturn_t ret = check(device);

	if (ret)
		return ret;
	if (!eventTypes)
		return NVML_ERROR_INVALID_ARGUMENT;
	*eventTypes = NVML_EVENT_TYPE_XID_CRITICAL_ERROR;
	return NVML_SUCCESS;
}

nvmlReturn_t nvmlEventSetCreate(nvmlEventSet_t *set)
{
	if (!initialized())
		return NVML_ERROR_UNINITIALIZED;
	if (!set)
		return NVML_ERROR_INVALID_ARGUMENT;
	*set = calloc(1, sizeof(**set));
	return *set ? NVML_SUCCESS : NVML_ERROR_MEMORY;
}

/* Reread a card's health through its polled file, which re-arms poll() */
static int read_watch(struct watch *watch, char *health, unsigned int *last_xid)
{
	char xid[16];

	if (read_line(watch->fd, health, sizeof(watch->health)) ||
	    read_attr(watch->device->minor, "last_xid", xid, sizeof(xid)))
		return -1;
	*last_xid = strtoul(xid, NULL, 10);
	return 0;
}

/*
 * Only faults after registering are events, as with NVML: a card that is
 * already faulty here stays quiet until its health changes again.
 */
nvmlReturn_t nvmlDeviceRegisterEvents(nvmlDevice_t device, unsigned long long eventTypes,
		nvmlEventSet_t set)
{
	nvmlReturn_t ret = check(device);
	struct watch *watch;
	char path[64];
	int i;

	if (ret)
		return ret;
	if (!set || !eventTypes)
		return NVML_ERROR_INVALID_ARGUMENT;
	if (eventTypes & ~NVML_EVENT_TYPE_XID_CRITICAL_ERROR)
		return NVML_ERROR_NOT_SUPPORTED;
	for (i = 0; i < set->count; i++)
		if (set->watches[i].device == device)
			return NVML_SUCCESS;

	watch = &set->watches[set->count];
	watch->device = device;
	snprintf(path, sizeof(path), PYRITE_SYSFS "/pyrite%d/health", device->minor);
	watch->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (watch->fd < 0)
		return from_errno(errno);
	if (read_watch(watch, watch->health, &watch->last_xid)) {
		close(watch->fd);
		return NVML_ERROR_UNKNOWN;
	}
	set->count++;
	return NVML_SUCCESS;
}

/* Turn a changed reading into an event; a reset back to ok is none */
static int changed(struct watch *watch, nvmlEventData_t *data)
{
	char health[sizeof(watch->health)];
	unsigned int last_xid;

	if (read_watch(watch, health, &last_xid))
		return -1;
	if (!strcmp(health, watch->health) && last_xid == watch->last_xid)
		return 0;
	strcpy(watch->health, health);
	watch->last_xid = last_xid;
	if (strcmp(health, "xid") && strcmp(health, "lost"))
		return 0;

	data->device = watch->device;
	data->eventType = NVML_EVENT_TYPE_XID_CRITICAL_ERROR;
	data->eventData = strcmp(health, "lost") ? last_xid : XID_LOST;
	data->gpuInstanceId = NO_INSTANCE;
	data->computeInstanceId = NO_INSTANCE;
	return 1;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/*
 * The next event: one card's change per call, so when several cards fault at
 * once the following calls return the others.
 */
nvmlReturn_t nvmlEventSetWait_v2(nvmlEventSet_t set, nvmlEventData_t *data,
		unsigned int timeoutms)
{
	struct pollfd fds[MAX_DEVICES];
	long long end = now_ms() + timeoutms;
	long long left;
	int i, r;

	if (!initialized())
		return NVML_ERROR_UNINITIALIZED;
	if (!set || !data)
		return NVML_ERROR_INVALID_ARGUMENT;

	for (;;) {
		for (i = 0; i < set->count; i++) {
			r = changed(&set->watches[i], data);
			if (r < 0)
				return NVML_ERROR_UNKNOWN;
			if (r)
				return NVML_SUCCESS;
		}

		left = end - now_ms();
		if (left <= 0)
			return NVML_ERROR_TIMEOUT;
		for (i = 0; i < set->count; i++) {
			fds[i].fd = set->watches[i].fd;
			fds[i].events = POLLPRI;
			fds[i].revents = 0;
		}
		poll(fds, set->count, left > INT_MAX ? INT_MAX : left);
	}
}

nvmlReturn_t nvmlEventSetFree(nvmlEventSet_t set)
{
	int i;

	if (!initialized())
		return NVML_ERROR_UNINITIALIZED;
	if (!set)
		return NVML_ERROR_INVALID_ARGUMENT;
	for (i = 0; i < set->count; i++)
		close(set->watches[i].fd);
	free(set);
	return NVML_SUCCESS;
}
