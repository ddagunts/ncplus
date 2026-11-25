/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Build configuration of the SGX DDK 1.9 kernel module for the MT5398 (LG NetCast 4.x),
 * force-included into every file, in place of the generated config_kernel.h of TI's build.
 *
 * The SGX options must give LG's pvrsrvkm.ko build exactly: the TV's own userspace
 * (DDK 1.9@2291151, "cobra_lg_linux") checks them. LG's SGX_BUILD_OPTIONS word is
 * 0x00062418 (SGXDevInitCompatCheck's literal pool): HW recovery, secure handles, MP,
 * system cache, per-context PB, HWPERF; MP core count field 0 (one core). Everything
 * else in that word is off, including active power management.
 * The Linux-side switches (LDM platform driver, workqueues) are ours and don't reach
 * userspace.
 */

/* --- what userspace checks (sgx_options.h, struct layouts) --- */
#define LINUX
#define SUPPORT_SGX 1
#define SGXCORE 544
#define SGX544
#define SUPPORT_SGX544
#define SGX_CORE_REV 115		/* CORE_REVISION 0x00010105 read on the TV */
#define TRANSFER_QUEUE
#define PVR_SECURE_HANDLES
#define RELEASE
#define SUPPORT_PERCONTEXT_PB 1
#define SUPPORT_HW_RECOVERY 1
#define SUPPORT_SGX_HWPERF 1
#define SUPPORT_SGX_NEW_STATUS_VALS 1
#define SUPPORT_DBGDRV_EVENT_OBJECTS 1
#define SGX_FEATURE_SYSTEM_CACHE 1
#define SGX_FEATURE_MP 1
#define SGX_FEATURE_MP_CORE_COUNT 1
#define SYS_USING_INTERRUPTS 1
#define SUPPORT_PVRSRV_GET_DC_SYSTEM_BUFFER 1
#define SUPPORT_LARGE_GENERAL_HEAP 1
#define PVRSRV_DISABLE_UM_SYNCOBJ_MAPPINGS 1

/* --- build identity --- */
#define PVR_BUILD_DIR "mt5398_linux"
#define PVR_BUILD_TYPE "release"
#define PVRSRV_MODNAME "pvrsrvkm"
#define PVR_SYSTEM mt5398

/* --- Linux environment (ours) --- */
#define LDM_PLATFORM 1
#define PVR_LDM_DRIVER_REGISTRATION_NAME "pvrsrvkm"
#define PVR_LDM_PLATFORM_PRE_REGISTERED 1	/* the DT makes the device; don't register one */
/* MISR as a tasklet and timers as kernel timers, as in LG's build: the workqueue mode
 * needs CONFIG_PREEMPT (services spins waiting for work items), and ours is PREEMPT_NONE. */
#define PVR_LINUX_MEM_AREA_POOL_MAX_PAGES 0

/* --- bring-up: keep PVR_DPF error/warning messages in a release build (module param
 * "gPVRDebugLevel"); changes no structure or bridge number --- */
#define PVRSRV_NEED_PVR_DPF 1
