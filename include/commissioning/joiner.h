/*
 *  Copyright (c) 2016, The OpenThread Authors.
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
 * @brief
 *   This file includes the platform abstraction for the Thread Joiner role.
 */

#ifndef OPENTHREAD_JOINER_H_
#define OPENTHREAD_JOINER_H_

#ifdef OTDLL
#ifndef OTAPI
#define OTAPI __declspec(dllimport)
#endif
#else
#define OTAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup core-commissioning
 *
 * @{
 *
 */

/**
 * This function enables the Thread Joiner role.
 *
 * @param[in]  aInstance         A pointer to an OpenThread instance.
 * @param[in]  aPSKd             A pointer to the PSKd.
 * @param[in]  aProvisioningUrl  A pointer to the Provisioning URL (may be NULL).
 *
 * @retval kThreadError_None         Successfully started the Commissioner role.
 * @retval kThreadError_InvalidArgs  @p aPSKd or @p aProvisioningUrl is invalid.
 *
 */
ThreadError otJoinerStart(otInstance *aInstance, const char *aPSKd, const char *aProvisioningUrl);

/**
 * This function disables the Thread Joiner role.
 *
 * @param[in]  aInstance  A pointer to an OpenThread instance.
 *
 */
OTAPI ThreadError otJoinerStop(otInstance *aInstance);

/**
 * @}
 *
 */

#ifdef __cplusplus
}  // end of extern "C"
#endif

#endif  // OPENTHREAD_JOINER_H_
