/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _UAPI_PYRITE_H
#define _UAPI_PYRITE_H


#include <linux/types.h>
#include <linux/ioctl.h>


#define PYRITE_IOCTL_MAGIC 0xBE


struct pyrite_alloc_req {
	__u64 bytes;    /* in: requested; out: rounded to 2 MiB multiple */
	__u64 alloc_id; /* out */
};


struct pyrite_free_req {
	__u64 alloc_id; /* in */
};


struct pyrite_device_info {
	__u64 capacity_bytes;
	__u64 used_bytes;
	__u64 util_pct; /* synthetic, 0..100 */
	__u64 alloc_count;
};


struct pyrite_proc_entry {
	__u64 tgid;
	__u64 used_bytes;
};


struct pyrite_procs_query {
	__u64 count;    /* in: capacity of buf in entries; out: entries present */
	__u64 buf_ptr;  /* user ptr to struct pyrite_proc_entry[]; 0 = probe */
};


#define PYRITE_IOC_ALLOC    _IOWR(PYRITE_IOCTL_MAGIC, 0x01, struct pyrite_alloc_req)
#define PYRITE_IOC_FREE     _IOW(PYRITE_IOCTL_MAGIC, 0x02, struct pyrite_free_req)
#define PYRITE_IOC_QUERY_DEVICE _IOR(PYRITE_IOCTL_MAGIC, 0x03, struct pyrite_device_info)
#define PYRITE_IOC_QUERY_PROCS  _IOWR(PYRITE_IOCTL_MAGIC, 0x04, struct pyrite_procs_query)


#endif /* _UAPI_PYRITE_H */
