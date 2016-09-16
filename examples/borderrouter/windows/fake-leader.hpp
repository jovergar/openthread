#pragma once

#include "br_coap_server.hpp"
#include "fake-leader.hpp"
#include "border-router-socket.hpp"
#include <windows.h>
#include <ws2tcpip.h>

#define THREAD_LEADER_PORT 19780
#define THREAD_LEADER_ADDR "127.0.0.1"

class FakeLeader
{
public:

    FakeLeader();

    HRESULT Start();

    static void HandleBorderRouterSocketReceive(void *aContext, uint8_t *aBuf, DWORD aLength);
    void HandleBorderRouterSocketReceive(uint8_t *aBuf, DWORD aLength);

    static void HandleCoapMessage(void *aContext, OffMesh::Coap::Header & aHeader, uint8_t *aMessage, uint16_t aLength, const char* aUriPath);

    void HandleLeaderPetition(OffMesh::Coap::Header & aHeader, uint8_t * aMessage, uint16_t aLength);
    void HandleLeaderKeepAlive(OffMesh::Coap::Header & aHeader, uint8_t * aMessage, uint16_t aLength);

private:

    BRSocket mBorderRouterSocket;
    OffMesh::Coap::Server mCoap;
    OffMesh::Coap::Resource mCoapHandler;
    uint16_t mSessionId;
};