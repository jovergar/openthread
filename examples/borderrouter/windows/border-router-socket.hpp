#pragma once

#include <windows.h>

typedef void(*BrSocketReadCallback)(void* aContext, uint8_t* aBuf, DWORD cbReceived);

class BRSocket
{
public:

    BRSocket();

    HRESULT Initialize(BrSocketReadCallback readCallback, void* clientContext);
    HRESULT Bind(unsigned short port);
    HRESULT Read();
    HRESULT Reply(const uint8_t* aBuf, uint16_t aLength);
    HRESULT SendTo(const uint8_t* aBuf, uint16_t aLength, sockaddr_in* peerToSendTo);

private:
    SOCKET mSocket;
    SOCKADDR mPeerAddr;

    BrSocketReadCallback mClientReceiveCallback;
    void* mClientContext;
};