/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * MT5398 (LG NetCast 4.x) system description for the SGX DDK 1.9 kernel module.
 * Values match LG's pvrsrvkm.ko (DDK 1.9@2291151, system "sgx_cobra"): SYS_DATA has
 * room for 5 devices.
 */
#ifndef __SYSINFO_H__
#define __SYSINFO_H__

#define MAX_HW_TIME_US		(500000)
#define WAIT_TRY_COUNT		(10000)

#define SYS_DEVICE_COUNT	5	/* SGX, display class, buffer class */

#endif /* __SYSINFO_H__ */
