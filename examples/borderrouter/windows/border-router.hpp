#pragma once

#include "br_dtls.hpp"
#include "br_coap_server.hpp"
#include "border-router-socket.hpp"
#include <windows.h>

/**
* @def OPENTHREAD_URI_COMMISSIONER_PETITION
*
* The URI Path for Commissioner Petition
*
*/
#define OPENTHREAD_URI_COMMISSIONER_PETITION  "c/cp"

/**
* @def OPENTHREAD_URI_COMMISSIONER_KEEP_ALIVE
*
* The URI Path for Commissioner Keep Alive
*
*/
#define OPENTHREAD_URI_COMMISSIONER_KEEP_ALIVE  "c/ca"

void getPSKc(const char* passPhrase, const char* networkName, const char* const xPanId, uint8_t* derivedKeyOut);

class BorderRouter
{
public:

    BorderRouter();

    HRESULT Start();
    void Stop();

private:
    static void HandleDtlsReceive(void *aContext, uint8_t *aBuf, uint16_t aLength);
    void HandleDtlsReceive(uint8_t *aBuf, uint16_t aLength);
    
    static ThreadError HandleDtlsSend(void *aContext, const uint8_t *aBuf, uint16_t aLength);
    ThreadError HandleDtlsSend(const uint8_t *aBuf, uint16_t aLength);

    static void HandleCommissionerSocketReceive(void *aContext, uint8_t *aBuf, DWORD aLength);
    void HandleCommissionerSocketReceive(uint8_t *aBuf, DWORD aLength);

    static void HandleThreadSocketReceive(void *aContext, uint8_t *aBuf, DWORD aLength);
    void HandleThreadSocketReceive(uint8_t *aBuf, DWORD aLength);

    // coap handlers
    static void HandleCoapMessage(void *aContext, OffMesh::Coap::Header & aHeader, uint8_t *aMessage, uint16_t aLength, const char* aUriPath);
    void HandleCoapMessage(OffMesh::Coap::Header & aHeader, uint8_t * aMessage, uint16_t aLength, const char* aUriPath);

    OffMesh::MeshCoP::Dtls mDtls;
    BRSocket mCommissionerSocket;
    BRSocket mThreadLeaderSocket;

    OffMesh::Coap::Server mCoap;
    OffMesh::Coap::Resource mCoapHandler;
};