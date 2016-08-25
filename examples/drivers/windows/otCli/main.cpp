/*
 *  Copyright (c) 2016, Microsoft Corporation.
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

#include <windows.h>
#include <openthread.h>
#include <cli/cli-uart.h>
#include <platform/uart.h>

int main(int argc, char *argv[])
{
    otCliUartInit();

    HANDLE hSTDIN = GetStdHandle(STD_INPUT_HANDLE);
    
    // Cache the original console mode
    DWORD originalConsoleMode;
    GetConsoleMode(hSTDIN, &originalConsoleMode);

    // Wait for console events
    while (WaitForSingleObject(hSTDIN, INFINITE) == WAIT_OBJECT_0)
    {
        DWORD numberOfEvent = 0;
        if (GetNumberOfConsoleInputEvents(hSTDIN, &numberOfEvent))
        {
            for (; numberOfEvent > 0; numberOfEvent--)
            {
                INPUT_RECORD record;
                DWORD numRead;
                if (!ReadConsoleInput(hSTDIN, &record, 1, &numRead) ||
                    record.EventType != KEY_EVENT ||
                    !record.Event.KeyEvent.bKeyDown)
                    continue;
                
                uint8_t ch = (uint8_t)record.Event.KeyEvent.uChar.AsciiChar;
                otPlatUartReceived(&ch, 1);
            }
        }
    }

    return 0;
}

EXTERN_C ThreadError otPlatUartSend(const uint8_t *aBuf, uint16_t aBufLength)
{
    ThreadError error = kThreadError_None;

    DWORD dwNumCharsWritten = 0;
    if (WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), aBuf, aBufLength, &dwNumCharsWritten, NULL))
    {
        otPlatUartSendDone();
    }
    else
    {
        error = kThreadError_Error;
    }

    return error;
}