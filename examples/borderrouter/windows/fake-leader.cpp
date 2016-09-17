// border-router.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "fake-leader.hpp"
#include <thread/thread_uris.hpp>
#include <thread/meshcop_tlvs.hpp>
#include <memory>

using namespace Thread::MeshCoP;

FakeLeader::FakeLeader() :
    mCoapHandler(HandleCoapMessage, this)
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
    responseHeader.AppendContentFormatOption(OffMesh::Coap::Header::kApplicationOctetStream);
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
    responseHeader.AppendContentFormatOption(OffMesh::Coap::Header::kApplicationOctetStream);
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