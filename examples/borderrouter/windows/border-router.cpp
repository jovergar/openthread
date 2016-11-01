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
#include <thread/thread_uris.hpp>
#include <crypto/mbedtls.hpp>
#include <memory>
#include "border-router.hpp"
#include "client.hpp"
#include "fake-leader.hpp"

#define MBED_MEMORY_BUF_SIZE  (2048 * sizeof(void*))

extern "C" void otSignalTaskletPending(otInstance *)
{
}

inline uint8_t _str1ToHex(const char charTuple) {
    if ('0' <= charTuple && charTuple <= '9') {
        return (uint8_t)(charTuple - '0');
    }
    else if ('A' <= charTuple && charTuple <= 'F') {
        return (uint8_t)(10 + (charTuple - 'A'));
    }
    else if ('a' <= charTuple && charTuple <= 'f') {
        return (uint8_t)(10 + (charTuple - 'a'));
    }
    return 0;
}

inline uint8_t _str2ToHex(const char hexByte[2]) {
    const char hi = hexByte[0];
    const char lo = hexByte[1];
    return (_str1ToHex(hi) * 16) + _str1ToHex(lo);
}

void printBuffer(char* buffer, int len)
{
    for (int i = 0; i < len; i++)
    {
        printf("%02x", (unsigned char)*buffer++);
        if (i % 4 == 3)
        {
            printf(" ");
        }
    }
    printf("\n");
}

void getPSKc(const char* passPhrase, const char* networkName, const char* const xPanId, uint8_t* derivedKeyOut) {
    const char* saltPrefix = "Thread";
    const size_t preLen = strlen(saltPrefix);
    const size_t xpiLen = xPanId ? strlen(xPanId) / 2 : 0;
    const size_t nwLen = strlen(networkName);
    size_t saltLen = preLen + xpiLen + nwLen;

    uint8_t* salt = new uint8_t[saltLen];
    memset(salt, 0, saltLen);

    memcpy_s((char*)salt, saltLen, saltPrefix, preLen);

    size_t  i;
    for (i = 0; i < xpiLen; i++) {
        uint8_t byteVal = _str2ToHex(xPanId + (2 * i));
        salt[preLen + i] = byteVal;
    }

    memcpy_s((char*)(salt + preLen + xpiLen), nwLen, networkName, nwLen);

    // Get a handle to the algorithm provider
    BCRYPT_ALG_HANDLE hKeyPbkdf2AlgoProv = nullptr;
    NTSTATUS ntStatus = BCryptOpenAlgorithmProvider(
        &hKeyPbkdf2AlgoProv,
        BCRYPT_PBKDF2_ALGORITHM,
        nullptr,
        0);

    if (!BCRYPT_SUCCESS(ntStatus))
    {
        printf("open algorithm provider failed, 0x%x!\n", ntStatus);
        return;
    }

    BCRYPT_ALG_HANDLE hKeyAesCmacAlgoProv = nullptr;
    ntStatus = BCryptOpenAlgorithmProvider(
        &hKeyAesCmacAlgoProv,
        BCRYPT_AES_CMAC_ALGORITHM,
        nullptr,
        0);


    BCRYPT_HASH_HANDLE  hHash = nullptr;
    uint8_t zeroKey[16] = {};
    ntStatus = BCryptCreateHash(hKeyAesCmacAlgoProv, &hHash, nullptr, 0, zeroKey, sizeof(zeroKey), 0);
    if (!BCRYPT_SUCCESS(ntStatus))
    {
        printf("BCryptCreateHash failed, 0x%x!\n", ntStatus);
        return;
    }

    ntStatus = BCryptHashData(hHash, (PUCHAR)passPhrase, (ULONG)strlen(passPhrase), 0);
    if (!BCRYPT_SUCCESS(ntStatus))
    {
        printf("BCryptHashData failed, 0x%x!\n", ntStatus);
        return;
    }

    BYTE    res1[128];
    ULONG   rlen = 16;
    ntStatus = BCryptFinishHash(hHash, res1, rlen, 0);
    if (!BCRYPT_SUCCESS(ntStatus))
    {
        printf("BCryptFinishHash failed, 0x%x!\n", ntStatus);
        return;
    }

    ntStatus = BCryptDestroyHash(hHash);
    if (!BCRYPT_SUCCESS(ntStatus))
    {
        printf("BCryptFinishHash failed, 0x%x!\n", ntStatus);
        return;
    }

    BCryptBufferDesc    ParamList;
    BCryptBuffer        pParamBuffer[3] = {};

    pParamBuffer[0].BufferType = KDF_HASH_ALGORITHM;
    pParamBuffer[0].cbBuffer = (ULONG)wcslen(BCRYPT_AES_CMAC_ALGORITHM) * sizeof(WCHAR);
    pParamBuffer[0].pvBuffer = (void*)BCRYPT_AES_CMAC_ALGORITHM;

    ULONGLONG ulIteration = 16384;
    pParamBuffer[1].BufferType = KDF_ITERATION_COUNT;
    pParamBuffer[1].cbBuffer = sizeof(ulIteration);
    pParamBuffer[1].pvBuffer = &ulIteration;

    pParamBuffer[2].BufferType = KDF_SALT;
    pParamBuffer[2].cbBuffer = (ULONG)saltLen;
    pParamBuffer[2].pvBuffer = salt;

    ParamList.cBuffers = 3;
    ParamList.pBuffers = pParamBuffer;
    ParamList.ulVersion = BCRYPTBUFFER_VERSION;

    BCRYPT_KEY_HANDLE hKeySymmetricKey;
    ntStatus = BCryptGenerateSymmetricKey(
        hKeyPbkdf2AlgoProv,
        &hKeySymmetricKey,
        nullptr,
        0,
        res1,
        16,
        0);

    if (!BCRYPT_SUCCESS(ntStatus))
    {
        printf("gen symmetrickey failed, 0x%x\n", ntStatus);
        return;
    }

    DWORD cbResult = 0;
    ntStatus = BCryptKeyDerivation(hKeySymmetricKey,
        &ParamList,
        derivedKeyOut,
        16,
        &cbResult,
        0
    );


    if (!BCRYPT_SUCCESS(ntStatus))
    {
        printf("Derivekey failed, 0x%x\n", ntStatus);
        return;
    }

    BCryptCloseAlgorithmProvider(hKeyPbkdf2AlgoProv, 0);
    BCryptCloseAlgorithmProvider(hKeyAesCmacAlgoProv, 0);

    delete[] salt;

    printf("PSKc Stretched Key:\n");
    printBuffer((char*)derivedKeyOut, 16);
}

static unsigned char sMemoryBuf[Thread::Crypto::MbedTls::kMemorySize];

int main()
{
    mbedtls_memory_buffer_alloc_init(sMemoryBuf, sizeof(sMemoryBuf));
    BorderRouter router;
    router.Start();
    //FakeLeader leader;
    //leader.Start();
    //Client client;
    //client.Start();
}

BorderRouter::BorderRouter() :
    mCoapHandler(HandleCoapMessage, this)
{
    mCoap.AddResource(mCoapHandler);
}

HRESULT BorderRouter::Start()
{
    WSADATA wsa;
    HRESULT hr = HRESULT_FROM_WIN32(WSAStartup(MAKEWORD(2, 2), &wsa));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = mCommissionerSocket.Initialize(HandleCommissionerSocketReceive, this);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = mThreadLeaderSocket.Initialize(HandleThreadSocketReceive, this);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = mCommissionerSocket.Bind(DEFAULT_MESHCOP_PORT);
    if (FAILED(hr))
    {
        return hr;
    }

    uint8_t derivedKey[16];
    getPSKc("12SECRETPASSWORD34", "TestNetwork1", "0001020304050607", derivedKey);
    mDtls.SetPsk(derivedKey, sizeof(derivedKey));
    mDtls.Start(false, HandleDtlsReceive, HandleDtlsSend, this);

    while (1)
    {
        printf("entering the read loop!\n");
        mCommissionerSocket.Read();
    }

    return S_OK;
}

void BorderRouter::Stop()
{
    WSACleanup();
}

// static
void BorderRouter::HandleCommissionerSocketReceive(void *aContext, uint8_t *aBuf, DWORD aLength)
{
    static_cast<BorderRouter *>(aContext)->HandleCommissionerSocketReceive(aBuf, aLength);
}

void BorderRouter::HandleCommissionerSocketReceive(uint8_t *aBuf, DWORD aLength)
{
    // just got something from the commissioner socket, need to decrypt it (or continue the DTLS handshake)
    if (!mDtls.IsConnected())
    {
        // The DTLS server requires that we set some client ID, or the handshake will fail. The documentation
        // states that it is usually an ip/port pair (something that identifies the peer on the transport).
        // Set it here.
        SOCKADDR currentPeer;
        mCommissionerSocket.GetLastPeer(&currentPeer);
        mDtls.SetClientId(reinterpret_cast<uint8_t*>(&currentPeer), sizeof(currentPeer));
    }
    mDtls.Receive(aBuf, static_cast<uint16_t>(aLength));
}

// static
void BorderRouter::HandleThreadSocketReceive(void *aContext, uint8_t *aBuf, DWORD aLength)
{
    static_cast<BorderRouter *>(aContext)->HandleThreadSocketReceive(aBuf, aLength);
}

void BorderRouter::HandleThreadSocketReceive(uint8_t* aBuf, DWORD aLength)
{
    // just got something from the thread socket. it will be a reply to something
    // we sent to the leader. replies don't have coap URIs so if the message format
    // is the same, we can just forward it directly as is
    //
    // currently, all responses are the same, so we just forward over DTLS to
    // the commissioner
    mDtls.Send(aBuf, static_cast<uint16_t>(aLength));
}

// static
void BorderRouter::HandleDtlsReceive(void * aContext, uint8_t * aBuf, uint16_t aLength)
{
    static_cast<BorderRouter *>(aContext)->HandleDtlsReceive(aBuf, aLength);
}

void BorderRouter::HandleDtlsReceive(uint8_t *aBuf, uint16_t aLength)
{
    printf("BorderRouter::HandleDtlsReceive called! The length is %d\n", aLength);
    mCoap.Receive(aBuf, aLength);
}

ThreadError BorderRouter::HandleDtlsSend(void* aContext, const uint8_t* aBuf, uint16_t aLength)
{
    return static_cast<BorderRouter *>(aContext)->HandleDtlsSend(aBuf, aLength);
}

ThreadError BorderRouter::HandleDtlsSend(const uint8_t* aBuf, uint16_t aLength)
{
    printf("static BorderRouter::HandleDtlsSend called!\n");
    HRESULT hr = mCommissionerSocket.Reply(aBuf, aLength);
    if (FAILED(hr))
    {
        return ThreadError::kThreadError_Error;
    }
    else
    {
        return ThreadError::kThreadError_None;
    }
}

void BorderRouter::HandleCoapMessage(void* aContext, OffMesh::Coap::Header& aHeader,
                                     uint8_t* aMessage, uint16_t aLength, const char* aUriPath)
{
    static_cast<BorderRouter*>(aContext)->HandleCoapMessage(aHeader, aMessage, aLength, aUriPath);
}

void BorderRouter::HandleCoapMessage(OffMesh::Coap::Header& aRequestHeader, uint8_t* aBuf,
                                     uint16_t aLength, const char* aUriPath)
{
    printf("BorderRouter::HandleCoapMessage called with URI %s!\n", aUriPath);

    const char* destinationUri = nullptr;
    if (strcmp(aUriPath, OPENTHREAD_URI_COMMISSIONER_PETITION) == 0)
    {
        destinationUri = OPENTHREAD_URI_LEADER_PETITION;
    }
    else if (strcmp(aUriPath, OPENTHREAD_URI_COMMISSIONER_KEEP_ALIVE) == 0)
    {
        destinationUri = OPENTHREAD_URI_LEADER_KEEP_ALIVE;
    }
    else if (strcmp(aUriPath, OPENTHREAD_URI_ACTIVE_GET) == 0 ||
             strcmp(aUriPath, OPENTHREAD_URI_ACTIVE_SET) == 0 ||
             strcmp(aUriPath, OPENTHREAD_URI_PENDING_GET) == 0 ||
             strcmp(aUriPath, OPENTHREAD_URI_PENDING_SET) == 0)
    {
        // these URIs don't need to be modified, send them as is
        destinationUri = aUriPath;
    }
    else
    {
        printf("BorderRouter::HandleCoapMessage unknown URI received: %s, ignoring", aUriPath);
        return;
    }

    sockaddr_in mThreadLeaderAddress;
    mThreadLeaderAddress.sin_family = AF_INET;
    // if part of this code becomes permanent, check return value
    inet_pton(AF_INET, THREAD_LEADER_ADDR, &mThreadLeaderAddress.sin_addr);
    mThreadLeaderAddress.sin_port = htons(THREAD_LEADER_PORT);
    // TODO: query thread leader address instead of hardcoding something

    OffMesh::Coap::Header header;
    header.Init();
    header.SetVersion(1);
    header.SetType(aRequestHeader.GetType());
    header.SetCode(aRequestHeader.GetCode());
    header.SetMessageId(aRequestHeader.GetMessageId());
    header.SetToken(aRequestHeader.GetToken(), aRequestHeader.GetTokenLength());
    header.AppendUriPathOptions(destinationUri);
    header.Finalize();

    uint8_t requiredSize = header.GetLength() + aLength;
    auto messageBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[requiredSize]);
    if (messageBuffer == nullptr)
    {
        // failed to alloc, return
        return;
    }

    memcpy_s(messageBuffer.get(), requiredSize, header.GetBytes(), header.GetLength());
    memcpy_s(messageBuffer.get() + header.GetLength(), aLength, aBuf, aLength);

    mThreadLeaderSocket.SendTo(messageBuffer.get(), requiredSize, &mThreadLeaderAddress);
    mThreadLeaderSocket.Read();
}
