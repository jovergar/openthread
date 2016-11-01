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
#include "fake-leader.hpp"
#include <thread/thread_uris.hpp>
#include <thread/meshcop_tlvs.hpp>
#include <memory>

using namespace Thread::MeshCoP;

FakeLeader::FakeLeader() :
    mCoapHandler(HandleCoapMessage, this),
    mNetwork(Tlv::kActiveTimestamp)
{
    mCoap.AddResource(mCoapHandler);
}

HRESULT FakeLeader::Start()
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

    hr = mBorderRouterSocket.Bind(THREAD_LEADER_PORT);
    if (FAILED(hr))
    {
        return hr;
    }

    while (1)
    {
        printf("entering the read loop!\n");
        mBorderRouterSocket.Read();
    }

    return S_OK;
}

// static
void FakeLeader::HandleBorderRouterSocketReceive(void* aContext, uint8_t* aBuf, DWORD aLength)
{
    static_cast<FakeLeader*>(aContext)->HandleBorderRouterSocketReceive(aBuf, aLength);
}

void FakeLeader::HandleBorderRouterSocketReceive(uint8_t* aBuf, DWORD aLength)
{
    printf("FakeLeader::HandleBorderRouterSocketReceive entered\n");
    mCoap.Receive(aBuf, static_cast<uint16_t>(aLength));
}

void FakeLeader::HandleCoapMessage(void* aContext, OffMesh::Coap::Header& aHeader,
                                   uint8_t* aMessage, uint16_t aLength, const char* aUriPath)
{
    FakeLeader *obj = static_cast<FakeLeader*>(aContext);

    if (strcmp(aUriPath, OPENTHREAD_URI_LEADER_PETITION) == 0)
    {
        obj->HandleLeaderPetition(aHeader, aMessage, aLength);
    }
    else if (strcmp(aUriPath, OPENTHREAD_URI_LEADER_KEEP_ALIVE) == 0)
    {
        obj->HandleLeaderKeepAlive(aHeader, aMessage, aLength);
    }
    else if (strcmp(aUriPath, OPENTHREAD_URI_ACTIVE_GET) == 0 ||
             strcmp(aUriPath, OPENTHREAD_URI_PENDING_GET) == 0)
    {
        // our fake leader doesn't do pending datasets. just treat it like
        // an active set
        obj->HandleActiveGet(aHeader, aMessage, aLength);
    }
    else if (strcmp(aUriPath, OPENTHREAD_URI_ACTIVE_SET) == 0 ||
             strcmp(aUriPath, OPENTHREAD_URI_PENDING_SET) == 0)
    {
        // our fake leader doesn't do pending datasets. just treat it like
        // an active set
        obj->HandleActiveSet(aHeader, aMessage, aLength);
    }
    else
    {
        printf("Unknown URI received: %s, ignoring", aUriPath);
    }
}

void FakeLeader::HandleLeaderPetition(OffMesh::Coap::Header& aRequestHeader, uint8_t* aBuf, uint16_t aLength)
{
    printf("FakeLeader::HandleLeaderPetition entered\n");
    // build a coap response that says yes and send it to the border router

    StateTlv state;
    CommissionerSessionIdTlv sessionId;

    OffMesh::Coap::Header responseHeader;
    responseHeader.Init();
    responseHeader.SetVersion(1);
    responseHeader.SetType(OffMesh::Coap::Header::kTypeAcknowledgment);
    responseHeader.SetCode(OffMesh::Coap::Header::kCodeChanged);
    responseHeader.SetMessageId(aRequestHeader.GetMessageId());
    responseHeader.SetToken(aRequestHeader.GetToken(), aRequestHeader.GetTokenLength());
    responseHeader.Finalize();

    state.Init();
    state.SetState(StateTlv::kAccept);

    sessionId.Init();
    sessionId.SetCommissionerSessionId(++mSessionId);

    uint16_t requiredSize = responseHeader.GetLength() + sizeof(state) + sizeof(sessionId);
    auto messageBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[requiredSize]);
    if (messageBuffer == nullptr)
    {
        // failed to alloc, return
        return;
    }

    memcpy_s(messageBuffer.get(), requiredSize, responseHeader.GetBytes(), responseHeader.GetLength());
    uint16_t offset = responseHeader.GetLength();
    memcpy_s(messageBuffer.get() + offset, requiredSize - offset, &state, sizeof(state));
    offset += sizeof(state);
    memcpy_s(messageBuffer.get() + offset, requiredSize - offset, &sessionId, sizeof(sessionId));

    mBorderRouterSocket.Reply(messageBuffer.get(), requiredSize);
}

void FakeLeader::HandleLeaderKeepAlive(OffMesh::Coap::Header& aRequestHeader, uint8_t* aBuf, uint16_t aLength)
{
    printf("FakeLeader::HandleLeaderPetition entered\n");
    // build a coap response that says yes and send it to the border router

    StateTlv state;
    CommissionerSessionIdTlv sessionId;

    OffMesh::Coap::Header responseHeader;
    responseHeader.Init();
    responseHeader.SetVersion(1);
    responseHeader.SetType(OffMesh::Coap::Header::kTypeAcknowledgment);
    responseHeader.SetCode(OffMesh::Coap::Header::kCodeChanged);
    responseHeader.SetMessageId(aRequestHeader.GetMessageId());
    responseHeader.SetToken(aRequestHeader.GetToken(), aRequestHeader.GetTokenLength());
    responseHeader.Finalize();

    state.Init();
    state.SetState(StateTlv::kAccept);

    sessionId.Init();
    sessionId.SetCommissionerSessionId(mSessionId);

    uint16_t requiredSize = responseHeader.GetLength() + sizeof(state) + sizeof(sessionId);
    auto messageBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[requiredSize]);
    if (messageBuffer == nullptr)
    {
        // failed to alloc, return
        return;
    }

    memcpy_s(messageBuffer.get(), requiredSize, responseHeader.GetBytes(), responseHeader.GetLength());
    uint16_t offset = responseHeader.GetLength();
    memcpy_s(messageBuffer.get() + offset, requiredSize - offset, &state, sizeof(state));
    offset += sizeof(state);
    memcpy_s(messageBuffer.get() + offset, requiredSize - offset, &sessionId, sizeof(sessionId));

    mBorderRouterSocket.Reply(messageBuffer.get(), requiredSize);
}

void FakeLeader::HandleActiveGet(OffMesh::Coap::Header& aRequestHeader, uint8_t* aBuf, uint16_t aLength)
{
    printf("FakeLeader::HandleActiveGet entered\n");

    OffMesh::Coap::Header responseHeader;
    responseHeader.Init();
    responseHeader.SetVersion(1);
    responseHeader.SetType(OffMesh::Coap::Header::kTypeAcknowledgment);
    responseHeader.SetCode(OffMesh::Coap::Header::kCodeChanged);
    responseHeader.SetMessageId(aRequestHeader.GetMessageId());
    responseHeader.SetToken(aRequestHeader.GetToken(), aRequestHeader.GetTokenLength());
    responseHeader.Finalize();

    // if the payload includes a Get TLV, then we send just those parameters. Otherwise we must send
    // the entire active operation dataset

    uint8_t sizeOfDataSetPayload = 0;
    bool needToSendWholeDataSet = false;
    uint8_t requestedTlvs[Dataset::kMaxSize];

    if (aLength == 0)
    {
        // have to send whole dataset
        sizeOfDataSetPayload = mNetwork.GetSize();
        needToSendWholeDataSet = true;
    }
    else
    {
        // assume this is a well formatted Get TLV. A get TLV is a list of 8 bit type identifiers
        Tlv tlv;
        memcpy_s(&tlv, sizeof(tlv), aBuf, sizeof(tlv));
        uint8_t length = tlv.GetLength();

        memcpy_s(requestedTlvs, sizeof(requestedTlvs), aBuf + sizeof(tlv), length);
        sizeOfDataSetPayload = length;
    }

    uint16_t requiredSize = responseHeader.GetLength() + sizeOfDataSetPayload;
    auto messageBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[requiredSize]);
    if (messageBuffer == nullptr)
    {
        // failed to alloc, return
        return;
    }

    memcpy_s(messageBuffer.get(), requiredSize, responseHeader.GetBytes(), responseHeader.GetLength());
    uint16_t offset = responseHeader.GetLength();
    if (needToSendWholeDataSet)
    {
        memcpy_s(messageBuffer.get() + offset, requiredSize - offset, mNetwork.GetBytes(), sizeOfDataSetPayload);
    }
    else
    {
        Tlv* tlv;
        for (uint8_t index = 0; index < sizeOfDataSetPayload; index++)
        {
            tlv = mNetwork.Get(static_cast<Tlv::Type>(requestedTlvs[index]));
            if (tlv != nullptr)
            {
                memcpy_s(messageBuffer.get() + offset, requiredSize - offset, tlv, sizeof(Tlv) + tlv->GetLength());
                offset += sizeof(Tlv) + tlv->GetLength();
            }
        }
    }
    

    mBorderRouterSocket.Reply(messageBuffer.get(), requiredSize);
}

void FakeLeader::HandleActiveSet(OffMesh::Coap::Header& aRequestHeader, uint8_t* aBuf, uint16_t aLength)
{
    printf("FakeLeader::HandleActiveSet entered\n");

    // The first TLV might be a commissioner session ID TLV, or an active timestamp TLV.
    // This fake leader isn't going to validate either of them, but we do need to figure out
    // where those 1/2 TLVs end so we can get to the active operational dataset TLVs that follow
    // A real leader would do something with the timestamp but we'll ignore that
    bool haveFoundActiveTimestampTlv = false;
    uint16_t offsetOfReceivedBuffer = 0;
    while (!haveFoundActiveTimestampTlv)
    {
        Tlv tlv;
        memcpy_s(&tlv, sizeof(tlv), aBuf + offsetOfReceivedBuffer, sizeof(tlv));
        
        if (tlv.GetType() == Tlv::kActiveTimestamp)
        {
            haveFoundActiveTimestampTlv = true;
        }

        offsetOfReceivedBuffer += sizeof(tlv) + tlv.GetLength();
    }

    // now set the rest of the TLVs
    while (offsetOfReceivedBuffer < aLength)
    {
        Tlv tlv;
        memcpy_s(&tlv, sizeof(tlv), aBuf + offsetOfReceivedBuffer, sizeof(tlv));
        mNetwork.Set(tlv);
        offsetOfReceivedBuffer += sizeof(tlv) + tlv.GetLength();
    }

    // tell the client we accepted all TLV by sending a state TLV with "Accept"

    StateTlv state;

    OffMesh::Coap::Header responseHeader;
    responseHeader.Init();
    responseHeader.SetVersion(1);
    responseHeader.SetType(OffMesh::Coap::Header::kTypeAcknowledgment);
    responseHeader.SetCode(OffMesh::Coap::Header::kCodeChanged);
    responseHeader.SetMessageId(aRequestHeader.GetMessageId());
    responseHeader.SetToken(aRequestHeader.GetToken(), aRequestHeader.GetTokenLength());
    responseHeader.Finalize();

    state.Init();
    state.SetState(StateTlv::kAccept);

    uint16_t requiredSize = responseHeader.GetLength() + sizeof(state);
    auto messageBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[requiredSize]);
    if (messageBuffer == nullptr)
    {
        // failed to alloc, return
        return;
    }

    memcpy_s(messageBuffer.get(), requiredSize, responseHeader.GetBytes(), responseHeader.GetLength());
    uint16_t offset = responseHeader.GetLength();
    memcpy_s(messageBuffer.get() + offset, requiredSize - offset, &state, sizeof(state));

    mBorderRouterSocket.Reply(messageBuffer.get(), requiredSize);
}