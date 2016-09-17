#pragma once

#include "br_dtls.hpp"
#include "br_coap_server.hpp"
#include "border-router-socket.hpp"
#include <windows.h>

class Client
{
public:

    HRESULT Start();
    void Stop();

private:

    static void HandleBorderRouterSocketReceive(void *aContext, uint8_t *aBuf, DWORD aLength);
    void HandleBorderRouterSocketReceive(uint8_t *aBuf, DWORD aLength);

    static void HandleDtlsReceive(void *aContext, uint8_t *aBuf, uint16_t aLength);
    void HandleDtlsReceive(uint8_t *aBuf, uint16_t aLength);

    static ThreadError HandleDtlsSend(void *aContext, const uint8_t *aBuf, uint16_t aLength);
    ThreadError HandleDtlsSend(const uint8_t *aBuf, uint16_t aLength);

    BRSocket mBorderRouterSocket;
    OffMesh::MeshCoP::Dtls mDtls;
    OffMesh::Coap::Server mCoap;

    uint8_t mCoapToken[2];
    uint16_t mCoapMessageId;
};