/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * MT5398 (LG NetCast 4.x) system layer for the SGX DDK 1.9 kernel module (TI's open
 * release), under the mainline kernel.
 *
 * It does what LG's sgx_cobra sysconfig does (from its pvrsrvkm.ko, docs/sgx544.md):
 * one SGX device, UMA (no local memory, every heap SYSMEM_NONCONTIG), static timing
 * (324 MHz, 100 Hz recovery, 1000 Hz microkernel timer, no active power management),
 * no clock or power control, and LG's two CKGEN writes after the device is initialised.
 * The registers and the interrupt come from the platform device (the DT node), so the
 * interrupt goes through the BIM irqchip like every other MT5398 device.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/platform_device.h>

#include "sgxdefs.h"
#include "services_headers.h"
#include "kerneldisplay.h"
#include "oemfuncs.h"
#include "sgxinfo.h"
#include "sgxinfokm.h"
#include "syslocal.h"

SYS_DATA *gpsSysData = (SYS_DATA *)IMG_NULL;
SYS_DATA gsSysData;

static SYS_SPECIFIC_DATA gsSysSpecificData;
SYS_SPECIFIC_DATA *gpsSysSpecificData;

static IMG_UINT32 gui32SGXDeviceID;
static SGX_DEVICE_MAP gsSGXDeviceMap;
static PVRSRV_DEVICE_NODE *gpsSGXDevNode;

IMG_UINT32 PVRSRV_BridgeDispatchKM(IMG_UINT32 Ioctl, IMG_BYTE *pInBuf, IMG_UINT32 InBufLen,
				   IMG_BYTE *pOutBuf, IMG_UINT32 OutBufLen,
				   IMG_UINT32 *pdwBytesTransferred);

static PVRSRV_ERROR SysLocateDevices(SYS_DATA *psSysData)
{
	struct resource *res;
	int irq;

	PVR_UNREFERENCED_PARAMETER(psSysData);

	if (!gpsPVRLDMDev)
		return PVRSRV_ERROR_INVALID_DEVICE;
	res = platform_get_resource(gpsPVRLDMDev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(gpsPVRLDMDev, 0);
	if (!res || irq < 0) {
		PVR_DPF((PVR_DBG_ERROR, "SysLocateDevices: no registers or interrupt"));
		return PVRSRV_ERROR_INVALID_DEVICE;
	}

	gsSGXDeviceMap.ui32Flags = 0;
	gsSGXDeviceMap.sRegsSysPBase.uiAddr = res->start;
	gsSGXDeviceMap.sRegsCpuPBase = SysSysPAddrToCpuPAddr(gsSGXDeviceMap.sRegsSysPBase);
	gsSGXDeviceMap.ui32RegsSize = resource_size(res);
	gsSGXDeviceMap.ui32IRQ = irq;
	/* UMA: no local device memory */
	gsSGXDeviceMap.sLocalMemSysPBase.uiAddr = 0;
	gsSGXDeviceMap.sLocalMemDevPBase.uiAddr = 0;
	gsSGXDeviceMap.sLocalMemCpuPBase.uiAddr = 0;
	gsSGXDeviceMap.ui32LocalMemSize = 0;

	printk(KERN_INFO "pvrsrvkm: SGX regs %#x+%#x irq %u\n",
	       (unsigned)gsSGXDeviceMap.sRegsSysPBase.uiAddr,
	       (unsigned)gsSGXDeviceMap.ui32RegsSize, (unsigned)gsSGXDeviceMap.ui32IRQ);
	return PVRSRV_OK;
}

static IMG_CHAR *SysCreateVersionString(void)
{
	static IMG_CHAR aszVersionString[100];
	IMG_VOID *pvRegs;
	IMG_UINT32 ui32Rev;

	pvRegs = OSMapPhysToLin(gsSGXDeviceMap.sRegsCpuPBase, gsSGXDeviceMap.ui32RegsSize,
				PVRSRV_HAP_UNCACHED | PVRSRV_HAP_KERNEL_ONLY, IMG_NULL);
	if (!pvRegs)
		return IMG_NULL;
	ui32Rev = OSReadHWReg(pvRegs, EUR_CR_CORE_REVISION);
	OSUnMapPhysToLin(pvRegs, gsSGXDeviceMap.ui32RegsSize,
			 PVRSRV_HAP_UNCACHED | PVRSRV_HAP_KERNEL_ONLY, IMG_NULL);

	if (OSSNPrintf(aszVersionString, sizeof(aszVersionString), "SGX revision = %u.%u.%u",
		       (IMG_UINT)((ui32Rev & EUR_CR_CORE_REVISION_MAJOR_MASK)
				  >> EUR_CR_CORE_REVISION_MAJOR_SHIFT),
		       (IMG_UINT)((ui32Rev & EUR_CR_CORE_REVISION_MINOR_MASK)
				  >> EUR_CR_CORE_REVISION_MINOR_SHIFT),
		       (IMG_UINT)((ui32Rev & EUR_CR_CORE_REVISION_MAINTENANCE_MASK)
				  >> EUR_CR_CORE_REVISION_MAINTENANCE_SHIFT)) == -1)
		return IMG_NULL;
	return aszVersionString;
}

/* LG's SysInitialise writes CKGEN 0x3d4 = 9 and 0x3d8 = 1 once the device is set up. */
static void SysSetupCkgen(void)
{
	void __iomem *ck = ioremap(SYS_MT5398_CKGEN_PHYS_BASE, 0x1000);
	u32 a, b;

	if (!ck)
		return;
	a = readl(ck + SYS_MT5398_CKGEN_SGX_3D4);
	b = readl(ck + SYS_MT5398_CKGEN_SGX_3D8);
	if (a != 9)
		writel(9, ck + SYS_MT5398_CKGEN_SGX_3D4);
	if (b != 1)
		writel(1, ck + SYS_MT5398_CKGEN_SGX_3D8);
	printk(KERN_INFO "pvrsrvkm: CKGEN 3d4 %#x 3d8 %#x (LG: 9, 1)\n", a, b);
	iounmap(ck);
}

PVRSRV_ERROR SysInitialise(IMG_VOID)
{
	IMG_UINT32 i;
	PVRSRV_ERROR eError;
	PVRSRV_DEVICE_NODE *psDeviceNode;
	SGX_TIMING_INFORMATION *psTimingInfo;

	gpsSysData = &gsSysData;
	OSMemSet(gpsSysData, 0, sizeof(SYS_DATA));
	gpsSysSpecificData = &gsSysSpecificData;
	OSMemSet(gpsSysSpecificData, 0, sizeof(SYS_SPECIFIC_DATA));
	gpsSysData->pvSysSpecificData = gpsSysSpecificData;

	eError = OSInitEnvData(&gpsSysData->pvEnvSpecificData);
	if (eError != PVRSRV_OK)
		goto fail;
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_ENVDATA);

	gpsSysData->ui32NumDevices = SYS_DEVICE_COUNT;
	for (i = 0; i < SYS_DEVICE_COUNT; i++) {
		gpsSysData->sDeviceID[i].uiID = i;
		gpsSysData->sDeviceID[i].bInUse = IMG_FALSE;
	}
	gpsSysData->psDeviceNodeList = IMG_NULL;
	gpsSysData->psQueueList = IMG_NULL;

	eError = SysInitialiseCommon(gpsSysData);
	if (eError != PVRSRV_OK)
		goto fail;

	psTimingInfo = &gsSGXDeviceMap.sTimingInfo;
	psTimingInfo->ui32CoreClockSpeed = SYS_SGX_CLOCK_SPEED;
	psTimingInfo->ui32HWRecoveryFreq = SYS_SGX_HWRECOVERY_TIMEOUT_FREQ;
	psTimingInfo->bEnableActivePM = IMG_FALSE;
	psTimingInfo->ui32ActivePowManLatencyms = SYS_SGX_ACTIVE_POWER_LATENCY_MS;
	psTimingInfo->ui32uKernelFreq = SYS_SGX_PDS_TIMER_FREQ;

	eError = SysLocateDevices(gpsSysData);
	if (eError != PVRSRV_OK)
		goto fail;
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_LOCATEDEV);

	eError = PVRSRVRegisterDevice(gpsSysData, SGXRegisterDevice, DEVICE_SGX_INTERRUPT,
				      &gui32SGXDeviceID);
	if (eError != PVRSRV_OK)
		goto fail;
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_REGDEV);

	for (psDeviceNode = gpsSysData->psDeviceNodeList; psDeviceNode;
	     psDeviceNode = psDeviceNode->psNext) {
		DEVICE_MEMORY_INFO *psDevMemoryInfo;
		DEVICE_MEMORY_HEAP_INFO *psHeap;

		if (psDeviceNode->sDevId.eDeviceType != PVRSRV_DEVICE_TYPE_SGX) {
			eError = PVRSRV_ERROR_INIT_FAILURE;
			goto fail;
		}
		/* page tables and every heap in system memory */
		psDeviceNode->psLocalDevMemArena = IMG_NULL;
		psDevMemoryInfo = &psDeviceNode->sDevMemoryInfo;
		psHeap = psDevMemoryInfo->psDeviceMemoryHeap;
		for (i = 0; i < psDevMemoryInfo->ui32HeapCount; i++)
			psHeap[i].ui32Attribs |= PVRSRV_BACKINGSTORE_SYSMEM_NONCONTIG;
		gpsSGXDevNode = psDeviceNode;
		gsSysSpecificData.psSGXDevNode = psDeviceNode;
	}

	eError = PVRSRVInitialiseDevice(gui32SGXDeviceID);
	if (eError != PVRSRV_OK)
		goto fail;
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_INITDEV);

	SysSetupCkgen();
	return PVRSRV_OK;

fail:
	PVR_DPF((PVR_DBG_ERROR, "SysInitialise: failed (%d)", eError));
	(IMG_VOID)SysDeinitialise(gpsSysData);
	gpsSysData = IMG_NULL;
	return eError;
}

/*
 * nckexec holds the whole GPU in reset across the warm boot (LG's microkernel must not keep
 * writing to RAM we own). A read of a core register in that state is a bus abort, and
 * SysCreateVersionString() and the ISR read some before the first SGXReset(). So bring it out
 * with SGXReset()'s own steps, whatever state it is in: with its microkernel gone it stays
 * idle until we load ours. The master bank registers are readable in reset.
 */
static PVRSRV_ERROR SysReleaseReset(IMG_VOID)
{
	void __iomem *regs = ioremap(gsSGXDeviceMap.sRegsCpuPBase.uiAddr, gsSGXDeviceMap.ui32RegsSize);

	if (!regs)
		return PVRSRV_ERROR_INIT_FAILURE;
	printk(KERN_INFO "pvrsrvkm: MASTER_SOFT_RESET was %#x\n", readl(regs + 0x4080));
	writel(0x1ff, regs + 0x4080);		/* MASTER_SOFT_RESET: cores 0-3, IPF, DPM, VDM, SLC, BIF */
	readl(regs + 0x4080);
	udelay(5);
	writel(0, regs + 0x4c00);		/* MASTER_BIF_CTRL */
	writel(0x0004c000, regs + 0x4d00);	/* MASTER_SLC_CTRL */
	writel(0x04000000, regs + 0x4d04);	/* MASTER_SLC_CTRL_BYPASS */
	readl(regs + 0x4080);
	udelay(5);
	writel(0, regs + 0x4080);
	readl(regs + 0x4080);
	udelay(5);
	iounmap(regs);
	return PVRSRV_OK;
}

PVRSRV_ERROR SysFinalise(IMG_VOID)
{
	PVRSRV_ERROR eError;

	eError = SysReleaseReset();
	if (eError != PVRSRV_OK)
		return eError;

	eError = OSInstallMISR(gpsSysData);
	if (eError != PVRSRV_OK)
		return eError;
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_MISR);

	eError = OSInstallDeviceLISR(gpsSysData, gsSGXDeviceMap.ui32IRQ, "SGX ISR", gpsSGXDevNode);
	if (eError != PVRSRV_OK)
		return eError;
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_LISR);

	gpsSysData->pszVersionString = SysCreateVersionString();
	if (!gpsSysData->pszVersionString)
		return PVRSRV_ERROR_INIT_FAILURE;
	printk(KERN_INFO "pvrsrvkm: %s\n", gpsSysData->pszVersionString);
	return PVRSRV_OK;
}

PVRSRV_ERROR SysDeinitialise(SYS_DATA *psSysData)
{
	PVRSRV_ERROR eError;

	PVR_UNREFERENCED_PARAMETER(psSysData);

	if (SYS_SPECIFIC_DATA_TEST(gpsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_LISR)) {
		eError = OSUninstallDeviceLISR(gpsSysData);
		if (eError != PVRSRV_OK)
			return eError;
	}
	if (SYS_SPECIFIC_DATA_TEST(gpsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_MISR)) {
		eError = OSUninstallMISR(gpsSysData);
		if (eError != PVRSRV_OK)
			return eError;
	}
	if (SYS_SPECIFIC_DATA_TEST(gpsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_INITDEV)) {
		eError = PVRSRVDeinitialiseDevice(gui32SGXDeviceID);
		if (eError != PVRSRV_OK)
			return eError;
	}
	if (SYS_SPECIFIC_DATA_TEST(gpsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_ENVDATA)) {
		eError = OSDeInitEnvData(gpsSysData->pvEnvSpecificData);
		if (eError != PVRSRV_OK)
			return eError;
	}
	SysDeinitialiseCommon(gpsSysData);

	gpsSysSpecificData->ui32SysSpecificData = 0;
	gpsSysData = IMG_NULL;
	return PVRSRV_OK;
}

PVRSRV_ERROR SysGetDeviceMemoryMap(PVRSRV_DEVICE_TYPE eDeviceType, IMG_VOID **ppvDeviceMap)
{
	if (eDeviceType == PVRSRV_DEVICE_TYPE_SGX)
		*ppvDeviceMap = (IMG_VOID *)&gsSGXDeviceMap;
	return PVRSRV_OK;
}

/* UMA: device, system and CPU physical addresses are the same */
IMG_DEV_PHYADDR SysCpuPAddrToDevPAddr(PVRSRV_DEVICE_TYPE eDeviceType, IMG_CPU_PHYADDR CpuPAddr)
{
	IMG_DEV_PHYADDR DevPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);
	DevPAddr.uiAddr = CpuPAddr.uiAddr;
	return DevPAddr;
}

IMG_CPU_PHYADDR SysSysPAddrToCpuPAddr(IMG_SYS_PHYADDR sys_paddr)
{
	IMG_CPU_PHYADDR cpu_paddr;

	cpu_paddr.uiAddr = sys_paddr.uiAddr;
	return cpu_paddr;
}

IMG_SYS_PHYADDR SysCpuPAddrToSysPAddr(IMG_CPU_PHYADDR cpu_paddr)
{
	IMG_SYS_PHYADDR sys_paddr;

	sys_paddr.uiAddr = cpu_paddr.uiAddr;
	return sys_paddr;
}

IMG_DEV_PHYADDR SysSysPAddrToDevPAddr(PVRSRV_DEVICE_TYPE eDeviceType, IMG_SYS_PHYADDR SysPAddr)
{
	IMG_DEV_PHYADDR DevPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);
	DevPAddr.uiAddr = SysPAddr.uiAddr;
	return DevPAddr;
}

IMG_SYS_PHYADDR SysDevPAddrToSysPAddr(PVRSRV_DEVICE_TYPE eDeviceType, IMG_DEV_PHYADDR DevPAddr)
{
	IMG_SYS_PHYADDR SysPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);
	SysPAddr.uiAddr = DevPAddr.uiAddr;
	return SysPAddr;
}

IMG_VOID SysRegisterExternalDevice(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);
}

IMG_VOID SysRemoveExternalDevice(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);
}

/* The SGX has its own interrupt line; the BIM irqchip acks it. */
IMG_UINT32 SysGetInterruptSource(SYS_DATA *psSysData, PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVR_UNREFERENCED_PARAMETER(psSysData);
	return psDeviceNode ? psDeviceNode->ui32SOCInterruptBit : 0;
}

IMG_VOID SysClearInterrupts(SYS_DATA *psSysData, IMG_UINT32 ui32ClearBits)
{
	PVR_UNREFERENCED_PARAMETER(ui32ClearBits);
	PVR_UNREFERENCED_PARAMETER(psSysData);
	/* flush the posted EVENT_HOST_CLEAR write */
	OSReadHWReg(((PVRSRV_SGXDEV_INFO *)gpsSGXDevNode->pvDevice)->pvRegsBaseKM,
		    EUR_CR_EVENT_HOST_CLEAR);
}

PVRSRV_ERROR SysSystemPrePowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (eNewPowerState == PVRSRV_SYS_POWER_STATE_D3 &&
	    SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_LISR)) {
		eError = OSUninstallDeviceLISR(gpsSysData);
		if (eError != PVRSRV_OK)
			return eError;
		SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNINSTALL_LISR);
		SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_LISR);
	}
	return eError;
}

PVRSRV_ERROR SysSystemPostPowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (eNewPowerState == PVRSRV_SYS_POWER_STATE_D0 &&
	    SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNINSTALL_LISR)) {
		eError = OSInstallDeviceLISR(gpsSysData, gsSGXDeviceMap.ui32IRQ, "SGX ISR",
					     gpsSGXDevNode);
		if (eError != PVRSRV_OK)
			return eError;
		SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_ENABLE_LISR);
		SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNINSTALL_LISR);
	}
	return eError;
}

/* No clock or power control on this SoC (LG's hooks do nothing that matters). */
PVRSRV_ERROR SysDevicePrePowerState(IMG_UINT32 ui32DeviceIndex,
				    PVRSRV_DEV_POWER_STATE eNewPowerState,
				    PVRSRV_DEV_POWER_STATE eCurrentPowerState)
{
	PVR_UNREFERENCED_PARAMETER(ui32DeviceIndex);
	PVR_UNREFERENCED_PARAMETER(eNewPowerState);
	PVR_UNREFERENCED_PARAMETER(eCurrentPowerState);
	return PVRSRV_OK;
}

PVRSRV_ERROR SysDevicePostPowerState(IMG_UINT32 ui32DeviceIndex,
				     PVRSRV_DEV_POWER_STATE eNewPowerState,
				     PVRSRV_DEV_POWER_STATE eCurrentPowerState)
{
	PVR_UNREFERENCED_PARAMETER(ui32DeviceIndex);
	PVR_UNREFERENCED_PARAMETER(eNewPowerState);
	PVR_UNREFERENCED_PARAMETER(eCurrentPowerState);
	return PVRSRV_OK;
}

PVRSRV_ERROR SysOEMFunction(IMG_UINT32 ui32ID, IMG_VOID *pvIn, IMG_UINT32 ulInSize,
			    IMG_VOID *pvOut, IMG_UINT32 ulOutSize)
{
	PVR_UNREFERENCED_PARAMETER(pvIn);
	PVR_UNREFERENCED_PARAMETER(ulInSize);

	if (ui32ID == OEM_GET_EXT_FUNCS && ulOutSize == sizeof(PVRSRV_DC_OEM_JTABLE)) {
		PVRSRV_DC_OEM_JTABLE *psOEMJTable = (PVRSRV_DC_OEM_JTABLE *)pvOut;

		psOEMJTable->pfnOEMBridgeDispatch = &PVRSRV_BridgeDispatchKM;
		return PVRSRV_OK;
	}
	return PVRSRV_ERROR_INVALID_PARAMS;
}
