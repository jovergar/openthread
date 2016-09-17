#pragma once

#include <windows.h>

typedef void(*BrSocketReadCallback)(void* aContext, uint8_t* aBuf, DWORD cbReceived);

class BRSocket
{
public:

    BRSocket();
    ~BRSocket();

    HRESULT Initialize(BrSocketReadCallback readCallback, void* clientContext);
    // safe to be called multiple times. called from destructor. if more fine grained control
    // of timing is desired, can be called manually
    void Uninitialize();
    HRESULT Bind(unsigned short port);
    HRESULT Read();
    HRESULT Reply(const uint8_t* aBuf, uint16_t aLength);
    HRESULT SendTo(const uint8_t* aBuf, uint16_t aLength, sockaddr_in* peerToSendTo);
    void GetLastPeer(SOCKADDR* mLastPeer);

private:
    SOCKET mSocket;
    SOCKADDR mPeerAddr;

    BrSocketReadCallback mClientReceiveCallback;
    void* mClientContext;
};