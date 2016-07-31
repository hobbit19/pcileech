// device.c : implementation related to the USB3380 hardware device.
//
// (c) Ulf Frisk, 2016
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "device.h"
#include "kmd.h"
#include "util.h"
#include <versionhelpers.h>

typedef struct tdEP_INFO {
	UCHAR pipe;
	WORD rCTL;
	WORD rSTAT;
	WORD rCOUNT;
	WORD rADDR;
} EP_INFO, *PEP_INFO;

EP_INFO CEP_INFO[3] = {
	{ .pipe = 0x84,.rCTL = REG_DMACTL_1,.rSTAT = REG_DMASTAT_1,.rCOUNT = REG_DMACOUNT_1,.rADDR = REG_DMAADDR_1 },
	{ .pipe = 0x86,.rCTL = REG_DMACTL_2,.rSTAT = REG_DMASTAT_2,.rCOUNT = REG_DMACOUNT_2,.rADDR = REG_DMAADDR_2 },
	{ .pipe = 0x88,.rCTL = REG_DMACTL_3,.rSTAT = REG_DMASTAT_3,.rCOUNT = REG_DMACOUNT_3,.rADDR = REG_DMAADDR_3 }
};

typedef struct tdThreadDataReadEP {
	PDEVICE_DATA pDeviceData;
	DWORD dwAddrPci32;
	PBYTE pb;
	DWORD cb;
	BOOL isFinished;
	BOOL result;
	PEP_INFO pep;
} THREAD_DATA_READ_EP, *PTHREAD_DATA_READ_EP;

typedef struct _DEVICE_MEMORY_RANGE {
	DWORD BaseAddress;
	DWORD TopAddress;
} DEVICE_MEMORY_RANGE, *PDEVICE_MEMORY_RANGE;

#define NUMBER_OF_DEVICE_RESERVED_MEMORY_RANGES 2
DEVICE_MEMORY_RANGE CDEVICE_RESERVED_MEMORY_RANGES[NUMBER_OF_DEVICE_RESERVED_MEMORY_RANGES] = {
	{ .BaseAddress = 0x000A0000,.TopAddress = 0x000FFFFF }, // SMM LOWER
	{ .BaseAddress = 0xF0000000,.TopAddress = 0xFFFFFFFF }, // PCI SPACE
};

BOOL _DeviceIsInReservedMemoryRange(_In_ DWORD dwAddrPci32, _In_ DWORD cb)
{
	PDEVICE_MEMORY_RANGE pmr;
	for(DWORD i = 0; i < NUMBER_OF_DEVICE_RESERVED_MEMORY_RANGES; i++) {
		pmr = &CDEVICE_RESERVED_MEMORY_RANGES[i];
		if(!((dwAddrPci32 > pmr->TopAddress) || (dwAddrPci32 + cb <= pmr->BaseAddress))) {
			return TRUE;
		}
	}
	return FALSE;
}

BOOL DeviceWriteCsr(_In_ PDEVICE_DATA pDeviceData, _In_ WORD wRegAddr, _In_ DWORD dwRegValue, _In_ BYTE fCSR)
{
	DWORD cbTransferred;
	PIPE_SEND_CSR_WRITE ps = { .u1 = fCSR | 0x40, .u2 = 0, .u3 = wRegAddr & 0xFF, .u4 = (wRegAddr >> 8) & 0xFF, .dwRegValue = dwRegValue };
	if(wRegAddr & 0x03) { return FALSE; } // must be dword aligned
	return WinUsb_WritePipe(pDeviceData->WinusbHandle, pDeviceData->PipeCsrOut, (PUCHAR)&ps, sizeof(ps), &cbTransferred, NULL);
}

BOOL DeviceReadCsr(_In_ PDEVICE_DATA pDeviceData, _In_ WORD wRegAddr, _Out_ PDWORD pdwRegValue, _In_ BYTE fCSR)
{
	DWORD cbTransferred;
	PIPE_SEND_CSR_WRITE ps = { .u1 = fCSR | 0xcf, .u2 = 0, .u3 = wRegAddr & 0xff, .u4 = (wRegAddr >> 8) & 0xff, .dwRegValue = 0 };
	if(wRegAddr & 0x03) { return FALSE; } // must be dword aligned
	return
		WinUsb_WritePipe(pDeviceData->WinusbHandle, pDeviceData->PipeCsrOut, (PUCHAR)&ps, sizeof(ps), &cbTransferred, NULL) &&
		WinUsb_ReadPipe(pDeviceData->WinusbHandle, pDeviceData->PipeCsrIn, (PUCHAR)pdwRegValue, 4, &cbTransferred, NULL);
}

BOOL _DeviceReadDMA_Retry(PTHREAD_DATA_READ_EP ptd)
{
	BOOL result;
	DWORD cbTransferred;
	DeviceWriteCsr(ptd->pDeviceData, ptd->pep->rCTL, 0xc2, CSR_CONFIGSPACE_MEMM | CSR_BYTE0); // DMA_ENABLE
	DeviceWriteCsr(ptd->pDeviceData, ptd->pep->rADDR, ptd->dwAddrPci32, CSR_CONFIGSPACE_MEMM | CSR_BYTEALL); // DMA_ADDRESS
	DeviceWriteCsr(ptd->pDeviceData, ptd->pep->rCOUNT, 0x40000000 | ptd->cb, CSR_CONFIGSPACE_MEMM | CSR_BYTEALL); // DMA_COUNT
	DeviceWriteCsr(ptd->pDeviceData, ptd->pep->rSTAT, 0x080000c1, CSR_CONFIGSPACE_MEMM | CSR_BYTE0 | CSR_BYTE3); // DMA_START & DMA_CLEAR_ABORT
	DeviceWriteCsr(ptd->pDeviceData, REGPCI_STATCMD, 0x07, CSR_CONFIGSPACE_PCIE | CSR_BYTE0); // BUS_MASTER ??? needed ???
	result = WinUsb_ReadPipe(ptd->pDeviceData->WinusbHandle, ptd->pep->pipe, ptd->pb, ptd->cb, &cbTransferred, NULL);
	return result;
}

VOID _DeviceReadDMA(PTHREAD_DATA_READ_EP ptd)
{
	DWORD cbTransferred;
	DeviceWriteCsr(ptd->pDeviceData, ptd->pep->rADDR, ptd->dwAddrPci32, CSR_CONFIGSPACE_MEMM | CSR_BYTEALL); // DMA_ADDRESS
	DeviceWriteCsr(ptd->pDeviceData, ptd->pep->rCOUNT, 0x40000000 | ptd->cb, CSR_CONFIGSPACE_MEMM | CSR_BYTEALL); // DMA_COUNT
	DeviceWriteCsr(ptd->pDeviceData, ptd->pep->rSTAT, 0x080000c1, CSR_CONFIGSPACE_MEMM | CSR_BYTE0 | CSR_BYTE3); // DMA_START & DMA_CLEAR_ABORT
	ptd->result = WinUsb_ReadPipe(ptd->pDeviceData->WinusbHandle, ptd->pep->pipe, ptd->pb, ptd->cb, &cbTransferred, NULL);
	if(!ptd->result) {
		ptd->result = _DeviceReadDMA_Retry(ptd);
	}
	ptd->isFinished = TRUE;
}

BOOL DeviceReadDMA(_In_ PDEVICE_DATA pDeviceData, _In_ DWORD dwAddrPci32, _Out_ PBYTE pb, _In_ DWORD cb)
{
	THREAD_DATA_READ_EP td[3];
	DWORD i, dwChunk;
	if(cb % 0x1000) { return FALSE; }
	if(cb > 0x01000000) { return FALSE; }
	if(_DeviceIsInReservedMemoryRange(dwAddrPci32, cb)) { return FALSE; }
	ZeroMemory(td, sizeof(THREAD_DATA_READ_EP) * 3);
	if(cb < 0x00300000 || !pDeviceData->IsAllowedMultiThreadDMA) {
		if(cb > 0x00800000) { // read max 8MB at a time.
			return
				DeviceReadDMA(pDeviceData, dwAddrPci32, pb, 0x00800000) &&
				DeviceReadDMA(pDeviceData, dwAddrPci32 + 0x00800000, pb + 0x00800000, cb - 0x00800000);
		}
		td[0].pDeviceData = pDeviceData;
		td[0].pep = &CEP_INFO[0];
		td[0].dwAddrPci32 = dwAddrPci32;
		td[0].pb = pb;
		td[0].cb = cb;
		_DeviceReadDMA(&td[0]);
		return td[0].result;
	} else {
		dwChunk = (cb / 3) & 0xfffff000;
		for(i = 0; i < 3; i++) {
			td[i].pDeviceData = pDeviceData;
			td[i].pep = &CEP_INFO[i];
			td[i].dwAddrPci32 = dwAddrPci32; dwAddrPci32 += dwChunk;
			td[i].pb = pb; pb += dwChunk;
			if(i == 2) {
				td[i].cb = cb - 2 * dwChunk;
				_DeviceReadDMA(&td[i]);
			}
			else {
				td[i].cb = dwChunk;
				CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)_DeviceReadDMA, &td[i], 0, NULL);
			}
		}
		while(!td[0].isFinished || !td[1].isFinished || !td[2].isFinished) {
			Sleep(0);
		}
		return td[0].result && td[1].result && td[2].result;
	}
}

BOOL DeviceReadDMARetryOnFail(_In_ PDEVICE_DATA pDeviceData, _In_ DWORD dwAddrPci32, _Out_ PBYTE pb, _In_ DWORD cb)
{
	BOOL result = DeviceReadDMA(pDeviceData, dwAddrPci32, pb, cb);
	return result ? TRUE : DeviceReadDMA(pDeviceData, dwAddrPci32, pb, cb);
}

BOOL DeviceWriteDMA_Retry(_In_ PDEVICE_DATA pDeviceData, _In_ DWORD dwAddrPci32, _In_ PBYTE pb, _In_ DWORD cb)
{
	BOOL result;
	DWORD cbTransferred;
	DeviceWriteCsr(pDeviceData, REG_FIFOSTAT_0, 0xffffffff, CSR_CONFIGSPACE_MEMM | CSR_BYTEALL); // USB_FIFO0 FLUSH
	DeviceWriteCsr(pDeviceData, REG_DMACTL_0, 0xc2, CSR_CONFIGSPACE_MEMM | CSR_BYTE0); // DMA_ENABLE
	DeviceWriteCsr(pDeviceData, REG_DMAADDR_0, dwAddrPci32, CSR_CONFIGSPACE_MEMM | CSR_BYTEALL); // DMA_ADDRESS
	DeviceWriteCsr(pDeviceData, REG_DMACOUNT_0, 0x00000000 | cb, CSR_CONFIGSPACE_MEMM | CSR_BYTEALL); // DMA_COUNT
	DeviceWriteCsr(pDeviceData, REG_DMASTAT_0, 0x080000d1, CSR_CONFIGSPACE_MEMM | CSR_BYTE0 | CSR_BYTE3); // DMA_START & DMA_CLEAR_ABORT
	DeviceWriteCsr(pDeviceData, REGPCI_STATCMD, 0x07, CSR_CONFIGSPACE_PCIE | CSR_BYTE0); // BUS_MASTER ??? needed ???
	result = WinUsb_WritePipe(pDeviceData->WinusbHandle, pDeviceData->PipeDmaOut, pb, cb, &cbTransferred, NULL);
	DeviceWriteCsr(pDeviceData, REG_DMASTAT_0, 0x080000d1, CSR_CONFIGSPACE_MEMM | CSR_BYTE0 | CSR_BYTE3); // DMA_START & DMA_CLEAR_ABORT - must be here for 1st transfer to work.
	return result;
}

BOOL DeviceWriteDMA(_In_ PDEVICE_DATA pDeviceData, _In_ DWORD dwAddrPci32, _In_ PBYTE pb, _In_ DWORD cb)
{
	if(cb > 0x00ffffff) { return FALSE; }
	return DeviceWriteDMA_Retry(pDeviceData, dwAddrPci32, pb, cb) || DeviceWriteDMA_Retry(pDeviceData, dwAddrPci32, pb, cb);
}

BOOL DeviceWriteDMAVerify(_In_ PDEVICE_DATA pDeviceData, _In_ DWORD dwAddrPci32, _In_ PBYTE pb, _In_ DWORD cb)
{
	PBYTE pbV;
	BOOL result = DeviceWriteDMA(pDeviceData, dwAddrPci32, pb, cb);
	if(!result) { return FALSE; }
	pbV = LocalAlloc(0, cb);
	if(!pbV) { return FALSE; }
	result = DeviceReadDMA(pDeviceData, dwAddrPci32, pbV, cb);
	result = result & (0 == memcmp(pb, pbV, cb));
	LocalFree(pbV);
	return result;
}

BOOL Device8051Start(_In_ PDEVICE_DATA pDeviceData, _In_ PBYTE pbProgram8051, _In_ DWORD cbProgram8051)
{
	WORD wAddr = 0;
	DWORD dwWriteValue;
	if(!pbProgram8051 || !cbProgram8051 || cbProgram8051 > 0x7FFF) { return FALSE; }
	while(wAddr < cbProgram8051) {
		dwWriteValue = *(DWORD*)(pbProgram8051 + wAddr); // TODO: may read out-of-buffer by max 3 bytes
		DeviceWriteCsr(pDeviceData, wAddr, dwWriteValue, CSR_CONFIGSPACE_8051 | CSR_BYTEALL); // write 8051 program memory (page 253).
		DeviceReadCsr(pDeviceData, wAddr, &dwWriteValue, CSR_CONFIGSPACE_8051);
		wAddr += 4;
	}
	DeviceReadCsr(pDeviceData, 0x00, &dwWriteValue, CSR_CONFIGSPACE_MEMM); // enable 8051
	dwWriteValue &= 0xFE;
	DeviceWriteCsr(pDeviceData, 0x00, dwWriteValue, CSR_CONFIGSPACE_MEMM | CSR_BYTE0); //DEVINIT - START 8051
	return TRUE;
}

VOID Device8051Stop(_In_ PDEVICE_DATA pDeviceData)
{
	DWORD dwWriteValue;
	DeviceReadCsr(pDeviceData, 0x00, &dwWriteValue, CSR_CONFIGSPACE_MEMM);
	dwWriteValue |= 0x01;
	DeviceWriteCsr(pDeviceData, 0x00, dwWriteValue, CSR_CONFIGSPACE_MEMM | CSR_BYTE0);
}

BOOL DeviceFlashEEPROM(_In_ PDEVICE_DATA pDeviceData, _In_ PBYTE pbEEPROM, _In_ DWORD cbEEPROM)
{
	WORD wAddr = 0;
	DWORD dwWriteValue;
	if(cbEEPROM < 3 || cbEEPROM > 0x7FFF) {
		return FALSE; // too small or too large for 2 byte addressing mode
	}
	if(pbEEPROM[0] != 0x5a || (pbEEPROM[1] & 0xf8) != 0x00) {
		return FALSE; // rudimentary signature sanity check
	}
	while(wAddr < cbEEPROM) {
		// initialize EEPROM for writing
		DeviceWriteCsr(pDeviceData, 0x260, 0x0000c000, CSR_CONFIGSPACE_PCIE | CSR_BYTE1); // write enable
		DeviceWriteCsr(pDeviceData, 0x260, 0x00000000, CSR_CONFIGSPACE_PCIE | CSR_BYTE1); // off
																						  // write data
		dwWriteValue = *(DWORD*)(pbEEPROM + wAddr);
		DeviceWriteCsr(pDeviceData, 0x264, dwWriteValue, CSR_CONFIGSPACE_PCIE | CSR_BYTEALL);
		// write control register and wait for action to finish
		dwWriteValue = 0x03004000 | (wAddr >> 2);
		DeviceWriteCsr(pDeviceData, 0x260, dwWriteValue, CSR_CONFIGSPACE_PCIE | CSR_BYTE0 | CSR_BYTE1 | CSR_BYTE3); // write serial EEPROM buffer (page 250).
		while(dwWriteValue & 0xFF000000) { // wait write finish
			DeviceReadCsr(pDeviceData, 0x260, &dwWriteValue, CSR_CONFIGSPACE_PCIE);
		}
		wAddr += 4;
	}
	return TRUE;
}

BOOL DeviceRetrievePath(_Out_bytecap_(BufLen) LPWSTR wszDevicePath, _In_ ULONG BufLen)
{
	BOOL result;
	HDEVINFO deviceInfo;
	SP_DEVICE_INTERFACE_DATA interfaceData;
	PSP_DEVICE_INTERFACE_DETAIL_DATA detailData = NULL;
	ULONG length, requiredLength = 0;
	deviceInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_android, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if(deviceInfo == INVALID_HANDLE_VALUE) {
		return FALSE;
	}
	interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
	result = SetupDiEnumDeviceInterfaces(deviceInfo, NULL, &GUID_DEVINTERFACE_android, 0, &interfaceData);
	if(!result) {
		SetupDiDestroyDeviceInfoList(deviceInfo);
		return FALSE;
	}
	result = SetupDiGetDeviceInterfaceDetail(deviceInfo, &interfaceData, NULL, 0, &requiredLength, NULL);
	if(!result && ERROR_INSUFFICIENT_BUFFER != GetLastError()) {
		SetupDiDestroyDeviceInfoList(deviceInfo);
		return FALSE;
	}
	detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)LocalAlloc(LMEM_FIXED, requiredLength);
	if(!detailData) {
		SetupDiDestroyDeviceInfoList(deviceInfo);
		return FALSE;
	}
	detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
	length = requiredLength;
	result = SetupDiGetDeviceInterfaceDetail(deviceInfo, &interfaceData, detailData, length, &requiredLength, NULL);
	if(!result) {
		LocalFree(detailData);
		SetupDiDestroyDeviceInfoList(deviceInfo);
		return FALSE;
	}
	wcscpy_s(wszDevicePath, BufLen, (LPWSTR)detailData->DevicePath);
	LocalFree(detailData);
	SetupDiDestroyDeviceInfoList(deviceInfo);
	return TRUE;
}

VOID DeviceOpen_SetPipePolicy(_In_ PDEVICE_DATA pDeviceData)
{
	BOOL boolTRUE = TRUE;
	ULONG ulTIMEOUT = 100; // ms
	WinUsb_SetPipePolicy(pDeviceData->WinusbHandle, pDeviceData->PipeDmaOut, AUTO_CLEAR_STALL, (ULONG)sizeof(BOOL), &boolTRUE);
	WinUsb_SetPipePolicy(pDeviceData->WinusbHandle, pDeviceData->PipeDmaOut, PIPE_TRANSFER_TIMEOUT, (ULONG)sizeof(BOOL), &ulTIMEOUT);
	WinUsb_SetPipePolicy(pDeviceData->WinusbHandle, pDeviceData->PipeDmaIn1, AUTO_CLEAR_STALL, (ULONG)sizeof(BOOL), &boolTRUE);
	WinUsb_SetPipePolicy(pDeviceData->WinusbHandle, pDeviceData->PipeDmaIn1, PIPE_TRANSFER_TIMEOUT, (ULONG)sizeof(BOOL), &ulTIMEOUT);
	WinUsb_SetPipePolicy(pDeviceData->WinusbHandle, pDeviceData->PipeDmaIn2, AUTO_CLEAR_STALL, (ULONG)sizeof(BOOL), &boolTRUE);
	WinUsb_SetPipePolicy(pDeviceData->WinusbHandle, pDeviceData->PipeDmaIn2, PIPE_TRANSFER_TIMEOUT, (ULONG)sizeof(BOOL), &ulTIMEOUT);
	WinUsb_SetPipePolicy(pDeviceData->WinusbHandle, pDeviceData->PipeDmaIn3, AUTO_CLEAR_STALL, (ULONG)sizeof(BOOL), &boolTRUE);
	WinUsb_SetPipePolicy(pDeviceData->WinusbHandle, pDeviceData->PipeDmaIn3, PIPE_TRANSFER_TIMEOUT, (ULONG)sizeof(BOOL), &ulTIMEOUT);
}

BOOL DeviceOpen(_Out_ PDEVICE_DATA pDeviceData)
{
	BOOL result;
	pDeviceData->HandlesOpen = FALSE;
	result = DeviceRetrievePath(pDeviceData->DevicePath, MAX_PATH);
	if(!result) {
		return FALSE;
	}
	pDeviceData->DeviceHandle = CreateFile(pDeviceData->DevicePath,
		GENERIC_WRITE | GENERIC_READ,
		FILE_SHARE_WRITE | FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		NULL);
	if(INVALID_HANDLE_VALUE == pDeviceData->DeviceHandle) {
		return FALSE;
	}
	result = WinUsb_Initialize(pDeviceData->DeviceHandle, &pDeviceData->WinusbHandle);
	if(!result) {
		CloseHandle(pDeviceData->DeviceHandle);
		return FALSE;
	}
	pDeviceData->PipePciIn = 0x8e;	// PCI in endpoint on the USB3380
	pDeviceData->PipePciOut = 0x0e;	// PCI out endpoint on the USB3380
	pDeviceData->PipeCsrIn = 0x8d;	// CSR in endpoint on the USB3380
	pDeviceData->PipeCsrOut = 0x0d;	// CSR out endpoint on the USB3380
	pDeviceData->PipeDmaOut = 0x02;	// GPEP0 endpoint on the USB3380
	pDeviceData->PipeDmaIn1 = 0x84;	// GPEP1 endpoint on the USB3380
	pDeviceData->PipeDmaIn2 = 0x86;	// GPEP2 endpoint on the USB3380
	pDeviceData->PipeDmaIn3 = 0x88;	// GPEP3 endpoint on the USB3380
	pDeviceData->KMDHandle = NULL;
	DeviceOpen_SetPipePolicy(pDeviceData);
	pDeviceData->HandlesOpen = TRUE;
	pDeviceData->IsAllowedMultiThreadDMA = IsWindows8OrGreater(); // multi threaded DMA read fails on WIN7.
	return TRUE;
}

VOID DeviceClose(_Inout_ PDEVICE_DATA pDeviceData)
{
	if(!pDeviceData->HandlesOpen) {
		return;
	}
	WinUsb_Free(pDeviceData->WinusbHandle);
	CloseHandle(pDeviceData->DeviceHandle);
	pDeviceData->HandlesOpen = FALSE;
}

BOOL DeviceWriteMEM(_In_ PDEVICE_DATA pDeviceData, _In_ QWORD qwAddr, _In_ PBYTE pb, _In_ DWORD cb)
{
	if(pDeviceData->KMDHandle) {
		return KMDWriteMemory(pDeviceData, qwAddr, pb, cb);
	} else if(qwAddr + cb > 0xffffffff) {
		return FALSE;
	} else {
		return DeviceWriteDMA(pDeviceData, (DWORD)qwAddr, pb, cb);
	}
}

BOOL DeviceReadMEM(_In_ PDEVICE_DATA pDeviceData, _In_ QWORD qwAddr, _Out_ PBYTE pb, _In_ DWORD cb)
{
	if(pDeviceData->KMDHandle) {
		return KMDReadMemory(pDeviceData, qwAddr, pb, cb);
	} else if(qwAddr + cb > 0xffffffff) {
		return FALSE;
	} else {
		return DeviceReadDMA(pDeviceData, (DWORD)qwAddr, pb, cb);
	}
}