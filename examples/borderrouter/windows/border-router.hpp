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

#define DEFAULT_MESHCOP_PORT 49191

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