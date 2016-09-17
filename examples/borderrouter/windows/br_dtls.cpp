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
 *   This file implements the necessary hooks for mbedTLS.
 */

#include <stdafx.h>

#include <assert.h>
#include <stdio.h>

#include <common/code_utils.hpp>
#include <common/debug.hpp>
#include <common/encoding.hpp>
#include <common/logging.hpp>

#include <mbedtls/debug.h>

#include <mbedtls/platform.h>

#include "br_dtls.hpp"

#ifdef WINDOWS_LOGGING
#include "dtls.tmh"
#endif

static void my_debug(void *ctx, int level, const char *file, int line, const char *str)
{
    ((void)level);

    fprintf((FILE *)ctx, "%s:%04d: %s\r\n", file, line, str);
    fflush((FILE *)ctx);
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

namespace OffMesh {
namespace MeshCoP {

Dtls::Dtls(void):
    mStarted(false),
    mTimerIntermediate(0),
    mTimerFinish(0)
{
}

ThreadError Dtls::Start(bool aClient, ReceiveHandler aReceiveHandler, SendHandler aSendHandler, void *aContext)
{
    mbedtls_platform_set_snprintf(mbedtls_platform_win32_snprintf);

    static const int ciphersuites[2] = {0xC0FF, 0}; // EC-JPAKE cipher suite
    int rval;

    mReceiveHandler = aReceiveHandler;
    mSendHandler = aSendHandler;
    mContext = aContext;
    mClient = aClient;

    mbedtls_ssl_init(&mSsl);
    mbedtls_ssl_config_init(&mConf);
    mbedtls_ctr_drbg_init(&mCtrDrbg);
    mbedtls_entropy_init(&mEntropy);

    mbedtls_debug_set_threshold(4);

    // XXX: should set personalization data to hardware address
    rval = mbedtls_ctr_drbg_seed(&mCtrDrbg, mbedtls_entropy_func, &mEntropy, NULL, 0);
    VerifyOrExit(rval == 0, ;);

    rval = mbedtls_ssl_config_defaults(&mConf, mClient ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
    VerifyOrExit(rval == 0, ;);

    mbedtls_ssl_conf_rng(&mConf, mbedtls_ctr_drbg_random, &mCtrDrbg);
    mbedtls_ssl_conf_min_version(&mConf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&mConf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_ciphersuites(&mConf, ciphersuites);
    mbedtls_ssl_conf_handshake_timeout(&mConf, 8000, 60000);
    mbedtls_ssl_conf_dbg(&mConf, HandleMbedtlsDebug, stdout);

    if (!mClient)
    {
        mbedtls_ssl_cookie_init(&mCookieCtx);

        rval = mbedtls_ssl_cookie_setup(&mCookieCtx, mbedtls_ctr_drbg_random, &mCtrDrbg);
        VerifyOrExit(rval == 0, ;);

        mbedtls_ssl_conf_dtls_cookies(&mConf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &mCookieCtx);
    }

    rval = mbedtls_ssl_setup(&mSsl, &mConf);
    VerifyOrExit(rval == 0, ;);

    mbedtls_ssl_set_bio(&mSsl, this, &Dtls::HandleMbedtlsTransmit, Dtls::HandleMbedtlsReceive, NULL);

    mbedtls_ssl_set_timer_cb(&mSsl, this, &Dtls::HandleMbedtlsSetTimer, Dtls::HandleMbedtlsGetTimer);

    rval = mbedtls_ssl_set_hs_ecjpake_password(&mSsl, mPsk, mPskLength);
    VerifyOrExit(rval == 0, ;);

    mStarted = true;
    if (mClient)
    {
        Process();
    }

    otLogInfoMeshCoP("DTLS started\r\n");

exit:
    return MapError(rval);
}

ThreadError Dtls::Stop(void)
{
    mStarted = false;
    mbedtls_ssl_close_notify(&mSsl);
    mbedtls_ssl_free(&mSsl);
    mbedtls_ssl_config_free(&mConf);
    mbedtls_ctr_drbg_free(&mCtrDrbg);
    mbedtls_entropy_free(&mEntropy);
    mbedtls_ssl_cookie_free(&mCookieCtx);
    return kThreadError_None;
}

ThreadError Dtls::SetPsk(const uint8_t* aPsk, uint8_t aPskLength)
{
    ThreadError error = kThreadError_None;

    VerifyOrExit(aPskLength <= sizeof(mPsk), error = kThreadError_InvalidArgs);

    memcpy(mPsk, aPsk, aPskLength);
    mPskLength = aPskLength;

exit:
    return error;
}

ThreadError Dtls::SetClientId(const uint8_t* aClientId, uint8_t aLength)
{
    int rval = mbedtls_ssl_set_client_transport_id(&mSsl, aClientId, aLength);
    return MapError(rval);
}

bool Dtls::IsConnected(void)
{
    return mSsl.state == MBEDTLS_SSL_HANDSHAKE_OVER;
}

ThreadError Dtls::Send(const uint8_t *aBuf, uint16_t aLength)
{
    int rval = mbedtls_ssl_write(&mSsl, aBuf, aLength);
    return MapError(rval);
}

ThreadError Dtls::Receive(uint8_t *aMessage, uint16_t aLength)
{
    printf("Dtls::Receive\r\n");

    mReceiveMessageBuffer = aMessage;
    mReceiveOffset = 0;
    mReceiveLength = aLength;

    Process();

    return kThreadError_None;
}

int Dtls::HandleMbedtlsTransmit(void *aContext, const unsigned char *aBuf, size_t aLength)
{
    return static_cast<Dtls *>(aContext)->HandleMbedtlsTransmit(aBuf, aLength);
}

int Dtls::HandleMbedtlsTransmit(const unsigned char *aBuf, size_t aLength)
{
    ThreadError error;
    int rval = 0;

    otLogInfoMeshCoP("Dtls::HandleMbedtlsTransmit\r\n");

    error = mSendHandler(mContext, aBuf, (uint16_t)aLength);

    switch (error)
    {
    case kThreadError_None:
        rval = static_cast<int>(aLength);
        break;

    case kThreadError_NoBufs:
        rval = MBEDTLS_ERR_SSL_WANT_WRITE;
        break;

    default:
        assert(false);
        break;
    }

    return rval;
}

int Dtls::HandleMbedtlsReceive(void *aContext, unsigned char *aBuf, size_t aLength)
{
    return static_cast<Dtls *>(aContext)->HandleMbedtlsReceive(aBuf, aLength);
}

int Dtls::HandleMbedtlsReceive(unsigned char *aBuf, size_t aLength)
{
    printf("Dtls::HandleMbedtlsReceive\r\n");
    int rval;

    otLogInfoMeshCoP("Dtls::HandleMbedtlsReceive\r\n");

    VerifyOrExit(mReceiveMessageBuffer != NULL && mReceiveLength != 0, rval = MBEDTLS_ERR_SSL_WANT_READ);

    if (aLength > mReceiveLength)
    {
        aLength = mReceiveLength;
    }

    memcpy_s(aBuf, aLength, mReceiveMessageBuffer + mReceiveOffset, mReceiveLength);
    rval = static_cast<int>(aLength);
    mReceiveOffset += static_cast<uint16_t>(rval);
    mReceiveLength -= static_cast<uint16_t>(rval);

exit:
    return rval;
}

int Dtls::HandleMbedtlsGetTimer(void *aContext)
{
    return static_cast<Dtls *>(aContext)->HandleMbedtlsGetTimer();
}

int Dtls::HandleMbedtlsGetTimer(void)
{
    uint32_t now = GetTickCount();
    int rval;

    otLogInfoMeshCoP("Dtls::HandleMbedtlsGetTimer\r\n");

    if (mTimerFinish == 0)
    {
        rval = -1;
    }
    else if ((int32_t)(mTimerFinish - now) <= 0)
    {
        rval = 2;
    }
    else if ((int32_t)(mTimerIntermediate - now) <= 0)
    {
        rval = 1;
    }
    else
    {
        rval = 0;
    }

    return rval;
}

void Dtls::HandleMbedtlsSetTimer(void *aContext, uint32_t aIntermediate, uint32_t aFinish)
{
    static_cast<Dtls *>(aContext)->HandleMbedtlsSetTimer(aIntermediate, aFinish);
}

void Dtls::HandleMbedtlsSetTimer(uint32_t aIntermediate, uint32_t aFinish)
{
    otLogInfoMeshCoP("Dtls::SetTimer\r\n");

    mTimerIntermediate = GetTickCount() + aIntermediate;

    if (aFinish == 0)
    {
        mTimerFinish = 0;
    }
    else
    {
        mTimerFinish = GetTickCount() + aFinish;

        if (mTimerFinish == 0)
        {
            mTimerFinish = 1;
        }
    }
}

void Dtls::Process(void)
{
    uint8_t buf[MBEDTLS_SSL_MAX_CONTENT_LEN];
    int rval;

    while (mStarted)
    {
        if (mSsl.state != MBEDTLS_SSL_HANDSHAKE_OVER)
        {
            rval = mbedtls_ssl_handshake(&mSsl);
        }
        else
        {
            rval = mbedtls_ssl_read(&mSsl, buf, sizeof(buf));
        }

        if (rval > 0)
        {
            mReceiveHandler(mContext, buf, static_cast<uint16_t>(rval));
        }
        else if (rval == 0 || rval == MBEDTLS_ERR_SSL_WANT_READ || rval == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            break;
        }
        else
        {
            mbedtls_ssl_session_reset(&mSsl);
            mbedtls_ssl_set_hs_ecjpake_password(&mSsl, mPsk, mPskLength);
            break;
        }
    }
}

ThreadError Dtls::MapError(int rval)
{
    ThreadError error = kThreadError_None;

    switch (rval)
    {
    case MBEDTLS_ERR_SSL_BAD_INPUT_DATA:
        error = kThreadError_InvalidArgs;
        break;

    case MBEDTLS_ERR_SSL_ALLOC_FAILED:
        error = kThreadError_NoBufs;
        break;

    default:
        assert(rval >= 0);
        break;
    }

    return error;
}

void Dtls::HandleMbedtlsDebug(void *ctx, int level, const char *file, int line, const char *str)
{
    otLogInfoMeshCoP("%s:%04d: %s\r\n", file, line, str);
    (void)ctx;
    (void)level;
    (void)file;
    (void)line;
    (void)str;
}

}  // namespace MeshCoP
}  // namespace OffMesh
