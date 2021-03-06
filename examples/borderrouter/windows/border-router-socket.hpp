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