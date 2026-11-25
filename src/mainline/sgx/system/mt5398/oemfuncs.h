/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* OEM function table between 3rd-party kernel drivers and services (DDK interface). */
#ifndef __OEMFUNCS_H__
#define __OEMFUNCS_H__

typedef IMG_UINT32 (*PFN_SRV_BRIDGEDISPATCH)(IMG_UINT32 Ioctl, IMG_BYTE *pInBuf,
					     IMG_UINT32 InBufLen, IMG_BYTE *pOutBuf,
					     IMG_UINT32 OutBufLen,
					     IMG_UINT32 *pdwBytesTransferred);

typedef PVRSRV_ERROR (*PFN_SRV_READREGSTRING)(IMG_VOID *psRegistryString);

/* LG's layout (12 bytes); display drivers pass sizeof() to SysOEMFunction */
typedef struct PVRSRV_DC_OEM_JTABLE_TAG
{
	PFN_SRV_BRIDGEDISPATCH	pfnOEMBridgeDispatch;
	PFN_SRV_READREGSTRING	pfnOEMReadRegistryString;
	PFN_SRV_READREGSTRING	pfnOEMWriteRegistryString;
} PVRSRV_DC_OEM_JTABLE;

#define OEM_GET_EXT_FUNCS	(1 << 1)

#endif /* __OEMFUNCS_H__ */
