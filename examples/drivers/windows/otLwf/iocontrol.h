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

#ifndef _IOCONTROL_H
#define _IOCONTROL_H

//
// Function prototype for general Io Control functions
//
typedef 
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
OTLWF_IOCTL_FUNC(
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    );

//
// General Io Control Functions
//

// Handles queries for the current list of Thread interfaces
OTLWF_IOCTL_FUNC otLwfIoCtlEnumerateInterfaces;

// Handles queries for the details of a specific Thread interface
OTLWF_IOCTL_FUNC otLwfIoCtlQueryInterface;

// Handles IOTCLs for OpenThread control
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
otLwfIoCtlOpenThreadControl(
    _In_ PIRP Irp
    );

//
// Function prototype for OpenThread Io Control functions
//
typedef 
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
OTLWF_OT_IOCTL_FUNC(
    _In_ PMS_FILTER         pFilter,
    _In_reads_bytes_(InBufferLength)
            PVOID           InBuffer,
    _In_    ULONG           InBufferLength,
    _Out_writes_bytes_opt_(*OutBufferLength)
            PVOID           OutBuffer,
    _Inout_ PULONG          OutBufferLength
    );

OTLWF_OT_IOCTL_FUNC otLwfIoCtl_otEnabled;
OTLWF_OT_IOCTL_FUNC otLwfIoCtl_otInterface;
OTLWF_OT_IOCTL_FUNC otLwfIoCtl_otThread;

OTLWF_OT_IOCTL_FUNC otLwfIoCtl_otLinkMode;

OTLWF_OT_IOCTL_FUNC otLwfIoCtl_otMeshLocalEid;

OTLWF_OT_IOCTL_FUNC otLwfIoCtl_otDeviceRole;

#endif // _IOCONTROL_H
