/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * MT5398 (LG NetCast 4.x) SGX544MP1 system constants. Values from LG's pvrsrvkm.ko
 * (sgx_cobra SysInitialise / SysLocateDevices), see docs/sgx544.md.
 */
#ifndef __SOCCONFIG_H__
#define __SOCCONFIG_H__

#define VS_PRODUCT_NAME			"MT5398"

#define SYS_SGX_CLOCK_SPEED		324000000	/* LG: 0x134fd900 */
#define SYS_SGX_HWRECOVERY_TIMEOUT_FREQ	(100)		/* 10 ms */
#define SYS_SGX_PDS_TIMER_FREQ		(1000)		/* 1 ms */
#define SYS_SGX_ACTIVE_POWER_LATENCY_MS	(1)

/* Register window; under mainline the platform device gives the same values. */
#define SYS_MT5398_SGX_REGS_SYS_PHYS_BASE	0xf0040000
#define SYS_MT5398_SGX_REGS_SIZE		0xc000

/* CKGEN words LG's SysInitialise writes after the device is initialised */
#define SYS_MT5398_CKGEN_PHYS_BASE	0xf000d000
#define SYS_MT5398_CKGEN_SGX_3D4	0x3d4	/* LG writes 9 */
#define SYS_MT5398_CKGEN_SGX_3D8	0x3d8	/* LG writes 1 */

#define DEVICE_SGX_INTERRUPT		(1 << 0)

#define SYS_SGX_DEV_NAME		"pvrsrvkm"

#endif /* __SOCCONFIG_H__ */
