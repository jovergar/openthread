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
 *   This file implements ICMPv6.
 */

#include <string.h>

#include <common/code_utils.hpp>
#include <common/debug.hpp>
#include <common/logging.hpp>
#include <common/message.hpp>
#include <net/icmp6.hpp>
#include <net/ip6.hpp>
#include <openthreadinstance.h>

#ifdef WINDOWS_LOGGING
#include "icmp6.tmh"
#endif

using Thread::Encoding::BigEndian::HostSwap16;

namespace Thread {
namespace Ip6 {

Message *Icmp::NewMessage(otInstance *aInstance, uint16_t aReserved)
{
    return Ip6::NewMessage(aInstance, sizeof(IcmpHeader) + aReserved);
}

ThreadError Icmp::RegisterCallbacks(otInstance *aInstance, IcmpHandler &aHandler)
{
    ThreadError error = kThreadError_None;

    for (IcmpHandler *cur = aInstance->mIcmpHandlers; cur; cur = cur->mNext)
    {
        if (cur == &aHandler)
        {
            ExitNow(error = kThreadError_Busy);
        }
    }

    aHandler.mNext = aInstance->mIcmpHandlers;
    aInstance->mIcmpHandlers = &aHandler;

exit:
    return error;
}

void Icmp::SetEchoReplyHandler(otInstance *aInstance, EchoReplyHandler aHandler, void *aContext)
{
    aInstance->mEchoReplyHandler = aHandler;
    aInstance->mEchoReplyContext = aContext;
}

ThreadError Icmp::SendEchoRequest(Message &aMessage, const MessageInfo &aMessageInfo)
{
    ThreadError error = kThreadError_None;
    MessageInfo messageInfoLocal;
    IcmpHeader icmpHeader;

    otInstance *aInstance = aMessage.GetInstance();

    messageInfoLocal = aMessageInfo;

    icmpHeader.Init();
    icmpHeader.SetType(IcmpHeader::kTypeEchoRequest);
    icmpHeader.SetId(1);
    icmpHeader.SetSequence(aInstance->mEchoSequence++);

    SuccessOrExit(error = aMessage.Prepend(&icmpHeader, sizeof(icmpHeader)));
    aMessage.SetOffset(0);
    SuccessOrExit(error = Ip6::SendDatagram(aMessage, messageInfoLocal, kProtoIcmp6));

    otLogInfoIcmp("Sent echo request\n");

exit:
    return error;
}

ThreadError Icmp::SendError(otInstance *aInstance, const Address &aDestination, IcmpHeader::Type aType,
                            IcmpHeader::Code aCode, const Header &aHeader)
{
    ThreadError error = kThreadError_None;
    MessageInfo messageInfo;
    Message *message = NULL;
    IcmpHeader icmp6Header;

    VerifyOrExit((message = Ip6::NewMessage(aInstance, 0)) != NULL, error = kThreadError_NoBufs);
    SuccessOrExit(error = message->SetLength(sizeof(icmp6Header) + sizeof(aHeader)));

    message->Write(sizeof(icmp6Header), sizeof(aHeader), &aHeader);

    icmp6Header.Init();
    icmp6Header.SetType(aType);
    icmp6Header.SetCode(aCode);
    message->Write(0, sizeof(icmp6Header), &icmp6Header);

    memset(&messageInfo, 0, sizeof(messageInfo));
    messageInfo.mPeerAddr = aDestination;

    SuccessOrExit(error = Ip6::SendDatagram(*message, messageInfo, kProtoIcmp6));

    otLogInfoIcmp("Sent ICMPv6 Error\n");

exit:

    if (error != kThreadError_None && message != NULL)
    {
        Message::Free(*message);
    }

    return error;
}

ThreadError Icmp::HandleMessage(Message &aMessage, MessageInfo &aMessageInfo)
{
    ThreadError error = kThreadError_None;
    uint16_t payloadLength;
    IcmpHeader icmp6Header;
    uint16_t checksum;

    payloadLength = aMessage.GetLength() - aMessage.GetOffset();

    // check length
    VerifyOrExit(payloadLength >= IcmpHeader::GetDataOffset(),  error = kThreadError_Drop);
    aMessage.Read(aMessage.GetOffset(), sizeof(icmp6Header), &icmp6Header);

    // verify checksum
    checksum = Ip6::ComputePseudoheaderChecksum(aMessageInfo.GetPeerAddr(), aMessageInfo.GetSockAddr(),
                                                payloadLength, kProtoIcmp6);
    checksum = aMessage.UpdateChecksum(checksum, aMessage.GetOffset(), payloadLength);
    VerifyOrExit(checksum == 0xffff, ;);

    switch (icmp6Header.GetType())
    {
    case IcmpHeader::kTypeEchoRequest:
        return HandleEchoRequest(aMessage, aMessageInfo);

    case IcmpHeader::kTypeEchoReply:
        return HandleEchoReply(aMessage, aMessageInfo);

    case IcmpHeader::kTypeDstUnreach:
        return HandleDstUnreach(aMessage, aMessageInfo, icmp6Header);
    }

exit:
    return error;
}

ThreadError Icmp::HandleDstUnreach(Message &aMessage, const MessageInfo &aMessageInfo,
                                   const IcmpHeader &aIcmpheader)
{
    aMessage.MoveOffset(sizeof(aIcmpheader));

    otInstance *aInstance = aMessage.GetInstance();

    for (IcmpHandler *handler = aInstance->mIcmpHandlers; handler; handler = handler->mNext)
    {
        handler->HandleDstUnreach(aMessage, aMessageInfo, aIcmpheader);
    }

    return kThreadError_None;
}

ThreadError Icmp::HandleEchoRequest(Message &aRequestMessage, const MessageInfo &aMessageInfo)
{
    ThreadError error = kThreadError_None;
    IcmpHeader icmp6Header;
    Message *replyMessage = NULL;
    MessageInfo replyMessageInfo;
    uint16_t payloadLength;

    otInstance *aInstance = aRequestMessage.GetInstance();

    VerifyOrExit(aInstance->mIsEchoEnabled, ;);

    otLogInfoIcmp("Received Echo Request\n");

    icmp6Header.Init();
    icmp6Header.SetType(IcmpHeader::kTypeEchoReply);

    if ((replyMessage = Ip6::NewMessage(aInstance, 0)) == NULL)
    {
        otLogDebgIcmp("icmp fail\n");
        goto exit;
    }

    payloadLength = aRequestMessage.GetLength() - aRequestMessage.GetOffset() - IcmpHeader::GetDataOffset();
    SuccessOrExit(replyMessage->SetLength(IcmpHeader::GetDataOffset() + payloadLength));

    replyMessage->Write(0, IcmpHeader::GetDataOffset(), &icmp6Header);
    aRequestMessage.CopyTo(aRequestMessage.GetOffset() + IcmpHeader::GetDataOffset(),
                           IcmpHeader::GetDataOffset(), payloadLength, *replyMessage);

    memset(&replyMessageInfo, 0, sizeof(replyMessageInfo));
    replyMessageInfo.GetPeerAddr() = aMessageInfo.GetPeerAddr();

    if (!aMessageInfo.GetSockAddr().IsMulticast())
    {
        replyMessageInfo.GetSockAddr() = aMessageInfo.GetSockAddr();
    }

    replyMessageInfo.mInterfaceId = aMessageInfo.mInterfaceId;

    SuccessOrExit(error = Ip6::SendDatagram(*replyMessage, replyMessageInfo, kProtoIcmp6));

    otLogInfoIcmp("Sent Echo Reply\n");

exit:

    if (error != kThreadError_None && replyMessage != NULL)
    {
        Message::Free(*replyMessage);
    }

    return error;
}

ThreadError Icmp::HandleEchoReply(Message &aMessage, const MessageInfo &aMessageInfo)
{
    otInstance *aInstance = aMessage.GetInstance();

    VerifyOrExit(aInstance->mIsEchoEnabled && aInstance->mEchoReplyHandler, ;);

    aInstance->mEchoReplyHandler(aInstance->mEchoReplyContext, aMessage, aMessageInfo);

exit:
    return kThreadError_None;
}

ThreadError Icmp::UpdateChecksum(Message &aMessage, uint16_t aChecksum)
{
    aChecksum = aMessage.UpdateChecksum(aChecksum, aMessage.GetOffset(),
                                        aMessage.GetLength() - aMessage.GetOffset());

    if (aChecksum != 0xffff)
    {
        aChecksum = ~aChecksum;
    }

    aChecksum = HostSwap16(aChecksum);
    aMessage.Write(aMessage.GetOffset() + IcmpHeader::GetChecksumOffset(), sizeof(aChecksum), &aChecksum);
    return kThreadError_None;
}

bool Icmp::IsEchoEnabled(otInstance *aInstance)
{
    return aInstance->mIsEchoEnabled;
}

void Icmp::SetEchoEnabled(otInstance *aInstance, bool aEnabled)
{
    aInstance->mIsEchoEnabled = aEnabled;
}

}  // namespace Ip6
}  // namespace Thread
