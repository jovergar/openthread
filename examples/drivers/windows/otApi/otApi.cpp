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

// Version string returned by the API
const char c_Version[] = "Windows"; // TODO - What should we really put here?

typedef tuple<otDeviceAvailabilityChangedCallback,PVOID> otApiDeviceAvailabilityCallback;
typedef tuple<GUID,otHandleActiveScanResult,PVOID> otApiActiveScanCallback;
typedef tuple<GUID,otStateChangedCallback,PVOID> otApiStateChangeCallback;

typedef struct otApiInstance
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
    otApiInstance() : 
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

    ~otApiInstance()
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

} otApiInstance;

typedef struct otInstance
{
    otApiInstance   *ApiHandle;      // Pointer to the Api handle
    NET_IFINDEX      InterfaceIndex; // Interface Index
    NET_LUID         InterfaceLuid;  // Interface Luid
    GUID             InterfaceGuid;  // Interface guid
    ULONG            CompartmentID;  // Interface Compartment ID

} otInstance;

// otpool wait callback for async IO completion
VOID CALLBACK 
otIoComplete(
    _Inout_     PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID                 Context,
    _Inout_     PTP_WAIT              Wait,
    _In_        TP_WAIT_RESULT        WaitResult
    );

OTAPI 
otApiInstance *
otApiInit(
    )
{
    DWORD dwError = ERROR_SUCCESS;
    otApiInstance *aApitInstance = nullptr;
    
    otLogFuncEntry();

    aApitInstance = new(std::nothrow)otApiInstance();
    if (aApitInstance == nullptr)
    {
        dwError = GetLastError();
        otLogWarnApi("Failed to allocate otApiInstance");
        goto error;
    }

    // Open the pipe to the OpenThread driver
    aApitInstance->DeviceHandle = 
        CreateFile(
            OTLWF_IOCLT_PATH,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,                // no SECURITY_ATTRIBUTES structure
            OPEN_EXISTING,          // No special create flags
            FILE_FLAG_OVERLAPPED,   // Allow asynchronous requests
            nullptr
            );
    if (aApitInstance->DeviceHandle == INVALID_HANDLE_VALUE)
    {
        dwError = GetLastError();
        otLogCritApi("CreateFile failed, %!WINERROR!", dwError);
        goto error;
    }

    // Create event for completion of callback cleanup
    aApitInstance->CallbackCompleteEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (aApitInstance->CallbackCompleteEvent == nullptr)
    {
        dwError = GetLastError();
        otLogCritApi("CreateEvent (CallbackCompleteEvent) failed, %!WINERROR!", dwError);
        goto error;
    }

    // Create event for completion of async IO
    aApitInstance->Overlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (aApitInstance->Overlapped.hEvent == nullptr)
    {
        dwError = GetLastError();
        otLogCritApi("CreateEvent (Overlapped.hEvent) failed, %!WINERROR!", dwError);
        goto error;
    }

    // Create the otpool wait
    aApitInstance->ThreadpoolWait = 
        CreateThreadpoolWait(
            otIoComplete,
            aApitInstance,
            nullptr
            );
    if (aApitInstance->ThreadpoolWait == nullptr)
    {
        dwError = GetLastError();
        otLogCritApi("CreateThreadpoolWait failed, %!WINERROR!", dwError);
        goto error;
    }

    // Start the otpool waiting on the overlapped event
    SetThreadpoolWait(aApitInstance->ThreadpoolWait, aApitInstance->Overlapped.hEvent, nullptr);

#ifdef DEBUG_ASYNC_IO
    otLogDebgApi("Querying for 1st notification");
#endif

    // Request first notification asynchronously
    if (!DeviceIoControl(
            aApitInstance->DeviceHandle,
            IOCTL_OTLWF_QUERY_NOTIFICATION,
            nullptr, 0,
            &aApitInstance->NotificationBuffer, sizeof(OTLWF_NOTIFICATION),
            nullptr, 
            &aApitInstance->Overlapped))
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
        otApiFinalize(aApitInstance);
        aApitInstance = nullptr;
    }
    
    otLogFuncExit();

    return aApitInstance;
}

OTAPI 
void 
otApiFinalize(
    _In_ otApiInstance *aApitInstance
)
{
    if (aApitInstance == nullptr) return;
    
    otLogFuncEntry();

    // If we never got the handle, nothing left to clean up
    if (aApitInstance->DeviceHandle == INVALID_HANDLE_VALUE) goto exit;

    //
    // Make sure we unregister callbacks
    //

    EnterCriticalSection(&aApitInstance->CallbackLock);

    // Clear all callbacks
    if (get<0>(aApitInstance->DeviceAvailabilityCallbacks))
    {
        aApitInstance->DeviceAvailabilityCallbacks = make_tuple((otDeviceAvailabilityChangedCallback)nullptr, (PVOID)nullptr);
        RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount);
    }
    for (size_t i = 0; i < aApitInstance->ActiveScanCallbacks.size(); i++)
    {
        RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount);
    }
    for (size_t i = 0; i < aApitInstance->DiscoverCallbacks.size(); i++)
    {
        RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount);
    }
    for (size_t i = 0; i < aApitInstance->StateChangedCallbacks.size(); i++)
    {
        RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount);
    }
    aApitInstance->ActiveScanCallbacks.clear();
    aApitInstance->DiscoverCallbacks.clear();
    aApitInstance->StateChangedCallbacks.clear();

#ifdef DEBUG_ASYNC_IO
    otLogDebgApi("Clearing Threadpool Wait");
#endif

    // Clear the threadpool wait to prevent further waits from being scheduled
    PTP_WAIT tpWait = aApitInstance->ThreadpoolWait;
    aApitInstance->ThreadpoolWait = nullptr;

    LeaveCriticalSection(&aApitInstance->CallbackLock);
    
    // Release last ref and wait for any pending callback to complete, if necessary
    if (!RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount))
    {
        WaitForSingleObject(aApitInstance->CallbackCompleteEvent, INFINITE);
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
        CancelIoEx(aApitInstance->DeviceHandle, &aApitInstance->Overlapped);

        // Free the threadpool wait
        CloseThreadpoolWait(tpWait);
    }

    // Clean up overlapped event
    if (aApitInstance->Overlapped.hEvent)
    {
        CloseHandle(aApitInstance->Overlapped.hEvent);
    }

    // Clean up callback complete event
    if (aApitInstance->CallbackCompleteEvent)
    {
        CloseHandle(aApitInstance->CallbackCompleteEvent);
    }
    
    // Close the device handle
    CloseHandle(aApitInstance->DeviceHandle);

exit:

    delete aApitInstance;
    
    otLogFuncExit();
}

OTAPI 
void 
otFreeMemory(
    _In_ const void *mem
    )
{
    free((void*)mem);
}

// Handles cleanly invoking the register callback
VOID
ProcessNotification(
    _In_ otApiInstance         *aApitInstance,
    _In_ POTLWF_NOTIFICATION    Notif
    )
{
    if (Notif->NotifType == OTLWF_NOTIF_DEVICE_AVAILABILITY)
    {
        otDeviceAvailabilityChangedCallback Callback = nullptr;
        PVOID                               CallbackContext = nullptr;
        
        EnterCriticalSection(&aApitInstance->CallbackLock);

        if (get<0>(aApitInstance->DeviceAvailabilityCallbacks) != nullptr)
        {
            // Add Ref
            RtlIncrementReferenceCount(&aApitInstance->CallbackRefCount);

            // Set callback
            Callback = get<0>(aApitInstance->DeviceAvailabilityCallbacks);
            CallbackContext = get<1>(aApitInstance->DeviceAvailabilityCallbacks);
        }

        LeaveCriticalSection(&aApitInstance->CallbackLock);

        // Invoke the callback outside the lock
        if (Callback)
        {
            Callback(Notif->DeviceAvailabilityPayload.Available != FALSE, &Notif->InterfaceGuid, CallbackContext);

            // Release ref
            if (RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(aApitInstance->CallbackCompleteEvent);
            }
        }
    }
    else if (Notif->NotifType == OTLWF_NOTIF_STATE_CHANGE)
    {
        otStateChangedCallback  Callback = nullptr;
        PVOID                   CallbackContext = nullptr;

        EnterCriticalSection(&aApitInstance->CallbackLock);

        // Set the callback
        for (size_t i = 0; i < aApitInstance->StateChangedCallbacks.size(); i++)
        {
            if (get<0>(aApitInstance->StateChangedCallbacks[i]) == Notif->InterfaceGuid)
            {
                // Add Ref
                RtlIncrementReferenceCount(&aApitInstance->CallbackRefCount);

                // Set callback
                Callback = get<1>(aApitInstance->StateChangedCallbacks[i]);
                CallbackContext = get<2>(aApitInstance->StateChangedCallbacks[i]);
                break;
            }
        }

        LeaveCriticalSection(&aApitInstance->CallbackLock);

        // Invoke the callback outside the lock
        if (Callback)
        {
            Callback(Notif->StateChangePayload.Flags, CallbackContext);

            // Release ref
            if (RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(aApitInstance->CallbackCompleteEvent);
            }
        }
    }
    else if (Notif->NotifType == OTLWF_NOTIF_DISCOVER)
    {
        otHandleActiveScanResult Callback = nullptr;
        PVOID                    CallbackContext = nullptr;

        EnterCriticalSection(&aApitInstance->CallbackLock);

        // Set the callback
        for (size_t i = 0; i < aApitInstance->DiscoverCallbacks.size(); i++)
        {
            if (get<0>(aApitInstance->DiscoverCallbacks[i]) == Notif->InterfaceGuid)
            {
                // Add Ref
                RtlIncrementReferenceCount(&aApitInstance->CallbackRefCount);

                // Set callback
                Callback = get<1>(aApitInstance->DiscoverCallbacks[i]);
                CallbackContext = get<2>(aApitInstance->DiscoverCallbacks[i]);
                break;
            }
        }

        LeaveCriticalSection(&aApitInstance->CallbackLock);

        // Invoke the callback outside the lock
        if (Callback)
        {
            if (Notif->DiscoverPayload.Valid)
            {
                Callback(&Notif->DiscoverPayload.Results, CallbackContext);
            }
            else
            {
                Callback(nullptr, CallbackContext);
            }

            // Release ref
            if (RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(aApitInstance->CallbackCompleteEvent);
            }
        }
    }
    else if (Notif->NotifType == OTLWF_NOTIF_ACTIVE_SCAN)
    {
        otHandleActiveScanResult Callback = nullptr;
        PVOID                    CallbackContext = nullptr;

        EnterCriticalSection(&aApitInstance->CallbackLock);

        // Set the callback
        for (size_t i = 0; i < aApitInstance->ActiveScanCallbacks.size(); i++)
        {
            if (get<0>(aApitInstance->ActiveScanCallbacks[i]) == Notif->InterfaceGuid)
            {
                // Add Ref
                RtlIncrementReferenceCount(&aApitInstance->CallbackRefCount);

                // Set callback
                Callback = get<1>(aApitInstance->ActiveScanCallbacks[i]);
                CallbackContext = get<2>(aApitInstance->ActiveScanCallbacks[i]);
                break;
            }
        }

        LeaveCriticalSection(&aApitInstance->CallbackLock);

        // Invoke the callback outside the lock
        if (Callback)
        {
            if (Notif->ActiveScanPayload.Valid)
            {
                Callback(&Notif->ActiveScanPayload.Results, CallbackContext);
            }
            else
            {
                Callback(nullptr, CallbackContext);
            }

            // Release ref
            if (RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount))
            {
                // Set completion event if there are no more refs
                SetEvent(aApitInstance->CallbackCompleteEvent);
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

    otApiInstance *aApitInstance = (otApiInstance*)Context;
    if (aApitInstance == nullptr) return;

    // Get the result of the IO operation
    DWORD dwBytesTransferred = 0;
    if (!GetOverlappedResult(
            aApitInstance->DeviceHandle,
            &aApitInstance->Overlapped,
            &dwBytesTransferred,
            FALSE))
    {
        DWORD dwError = GetLastError();
        otLogCritApi("GetOverlappedResult for notification failed, %!WINERROR!", dwError);
    }
    else
    {
        otLogDebgApi("Received successful callback for notification, type=%d", 
                     aApitInstance->NotificationBuffer.NotifType);

        // Invoke the callback if set
        ProcessNotification(aApitInstance, &aApitInstance->NotificationBuffer);
            
        // Try to get the threadpool wait to see if we are allowed to continue processing notifications
        EnterCriticalSection(&aApitInstance->CallbackLock);
        PTP_WAIT tpWait = aApitInstance->ThreadpoolWait;
        LeaveCriticalSection(&aApitInstance->CallbackLock);

        if (tpWait)
        {
            // Start waiting for next notification
            SetThreadpoolWait(tpWait, aApitInstance->Overlapped.hEvent, nullptr);
            
#ifdef DEBUG_ASYNC_IO
            otLogDebgApi("Querying for next notification");
#endif

            // Request next notification
            if (!DeviceIoControl(
                    aApitInstance->DeviceHandle,
                    IOCTL_OTLWF_QUERY_NOTIFICATION,
                    nullptr, 0,
                    &aApitInstance->NotificationBuffer, sizeof(OTLWF_NOTIFICATION),
                    nullptr, 
                    &aApitInstance->Overlapped))
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
    _In_ otApiInstance *aApitInstance,
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
            aApitInstance->DeviceHandle,
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
            aApitInstance->DeviceHandle,
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
            CancelIoEx(aApitInstance->DeviceHandle, &Overlapped);
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
    _In_ otInstance *aInstance,
    _In_ DWORD dwIoControlCode,
    _In_ const in *input,
    _Out_ out* output
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(in)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), input, sizeof(in));
    return SendIOCTL(aInstance->ApiHandle, dwIoControlCode, Buffer, sizeof(Buffer), output, sizeof(out));
}

template <class out>
DWORD
QueryIOCTL(
    _In_ otInstance *aInstance,
    _In_ DWORD dwIoControlCode,
    _Out_ out* output
    )
{
    return SendIOCTL(aInstance->ApiHandle, dwIoControlCode, &aInstance->InterfaceGuid, sizeof(GUID), output, sizeof(out));
}

template <class in>
DWORD
SetIOCTL(
    _In_ otInstance *aInstance,
    _In_ DWORD dwIoControlCode,
    _In_ const in* input
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(in)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), input, sizeof(in));
    return SendIOCTL(aInstance->ApiHandle, dwIoControlCode, Buffer, sizeof(Buffer), nullptr, 0);
}

template <class in>
DWORD
SetIOCTL(
    _In_ otInstance *aInstance,
    _In_ DWORD dwIoControlCode,
    _In_ const in input
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(in)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), &input, sizeof(in));
    return SendIOCTL(aInstance->ApiHandle, dwIoControlCode, Buffer, sizeof(Buffer), nullptr, 0);
}

DWORD
SetIOCTL(
    _In_ otInstance *aInstance,
    _In_ DWORD dwIoControlCode
    )
{
    return SendIOCTL(aInstance->ApiHandle, dwIoControlCode, &aInstance->InterfaceGuid, sizeof(GUID), nullptr, 0);
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
    _In_ otApiInstance *aApitInstance,
    _In_ otDeviceAvailabilityChangedCallback aCallback,
    _In_ void *aCallbackContext
    )
{
    bool releaseRef = false;

    EnterCriticalSection(&aApitInstance->CallbackLock);

    if (aCallback == nullptr)
    {
        if (get<0>(aApitInstance->DeviceAvailabilityCallbacks) != nullptr)
        {
            releaseRef = true;
            aApitInstance->DeviceAvailabilityCallbacks = make_tuple(aCallback, aCallbackContext);
        }
    }
    else
    {
        if (get<0>(aApitInstance->DeviceAvailabilityCallbacks) == nullptr)
        {
            RtlIncrementReferenceCount(&aApitInstance->CallbackRefCount);
            aApitInstance->DeviceAvailabilityCallbacks = make_tuple(aCallback, aCallbackContext);
        }
    }
    
    LeaveCriticalSection(&aApitInstance->CallbackLock);

    if (releaseRef)
    {
        if (RtlDecrementReferenceCount(&aApitInstance->CallbackRefCount))
        {
            // Set completion event if there are no more refs
            SetEvent(aApitInstance->CallbackCompleteEvent);
        }
    }
}

OTAPI 
otDeviceList* 
otEnumerateDevices(
    _In_ otApiInstance *aApitInstance
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
                aApitInstance->DeviceHandle,
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
                aApitInstance->DeviceHandle,
                &Overlapped,
                &dwBytesReturned,
                c_MaxOverlappedWaitTimeMS,
                TRUE))
        {
            dwError = GetLastError();
            if (dwError == WAIT_TIMEOUT)
            {
                dwError = ERROR_TIMEOUT;
                CancelIoEx(aApitInstance->DeviceHandle, &Overlapped);
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
otInstance *
otInstanceInit(
    _In_ otApiInstance *aApitInstance, 
    _In_ const GUID *aDeviceGuid
    )
{
    otInstance *aInstance = nullptr;

    OTLWF_DEVICE Result = {0};
    if (SendIOCTL(
            aApitInstance, 
            IOCTL_OTLWF_QUERY_DEVICE, 
            (LPVOID)aDeviceGuid, 
            sizeof(GUID), 
            &Result, 
            sizeof(Result)
            ) == ERROR_SUCCESS)
    {
        aInstance = (otInstance*)malloc(sizeof(otInstance));
        if (aInstance)
        {
            aInstance->ApiHandle = aApitInstance;
            aInstance->InterfaceGuid = *aDeviceGuid;
            aInstance->CompartmentID = Result.CompartmentID;

            if (ConvertInterfaceGuidToLuid(aDeviceGuid, &aInstance->InterfaceLuid) != ERROR_SUCCESS ||
                ConvertInterfaceLuidToIndex(&aInstance->InterfaceLuid, &aInstance->InterfaceIndex) != ERROR_SUCCESS)
            {
                otLogCritApi("Failed to convert interface guid to index!");
                free(aInstance);
                aInstance = nullptr;
            }
        }
    }

    return aInstance;
}

OTAPI 
GUID 
otGetDeviceGuid(
    otInstance *aInstance
    )
{
    return aInstance->InterfaceGuid;
}

OTAPI 
uint32_t 
otGetDeviceIfIndex(
    otInstance *aInstance
    )
{
    return aInstance->InterfaceIndex;
}

OTAPI 
uint32_t 
otGetCompartmentId(
    _In_ otInstance *aInstance
    )
{
    return aInstance->CompartmentID;
}

OTAPI 
const char *
otGetVersionString()
{
    char* szVersion = (char*)malloc(sizeof(c_Version));
    if (szVersion)
    {
        memcpy_s(szVersion, sizeof(c_Version), c_Version, sizeof(c_Version));
    }
    return szVersion;
}

OTAPI 
ThreadError 
otEnable(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_ENABLED, (BOOLEAN)TRUE));
}

OTAPI 
ThreadError 
otDisable(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_ENABLED, (BOOLEAN)FALSE));
}

OTAPI 
ThreadError 
otInterfaceUp(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_INTERFACE, (BOOLEAN)TRUE));
}

OTAPI 
ThreadError 
otInterfaceDown(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_INTERFACE, (BOOLEAN)FALSE));
}

OTAPI 
bool 
otIsInterfaceUp(
    _In_ otInstance *aInstance
    )
{
    BOOLEAN Result = FALSE;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_INTERFACE, &Result);
    return Result != FALSE;
}

OTAPI 
ThreadError 
otThreadStart(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_THREAD, (BOOLEAN)TRUE));
}

OTAPI 
ThreadError 
otThreadStop(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_THREAD, (BOOLEAN)FALSE));
}

OTAPI 
bool 
otIsSingleton(
    _In_ otInstance *aInstance
    )
{
    BOOLEAN Result = FALSE;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_SINGLETON, &Result);
    return Result != FALSE;
}

OTAPI 
ThreadError 
otActiveScan(
    _In_ otInstance *aInstance, 
    uint32_t aScanChannels, 
    uint16_t aScanDuration,
    otHandleActiveScanResult aCallback,
    void *aCallbackContext
    )
{
    aInstance->ApiHandle->SetCallback(
        aInstance->ApiHandle->ActiveScanCallbacks,
        make_tuple(aInstance->InterfaceGuid, aCallback, aCallbackContext)
        );

    BYTE Buffer[sizeof(GUID) + sizeof(uint32_t) + sizeof(uint16_t)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), &aScanChannels, sizeof(aScanChannels));
    memcpy(Buffer + sizeof(GUID) + sizeof(uint32_t), &aScanDuration, sizeof(aScanDuration));
    
    return DwordToThreadError(SendIOCTL(aInstance->ApiHandle, IOCTL_OTLWF_OT_ACTIVE_SCAN, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI 
bool 
otIsActiveScanInProgress(
    _In_ otInstance *aInstance
    )
{
    BOOLEAN Result = FALSE;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_ACTIVE_SCAN, &Result);
    return Result != FALSE;
}

OTAPI 
ThreadError 
otDiscover(
    _In_ otInstance *aInstance, 
    uint32_t aScanChannels, 
    uint16_t aScanDuration, 
    uint16_t aPanid,
    otHandleActiveScanResult aCallback,
    void *aCallbackContext
    )
{
    aInstance->ApiHandle->SetCallback(
        aInstance->ApiHandle->DiscoverCallbacks,
        make_tuple(aInstance->InterfaceGuid, aCallback, aCallbackContext)
        );

    BYTE Buffer[sizeof(GUID) + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), &aScanChannels, sizeof(aScanChannels));
    memcpy(Buffer + sizeof(GUID) + sizeof(uint32_t), &aScanDuration, sizeof(aScanDuration));
    memcpy(Buffer + sizeof(GUID) + sizeof(uint32_t) + sizeof(uint16_t), &aPanid, sizeof(aPanid));
    
    return DwordToThreadError(SendIOCTL(aInstance->ApiHandle, IOCTL_OTLWF_OT_DISCOVER, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI 
bool 
otIsDiscoverInProgress(
    _In_ otInstance *aInstance
    )
{
    BOOLEAN Result = FALSE;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_DISCOVER, &Result);
    return Result != FALSE;
}

OTAPI 
uint8_t 
otGetChannel(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_CHANNEL, &Result);
    return Result;
}

OTAPI 
ThreadError 
otSetChannel(
    _In_ otInstance *aInstance, 
    uint8_t aChannel
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_CHANNEL, aChannel));
}

OTAPI 
uint8_t 
otGetMaxAllowedChildren(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MAX_CHILDREN, &Result);
    return Result;
}

OTAPI 
ThreadError 
otSetMaxAllowedChildren(
    _In_ otInstance *aInstance, 
    uint8_t aMaxChildren
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_MAX_CHILDREN, aMaxChildren));
}

OTAPI 
uint32_t 
otGetChildTimeout(
    _In_ otInstance *aInstance
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_CHILD_TIMEOUT, &Result);
    return Result;
}

OTAPI 
void 
otSetChildTimeout(
    _In_ otInstance *aInstance, 
    uint32_t aTimeout
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_CHILD_TIMEOUT, aTimeout);
}

OTAPI 
const 
uint8_t *
otGetExtendedAddress(
    _In_ otInstance *aInstance
    )
{
    otExtAddress *Result = (otExtAddress*)malloc(sizeof(otExtAddress));
    if (Result && QueryIOCTL(aInstance, IOCTL_OTLWF_OT_EXTENDED_ADDRESS, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return (uint8_t*)Result;
}

OTAPI 
ThreadError 
otSetExtendedAddress(
    _In_ otInstance *aInstance, 
    const otExtAddress *aExtendedAddress
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_EXTENDED_ADDRESS, aExtendedAddress));
}

OTAPI 
const uint8_t *
otGetExtendedPanId(
    _In_ otInstance *aInstance
    )
{
    otExtendedPanId *Result = (otExtendedPanId*)malloc(sizeof(otExtendedPanId));
    if (Result && QueryIOCTL(aInstance, IOCTL_OTLWF_OT_EXTENDED_PANID, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return (uint8_t*)Result;
}

OTAPI 
void 
otSetExtendedPanId(
    _In_ otInstance *aInstance, 
    const uint8_t *aExtendedPanId
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_EXTENDED_PANID, (const otExtendedPanId*)aExtendedPanId);
}

OTAPI 
ThreadError 
otGetLeaderRloc(
    _In_ otInstance *aInstance, 
    _Out_ otIp6Address *aLeaderRloc
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_LEADER_RLOC, aLeaderRloc));
}

OTAPI 
otLinkModeConfig 
otGetLinkMode(
    _In_ otInstance *aInstance
    )
{
    otLinkModeConfig Result = {0};
    static_assert(sizeof(otLinkModeConfig) == 4, "The size of otLinkModeConfig should be 4 bytes");
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_LINK_MODE, &Result);
    return Result;
}

OTAPI 
ThreadError 
otSetLinkMode(
    _In_ otInstance *aInstance, 
    otLinkModeConfig aConfig
    )
{
    static_assert(sizeof(otLinkModeConfig) == 4, "The size of otLinkModeConfig should be 4 bytes");
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_LINK_MODE, aConfig));
}

OTAPI 
const uint8_t *
otGetMasterKey(
    _In_ otInstance *aInstance, 
    _Out_ uint8_t *aKeyLength
    )
{
    struct otMasterKeyAndLength
    {
        otMasterKey Key;
        uint8_t Length;
    };
    otMasterKeyAndLength *Result = (otMasterKeyAndLength*)malloc(sizeof(otMasterKeyAndLength));
    if (Result == nullptr) return nullptr;
    if (QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MASTER_KEY, Result) != ERROR_SUCCESS)
    {
        free(Result);
        return nullptr;
    }
    else
    {
        *aKeyLength = Result->Length;
    }
    return (uint8_t*)Result;
}

OTAPI
ThreadError
otSetMasterKey(
    _In_ otInstance *aInstance, 
    const uint8_t *aKey, 
    uint8_t aKeyLength
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(otMasterKey) + sizeof(uint8_t)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), aKey, aKeyLength);
    memcpy(Buffer + sizeof(GUID) + sizeof(otMasterKey), &aKeyLength, sizeof(aKeyLength));
    
    return DwordToThreadError(SendIOCTL(aInstance->ApiHandle, IOCTL_OTLWF_OT_MASTER_KEY, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI 
int8_t 
otGetMaxTransmitPower(
    _In_ otInstance *aInstance
    )
{
    int8_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MAX_TRANSMIT_POWER, &Result);
    return Result;
}

OTAPI 
void 
otSetMaxTransmitPower(
    _In_ otInstance *aInstance, 
    int8_t aPower
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_MAX_TRANSMIT_POWER, aPower);
}

OTAPI
const otIp6Address *
otGetMeshLocalEid(
    _In_ otInstance *aInstance
    )
{
    otIp6Address *Result = (otIp6Address*)malloc(sizeof(otIp6Address));
    if (Result && QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MESH_LOCAL_EID, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return Result;
}

OTAPI
const uint8_t *
otGetMeshLocalPrefix(
    _In_ otInstance *aInstance
    )
{
    otMeshLocalPrefix *Result = (otMeshLocalPrefix*)malloc(sizeof(otMeshLocalPrefix));
    if (Result && QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MESH_LOCAL_PREFIX, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return (uint8_t*)Result;
}

OTAPI
ThreadError
otSetMeshLocalPrefix(
    _In_ otInstance *aInstance, 
    const uint8_t *aMeshLocalPrefix
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_MESH_LOCAL_PREFIX, (const otMeshLocalPrefix*)aMeshLocalPrefix));
}

OTAPI
ThreadError
otGetNetworkDataLeader(
    _In_ otInstance *aInstance, 
    bool aStable, 
    _Out_ uint8_t *aData, 
    _Out_ uint8_t *aDataLength
    )
{
    UNREFERENCED_PARAMETER(aInstance);
    UNREFERENCED_PARAMETER(aStable);
    UNREFERENCED_PARAMETER(aData);
    UNREFERENCED_PARAMETER(aDataLength);
    return kThreadError_NotImplemented;
}

OTAPI
ThreadError
otGetNetworkDataLocal(
    _In_ otInstance *aInstance, 
    bool aStable, 
    _Out_ uint8_t *aData, 
    _Out_ uint8_t *aDataLength
    )
{
    UNREFERENCED_PARAMETER(aInstance);
    UNREFERENCED_PARAMETER(aStable);
    UNREFERENCED_PARAMETER(aData);
    UNREFERENCED_PARAMETER(aDataLength);
    return kThreadError_NotImplemented;
}

OTAPI
const char *
otGetNetworkName(
    _In_ otInstance *aInstance
    )
{
    otNetworkName *Result = (otNetworkName*)malloc(sizeof(otNetworkName));
    if (Result && QueryIOCTL(aInstance, IOCTL_OTLWF_OT_NETWORK_NAME, Result) != ERROR_SUCCESS)
    {
        free(Result);
        Result = nullptr;
    }
    return (char*)Result;
}

OTAPI
ThreadError
otSetNetworkName(
    _In_ otInstance *aInstance, 
    _In_ const char *aNetworkName
    )
{
    otNetworkName Buffer = {0};
    strcpy_s(Buffer.m8, sizeof(Buffer), aNetworkName);
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_NETWORK_NAME, (const otNetworkName*)&Buffer));
}

OTAPI 
ThreadError 
otGetNextOnMeshPrefix(
    _In_ otInstance *aInstance, 
    bool _aLocal, 
    _Inout_ otNetworkDataIterator *aIterator,
    _Out_ otBorderRouterConfig *aConfig
    )
{
    BYTE InBuffer[sizeof(GUID) + sizeof(BOOLEAN) + sizeof(uint8_t)];
    BYTE OutBuffer[sizeof(uint8_t) + sizeof(otBorderRouterConfig)];

    BOOLEAN aLocal = _aLocal ? TRUE : FALSE;
    memcpy(InBuffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(InBuffer + sizeof(GUID), &aLocal, sizeof(aLocal));
    memcpy(InBuffer + sizeof(GUID) + sizeof(BOOLEAN), aIterator, sizeof(uint8_t));

    ThreadError aError = 
        DwordToThreadError(
            SendIOCTL(
                aInstance->ApiHandle, 
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
    _In_ otInstance *aInstance
    )
{
    otPanId Result = {0};
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_PAN_ID, &Result);
    return Result;
}

OTAPI
ThreadError
otSetPanId(
    _In_ otInstance *aInstance, 
    otPanId aPanId
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_PAN_ID, aPanId));
}

OTAPI
bool 
otIsRouterRoleEnabled(
    _In_ otInstance *aInstance
    )
{
    BOOLEAN Result = {0};
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_ROUTER_ROLL_ENABLED, &Result);
    return Result != FALSE;
}

OTAPI
void 
otSetRouterRoleEnabled(
    _In_ otInstance *aInstance, 
    bool aEnabled
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_ROUTER_ROLL_ENABLED, (BOOLEAN)aEnabled);
}

OTAPI
otShortAddress 
otGetShortAddress(
    _In_ otInstance *aInstance
    )
{
    otShortAddress Result = {0};
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_SHORT_ADDRESS, &Result);
    return Result;
}

BOOL
GetAdapterAddresses(
    PIP_ADAPTER_ADDRESSES * ppIAA
)
{
    PIP_ADAPTER_ADDRESSES pIAA = NULL;
    DWORD len = 0;
    DWORD flags;

    flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET6, flags, NULL, NULL, &len) != ERROR_BUFFER_OVERFLOW)
        return FALSE;

    pIAA = (PIP_ADAPTER_ADDRESSES)malloc(len);
    if (pIAA) {
        GetAdaptersAddresses(AF_INET6, flags, NULL, pIAA, &len);
        *ppIAA = pIAA;
        return TRUE;
    }
    return FALSE;
}

OTAPI
const otNetifAddress *
otGetUnicastAddresses(
    _In_ otInstance *aInstance
    )
{
    // Put the current thead in the correct compartment
    bool RevertCompartmentOnExit = false;
    ULONG OriginalCompartmentID = GetCurrentThreadCompartmentId();
    if (OriginalCompartmentID != aInstance->CompartmentID)
    {
        DWORD dwError = ERROR_SUCCESS;
        if ((dwError = SetCurrentThreadCompartmentId(aInstance->CompartmentID)) != ERROR_SUCCESS)
        {
            otLogCritApi("SetCurrentThreadCompartmentId failed, %!WINERROR!", dwError);
            return nullptr;
        }
        RevertCompartmentOnExit = true;
    }

    otNetifAddress *addrs = nullptr;

    // Query the current adapter addresses and format them in the proper output format
    PIP_ADAPTER_ADDRESSES pIAAList;
    if (GetAdapterAddresses(&pIAAList))
    {
        ULONG AddrCount = 0;

        // Loop through all the interfaces
        for (auto pIAA = pIAAList; pIAA != nullptr; pIAA = pIAA->Next) 
        {
            // Look for the right interface
            if (pIAA->Ipv6IfIndex != aInstance->InterfaceIndex) continue;

            // Look through all unicast addresses
            for (auto pUnicastAddr = pIAA->FirstUnicastAddress; 
                 pUnicastAddr != nullptr; 
                 pUnicastAddr = pUnicastAddr->Next)
            {
                AddrCount++;
            }

            break;
        }

        // Allocate the addresses
        addrs = (otNetifAddress*)malloc(AddrCount * sizeof(otNetifAddress));
        if (addrs == nullptr)
        {
            otLogWarnApi("Not enough memory to alloc otNetifAddress array");
            goto error;
        }
        ZeroMemory(addrs, AddrCount * sizeof(otNetifAddress));

        // Initialize the next pointers
        for (ULONG i = 0; i < AddrCount; i++)
        {
            addrs[i].mNext = (i + 1 == AddrCount) ? nullptr : &addrs[i + 1];
        }

        AddrCount = 0;

        // Loop through all the interfaces
        for (auto pIAA = pIAAList; pIAA != nullptr; pIAA = pIAA->Next) 
        {
            // Look for the right interface
            if (pIAA->Ipv6IfIndex != aInstance->InterfaceIndex) continue;

            // Look through all unicast addresses
            for (auto pUnicastAddr = pIAA->FirstUnicastAddress; 
                 pUnicastAddr != nullptr; 
                 pUnicastAddr = pUnicastAddr->Next)
            {
                LPSOCKADDR_IN6 pAddr = (LPSOCKADDR_IN6)pUnicastAddr->Address.lpSockaddr;

                // Copy the necessary parameters
                memcpy(&addrs[AddrCount].mAddress, &pAddr->sin6_addr, sizeof(pAddr->sin6_addr));
                addrs[AddrCount].mPreferredLifetime = pUnicastAddr->PreferredLifetime;
                addrs[AddrCount].mValidLifetime = pUnicastAddr->ValidLifetime;
                addrs[AddrCount].mPrefixLength = pUnicastAddr->OnLinkPrefixLength;

                AddrCount++;
            }

            break;
        }

    error:
        free(pIAAList);
    }
    else
    {
        otLogCritApi("GetAdapterAddresses failed!");
    }

    // Revert the comparment if necessary
    if (RevertCompartmentOnExit)
    {
        (VOID)SetCurrentThreadCompartmentId(OriginalCompartmentID);
    }

    return addrs;
}

OTAPI
ThreadError
otAddUnicastAddress(
    _In_ otInstance *aInstance, 
    const otNetifAddress *aAddress
    )
{
    MIB_UNICASTIPADDRESS_ROW newRow;
    InitializeUnicastIpAddressEntry(&newRow);

    newRow.InterfaceIndex = aInstance->InterfaceIndex;
    newRow.InterfaceLuid = aInstance->InterfaceLuid;
    newRow.Address.si_family = AF_INET6;
    newRow.Address.Ipv6.sin6_family = AF_INET6;
        
    static_assert(sizeof(IN6_ADDR) == sizeof(otIp6Address), "Windows and OpenThread IPv6 Addr Structs must be same size");

    memcpy(&newRow.Address.Ipv6.sin6_addr, &aAddress->mAddress, sizeof(IN6_ADDR));
    newRow.OnLinkPrefixLength = aAddress->mPrefixLength;
    newRow.PreferredLifetime = aAddress->mPreferredLifetime;
    newRow.ValidLifetime = aAddress->mValidLifetime;
    newRow.PrefixOrigin = IpPrefixOriginOther;  // Derived from network XPANID
    newRow.SkipAsSource = FALSE;                // Allow automatic binding to this address (default)

    if (IN6_IS_ADDR_LINKLOCAL(&newRow.Address.Ipv6.sin6_addr))
    {
        newRow.SuffixOrigin = IpSuffixOriginLinkLayerAddress;   // Derived from Extended MAC address
    }
    else
    {
        newRow.SuffixOrigin = IpSuffixOriginRandom;             // Was created randomly
    }

    DWORD dwError = CreateUnicastIpAddressEntry(&newRow);
    if (dwError != ERROR_SUCCESS)
    {
        otLogCritApi("CreateUnicastIpAddressEntry failed %!WINERROR!", dwError);
        return kThreadError_Failed;
    }

    return kThreadError_None;
}

OTAPI
ThreadError
otRemoveUnicastAddress(
    _In_ otInstance *aInstance, 
    const otIp6Address *aAddress
    )
{
    MIB_UNICASTIPADDRESS_ROW deleteRow;
    InitializeUnicastIpAddressEntry(&deleteRow);

    deleteRow.InterfaceIndex = aInstance->InterfaceIndex;
    deleteRow.InterfaceLuid = aInstance->InterfaceLuid;
    deleteRow.Address.si_family = AF_INET6;

    memcpy(&deleteRow.Address.Ipv6.sin6_addr, aAddress, sizeof(IN6_ADDR));
    
    DWORD dwError = DeleteUnicastIpAddressEntry(&deleteRow);
    if (dwError != ERROR_SUCCESS)
    {
        otLogCritApi("DeleteUnicastIpAddressEntry failed %!WINERROR!", dwError);
        return kThreadError_Failed;
    }

    return kThreadError_None;
}

OTAPI
void otSetStateChangedCallback(
    _In_ otInstance *aInstance, 
    _In_ otStateChangedCallback aCallback, 
    _In_ void *aContext
    )
{
    aInstance->ApiHandle->SetCallback(
        aInstance->ApiHandle->StateChangedCallbacks,
        make_tuple(aInstance->InterfaceGuid, aCallback, aContext)
        );
}

OTAPI
ThreadError
otGetActiveDataset(
    _In_ otInstance *aInstance, 
    _Out_ otOperationalDataset *aDataset
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_ACTIVE_DATASET, aDataset));
}

OTAPI
ThreadError
otSetActiveDataset(
    _In_ otInstance *aInstance, 
    const otOperationalDataset *aDataset
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_ACTIVE_DATASET, aDataset));
}

OTAPI
ThreadError
otGetPendingDataset(
    _In_ otInstance *aInstance, 
    _Out_ otOperationalDataset *aDataset
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_PENDING_DATASET, aDataset));
}

OTAPI
ThreadError
otSetPendingDataset(
    _In_ otInstance *aInstance, 
    const otOperationalDataset *aDataset
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_PENDING_DATASET, aDataset));
}

OTAPI 
ThreadError 
otSendActiveGet(
    _In_ otInstance *aInstance, 
    const uint8_t *aTlvTypes, 
    uint8_t aLength
    )
{
    UNREFERENCED_PARAMETER(aInstance);
    UNREFERENCED_PARAMETER(aTlvTypes);
    UNREFERENCED_PARAMETER(aLength);
    return kThreadError_NotImplemented;
}

OTAPI 
ThreadError 
otSendActiveSet(
    _In_ otInstance *aInstance, 
    const otOperationalDataset *aDataset, 
    const uint8_t *aTlvs,
    uint8_t aLength
    )
{
    UNREFERENCED_PARAMETER(aInstance);
    UNREFERENCED_PARAMETER(aDataset);
    UNREFERENCED_PARAMETER(aTlvs);
    UNREFERENCED_PARAMETER(aLength);
    return kThreadError_NotImplemented;
}

OTAPI 
ThreadError 
otSendPendingGet(
    _In_ otInstance *aInstance, 
    const uint8_t *aTlvTypes, 
    uint8_t aLength
    )
{
    UNREFERENCED_PARAMETER(aInstance);
    UNREFERENCED_PARAMETER(aTlvTypes);
    UNREFERENCED_PARAMETER(aLength);
    return kThreadError_NotImplemented;
}

OTAPI 
ThreadError 
otSendPendingSet(
    _In_ otInstance *aInstance, 
    const otOperationalDataset *aDataset, 
    const uint8_t *aTlvs,
    uint8_t aLength
    )
{
    UNREFERENCED_PARAMETER(aInstance);
    UNREFERENCED_PARAMETER(aDataset);
    UNREFERENCED_PARAMETER(aTlvs);
    UNREFERENCED_PARAMETER(aLength);
    return kThreadError_NotImplemented;
}

OTAPI 
uint32_t 
otGetPollPeriod(
    _In_ otInstance *aInstance
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_POLL_PERIOD, &Result);
    return Result;
}

OTAPI 
void 
otSetPollPeriod(
    _In_ otInstance *aInstance, 
    uint32_t aPollPeriod
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_POLL_PERIOD, aPollPeriod);
}

OTAPI
uint8_t 
otGetLocalLeaderWeight(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_LOCAL_LEADER_WEIGHT, &Result);
    return Result;
}

OTAPI
void otSetLocalLeaderWeight(
    _In_ otInstance *aInstance, 
    uint8_t aWeight
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_LOCAL_LEADER_WEIGHT, aWeight);
}

OTAPI 
uint32_t 
otGetLocalLeaderPartitionId(
    _In_ otInstance *aInstance
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_LOCAL_LEADER_PARTITION_ID, &Result);
    return Result;
}

OTAPI 
void 
otSetLocalLeaderPartitionId(
    _In_ otInstance *aInstance, 
    uint32_t aPartitionId
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_LOCAL_LEADER_PARTITION_ID, aPartitionId);
}

OTAPI
ThreadError
otAddBorderRouter(
    _In_ otInstance *aInstance, 
    const otBorderRouterConfig *aConfig
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_ADD_BORDER_ROUTER, aConfig));
}

OTAPI
ThreadError
otRemoveBorderRouter(
    _In_ otInstance *aInstance, 
    const otIp6Prefix *aPrefix
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_REMOVE_BORDER_ROUTER, aPrefix));
}

OTAPI
ThreadError
otAddExternalRoute(
    _In_ otInstance *aInstance, 
    const otExternalRouteConfig *aConfig
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_ADD_EXTERNAL_ROUTE, aConfig));
}

OTAPI
ThreadError
otRemoveExternalRoute(
    _In_ otInstance *aInstance, 
    const otIp6Prefix *aPrefix
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_REMOVE_EXTERNAL_ROUTE, aPrefix));
}

OTAPI
ThreadError
otSendServerData(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_SEND_SERVER_DATA));
}

OTAPI
uint32_t 
otGetContextIdReuseDelay(
    _In_ otInstance *aInstance
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_CONTEXT_ID_REUSE_DELAY, &Result);
    return Result;
}

OTAPI
void 
otSetContextIdReuseDelay(
    _In_ otInstance *aInstance, 
    uint32_t aDelay
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_REMOVE_EXTERNAL_ROUTE, aDelay);
}

OTAPI
uint32_t 
otGetKeySequenceCounter(
    _In_ otInstance *aInstance
    )
{
    uint32_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_KEY_SEQUENCE_COUNTER, &Result);
    return Result;
}

OTAPI
void 
otSetKeySequenceCounter(
    _In_ otInstance *aInstance, 
    uint32_t aKeySequenceCounter
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_REMOVE_EXTERNAL_ROUTE, aKeySequenceCounter);
}

OTAPI
uint8_t otGetNetworkIdTimeout(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_NETWORK_ID_TIMEOUT, &Result);
    return Result;
}

OTAPI
void 
otSetNetworkIdTimeout(
    _In_ otInstance *aInstance, 
    uint8_t aTimeout
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_NETWORK_ID_TIMEOUT, aTimeout);
}

OTAPI
uint8_t 
otGetRouterUpgradeThreshold(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_ROUTER_UPGRADE_THRESHOLD, &Result);
    return Result;
}

OTAPI
void 
otSetRouterUpgradeThreshold(
    _In_ otInstance *aInstance, 
    uint8_t aThreshold
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_ROUTER_UPGRADE_THRESHOLD, aThreshold);
}

OTAPI
ThreadError
otReleaseRouterId(
    _In_ otInstance *aInstance, 
    uint8_t aRouterId
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_RELEASE_ROUTER_ID, aRouterId));
}

OTAPI
ThreadError
otAddMacWhitelist(
    _In_ otInstance *aInstance, 
    const uint8_t *aExtAddr
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_ADD_MAC_WHITELIST, (const otExtAddress*)aExtAddr));
}

OTAPI
ThreadError
otAddMacWhitelistRssi(
    _In_ otInstance *aInstance, 
    const uint8_t *aExtAddr, 
    int8_t aRssi
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(otExtAddress) + sizeof(int8_t)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), aExtAddr, sizeof(otExtAddress));
    memcpy(Buffer + sizeof(GUID) + sizeof(otExtAddress), &aRssi, sizeof(aRssi));
    
    return DwordToThreadError(SendIOCTL(aInstance->ApiHandle, IOCTL_OTLWF_OT_ADD_MAC_WHITELIST, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI
void 
otRemoveMacWhitelist(
    _In_ otInstance *aInstance, 
    const uint8_t *aExtAddr
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_REMOVE_MAC_WHITELIST, (const otExtAddress*)aExtAddr);
}

OTAPI
ThreadError
otGetMacWhitelistEntry(
    _In_ otInstance *aInstance, 
    uint8_t aIndex, 
    _Out_ otMacWhitelistEntry *aEntry
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_WHITELIST_ENTRY, &aIndex, aEntry));
}

OTAPI
void 
otClearMacWhitelist(
    _In_ otInstance *aInstance
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_CLEAR_MAC_WHITELIST);
}

OTAPI
void 
otDisableMacWhitelist(
    _In_ otInstance *aInstance
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_WHITELIST_ENABLED, (BOOLEAN)FALSE);
}

OTAPI
void 
otEnableMacWhitelist(
    _In_ otInstance *aInstance
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_WHITELIST_ENABLED, (BOOLEAN)TRUE);
}

OTAPI
bool 
otIsMacWhitelistEnabled(
    _In_ otInstance *aInstance
    )
{
    BOOLEAN Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_WHITELIST_ENABLED, &Result);
    return Result != FALSE;
}

OTAPI
ThreadError
otBecomeDetached(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_DEVICE_ROLE, (uint8_t)kDeviceRoleDetached));
}

OTAPI
ThreadError
otBecomeChild(
    _In_ otInstance *aInstance, 
    otMleAttachFilter aFilter
    )
{
    uint8_t Role = kDeviceRoleDetached;
    uint8_t Filter = (uint8_t)aFilter;

    BYTE Buffer[sizeof(GUID) + sizeof(Role) + sizeof(Filter)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), &Role, sizeof(Role));
    memcpy(Buffer + sizeof(GUID) + sizeof(Role), &Filter, sizeof(Filter));
    
    return DwordToThreadError(SendIOCTL(aInstance->ApiHandle, IOCTL_OTLWF_OT_DEVICE_ROLE, Buffer, sizeof(Buffer), nullptr, 0));
}

OTAPI
ThreadError
otBecomeRouter(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_DEVICE_ROLE, (uint8_t)kDeviceRoleRouter));
}

OTAPI
ThreadError
otBecomeLeader(
    _In_ otInstance *aInstance
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_DEVICE_ROLE, (uint8_t)kDeviceRoleLeader));
}

OTAPI
ThreadError
otAddMacBlacklist(
    _In_ otInstance *aInstance, 
    const uint8_t *aExtAddr
    )
{
    return DwordToThreadError(SetIOCTL(aInstance, IOCTL_OTLWF_OT_ADD_MAC_BLACKLIST, (const otExtAddress*)aExtAddr));
}

OTAPI
void 
otRemoveMacBlacklist(
    _In_ otInstance *aInstance, 
    const uint8_t *aExtAddr
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_REMOVE_MAC_BLACKLIST, (const otExtAddress*)aExtAddr);
}

OTAPI
ThreadError
otGetMacBlacklistEntry(
    _In_ otInstance *aInstance, 
    uint8_t aIndex, 
    _Out_ otMacBlacklistEntry *aEntry
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_BLACKLIST_ENTRY, &aIndex, aEntry));
}

OTAPI
void 
otClearMacBlacklist(
    _In_ otInstance *aInstance
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_CLEAR_MAC_BLACKLIST);
}

OTAPI
void 
otDisableMacBlacklist(
    _In_ otInstance *aInstance
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_BLACKLIST_ENABLED, (BOOLEAN)FALSE);
}

OTAPI
void 
otEnableMacBlacklist(
    _In_ otInstance *aInstance
    )
{
    (void)SetIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_BLACKLIST_ENABLED, (BOOLEAN)TRUE);
}

OTAPI
bool 
otIsMacBlacklistEnabled(
    _In_ otInstance *aInstance
    )
{
    BOOLEAN Result = 0;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_BLACKLIST_ENABLED, &Result);
    return Result != FALSE;
}

OTAPI 
ThreadError 
otGetAssignLinkQuality(
    _In_ otInstance *aInstance, 
    const uint8_t *aExtAddr, 
    _Out_ uint8_t *aLinkQuality
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_ASSIGN_LINK_QUALITY, (otExtAddress*)aExtAddr, aLinkQuality));
}

OTAPI 
void 
otSetAssignLinkQuality(
    _In_ otInstance *aInstance,
    const uint8_t *aExtAddr, 
    uint8_t aLinkQuality
    )
{
    BYTE Buffer[sizeof(GUID) + sizeof(otExtAddress) + sizeof(uint8_t)];
    memcpy(Buffer, &aInstance->InterfaceGuid, sizeof(GUID));
    memcpy(Buffer + sizeof(GUID), aExtAddr, sizeof(otExtAddress));
    memcpy(Buffer + sizeof(GUID) + sizeof(otExtAddress), &aLinkQuality, sizeof(aLinkQuality));
    (void)SendIOCTL(aInstance->ApiHandle, IOCTL_OTLWF_OT_ASSIGN_LINK_QUALITY, Buffer, sizeof(Buffer), nullptr, 0);
}

OTAPI 
void 
otPlatformReset(
    _In_ otInstance *aInstance
    )
{
    SetIOCTL(aInstance, IOCTL_OTLWF_OT_PLATFORM_RESET);
}

OTAPI
ThreadError
otGetChildInfoById(
    _In_ otInstance *aInstance, 
    uint16_t aChildId, 
    _Out_ otChildInfo *aChildInfo
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_CHILD_INFO_BY_ID, &aChildId, aChildInfo));
}

OTAPI
ThreadError
otGetChildInfoByIndex(
    _In_ otInstance *aInstance, 
    uint8_t aChildIndex, 
    _Out_ otChildInfo *aChildInfo
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_CHILD_INFO_BY_INDEX, &aChildIndex, aChildInfo));
}

OTAPI
otDeviceRole 
otGetDeviceRole(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = kDeviceRoleOffline;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_DEVICE_ROLE, &Result);
    return (otDeviceRole)Result;
}

OTAPI
ThreadError
otGetEidCacheEntry(
    _In_ otInstance *aInstance, 
    uint8_t aIndex, 
    _Out_ otEidCacheEntry *aEntry
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_EID_CACHE_ENTRY, &aIndex, aEntry));
}

OTAPI
ThreadError
otGetLeaderData(
    _In_ otInstance *aInstance, 
    _Out_ otLeaderData *aLeaderData
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_LEADER_DATA, aLeaderData));
}

OTAPI
uint8_t 
otGetLeaderRouterId(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_LEADER_ROUTER_ID, &Result);
    return Result;
}

OTAPI
uint8_t 
otGetLeaderWeight(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_LEADER_WEIGHT, &Result);
    return Result;
}

OTAPI
uint8_t 
otGetNetworkDataVersion(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_NETWORK_DATA_VERSION, &Result);
    return Result;
}

OTAPI
uint32_t 
otGetPartitionId(
    _In_ otInstance *aInstance
    )
{
    uint32_t Result = 0xFFFFFFFF;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_PARTITION_ID, &Result);
    return Result;
}

OTAPI
uint16_t 
otGetRloc16(
    _In_ otInstance *aInstance
    )
{
    uint16_t Result = 0xFFFF;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_RLOC16, &Result);
    return Result;
}

OTAPI
uint8_t 
otGetRouterIdSequence(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_ROUTER_ID_SEQUENCE, &Result);
    return Result;
}

OTAPI
ThreadError
otGetRouterInfo(
    _In_ otInstance *aInstance, 
    uint16_t aRouterId, 
    _Out_ otRouterInfo *aRouterInfo
    )
{
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_ROUTER_INFO, &aRouterId, aRouterInfo));
}

OTAPI 
ThreadError 
otGetParentInfo(
    _In_ otInstance *aInstance, 
    _Out_ otRouterInfo *aParentInfo
    )
{
    static_assert(sizeof(otRouterInfo) == 20, "The size of otRouterInfo should be 20 bytes");
    return DwordToThreadError(QueryIOCTL(aInstance, IOCTL_OTLWF_OT_PARENT_INFO, aParentInfo));
}

OTAPI
uint8_t 
otGetStableNetworkDataVersion(
    _In_ otInstance *aInstance
    )
{
    uint8_t Result = 0xFF;
    (void)QueryIOCTL(aInstance, IOCTL_OTLWF_OT_STABLE_NETWORK_DATA_VERSION, &Result);
    return Result;
}

OTAPI
const otMacCounters*
otGetMacCounters(
    _In_ otInstance *aInstance
    )
{
    otMacCounters* aCounters = (otMacCounters*)malloc(sizeof(otMacCounters));
    if (aCounters)
    {
        if (ERROR_SUCCESS != QueryIOCTL(aInstance, IOCTL_OTLWF_OT_MAC_COUNTERS, aCounters))
        {
            free(aCounters);
            aCounters = nullptr;
        }
    }
    return aCounters;
}

OTAPI
bool 
otIsIp6AddressEqual(
    const otIp6Address *a, 
    const otIp6Address *b
    )
{
    return memcmp(a->mFields.m8, b->mFields.m8, sizeof(otIp6Address)) == 0;
}

OTAPI
ThreadError 
otIp6AddressFromString(
    const char *str, 
    otIp6Address *address
    )
{
    ThreadError error = kThreadError_None;
    uint8_t *dst = reinterpret_cast<uint8_t *>(address->mFields.m8);
    uint8_t *endp = reinterpret_cast<uint8_t *>(address->mFields.m8 + 15);
    uint8_t *colonp = NULL;
    uint16_t val = 0;
    uint8_t count = 0;
    bool first = true;
    char ch;
    uint8_t d;

    memset(address->mFields.m8, 0, 16);

    dst--;

    for (;;)
    {
        ch = *str++;
        d = ch & 0xf;

        if (('a' <= ch && ch <= 'f') || ('A' <= ch && ch <= 'F'))
        {
            d += 9;
        }
        else if (ch == ':' || ch == '\0' || ch == ' ')
        {
            if (count)
            {
                if (dst + 2 > endp)
                {
                    error = kThreadError_Parse;
                    goto exit;
                }
                *(dst + 1) = static_cast<uint8_t>(val >> 8);
                *(dst + 2) = static_cast<uint8_t>(val);
                dst += 2;
                count = 0;
                val = 0;
            }
            else if (ch == ':')
            {
                if (!(colonp == nullptr || first))
                {
                    error = kThreadError_Parse;
                    goto exit;
                }
                colonp = dst;
            }

            if (ch == '\0' || ch == ' ')
            {
                break;
            }

            continue;
        }
        else
        {
            if (!('0' <= ch && ch <= '9'))
            {
                error = kThreadError_Parse;
                goto exit;
            }
        }

        first = false;
        val = static_cast<uint16_t>((val << 4) | d);
        if (!(++count <= 4))
        {
            error = kThreadError_Parse;
            goto exit;
        }
    }

    while (colonp && dst > colonp)
    {
        *endp-- = *dst--;
    }

    while (endp > dst)
    {
        *endp-- = 0;
    }

exit:
    return error;
}

OTAPI 
uint8_t 
otIp6PrefixMatch(
    const otIp6Address *aFirst, 
    const otIp6Address *aSecond
    )
{
    uint8_t rval = 0;
    uint8_t diff;

    for (uint8_t i = 0; i < sizeof(otIp6Address); i++)
    {
        diff = aFirst->mFields.m8[i] ^ aSecond->mFields.m8[i];

        if (diff == 0)
        {
            rval += 8;
        }
        else
        {
            while ((diff & 0x80) == 0)
            {
                rval++;
                diff <<= 1;
            }

            break;
        }
    }

    return rval;
}
