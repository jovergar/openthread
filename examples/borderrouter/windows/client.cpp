// border-router.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <mbedtls/memory_buffer_alloc.h>
#include <thread/meshcop_tlvs.hpp>
#include <ws2tcpip.h>
#include <memory>
#include "client.hpp"
#include "border-router.hpp"

using namespace Thread::MeshCoP;

static inline uint8_t _str1ToHex(const char charTuple) {
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

static inline uint8_t _str2ToHex(const char hexByte[2]) {
    const char hi = hexByte[0];
    const char lo = hexByte[1];
    return (_str1ToHex(hi) * 16) + _str1ToHex(lo);
}

static void printBuffer(char* buffer, int len)
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

static void getPSKc(const char* passPhrase, const char* networkName, const char* const xPanId, uint8_t* derivedKeyOut) {
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


HRESULT Client::Start()
{
    WSADATA wsa;
    HRESULT hr = HRESULT_FROM_WIN32(WSAStartup(MAKEWORD(2, 2), &wsa));
    if (FAILED(hr))
    {
        return hr;
    }

    mSocket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (mSocket == INVALID_SOCKET)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    uint8_t derivedKey[16];
    getPSKc("12SECRETPASSWORD34", "TestNetwork1", "0001020304050607", derivedKey);
    mDtls.SetPsk(derivedKey, sizeof(derivedKey));
    mDtls.Start(true, HandleDtlsReceive, HandleDtlsSend, this);

    while (!mDtls.IsConnected())
    {
        printf("entering the read loop!\n");
        ReadFromSocket();
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
        ReadFromSocket();
    }

    return S_OK;
}

void Client::Stop()
{
    WSACleanup();
}

HRESULT Client::ReadFromSocket()
{
    printf("ReadFromSocket called!\n");

    char recvBuffer[MBEDTLS_SSL_MAX_CONTENT_LEN];
    WSABUF wsaRecvBuffer = { sizeof(recvBuffer), recvBuffer };
    DWORD cbReceived = 0;
    DWORD dwFlags = MSG_PARTIAL;

    SOCKADDR clientAddr;
    int cbSourceAddr = sizeof(clientAddr);
    WSAOVERLAPPED overlapped = {};
    WSAEVENT overlappedEvent = WSACreateEvent();
    if (WSA_INVALID_EVENT == overlappedEvent)
    {
        return WSAGetLastError();
    }
    overlapped.hEvent = overlappedEvent;

    bool pending = false;
    if (SOCKET_ERROR == WSARecvFrom(mSocket, &wsaRecvBuffer, 1, &cbReceived, &dwFlags, &clientAddr, &cbSourceAddr, &overlapped, nullptr))
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
    mDtls.Receive((uint8_t*)recvBuffer, (uint16_t)cbReceived);
    //return dtls_handle_message(ctx, &session, (uint8_t*)recvBuffer, cbReceived);
    return S_OK;
}

// static
void Client::HandleDtlsReceive(void* aContext, uint8_t * aBuf, uint16_t aLength)
{
    printf("static Client::HandleDtlsReceive called!\n");
    Client *obj = reinterpret_cast<Client *>(aContext);
    printf("static Client::HandleDtlsSend called!\n");
    return obj->HandleDtlsReceive(aBuf, aLength);
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
    DWORD result = sendto(mSocket, (char*)aBuf, (int)aLength, 0, (sockaddr*)&sendAddr, sizeof(sendAddr));
    if (result == SOCKET_ERROR)
    {
        DWORD wsaError = WSAGetLastError();
        printf("wsaError in send_to_peer occurred. %d\n", wsaError);
        return ThreadError::kThreadError_Error;
    }
    else
    {
        printf("wrote %d bytes out of %d in send_to_peer\n", result, aLength);
        return ThreadError::kThreadError_None;
    }
}
