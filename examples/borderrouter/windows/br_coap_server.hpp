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

/**
 * @file
 *   This file includes definitions for the CoAP server.
 */

#ifndef COAP_SERVER_HPP_
#define COAP_SERVER_HPP_

#include "br_coap_header.hpp"

namespace OffMesh {
namespace Coap {

/**
 * @addtogroup core-coap
 *
 * @{
 *
 */

/**
 * This class implements CoAP resource handling.
 *
 */
class Resource
{
    friend class Server;

public:
    /**
     * This function pointer is called when a CoAP message with a given Uri-Path is received.
     *
     * @param[in]  aContext      A pointer to arbitrary context information.
     * @param[in]  aHeader       A reference to the CoAP header.
     * @param[in]  aMessage      A reference to the message.
     * @param[in]  aMessageInfo  A reference to the message info for @p aMessage.
     *
     */
    typedef void (*CoapMessageHandler)(void *aContext, Header &aHeader, uint8_t *aMessage,
                                       uint16_t aLength, const char *aUriPath);

    /**
     * This constructor initializes the resource.
     *
     * @param[in]  aHandler  A function pointer that is called when receiving a CoAP message for @p aUriPath.
     * @param[in]  aContext  A pointer to arbitrary context information.
     */
    Resource(CoapMessageHandler aHandler, void *aContext) {
        mHandler = aHandler;
        mContext = aContext;
        mNext = NULL;
    }

private:
    void HandleRequest(Header &aHeader, uint8_t *aMessage, uint16_t aLength, const char *aUriPath) {
        mHandler(mContext, aHeader, aMessage, aLength, aUriPath);
    }

    CoapMessageHandler mHandler;
    void *mContext;
    Resource *mNext;
};

/**
 * This class implements the CoAP server.
 *
 */
class Server
{
public:

    /**
    * This constructor initializes the object.
    *
    */
    Server(void);

    /**
     * This method adds a resource to the CoAP server.
     *
     * @param[in]  aResource  A reference to the resource.
     *
     * @retval kThreadError_None  Successfully added @p aResource.
     * @retval kThreadError_Busy  The @p aResource was already added.
     *
     */
    ThreadError AddResource(Resource &aResource);

    void Receive(uint8_t *aMessage, uint16_t aLength);

private:
    enum
    {
        kMaxReceivedUriPath = 32,   ///< Maximum supported URI path on received messages.
    };

    Resource *mResources;
};

/**
 * @}
 *
 */

}  // namespace Coap
}  // namespace OffMesh

#endif  // COAP_SERVER_HPP_
