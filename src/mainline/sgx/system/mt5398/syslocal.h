/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* MT5398 system-private data for the SGX DDK 1.9 kernel module. */
#ifndef __SYSLOCAL_H__
#define __SYSLOCAL_H__

#include <linux/platform_device.h>

#define SYS_SPECIFIC_DATA_ENABLE_ENVDATA	0x00000001
#define SYS_SPECIFIC_DATA_ENABLE_LOCATEDEV	0x00000002
#define SYS_SPECIFIC_DATA_ENABLE_LISR		0x00000004
#define SYS_SPECIFIC_DATA_ENABLE_MISR		0x00000008
#define SYS_SPECIFIC_DATA_ENABLE_REGDEV		0x00000010
#define SYS_SPECIFIC_DATA_ENABLE_INITDEV	0x00000020
#define SYS_SPECIFIC_DATA_PM_UNINSTALL_LISR	0x00000080

#define SYS_SPECIFIC_DATA_SET(psSysSpecData, flag) \
	((IMG_VOID)((psSysSpecData)->ui32SysSpecificData |= (flag)))
#define SYS_SPECIFIC_DATA_CLEAR(psSysSpecData, flag) \
	((IMG_VOID)((psSysSpecData)->ui32SysSpecificData &= ~(flag)))
#define SYS_SPECIFIC_DATA_TEST(psSysSpecData, flag) \
	(((psSysSpecData)->ui32SysSpecificData & (flag)) != 0)

typedef struct _SYS_SPECIFIC_DATA_TAG_
{
	IMG_UINT32		ui32SysSpecificData;
	PVRSRV_DEVICE_NODE	*psSGXDevNode;
} SYS_SPECIFIC_DATA;

extern SYS_SPECIFIC_DATA *gpsSysSpecificData;
extern struct platform_device *gpsPVRLDMDev;

#endif /* __SYSLOCAL_H__ */
