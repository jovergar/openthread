/*
 *  Copyright (c) 2016, Microsoft Corporation.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "precomp.h"
#include "iocontrol.tmh"

// Handles queries for the current list of Thread interfaces
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtlEnumerateInterfaces(
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG NewOutBufferLength = 0;
    POTLWF_INTERFACE_LIST pInterfaceList = (POTLWF_INTERFACE_LIST)OutBuffer;
    
    UNREFERENCED_PARAMETER(InBuffer);
    UNREFERENCED_PARAMETER(InBufferLength);

    LogFuncEntry(DRIVER_IOCTL);

    // Make sure to zero out the output first
    RtlZeroMemory(OutBuffer, *OutBufferLength);

    NdisAcquireSpinLock(&FilterListLock);

    // Make sure there is enough space for the first uint16_t
    if (*OutBufferLength < sizeof(uint16_t))
    {
        status = STATUS_BUFFER_TOO_SMALL;
        goto error;
    }

	// Iterate through each interface and build up the list of running interfaces
    for (PLIST_ENTRY Link = FilterModuleList.Flink; Link != &FilterModuleList; Link = Link->Flink)
    {
        PMS_FILTER pFilter = CONTAINING_RECORD(Link, MS_FILTER, FilterModuleLink);
        if (pFilter->State != FilterRunning) continue;

        PGUID pInterfaceGuid = &pInterfaceList->InterfaceGuids[pInterfaceList->cInterfaceGuids];
        pInterfaceList->cInterfaceGuids++;

        NewOutBufferLength =
            FIELD_OFFSET(OTLWF_INTERFACE_LIST, InterfaceGuids) +
            pInterfaceList->cInterfaceGuids * sizeof(GUID);

        if (NewOutBufferLength <= *OutBufferLength)
        {
            *pInterfaceGuid = pFilter->InterfaceGuid;
        }
    }

    if (NewOutBufferLength > *OutBufferLength)
    {
        NewOutBufferLength = sizeof(USHORT);
    }

error:

    NdisReleaseSpinLock(&FilterListLock);

    *OutBufferLength = NewOutBufferLength;

    LogFuncExitNT(DRIVER_IOCTL, status);

    return status;
}

// Handles queries for the details of a specific Thread interface
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtlQueryInterface(
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG    NewOutBufferLength = 0;

    LogFuncEntry(DRIVER_IOCTL);

    // Make sure there is enough space for the first USHORT
    if (InBufferLength < sizeof(GUID) || *OutBufferLength < sizeof(OTLWF_DEVICE))
    {
        status = STATUS_BUFFER_TOO_SMALL;
        goto error;
    }
    
    PGUID pInterfaceGuid = (PGUID)InBuffer;
    POTLWF_DEVICE pDevice = (POTLWF_DEVICE)OutBuffer;

	// Look up the interface
    PMS_FILTER pFilter = otLwfFindAndRefInterface(pInterfaceGuid);
    if (pFilter == NULL)
    {
        status = STATUS_DEVICE_DOES_NOT_EXIST;
        goto error;
    }

    NewOutBufferLength = sizeof(OTLWF_DEVICE);
    pDevice->CompartmentID = pFilter->InterfaceCompartmentID;

	// Release the ref on the interface
	otLwfReleaseInterface(pFilter);

error:

    if (NewOutBufferLength < *OutBufferLength)
    {
        RtlZeroMemory((PUCHAR)OutBuffer + NewOutBufferLength, *OutBufferLength - NewOutBufferLength);
    }

    *OutBufferLength = NewOutBufferLength;

    LogFuncExitNT(DRIVER_IOCTL, status);

    return status;
}

// Handles IOTCLs for OpenThread control
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtlOpenThreadControl(
    _In_ PIRP Irp
    )
{
    NTSTATUS   status = STATUS_PENDING;
    PMS_FILTER pFilter = NULL;

    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    LogFuncEntry(DRIVER_IOCTL);

    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(GUID))
    {
        status = STATUS_INVALID_PARAMETER;
        goto error;
    }

    pFilter = otLwfFindAndRefInterface((PGUID)Irp->AssociatedIrp.SystemBuffer);
    if (pFilter == NULL)
    {
        status = STATUS_DEVICE_DOES_NOT_EXIST;
        goto error;
    }

    // Pend the Irp for processing on the OpenThread event processing thread
    otLwfEventProcessingQueueIrp(pFilter, Irp);

    // Release our ref on the filter
    otLwfReleaseInterface(pFilter);

error:

    // Complete the IRP if we aren't pending (indicates we failed)
    if (status != STATUS_PENDING)
    {
        NT_ASSERT(status != STATUS_SUCCESS);
        RtlZeroMemory(Irp->AssociatedIrp.SystemBuffer, IrpSp->Parameters.DeviceIoControl.OutputBufferLength);
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    LogFuncExitNT(DRIVER_IOCTL, status);

    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtl_otEnabled(
    _In_ PMS_FILTER         pFilter,
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    
    *OutBufferLength = 0;
    UNREFERENCED_PARAMETER(OutBuffer);

    if (InBufferLength >= sizeof(BOOLEAN))
    {
        BOOLEAN IsEnabled = *(BOOLEAN*)InBuffer;
        if (IsEnabled)
        {
            status = ThreadErrorToNtstatus(otEnable(pFilter->otCtx));
        }
        else
        {
            status = ThreadErrorToNtstatus(otDisable(pFilter->otCtx));
        }
    }

    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtl_otInterface(
    _In_ PMS_FILTER         pFilter,
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    if (InBufferLength >= sizeof(BOOLEAN))
    {
        BOOLEAN IsEnabled = *(BOOLEAN*)InBuffer;
        if (IsEnabled)
        {
            status = ThreadErrorToNtstatus(otInterfaceUp(pFilter->otCtx));
        }
        else
        {
            status = ThreadErrorToNtstatus(otInterfaceDown(pFilter->otCtx));
        }
        
        *OutBufferLength = 0;
    }
    else if (*OutBufferLength >= sizeof(BOOLEAN))
    {
        *(BOOLEAN*)OutBuffer = otIsInterfaceUp(pFilter->otCtx) ? TRUE : FALSE;
        status = STATUS_SUCCESS;
    }
    else
    {
        *OutBufferLength = 0;
    }

    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtl_otThread(
    _In_ PMS_FILTER         pFilter,
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    
    *OutBufferLength = 0;
    UNREFERENCED_PARAMETER(OutBuffer);

    if (InBufferLength >= sizeof(BOOLEAN))
    {
        BOOLEAN IsEnabled = *(BOOLEAN*)InBuffer;
        if (IsEnabled)
        {
            status = ThreadErrorToNtstatus(otThreadStart(pFilter->otCtx));
        }
        else
        {
            status = ThreadErrorToNtstatus(otThreadStop(pFilter->otCtx));
        }
    }

    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtl_otLinkMode(
    _In_ PMS_FILTER         pFilter,
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
	
	/*
		For some reason (sizeof(otLinkModeConfig) == 4) in C, but not C++,
		so this code needs to do a bit of manual labor to get the one byte
		into the otLinkModeConfig struct.
	*/
	static_assert(sizeof(otLinkModeConfig) == 4, "The size of otLinkModeConfig should be 4 bytes");
	if (InBufferLength >= sizeof(uint8_t))
	{
		otLinkModeConfig Config = {0};
		memcpy(&Config, InBuffer, sizeof(uint8_t));
		status = ThreadErrorToNtstatus(
					otSetLinkMode(pFilter->otCtx, Config)
					);
		*OutBufferLength = 0;
	}
	else if (*OutBufferLength >= sizeof(uint8_t))
	{
		otLinkModeConfig Config = otGetLinkMode(pFilter->otCtx);
		memcpy(OutBuffer, &Config, sizeof(uint8_t));
		status = STATUS_SUCCESS;
	}
	else
	{
		*OutBufferLength = 0;
	}

    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtl_otMeshLocalEid(
    _In_ PMS_FILTER         pFilter,
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
	
    UNREFERENCED_PARAMETER(InBuffer);
    UNREFERENCED_PARAMETER(InBufferLength);

    if (*OutBufferLength >= sizeof(otIp6Address))
    {
		memcpy(OutBuffer,  otGetMeshLocalEid(pFilter->otCtx), sizeof(otIp6Address));
        status = STATUS_SUCCESS;
    }
    else
    {
        *OutBufferLength = 0;
    }

    return status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtl_otDeviceRole(
    _In_ PMS_FILTER         pFilter,
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
	
    if (InBufferLength >= sizeof(uint8_t))
    {
		otDeviceRole role = *(uint8_t*)InBuffer;

		InBufferLength -= sizeof(uint8_t);
		InBuffer = (PUCHAR)InBuffer + sizeof(uint8_t);

		if (role == kDeviceRoleLeader)
		{
			status = ThreadErrorToNtstatus(
						otBecomeLeader(pFilter->otCtx)
						);
		}
		else if (role == kDeviceRoleRouter)
		{
			status = ThreadErrorToNtstatus(
						otBecomeRouter(pFilter->otCtx)
						);
		}
		else if (role == kDeviceRoleChild)
		{
			if (InBufferLength >= sizeof(uint8_t))
			{
				status = ThreadErrorToNtstatus(
							otBecomeChild(pFilter->otCtx, *(uint8_t*)InBuffer)
							);
			}
		}
		else if (role == kDeviceRoleDetached)
		{
			status = ThreadErrorToNtstatus(
						otBecomeDetached(pFilter->otCtx)
						);
		}
        *OutBufferLength = 0;
    }
    else if (*OutBufferLength >= sizeof(uint8_t))
    {
        *(uint8_t*)OutBuffer = otGetDeviceRole(pFilter->otCtx);
        status = STATUS_SUCCESS;
    }
    else
    {
        *OutBufferLength = 0;
    }

    return status;
}
