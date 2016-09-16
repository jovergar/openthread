// border-router.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <mbedtls/platform.h>
#include "border-router-socket.hpp"

BRSocket::BRSocket()
{
}

HRESULT BRSocket::Initialize(BrSocketReadCallback readCallback, void* clientContext)
{
    mClientReceiveCallback = readCallback;
    mClientContext = clientContext;

    mSocket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (mSocket == INVALID_SOCKET)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    return S_OK;
}

HRESULT BRSocket::Bind(unsigned short port)
{
    SOCKADDR_IN recvAddr = {};
    recvAddr.sin_family = AF_INET;
    recvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    recvAddr.sin_port = htons(port);

    if (SOCKET_ERROR == bind(mSocket, (SOCKADDR*)&recvAddr, sizeof(recvAddr)))
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }
    return S_OK;
}

HRESULT BRSocket::Read()
{
    printf("ReadFromSocket called!\n");

    char recvBuffer[MBEDTLS_SSL_MAX_CONTENT_LEN];
    WSABUF wsaRecvBuffer = { sizeof(recvBuffer), recvBuffer };
    DWORD cbReceived = 0;
    DWORD dwFlags = MSG_PARTIAL;


    int cbSourceAddr = sizeof(mPeerAddr);
    WSAOVERLAPPED overlapped = {};
    WSAEVENT overlappedEvent = WSACreateEvent();
    if (WSA_INVALID_EVENT == overlappedEvent)
    {
        return WSAGetLastError();
    }
    overlapped.hEvent = overlappedEvent;

    bool pending = false;
    if (SOCKET_ERROR == WSARecvFrom(mSocket, &wsaRecvBuffer, 1, &cbReceived, &dwFlags, &mPeerAddr, &cbSourceAddr, &overlapped, nullptr))
    {
        DWORD dwError = WSAGetLastError();
        if (WSA_IO_PENDING == dwError)
        {
            pending = true;
        }
        else
        {
            wprintf(L"We failed to RecvFrom. The error is 0x%x\n", dwError);
            return dwError;
        }
    }

    while (pending)
    {
        wprintf(L"pending. going to call WSAWaitForMultipleEvents!\n");
        DWORD result = WSAWaitForMultipleEvents(1, &overlapped.hEvent, true, INFINITE, true);
        if (result == WSA_WAIT_EVENT_0)
        {
            pending = false;
            if (!WSAGetOverlappedResult(mSocket, &overlapped, &cbReceived, false, &dwFlags))
            {
                return WSAGetLastError();
            }
        }
        else if (result == WSA_WAIT_TIMEOUT)
        {
            return WSA_WAIT_TIMEOUT;
        }
        else if (result == WSA_WAIT_FAILED)
        {
            return WSAGetLastError();
        }
        // in the case of WSA_WAIT_IO_COMPLETION, our event is not yet signaled, and
        // WSAWaitForMultipleEvents needs to be called again
    }

    // at this time we should have some bytes in our buffer and dwFlags should let us know if there is more data to read, which we would read
    // using WSARecvFrom/WSAGetOverlappedResult
    printf("Looks like we got a message! dwFlags is 0x%x, cbReceived is %d\n", dwFlags, cbReceived);
    WSACloseEvent(overlappedEvent);

    mClientReceiveCallback(mClientContext, (uint8_t*)recvBuffer, cbReceived);

    return S_OK;
}

HRESULT BRSocket::Reply(const uint8_t* aBuf, uint16_t aLength)
{
    DWORD result = sendto(mSocket, (char*)aBuf, (int)aLength, 0, &mPeerAddr, sizeof(mPeerAddr));
    if (result == SOCKET_ERROR)
    {
        DWORD wsaError = WSAGetLastError();
        printf("wsaError in Reply occurred. %d\n", wsaError);
        return HRESULT_FROM_WIN32(wsaError);
    }
    else
    {
        printf("wrote %d bytes out of %u in Reply\n", result, aLength);
        return S_OK;
    }
}

HRESULT BRSocket::SendTo(const uint8_t* aBuf, uint16_t aLength, sockaddr_in* peerToSendTo)
{
    DWORD result = sendto(mSocket, (char*)aBuf, (int)aLength, 0, reinterpret_cast<SOCKADDR*>(peerToSendTo), sizeof(*peerToSendTo));
    if (result == SOCKET_ERROR)
    {
        DWORD wsaError = WSAGetLastError();
        printf("wsaError in SendTo occurred. %d\n", wsaError);
        return HRESULT_FROM_WIN32(wsaError);
    }
    else
    {
        printf("wrote %d bytes out of %d in SendTo\n", result, aLength);
        return S_OK;
    }
}