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
#include "otApi.tmh"

//#define DEBUG_ASYNC_IO

using namespace std;

// The maximum time we will wait for an overlapped result. Essentially, the maximum
// amount of time each synchronous IOCTL should take.
const DWORD c_MaxOverlappedWaitTimeMS = 10 * 1000;

typedef tuple<otDeviceAvailabilityChangedCallback,PVOID> otApiDeviceAvailabilityCallback;
typedef tuple<GUID,otHandleActiveScanResult,PVOID> otApiActiveScanCallback;
typedef tuple<GUID,otStateChangedCallback,PVOID> otApiStateChangeCallback;

typedef struct otApiContext
{
    // Handle to the driver
    HANDLE                      DeviceHandle;

    // Async IO variables
    OVERLAPPED                  Overlapped;
    PTP_WAIT                    ThreadpoolWait;

    // Notification variables
    CRITICAL_SECTION            CallbackLock;
    RTL_REFERENCE_COUNT         CallbackRefCount;
    HANDLE                      CallbackCompleteEvent;
    OTLWF_NOTIFICATION          NotificationBuffer;

    // Callbacks
    otApiDeviceAvailabilityCallback    DeviceAvailabilityCallbacks;
    vector<otApiActiveScanCallback>    ActiveScanCallbacks;
    vector<otApiActiveScanCallback>    DiscoverCallbacks;
    vector<otApiStateChangeCallback>   StateChangedCallbacks;

    // Constructor
    otApiContext() : 
        DeviceHandle(INVALID_HANDLE_VALUE),
        Overlapped({0}),
        ThreadpoolWait(nullptr),
        CallbackRefCount(0),
        CallbackCompleteEvent(nullptr),
        DeviceAvailabilityCallbacks((otDeviceAvailabilityChangedCallback)nullptr, (PVOID)nullptr)
    { 
        InitializeCriticalSection(&CallbackLock);
		RtlInitializeReferenceCount(&CallbackRefCount);
    }

    ~otApiContext()
    {
        DeleteCriticalSection(&CallbackLock);
    }

    // Helper function to set a callback
    template<class CallbackT>
    bool SetCallback(vector<tuple<GUID,CallbackT,PVOID>> &Callbacks, const tuple<GUID,CallbackT,PVOID>& Callback)
    {
        bool alreadyExists = false;
        bool releaseRef = false;

        EnterCriticalSection(&CallbackLock);

        if (get<1>(Callback) == nullptr)
        {
            for (size_t i = 0; i < Callbacks.size(); i++)
            {
                if (get<0>(Callbacks[i]) == get<0>(Callback))
                {
                    Callbacks.erase(Callbacks.begin() + i);
                    releaseRef = true;
                    break;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < Callbacks.size(); i++)
            {
                if (get<0>(Callbacks[i]) == get<0>(Callback))
                {
                    alreadyExists = true;
                    break;
                }
            }

            if (!alreadyExists)
            {
                RtlIncrementReferenceCount(&CallbackRefCount);
                Callbacks.push_back(Callback);
            }
        }

        LeaveCriticalSection(&CallbackLock);

        if (releaseRef)
        {
            if (RtlDecrementReferenceCount(&CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(CallbackCompleteEvent);
            }
        }

        return !alreadyExists;
    }

} otApiContext;

typedef struct otContext
{
    otApiContext    *ApiHandle;      // Pointer to the Api handle
    GUID             InterfaceGuid;  // Interface guid
    ULONG            CompartmentID;  // Interface Compartment ID

} otContext;

// otpool wait callback for async IO completion
VOID CALLBACK 
otIoComplete(
    _Inout_     PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID                 Context,
    _Inout_     PTP_WAIT              Wait,
    _In_        TP_WAIT_RESULT        WaitResult
    );

OTAPI 
otApiContext *
otApiInit(
    )
{
    DWORD dwError = ERROR_SUCCESS;
    otApiContext *aApiContext = nullptr;
    
    otLogFuncEntry();

    aApiContext = new(std::nothrow)otApiContext();
    if (aApiContext == nullptr)
    {
        dwError = GetLastError();
        otLogWarnApi("Failed to allocate otApiContext");
        goto error;
    }

    // Open the pipe to the OpenThread driver
    aApiContext->DeviceHandle = 
        CreateFile(
            OTLWF_IOCLT_PATH,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,                   // no SECURITY_ATTRIBUTES structure
            OPEN_EXISTING,          // No special create flags
            FILE_FLAG_OVERLAPPED,   // Allow asynchronous requests
            nullptr
            );
    if (aApiContext->DeviceHandle == INVALID_HANDLE_VALUE)
    {
        dwError = GetLastError();
        otLogCritApi("CreateFile failed, %!WINERROR!", dwError);
        goto error;
    }

    // Create event for completion of callback cleanup
    aApiContext->CallbackCompleteEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (aApiContext->CallbackCompleteEvent == nullptr)
    {
        dwError = GetLastError();
        otLogCritApi("CreateEvent (CallbackCompleteEvent) failed, %!WINERROR!", dwError);
        goto error;
    }

    // Create event for completion of async IO
    aApiContext->Overlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (aApiContext->Overlapped.hEvent == nullptr)
    {
        dwError = GetLastError();
        otLogCritApi("CreateEvent (Overlapped.hEvent) failed, %!WINERROR!", dwError);
        goto error;
    }

    // Create the otpool wait
    aApiContext->ThreadpoolWait = 
        CreateThreadpoolWait(
            otIoComplete,
            aApiContext,
            nullptr
            );
    if (aApiContext->ThreadpoolWait == nullptr)
    {
        dwError = GetLastError();
        otLogCritApi("CreateThreadpoolWait failed, %!WINERROR!", dwError);
        goto error;
    }

    // Start the otpool waiting on the overlapped event
    SetThreadpoolWait(aApiContext->ThreadpoolWait, aApiContext->Overlapped.hEvent, nullptr);

#ifdef DEBUG_ASYNC_IO
    otLogDebgApi("Querying for 1st notification");
#endif

    // Request first notification asynchronously
    if (!DeviceIoControl(
            aApiContext->DeviceHandle,
            IOCTL_OTLWF_QUERY_NOTIFICATION,
            nullptr, 0,
            &aApiContext->NotificationBuffer, sizeof(OTLWF_NOTIFICATION),
            nullptr, 
            &aApiContext->Overlapped))
    {
        dwError = GetLastError();
        if (dwError != ERROR_IO_PENDING)
        {
            otLogCritApi("DeviceIoControl for first notification failed, %!WINERROR!", dwError);
            goto error;
        }
        dwError = ERROR_SUCCESS;
    }

error:

    if (dwError != ERROR_SUCCESS)
    {
        otApiUninit(aApiContext);
        aApiContext = nullptr;
    }
    
    otLogFuncExit();

    return aApiContext;
}

OTAPI 
void 
otApiUninit(
    _In_ otApiContext *aApiContext
)
{
    if (aApiContext == nullptr) return;
    
    otLogFuncEntry();

	// If we never got the handle, nothing left to clean up
	if (aApiContext->DeviceHandle == INVALID_HANDLE_VALUE) goto exit;

    //
    // Make sure we unregister callbacks
    //

    EnterCriticalSection(&aApiContext->CallbackLock);

    // Clear all callbacks
    if (get<0>(aApiContext->DeviceAvailabilityCallbacks))
    {
        aApiContext->DeviceAvailabilityCallbacks = make_tuple((otDeviceAvailabilityChangedCallback)nullptr, (PVOID)nullptr);
        RtlDecrementReferenceCount(&aApiContext->CallbackRefCount);
    }
    for (size_t i = 0; i < aApiContext->ActiveScanCallbacks.size(); i++)
    {
        RtlDecrementReferenceCount(&aApiContext->CallbackRefCount);
    }
    for (size_t i = 0; i < aApiContext->DiscoverCallbacks.size(); i++)
    {
        RtlDecrementReferenceCount(&aApiContext->CallbackRefCount);
    }
    for (size_t i = 0; i < aApiContext->StateChangedCallbacks.size(); i++)
    {
        RtlDecrementReferenceCount(&aApiContext->CallbackRefCount);
    }
    aApiContext->ActiveScanCallbacks.clear();
    aApiContext->DiscoverCallbacks.clear();
    aApiContext->StateChangedCallbacks.clear();

#ifdef DEBUG_ASYNC_IO
    otLogDebgApi("Clearing Threadpool Wait");
#endif

    // Clear the threadpool wait to prevent further waits from being scheduled
    PTP_WAIT tpWait = aApiContext->ThreadpoolWait;
    aApiContext->ThreadpoolWait = nullptr;

    LeaveCriticalSection(&aApiContext->CallbackLock);
    
    // Release last ref and wait for any pending callback to complete, if necessary
    if (!RtlDecrementReferenceCount(&aApiContext->CallbackRefCount))
    {
        WaitForSingleObject(aApiContext->CallbackCompleteEvent, INFINITE);
    }
        
    // Clean up threadpool wait
    if (tpWait)
    {
#ifdef DEBUG_ASYNC_IO
        otLogDebgApi("Waiting for outstanding threadpool callbacks to compelte");
#endif

        // Cancel any queued waits and wait for any outstanding calls to compelte
        WaitForThreadpoolWaitCallbacks(tpWait, TRUE);
        
#ifdef DEBUG_ASYNC_IO
        otLogDebgApi("Cancelling any pending IO");
#endif

        // Cancel any async IO
        CancelIoEx(aApiContext->DeviceHandle, &aApiContext->Overlapped);

        // Free the threadpool wait
        CloseThreadpoolWait(tpWait);
    }

    // Clean up overlapped event
    if (aApiContext->Overlapped.hEvent)
    {
        CloseHandle(aApiContext->Overlapped.hEvent);
    }

    // Clean up callback complete event
    if (aApiContext->CallbackCompleteEvent)
    {
        CloseHandle(aApiContext->CallbackCompleteEvent);
    }
	
	// Close the device handle
    CloseHandle(aApiContext->DeviceHandle);

exit:

    delete aApiContext;
    
    otLogFuncExit();
}

OTAPI 
void 
otFreeMemory(
    _In_ void *mem
    )
{
    free(mem);
}

// Handles cleanly invoking the register callback
VOID
ProcessNotification(
    _In_ otApiContext          *aApiContext,
    _In_ POTLWF_NOTIFICATION    Notif
    )
{
    if (Notif->NotifType == OTLWF_NOTIF_DEVICE_AVAILABILITY)
    {
        otDeviceAvailabilityChangedCallback Callback = nullptr;
        PVOID                               CallbackContext = nullptr;
        
        EnterCriticalSection(&aApiContext->CallbackLock);

        if (get<0>(aApiContext->DeviceAvailabilityCallbacks) != nullptr)
        {
            // Add Ref
            RtlIncrementReferenceCount(&aApiContext->CallbackRefCount);

            // Set callback
            Callback = get<0>(aApiContext->DeviceAvailabilityCallbacks);
            CallbackContext = get<1>(aApiContext->DeviceAvailabilityCallbacks);
        }

        LeaveCriticalSection(&aApiContext->CallbackLock);

        // Invoke the callback outside the lock
        if (Callback)
        {
            Callback(Notif->DeviceAvailabilityPayload.Available != FALSE, &Notif->InterfaceGuid, CallbackContext);

            // Release ref
            if (RtlDecrementReferenceCount(&aApiContext->CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(aApiContext->CallbackCompleteEvent);
            }
        }
    }
    else if (Notif->NotifType == OTLWF_NOTIF_STATE_CHANGE)
    {
        otStateChangedCallback  Callback = nullptr;
        PVOID                   CallbackContext = nullptr;

        EnterCriticalSection(&aApiContext->CallbackLock);

        // Set the callback
        for (size_t i = 0; i < aApiContext->StateChangedCallbacks.size(); i++)
        {
            if (get<0>(aApiContext->StateChangedCallbacks[i]) == Notif->InterfaceGuid)
            {
                // Add Ref
                RtlIncrementReferenceCount(&aApiContext->CallbackRefCount);

                // Set callback
                Callback = get<1>(aApiContext->StateChangedCallbacks[i]);
                CallbackContext = get<2>(aApiContext->StateChangedCallbacks[i]);
                break;
            }
        }

        LeaveCriticalSection(&aApiContext->CallbackLock);

        // Invoke the callback outside the lock
        if (Callback)
        {
            Callback(Notif->StateChangePayload.Flags, CallbackContext);

            // Release ref
            if (RtlDecrementReferenceCount(&aApiContext->CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(aApiContext->CallbackCompleteEvent);
            }
        }
    }
    else if (Notif->NotifType == OTLWF_NOTIF_DISCOVER)
    {
        Notif->DiscoverPayload.Results.mExtendedPanId = Notif->DiscoverPayload.ExtendedPanId;
        Notif->DiscoverPayload.Results.mNetworkName = Notif->DiscoverPayload.NetworkName;

        otHandleActiveScanResult Callback = nullptr;

        EnterCriticalSection(&aApiContext->CallbackLock);

        // Set the callback
        for (size_t i = 0; i < aApiContext->DiscoverCallbacks.size(); i++)
        {
            if (get<0>(aApiContext->DiscoverCallbacks[i]) == Notif->InterfaceGuid)
            {
                // Add Ref
                RtlIncrementReferenceCount(&aApiContext->CallbackRefCount);

                // Set callback
                Callback = get<1>(aApiContext->DiscoverCallbacks[i]);
                break;
            }
        }

        LeaveCriticalSection(&aApiContext->CallbackLock);

        // Invoke the callback outside the lock
        if (Callback)
        {
            Callback(&Notif->DiscoverPayload.Results);

            // Release ref
            if (RtlDecrementReferenceCount(&aApiContext->CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(aApiContext->CallbackCompleteEvent);
            }
        }
    }
    else if (Notif->NotifType == OTLWF_NOTIF_ACTIVE_SCAN)
    {
        Notif->ActiveScanPayload.Results.mExtendedPanId = Notif->ActiveScanPayload.ExtendedPanId;
        Notif->ActiveScanPayload.Results.mNetworkName = Notif->ActiveScanPayload.NetworkName;

        otHandleActiveScanResult Callback = nullptr;

        EnterCriticalSection(&aApiContext->CallbackLock);

        // Set the callback
        for (size_t i = 0; i < aApiContext->ActiveScanCallbacks.size(); i++)
        {
            if (get<0>(aApiContext->ActiveScanCallbacks[i]) == Notif->InterfaceGuid)
            {
                // Add Ref
                RtlIncrementReferenceCount(&aApiContext->CallbackRefCount);

                // Set callback
                Callback = get<1>(aApiContext->ActiveScanCallbacks[i]);
                break;
            }
        }

        LeaveCriticalSection(&aApiContext->CallbackLock);

        // Invoke the callback outside the lock
        if (Callback)
        {
            Callback(&Notif->ActiveScanPayload.Results);

            // Release ref
            if (RtlDecrementReferenceCount(&aApiContext->CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(aApiContext->CallbackCompleteEvent);
            }
        }
    }
    else
    {
        // Unexpected notif type
    }
}

// Threadpool wait callback for async IO completion
VOID CALLBACK 
otIoComplete(
    _Inout_     PTP_CALLBACK_INSTANCE /* Instance */,
    _Inout_opt_ PVOID                 Context,
    _Inout_     PTP_WAIT              /* Wait */,
    _In_        TP_WAIT_RESULT        /* WaitResult */
    )
{
#ifdef DEBUG_ASYNC_IO
    otLogFuncEntry();
#endif

    otApiContext *aApiContext = (otApiContext*)Context;
    if (aApiContext == nullptr) return;

    // Get the result of the IO operation
    DWORD dwBytesTransferred = 0;
    if (!GetOverlappedResult(
            aApiContext->DeviceHandle,
            &aApiContext->Overlapped,
            &dwBytesTransferred,
            FALSE))
    {
        DWORD dwError = GetLastError();
        otLogCritApi("GetOverlappedResult for notification failed, %!WINERROR!", dwError);
    }
    else
    {
        otLogDebgApi("Received successful callback for notification, type=%d", 
                     aApiContext->NotificationBuffer.NotifType);

        // Invoke the callback if set
        ProcessNotification(aApiContext, &aApiContext->NotificationBuffer);
            
        // Try to get the threadpool wait to see if we are allowed to continue processing notifications
        EnterCriticalSection(&aApiContext->CallbackLock);
        PTP_WAIT tpWait = aApiContext->ThreadpoolWait;
        LeaveCriticalSection(&aApiContext->CallbackLock);

        if (tpWait)
        {
            // Start waiting for next notification
            SetThreadpoolWait(tpWait, aApiContext->Overlapped.hEvent, nullptr);
            
#ifdef DEBUG_ASYNC_IO
            otLogDebgApi("Querying for next notification");
#endif

            // Request next notification
            if (!DeviceIoControl(
                    aApiContext->DeviceHandle,
                    IOCTL_OTLWF_QUERY_NOTIFICATION,
                    nullptr, 0,
                    &aApiContext->NotificationBuffer, sizeof(OTLWF_NOTIFICATION),
                    nullptr, 
                    &aApiContext->Overlapped))
            {
                DWORD dwError = GetLastError();
                if (dwError != ERROR_IO_PENDING)
                {
                    otLogCritApi("DeviceIoControl for new notification failed, %!WINERROR!", dwError);
                }
            }
        }
    }
    
#ifdef DEBUG_ASYNC_IO
    otLogFuncExit();
#endif
}

DWORD
SendIOCTL(
    _In_ otApiContext *aApiContext,
    _In_ DWORD dwIoControlCode,
    _In_reads_bytes_opt_(nInBufferSize) LPVOID lpInBuffer,
    _In_ DWORD nInBufferSize,
    _Out_writes_bytes_to_opt_(nOutBufferSize, *lpBytesReturned) LPVOID lpOutBuffer,
    _In_ DWORD nOutBufferSize
    )
{
    DWORD dwError = ERROR_SUCCESS;
    OVERLAPPED Overlapped = { 0 };
    DWORD dwBytesReturned = 0;
    
    Overlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (Overlapped.hEvent == nullptr)
    {
        dwError = GetLastError();
        otLogCritApi("CreateEvent (Overlapped.hEvent) failed, %!WINERROR!", dwError);
        goto error;
    }
    
    // Send the IOCTL the OpenThread driver
    if (!DeviceIoControl(
            aApiContext->DeviceHandle,
            dwIoControlCode,
            lpInBuffer, nInBufferSize,
            lpOutBuffer, nOutBufferSize,
            nullptr, 
            &Overlapped))
    {
        dwError = GetLastError();
        if (dwError != ERROR_IO_PENDING)
        {
            otLogCritApi("DeviceIoControl(0x%x) failed, %!WINERROR!", dwIoControlCode, dwError);
            goto error;
        }
        dwError = ERROR_SUCCESS;
    }

    // Get the result of the IO operation
    if (!GetOverlappedResultEx(
            aApiContext->DeviceHandle,
            &Overlapped,
            &dwBytesReturned,
            c_MaxOverlappedWaitTimeMS,
            TRUE
            ))
    {
        dwError = GetLastError();
        if (dwError == WAIT_TIMEOUT)
        {
            dwError = ERROR_TIMEOUT;
            CancelIoEx(aApiContext->DeviceHandle, &Overlapped);
        }
        otLogCritApi("GetOverlappedResult failed, %!WINERROR!", dwError);
        goto error;
    }

	if (dwBytesReturned != nOutBufferSize)
	{
		dwError = ERROR_INVALID_DATA;
        otLogCritApi("GetOverlappedResult returned invalid output size, expected=%u actual=%u", 
			         nOutBufferSize, dwBytesReturned);
		goto error;
	}

error:

    if (Overlapped.hEvent)
    {
        CloseHandle(Overlapped.hEvent);
    }

    return dwError;
}

template <class in, class out>
DWORD
QueryIOCTL(
    _In_ otContext *aContext,
    _In_ DWORD dwIoControlCode,
    _In_ const in *input,
    _Out_ out* output
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(in)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), input, sizeof(in));
    return SendIOCTL(aContext->ApiHandle, dwIoControlCode, Buffer, sizeof(Buffer), output, sizeof(out));
}

template <class out>
DWORD
QueryIOCTL(
    _In_ otContext *aContext,
    _In_ DWORD dwIoControlCode,
    _Out_ out* output
    )
{
    return SendIOCTL(aContext->ApiHandle, dwIoControlCode, &aContext->InterfaceGuid, sizeof(GUID), output, sizeof(out));
}

template <class in>
DWORD
SetIOCTL(
    _In_ otContext *aContext,
    _In_ DWORD dwIoControlCode,
    _In_ const in* input
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(in)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), input, sizeof(in));
    return SendIOCTL(aContext->ApiHandle, dwIoControlCode, Buffer, sizeof(Buffer), nullptr, 0);
}

template <class in>
DWORD
SetIOCTL(
    _In_ otContext *aContext,
    _In_ DWORD dwIoControlCode,
    _In_ const in input
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(in)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), &input, sizeof(in));
    return SendIOCTL(aContext->ApiHandle, dwIoControlCode, Buffer, sizeof(Buffer), nullptr, 0);
}

DWORD
SetIOCTL(
    _In_ otContext *aContext,
    _In_ DWORD dwIoControlCode
    )
{
    return SendIOCTL(aContext->ApiHandle, dwIoControlCode, &aContext->InterfaceGuid, sizeof(GUID), nullptr, 0);
}

ThreadError
DwordToThreadError(
    DWORD dwError
    )
{
    if (((int)dwError) > 0)
    {
        return kThreadError_Error;
    }
    else
    {
        return (ThreadError)(-(int)dwError);
    }
}

OTAPI 
void 
otSetDeviceAvailabilityChangedCallback(
    _In_ otApiContext *aApiContext,
    _In_ otDeviceAvailabilityChangedCallback aCallback,
    _In_ void *aCallbackContext
    )
{
    bool releaseRef = false;

    EnterCriticalSection(&aApiContext->CallbackLock);

    if (aCallback == nullptr)
    {
        if (get<0>(aApiContext->DeviceAvailabilityCallbacks) != nullptr)
        {
            releaseRef = true;
            aApiContext->DeviceAvailabilityCallbacks = make_tuple(aCallback, aCallbackContext);
        }
    }
    else
    {
        if (get<0>(aApiContext->DeviceAvailabilityCallbacks) == nullptr)
        {
            RtlIncrementReferenceCount(&aApiContext->CallbackRefCount);
            aApiContext->DeviceAvailabilityCallbacks = make_tuple(aCallback, aCallbackContext);
        }
    }
    
    LeaveCriticalSection(&aApiContext->CallbackLock);

    if (releaseRef)
    {
        if (RtlDecrementReferenceCount(&aApiContext->CallbackRefCount))
        {
            // Set completion event if there are no more refs
            SetEvent(aApiContext->CallbackCompleteEvent);
        }
    }
}

OTAPI 
otDeviceList* 
otEnumerateDevices(
    _In_ otApiContext *aApiContext
    )
{
    DWORD dwError = ERROR_SUCCESS;
    OVERLAPPED Overlapped = { 0 };
    DWORD dwBytesReturned = 0;
    otDeviceList* pDeviceList = nullptr;
    DWORD cbDeviceList = sizeof(otDeviceList);
	
    otLogFuncEntry();

    Overlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (Overlapped.hEvent == nullptr)
    {
        dwError = GetLastError();
        otLogCritApi("CreateEvent (Overlapped.hEvent) failed, %!WINERROR!", dwError);
        goto error;
    }
    
    pDeviceList = (otDeviceList*)malloc(cbDeviceList);
    if (pDeviceList == nullptr)
    {
        otLogWarnApi("Failed to allocate otDeviceList of %u bytes.", cbDeviceList);
        dwError = ERROR_NOT_ENOUGH_MEMORY;
        goto error;
    }
	RtlZeroMemory(pDeviceList, cbDeviceList);
    
    // Query in a loop to account for it changing between calls
    while (true)
    {
        // Send the IOCTL to query the interfaces
        if (!DeviceIoControl(
                aApiContext->DeviceHandle,
                IOCTL_OTLWF_ENUMERATE_DEVICES,
                nullptr, 0,
                pDeviceList, cbDeviceList,
                nullptr, 
                &Overlapped))
        {
            dwError = GetLastError();
            if (dwError != ERROR_IO_PENDING)
            {
                otLogCritApi("DeviceIoControl(IOCTL_OTLWF_ENUMERATE_DEVICES) failed, %!WINERROR!", dwError);
                goto error;
            }
            dwError = ERROR_SUCCESS;
        }

        // Get the result of the IO operation
        if (!GetOverlappedResultEx(
                aApiContext->DeviceHandle,
                &Overlapped,
                &dwBytesReturned,
                c_MaxOverlappedWaitTimeMS,
                TRUE))
        {
            dwError = GetLastError();
            if (dwError == WAIT_TIMEOUT)
            {
                dwError = ERROR_TIMEOUT;
                CancelIoEx(aApiContext->DeviceHandle, &Overlapped);
            }
            otLogCritApi("GetOverlappedResult for notification failed, %!WINERROR!", dwError);
            goto error;
        }
        
        // Calculate the expected size of the full buffer
        cbDeviceList = 
            FIELD_OFFSET(otDeviceList, aDevices) +
            pDeviceList->aDevicesLength * sizeof(otDeviceList::aDevices);
        
        // Make sure they returned a complete buffer
        if (dwBytesReturned != sizeof(otDeviceList::aDevicesLength)) break;
        
        // If we get here that means we didn't have a big enough buffer
        // Reallocate a new buffer
        free(pDeviceList);
        pDeviceList = (otDeviceList*)malloc(cbDeviceList);
        if (pDeviceList == nullptr)
        {
            otLogCritApi("Failed to allocate otDeviceList of %u bytes.", cbDeviceList);
            dwError = ERROR_NOT_ENOUGH_MEMORY;
            goto error;
        }
		RtlZeroMemory(pDeviceList, cbDeviceList);
    }

error:

    if (dwError != ERROR_SUCCESS)
    {
        free(pDeviceList);
        pDeviceList = nullptr;
    }

    if (Overlapped.hEvent)
    {
        CloseHandle(Overlapped.hEvent);
    }
	
    otLogFuncExitMsg("%d devices", pDeviceList == nullptr ? -1 : (int)pDeviceList->aDevicesLength);

    return pDeviceList;
}
    
OTAPI 
otContext *
otInit(
    _In_ otApiContext *aApiContext, 
    _In_ const GUID *aDeviceGuid
    )
{
    otContext *aContext = nullptr;

    OTLWF_DEVICE Result = {0};
    if (SendIOCTL(
            aApiContext, 
            IOCTL_OTLWF_QUERY_DEVICE, 
            (LPVOID)aDeviceGuid, 
            sizeof(GUID), 
            &Result, 
            sizeof(Result)
            ) == ERROR_SUCCESS)
    {
        aContext = (otContext*)malloc(sizeof(otContext));
        if (aContext)
        {
            aContext->ApiHandle = aApiContext;
            aContext->InterfaceGuid = *aDeviceGuid;
            aContext->CompartmentID = Result.CompartmentID;
        }
    }

    return aContext;
}

OTAPI 
GUID 
otGetDeviceGuid(
    otContext *aContext
    )
{
    return aContext->InterfaceGuid;
}

OTAPI 
uint32_t 
otGetCompartmentId(
    _In_ otContext *aContext
    )
{
    return aContext->CompartmentID;
}

OTAPI 
ThreadError 
otEnable(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_ENABLED, (BOOLEAN)TRUE));
}

OTAPI 
ThreadError 
otDisable(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_ENABLED, (BOOLEAN)FALSE));
}

OTAPI 
ThreadError 
otInterfaceUp(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_INTERFACE, (BOOLEAN)TRUE));
}

OTAPI 
ThreadError 
otInterfaceDown(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_INTERFACE, (BOOLEAN)FALSE));
}

OTAPI 
bool 
otIsInterfaceUp(
    _In_ otContext *aContext
    )
{
    BOOLEAN Result = FALSE;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_INTERFACE, &Result);
    return Result != FALSE;
}

OTAPI 
ThreadError 
otThreadStart(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_THREAD, (BOOLEAN)TRUE));
}

OTAPI 
ThreadError 
otThreadStop(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_THREAD, (BOOLEAN)FALSE));
}

OTAPI 
ThreadError 
otActiveScan(
    _In_ otContext *aContext, 
    uint32_t aScanChannels, 
    uint16_t aScanDuration,
    otHandleActiveScanResult aCallback
    )
{
    aContext->ApiHandle->SetCallback(
        aContext->ApiHandle->ActiveScanCallbacks,
        make_tuple(aContext->InterfaceGuid, aCallback, (PVOID)nullptr)
        );

    BYTE Buffer[sizeof(GUID) + sizeof(uint32_t) + sizeof(uint16_t)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), &aScanChannels, sizeof(aScanChannels));
    memcpy(Buffer + sizeof(GUID) + sizeof(uint32_t), &aScanDuration, sizeof(aScanDuration));
    
    return DwordToThreadError(SendIOCTL(aContext->ApiHandle, IOCTL_OTLWF_OT_ACTIVE_SCAN, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI 
bool 
otActiveScanInProgress(
    _In_ otContext *aContext
    )
{
    BOOLEAN Result = FALSE;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_ACTIVE_SCAN, &Result);
    return Result != FALSE;
}

OTAPI 
ThreadError 
otDiscover(
    _In_ otContext *aContext, 
    uint32_t aScanChannels, 
    uint16_t aScanDuration, 
    uint16_t aPanid,
    otHandleActiveScanResult aCallback
    )
{
    aContext->ApiHandle->SetCallback(
        aContext->ApiHandle->DiscoverCallbacks,
        make_tuple(aContext->InterfaceGuid, aCallback, (PVOID)nullptr)
        );

    BYTE Buffer[sizeof(GUID) + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), &aScanChannels, sizeof(aScanChannels));
    memcpy(Buffer + sizeof(GUID) + sizeof(uint32_t), &aScanDuration, sizeof(aScanDuration));
    memcpy(Buffer + sizeof(GUID) + sizeof(uint32_t) + sizeof(uint16_t), &aPanid, sizeof(aPanid));
    
    return DwordToThreadError(SendIOCTL(aContext->ApiHandle, IOCTL_OTLWF_OT_DISCOVER, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI 
bool 
otDiscoverInProgress(
    _In_ otContext *aContext
    )
{
    BOOLEAN Result = FALSE;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_DISCOVER, &Result);
    return Result != FALSE;
}

OTAPI 
uint8_t 
otGetChannel(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_CHANNEL, &Result);
    return Result;
}

OTAPI 
ThreadError 
otSetChannel(
    _In_ otContext *aContext, 
    uint8_t aChannel
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_CHANNEL, aChannel));
}

OTAPI 
uint32_t 
otGetChildTimeout(
    _In_ otContext *aContext
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_CHILD_TIMEOUT, &Result);
    return Result;
}

OTAPI 
void 
otSetChildTimeout(
    _In_ otContext *aContext, 
    uint32_t aTimeout
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_CHILD_TIMEOUT, aTimeout);
}

OTAPI 
const 
uint8_t *
otGetExtendedAddress(
    _In_ otContext *aContext
    )
{
    otExtAddress *Result = (otExtAddress*)malloc(sizeof(otExtAddress));
    if (Result && QueryIOCTL(aContext, IOCTL_OTLWF_OT_EXTENDED_ADDRESS, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return (uint8_t*)Result;
}

OTAPI 
ThreadError 
otSetExtendedAddress(
    _In_ otContext *aContext, 
    const otExtAddress *aExtendedAddress
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_EXTENDED_ADDRESS, aExtendedAddress));
}

OTAPI 
const uint8_t *
otGetExtendedPanId(
    _In_ otContext *aContext
    )
{
    otExtendedPanId *Result = (otExtendedPanId*)malloc(sizeof(otExtendedPanId));
    if (Result && QueryIOCTL(aContext, IOCTL_OTLWF_OT_EXTENDED_PANID, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return (uint8_t*)Result;
}

OTAPI 
void 
otSetExtendedPanId(
    _In_ otContext *aContext, 
    const uint8_t *aExtendedPanId
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_EXTENDED_PANID, (const otExtendedPanId*)aExtendedPanId);
}

OTAPI 
ThreadError 
otGetLeaderRloc(
    _In_ otContext *aContext, 
    _Out_ otIp6Address *aLeaderRloc
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_LEADER_RLOC, aLeaderRloc));
}

OTAPI 
otLinkModeConfig 
otGetLinkMode(
    _In_ otContext *aContext
    )
{
    otLinkModeConfig Result = {0};
	static_assert(sizeof(Result) == 1, "otLinkModeConfig must be 1 byte");
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_LINK_MODE, &Result);
    return Result;
}

OTAPI 
ThreadError 
otSetLinkMode(
    _In_ otContext *aContext, 
    otLinkModeConfig aConfig
    )
{
	static_assert(sizeof(aConfig) == 1, "otLinkModeConfig must be 1 byte");
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_LINK_MODE, aConfig));
}

OTAPI 
const uint8_t *
otGetMasterKey(
    _In_ otContext *aContext, 
    _Out_ uint8_t *aKeyLength
    )
{
    otMasterKey *Result = (otMasterKey*)malloc(sizeof(otMasterKey) + sizeof(uint8_t));
    if (Result == nullptr) return nullptr;
    if (QueryIOCTL(aContext, IOCTL_OTLWF_OT_MASTER_KEY, Result) != ERROR_SUCCESS)
    {
        free(Result);
        return nullptr;
    }
    else
    {
        *aKeyLength = *(uint8_t*)(Result + 1);
    }
    return (uint8_t*)Result;
}

OTAPI
ThreadError
otSetMasterKey(
    _In_ otContext *aContext, 
    const uint8_t *aKey, 
    uint8_t aKeyLength
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(otMasterKey) + sizeof(uint8_t)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), aKey, aKeyLength);
    memcpy(Buffer + sizeof(GUID) + sizeof(otMasterKey), &aKeyLength, sizeof(aKeyLength));
    
    return DwordToThreadError(SendIOCTL(aContext->ApiHandle, IOCTL_OTLWF_OT_MASTER_KEY, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI 
int8_t 
otGetMaxTransmitPower(
    _In_ otContext *aContext
    )
{
    int8_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_MAX_TRANSMIT_POWER, &Result);
    return Result;
}

OTAPI 
void 
otSetMaxTransmitPower(
    _In_ otContext *aContext, 
    int8_t aPower
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_MAX_TRANSMIT_POWER, aPower);
}

OTAPI
const otIp6Address *
otGetMeshLocalEid(
    _In_ otContext *aContext
    )
{
    otIp6Address *Result = (otIp6Address*)malloc(sizeof(otIp6Address));
    if (Result && QueryIOCTL(aContext, IOCTL_OTLWF_OT_MESH_LOCAL_EID, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return Result;
}

OTAPI
const uint8_t *
otGetMeshLocalPrefix(
    _In_ otContext *aContext
    )
{
    otMeshLocalPrefix *Result = (otMeshLocalPrefix*)malloc(sizeof(otMeshLocalPrefix));
    if (Result && QueryIOCTL(aContext, IOCTL_OTLWF_OT_MESH_LOCAL_PREFIX, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return (uint8_t*)Result;
}

OTAPI
ThreadError
otSetMeshLocalPrefix(
    _In_ otContext *aContext, 
    const uint8_t *aMeshLocalPrefix
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_MESH_LOCAL_PREFIX, (const otMeshLocalPrefix*)aMeshLocalPrefix));
}

OTAPI
ThreadError
otGetNetworkDataLeader(
    _In_ otContext *aContext, 
    bool aStable, 
    _Out_ uint8_t *aData, 
    _Out_ uint8_t *aDataLength
    )
{
    UNREFERENCED_PARAMETER(aContext);
    UNREFERENCED_PARAMETER(aStable);
    UNREFERENCED_PARAMETER(aData);
    UNREFERENCED_PARAMETER(aDataLength);
    return kThreadError_NotImplemented;
}

OTAPI
ThreadError
otGetNetworkDataLocal(
    _In_ otContext *aContext, 
    bool aStable, 
    _Out_ uint8_t *aData, 
    _Out_ uint8_t *aDataLength
    )
{
    UNREFERENCED_PARAMETER(aContext);
    UNREFERENCED_PARAMETER(aStable);
    UNREFERENCED_PARAMETER(aData);
    UNREFERENCED_PARAMETER(aDataLength);
    return kThreadError_NotImplemented;
}

OTAPI
const char *
otGetNetworkName(
    _In_ otContext *aContext
    )
{
    otNetworkName *Result = (otNetworkName*)malloc(sizeof(otNetworkName));
    if (Result && QueryIOCTL(aContext, IOCTL_OTLWF_OT_NETWORK_NAME, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return (char*)Result;
}

OTAPI
ThreadError
otSetNetworkName(
    _In_ otContext *aContext, 
    _In_ const char *aNetworkName
    )
{
    otNetworkName Buffer = {0};
    strcpy_s(Buffer.m8, sizeof(Buffer), aNetworkName);
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_NETWORK_NAME, (const otNetworkName*)&Buffer));
}

OTAPI 
ThreadError 
otGetNextOnMeshPrefix(
    _In_ otContext *aContext, 
    bool _aLocal, 
    _Inout_ otNetworkDataIterator *aIterator,
    _Out_ otBorderRouterConfig *aConfig
    )
{
    BYTE InBuffer[sizeof(GUID) + sizeof(BOOLEAN) + sizeof(uint8_t)];
    BYTE OutBuffer[sizeof(uint8_t) + sizeof(otBorderRouterConfig)];

    BOOLEAN aLocal = _aLocal ? TRUE : FALSE;
    memcpy(InBuffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(InBuffer + sizeof(GUID), &aLocal, sizeof(aLocal));
    memcpy(InBuffer + sizeof(GUID) + sizeof(BOOLEAN), aIterator, sizeof(uint8_t));

    ThreadError aError = 
        DwordToThreadError(
            SendIOCTL(
                aContext->ApiHandle, 
                IOCTL_OTLWF_OT_NEXT_ON_MESH_PREFIX, 
                InBuffer, sizeof(InBuffer), 
                OutBuffer, sizeof(OutBuffer)));

    if (aError == kThreadError_None)
    {
        memcpy(aIterator, OutBuffer, sizeof(uint8_t));
        memcpy(aConfig, OutBuffer + sizeof(uint8_t), sizeof(otBorderRouterConfig));
    }

    return aError;
}

OTAPI
otPanId otGetPanId(
    _In_ otContext *aContext
    )
{
    otPanId Result = {0};
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_PAN_ID, &Result);
    return Result;
}

OTAPI
ThreadError
otSetPanId(
    _In_ otContext *aContext, 
    otPanId aPanId
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_PAN_ID, aPanId));
}

OTAPI
bool 
otIsRouterRoleEnabled(
    _In_ otContext *aContext
    )
{
    BOOLEAN Result = {0};
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_ROUTER_ROLL_ENABLED, &Result);
    return Result != FALSE;
}

OTAPI
void 
otSetRouterRoleEnabled(
    _In_ otContext *aContext, 
    bool aEnabled
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_ROUTER_ROLL_ENABLED, (BOOLEAN)aEnabled);
}

OTAPI
otShortAddress 
otGetShortAddress(
    _In_ otContext *aContext
    )
{
    otShortAddress Result = {0};
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_SHORT_ADDRESS, &Result);
    return Result;
}

OTAPI
const otNetifAddress *
otGetUnicastAddresses(
    _In_ otContext *aContext
    )
{
    // TODO
    UNREFERENCED_PARAMETER(aContext);
    return nullptr;
}

OTAPI
ThreadError
otAddUnicastAddress(
    _In_ otContext *aContext, 
    _In_ otNetifAddress *aAddress
    )
{
    // TODO
    UNREFERENCED_PARAMETER(aContext);
    UNREFERENCED_PARAMETER(aAddress);
    return kThreadError_NotImplemented;
}

OTAPI
ThreadError
otRemoveUnicastAddress(
    _In_ otContext *aContext, 
    _In_ otNetifAddress *aAddress
    )
{
    // TODO
    UNREFERENCED_PARAMETER(aContext);
    UNREFERENCED_PARAMETER(aAddress);
    return kThreadError_NotImplemented;
}

OTAPI
void otSetStateChangedCallback(
    _In_ otContext *aContext, 
    otStateChangedCallback aCallback, 
    _In_ void *aCallbackContext
    )
{
    aContext->ApiHandle->SetCallback(
        aContext->ApiHandle->StateChangedCallbacks,
        make_tuple(aContext->InterfaceGuid, aCallback, aCallbackContext)
        );
}

OTAPI
ThreadError
otGetActiveDataset(
    _In_ otContext *aContext, 
    _Out_ otOperationalDataset *aDataset
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_ACTIVE_DATASET, aDataset));
}

OTAPI
ThreadError
otSetActiveDataset(
    _In_ otContext *aContext, 
    _In_ otOperationalDataset *aDataset
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_ACTIVE_DATASET, (const otOperationalDataset*)aDataset));
}

OTAPI
ThreadError
otGetPendingDataset(
    _In_ otContext *aContext, 
    _Out_ otOperationalDataset *aDataset
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_PENDING_DATASET, aDataset));
}

OTAPI
ThreadError
otSetPendingDataset(
    _In_ otContext *aContext, 
    _In_ otOperationalDataset *aDataset
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_PENDING_DATASET, (const otOperationalDataset*)aDataset));
}

OTAPI 
uint32_t 
otGetPollPeriod(
    _In_ otContext *aContext
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_POLL_PERIOD, &Result);
    return Result;
}

OTAPI 
void 
otSetPollPeriod(
    _In_ otContext *aContext, 
    uint32_t aPollPeriod
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_POLL_PERIOD, aPollPeriod);
}

OTAPI
uint8_t 
otGetLocalLeaderWeight(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_LOCAL_LEADER_WEIGHT, &Result);
    return Result;
}

OTAPI
void otSetLocalLeaderWeight(
    _In_ otContext *aContext, 
    uint8_t aWeight
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_LOCAL_LEADER_WEIGHT, aWeight);
}

OTAPI 
uint32_t 
otGetLocalLeaderPartitionId(
    _In_ otContext *aContext
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_LOCAL_LEADER_PARTITION_ID, &Result);
    return Result;
}

OTAPI 
void 
otSetLocalLeaderPartitionId(
    _In_ otContext *aContext, 
    uint32_t aPartitionId
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_LOCAL_LEADER_PARTITION_ID, aPartitionId);
}

OTAPI
ThreadError
otAddBorderRouter(
    _In_ otContext *aContext, 
    const otBorderRouterConfig *aConfig
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_ADD_BORDER_ROUTER, aConfig));
}

OTAPI
ThreadError
otRemoveBorderRouter(
    _In_ otContext *aContext, 
    const otIp6Prefix *aPrefix
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_REMOVE_BORDER_ROUTER, aPrefix));
}

OTAPI
ThreadError
otAddExternalRoute(
    _In_ otContext *aContext, 
    const otExternalRouteConfig *aConfig
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_ADD_EXTERNAL_ROUTE, aConfig));
}

OTAPI
ThreadError
otRemoveExternalRoute(
    _In_ otContext *aContext, 
    const otIp6Prefix *aPrefix
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_REMOVE_EXTERNAL_ROUTE, aPrefix));
}

OTAPI
ThreadError
otSendServerData(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_SEND_SERVER_DATA));
}

OTAPI
uint32_t 
otGetContextIdReuseDelay(
    _In_ otContext *aContext
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_CONTEXT_ID_REUSE_DELAY, &Result);
    return Result;
}

OTAPI
void 
otSetContextIdReuseDelay(
    _In_ otContext *aContext, 
    uint32_t aDelay
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_REMOVE_EXTERNAL_ROUTE, aDelay);
}

OTAPI
uint32_t 
otGetKeySequenceCounter(
    _In_ otContext *aContext
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_KEY_SEQUENCE_COUNTER, &Result);
    return Result;
}

OTAPI
void 
otSetKeySequenceCounter(
    _In_ otContext *aContext, 
    uint32_t aKeySequenceCounter
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_REMOVE_EXTERNAL_ROUTE, aKeySequenceCounter);
}

OTAPI
uint8_t otGetNetworkIdTimeout(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_NETWORK_ID_TIMEOUT, &Result);
    return Result;
}

OTAPI
void 
otSetNetworkIdTimeout(
    _In_ otContext *aContext, 
    uint8_t aTimeout
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_NETWORK_ID_TIMEOUT, aTimeout);
}

OTAPI
uint8_t 
otGetRouterUpgradeThreshold(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_ROUTER_UPGRADE_THRESHOLD, &Result);
    return Result;
}

OTAPI
void 
otSetRouterUpgradeThreshold(
    _In_ otContext *aContext, 
    uint8_t aThreshold
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_ROUTER_UPGRADE_THRESHOLD, aThreshold);
}

OTAPI
ThreadError
otReleaseRouterId(
    _In_ otContext *aContext, 
    uint8_t aRouterId
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_RELEASE_ROUTER_ID, aRouterId));
}

OTAPI
ThreadError
otAddMacWhitelist(
    _In_ otContext *aContext, 
    const uint8_t *aExtAddr
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_ADD_MAC_WHITELIST, (const otExtAddress*)aExtAddr));
}

OTAPI
ThreadError
otAddMacWhitelistRssi(
    _In_ otContext *aContext, 
    const uint8_t *aExtAddr, 
    int8_t aRssi
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(otExtAddress) + sizeof(int8_t)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), aExtAddr, sizeof(otExtAddress));
    memcpy(Buffer + sizeof(GUID) + sizeof(otExtAddress), &aRssi, sizeof(aRssi));
    
    return DwordToThreadError(SendIOCTL(aContext->ApiHandle, IOCTL_OTLWF_OT_ADD_MAC_WHITELIST, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI
void 
otRemoveMacWhitelist(
    _In_ otContext *aContext, 
    const uint8_t *aExtAddr
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_REMOVE_MAC_WHITELIST, (const otExtAddress*)aExtAddr);
}

OTAPI
ThreadError
otGetMacWhitelistEntry(
    _In_ otContext *aContext, 
    uint8_t aIndex, 
    _Out_ otMacWhitelistEntry *aEntry
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_MAC_WHITELIST_ENTRY, &aIndex, aEntry));
}

OTAPI
void 
otClearMacWhitelist(
    _In_ otContext *aContext
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_CLEAR_MAC_WHITELIST);
}

OTAPI
void 
otDisableMacWhitelist(
    _In_ otContext *aContext
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_MAC_WHITELIST_ENABLED, (BOOLEAN)FALSE);
}

OTAPI
void 
otEnableMacWhitelist(
    _In_ otContext *aContext
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_MAC_WHITELIST_ENABLED, (BOOLEAN)TRUE);
}

OTAPI
bool 
otIsMacWhitelistEnabled(
    _In_ otContext *aContext
    )
{
    BOOLEAN Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_MAC_WHITELIST_ENABLED, &Result);
    return Result != FALSE;
}

OTAPI
ThreadError
otBecomeDetached(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_DEVICE_ROLE, (uint8_t)kDeviceRoleDetached));
}

OTAPI
ThreadError
otBecomeChild(
    _In_ otContext *aContext, 
    otMleAttachFilter aFilter
    )
{
    uint8_t Role = kDeviceRoleDetached;
	uint8_t Filter = (uint8_t)aFilter;

    BYTE Buffer[sizeof(GUID) + sizeof(Role) + sizeof(Filter)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), &Role, sizeof(Role));
    memcpy(Buffer + sizeof(GUID) + sizeof(Role), &Filter, sizeof(Filter));
    
    return DwordToThreadError(SendIOCTL(aContext->ApiHandle, IOCTL_OTLWF_OT_DEVICE_ROLE, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI
ThreadError
otBecomeRouter(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_DEVICE_ROLE, (uint8_t)kDeviceRoleRouter));
}

OTAPI
ThreadError
otBecomeLeader(
    _In_ otContext *aContext
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_DEVICE_ROLE, (uint8_t)kDeviceRoleLeader));
}

OTAPI
ThreadError
otAddMacBlacklist(
    _In_ otContext *aContext, 
    const uint8_t *aExtAddr
    )
{
    return DwordToThreadError(SetIOCTL(aContext, IOCTL_OTLWF_OT_ADD_MAC_BLACKLIST, (const otExtAddress*)aExtAddr));
}

OTAPI
void 
otRemoveMacBlacklist(
    _In_ otContext *aContext, 
    const uint8_t *aExtAddr
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_REMOVE_MAC_BLACKLIST, (const otExtAddress*)aExtAddr);
}

OTAPI
ThreadError
otGetMacBlacklistEntry(
    _In_ otContext *aContext, 
    uint8_t aIndex, 
    _Out_ otMacBlacklistEntry *aEntry
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_MAC_BLACKLIST_ENTRY, &aIndex, aEntry));
}

OTAPI
void 
otClearMacBlacklist(
    _In_ otContext *aContext
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_CLEAR_MAC_BLACKLIST);
}

OTAPI
void 
otDisableMacBlacklist(
    _In_ otContext *aContext
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_MAC_BLACKLIST_ENABLED, (BOOLEAN)FALSE);
}

OTAPI
void 
otEnableMacBlacklist(
    _In_ otContext *aContext
    )
{
    (void)SetIOCTL(aContext, IOCTL_OTLWF_OT_MAC_BLACKLIST_ENABLED, (BOOLEAN)TRUE);
}

OTAPI
bool 
otIsMacBlacklistEnabled(
    _In_ otContext *aContext
    )
{
    BOOLEAN Result = 0;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_MAC_BLACKLIST_ENABLED, &Result);
    return Result != FALSE;
}

OTAPI 
ThreadError 
otGetAssignLinkQuality(
    _In_ otContext *aContext, 
    const uint8_t *aExtAddr, 
    _Out_ uint8_t *aLinkQuality
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_ASSIGN_LINK_QUALITY, (otExtAddress*)aExtAddr, aLinkQuality));
}

OTAPI 
void 
otSetAssignLinkQuality(
    _In_ otContext *aContext,
    const uint8_t *aExtAddr, 
    uint8_t aLinkQuality
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(otExtAddress) + sizeof(uint8_t)];
    memcpy(Buffer, &aContext->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), aExtAddr, sizeof(otExtAddress));
    memcpy(Buffer + sizeof(GUID) + sizeof(otExtAddress), &aLinkQuality, sizeof(aLinkQuality));
    (void)SendIOCTL(aContext->ApiHandle, IOCTL_OTLWF_OT_ASSIGN_LINK_QUALITY, Buffer, sizeof(Buffer), NULL, 0);
}

OTAPI 
void 
otPlatformReset(
    _In_ otContext *aContext
    )
{
    SetIOCTL(aContext, IOCTL_OTLWF_OT_PLATFORM_RESET);
}

OTAPI
ThreadError
otGetChildInfoById(
    _In_ otContext *aContext, 
    uint16_t aChildId, 
    _Out_ otChildInfo *aChildInfo
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_CHILD_INFO_BY_ID, &aChildId, aChildInfo));
}

OTAPI
ThreadError
otGetChildInfoByIndex(
    _In_ otContext *aContext, 
    uint8_t aChildIndex, 
    _Out_ otChildInfo *aChildInfo
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_CHILD_INFO_BY_INDEX, &aChildIndex, aChildInfo));
}

OTAPI
otDeviceRole 
otGetDeviceRole(
    _In_ otContext *aContext
    )
{
    uint8_t Result = kDeviceRoleOffline;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_DEVICE_ROLE, &Result);
    return (otDeviceRole)Result;
}

OTAPI
ThreadError
otGetEidCacheEntry(
    _In_ otContext *aContext, 
    uint8_t aIndex, 
    _Out_ otEidCacheEntry *aEntry
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_EID_CACHE_ENTRY, &aIndex, aEntry));
}

OTAPI
ThreadError
otGetLeaderData(
    _In_ otContext *aContext, 
    _Out_ otLeaderData *aLeaderData
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_LEADER_DATA, aLeaderData));
}

OTAPI
uint8_t 
otGetLeaderRouterId(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_LEADER_ROUTER_ID, &Result);
    return Result;
}

OTAPI
uint8_t 
otGetLeaderWeight(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_LEADER_WEIGHT, &Result);
    return Result;
}

OTAPI
uint8_t 
otGetNetworkDataVersion(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_NETWORK_DATA_VERSION, &Result);
    return Result;
}

OTAPI
uint32_t 
otGetPartitionId(
    _In_ otContext *aContext
    )
{
    uint32_t Result = 0xFFFFFFFF;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_PARTITION_ID, &Result);
    return Result;
}

OTAPI
uint16_t 
otGetRloc16(
    _In_ otContext *aContext
    )
{
    uint16_t Result = 0xFFFF;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_RLOC16, &Result);
    return Result;
}

OTAPI
uint8_t 
otGetRouterIdSequence(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_ROUTER_ID_SEQUENCE, &Result);
    return Result;
}

OTAPI
ThreadError
otGetRouterInfo(
    _In_ otContext *aContext, 
    uint16_t aRouterId, 
    _Out_ otRouterInfo *aRouterInfo
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_ROUTER_INFO, &aRouterId, aRouterInfo));
}

OTAPI 
ThreadError 
otGetParentInfo(
    _In_ otContext *aContext, 
    _Out_ otRouterInfo *aParentInfo
    )
{
    return DwordToThreadError(QueryIOCTL(aContext, IOCTL_OTLWF_OT_PARENT_INFO, aParentInfo));
}

OTAPI
uint8_t 
otGetStableNetworkDataVersion(
    _In_ otContext *aContext
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aContext, IOCTL_OTLWF_OT_STABLE_NETWORK_DATA_VERSION, &Result);
    return Result;
}
