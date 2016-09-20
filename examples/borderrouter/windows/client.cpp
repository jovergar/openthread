/*
*  Copyright (c) 2016, Nest Labs, Inc.
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

#include "stdafx.h"
#include <mbedtls/memory_buffer_alloc.h>
#include <thread/meshcop_tlvs.hpp>
#include <ws2tcpip.h>
#include <memory>
#include "client.hpp"
#include "border-router.hpp"

using namespace Thread::MeshCoP;

HRESULT Client::Start()
{
    WSADATA wsa;
    HRESULT hr = HRESULT_FROM_WIN32(WSAStartup(MAKEWORD(2, 2), &wsa));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = mBorderRouterSocket.Initialize(HandleBorderRouterSocketReceive, this);
    if (FAILED(hr))
    {
        return hr;
    }

    uint8_t derivedKey[16];
    getPSKc("12SECRETPASSWORD34", "TestNetwork1", "0001020304050607", derivedKey);
    mDtls.SetPsk(derivedKey, sizeof(derivedKey));
    mDtls.Start(true, HandleDtlsReceive, HandleDtlsSend, this);

    while (!mDtls.IsConnected())
    {
        printf("entering the read loop!\n");
        mBorderRouterSocket.Read();
    }

    // once we are connected, we don't want to read infinitely anymore, we want to send the comm_pet

    for (size_t i = 0; i < sizeof(mCoapToken); i++)
    {
        mCoapToken[i] = rand() & 0xff;
    }

    CommissionerIdTlv commissionerId;
    OffMesh::Coap::Header header;
    header.Init();
    header.SetVersion(1);
    header.SetType(OffMesh::Coap::Header::kTypeConfirmable);
    header.SetCode(OffMesh::Coap::Header::kCodePost);
    header.SetMessageId(++mCoapMessageId);
    header.SetToken(mCoapToken, sizeof(mCoapToken));
    header.AppendUriPathOptions(OPENTHREAD_URI_COMMISSIONER_PETITION);
    header.AppendContentFormatOption(OffMesh::Coap::Header::kApplicationOctetStream);
    header.Finalize();
    commissionerId.Init();
    commissionerId.SetCommissionerId("Windows Test Commissioner");

    uint16_t requiredSize = header.GetLength() + sizeof(commissionerId);
    auto messageBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[requiredSize]);
    if (messageBuffer == nullptr)
    {
        // failed to alloc, return
        return E_OUTOFMEMORY;
    }

    memcpy_s(messageBuffer.get(), requiredSize, header.GetBytes(), header.GetLength());
    uint16_t offset = header.GetLength();
    memcpy_s(messageBuffer.get() + offset, requiredSize - offset, &commissionerId, sizeof(commissionerId));

    mDtls.Send(messageBuffer.get(), requiredSize);

    while (1)
    {
        mBorderRouterSocket.Read();
    }

    return S_OK;
}

void Client::Stop()
{
    WSACleanup();
}

// static
void Client::HandleBorderRouterSocketReceive(void* aContext, uint8_t* aBuf, DWORD aLength)
{
    static_cast<Client*>(aContext)->HandleBorderRouterSocketReceive(aBuf, aLength);
}

void Client::HandleBorderRouterSocketReceive(uint8_t* aBuf, DWORD aLength)
{
    printf("FakeLeader::HandleBorderRouterSocketReceive entered\n");
    mDtls.Receive(aBuf, static_cast<uint16_t>(aLength));
}

// static
void Client::HandleDtlsReceive(void* aContext, uint8_t* aBuf, uint16_t aLength)
{
    printf("static Client::HandleDtlsReceive called!\n");
    static_cast<Client*>(aContext)->HandleDtlsReceive(aBuf, aLength);
}

void Client::HandleDtlsReceive(uint8_t* aBuf, uint16_t aLength)
{
    printf("Client::HandleDtlsReceive called!\n");
    OffMesh::Coap::Header receiveHeader;
    receiveHeader.FromBytes(aBuf, aLength);
    uint16_t messageId = receiveHeader.GetMessageId();
    printf("messageId from header is %d, hoping for %d\n", messageId, mCoapMessageId);

    StateTlv state;

    memcpy_s(&state, sizeof(state), aBuf + receiveHeader.GetLength(), sizeof(state));
    auto acceptStatus = state.GetState();
    if (acceptStatus == StateTlv::kAccept)
    {
        printf("we were accepted!\n");
    }
    else
    {
        printf("we were not accepted!\n");
    }
}

ThreadError Client::HandleDtlsSend(void * aContext, const uint8_t * aBuf, uint16_t aLength)
{
    Client *obj = reinterpret_cast<Client *>(aContext);
    printf("static Client::HandleDtlsSend called!\n");
    return obj->HandleDtlsSend(aBuf, aLength);
}

ThreadError Client::HandleDtlsSend(const uint8_t * aBuf, uint16_t aLength)
{
    printf("Client::HandleDtlsSend called!\n");
    SOCKADDR_IN sendAddr = {};
    sendAddr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sendAddr.sin_addr);
    sendAddr.sin_port = htons(19779);

    HRESULT hr = mBorderRouterSocket.SendTo(aBuf, aLength, &sendAddr);
    if (FAILED(hr))
    {
        return ThreadError::kThreadError_Error;
    }
    else
    {
        return ThreadError::kThreadError_None;
    }
}
