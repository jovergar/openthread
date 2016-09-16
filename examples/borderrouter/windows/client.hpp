#pragma once

#include "br_dtls.hpp"
#include "br_coap_server.hpp"
#include <windows.h>

class Client
{
public:
    HRESULT Start();
    void Stop();
    HRESULT ReadFromSocket();

private:
    static void HandleDtlsReceive(void *aContext, uint8_t *aBuf, uint16_t aLength);
    void HandleDtlsReceive(uint8_t *aBuf, uint16_t aLength);

    static ThreadError HandleDtlsSend(void *aContext, const uint8_t *aBuf, uint16_t aLength);
    ThreadError HandleDtlsSend(const uint8_t *aBuf, uint16_t aLength);

    OffMesh::MeshCoP::Dtls mDtls;
    SOCKET mSocket;
    OffMesh::Coap::Server mCoap;

    uint8_t mCoapToken[2];
    uint16_t mCoapMessageId;
};