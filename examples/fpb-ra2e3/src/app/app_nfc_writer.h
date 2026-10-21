/** \file
    ---------------------------------------------------------------
    SPDX-License-Identifier: BSD-3-Clause

    Copyright (c) 2024-2026, Renesas Electronics Corporation and/or its affiliates


    Redistribution and use in source and binary forms, with or without modification,
    are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this
       list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright notice, this list of
       conditions and the following disclaimer in the documentation and/or other
       materials provided with the distribution.

    3. Neither the name of Renesas nor the names of its
       contributors may be used to endorse or promote products derived from this
       software without specific prior written permission.



    THIS SOFTWARE IS PROVIDED BY Renesas "AS IS" AND ANY EXPRESS
    OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
    OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL RENESAS OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
    GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
    OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
    ---------------------------------------------------------------

    Project     : PTX1K
    Module      : IOT_WRITER Demo
    File        : app_nfc_writer.h

    Description : Interactive NFC card writer demo. A small CLI over the
                  console_io UART drives the NFC Writer SBB
                  (read / erase / write NDEF).
*/

#ifndef APP_NFC_WRITER_H_
#define APP_NFC_WRITER_H_

#include "hal_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ####################################################################################################################
 * DEFINES
 * ####################################################################################################################
 */

/* Enable on-board LED feedback (comment out to disable LED code entirely). */
#define APP_NFC_WRITER_LED_EN

#define APP_NFC_WRITER_SEPARATOR_TOP "=========== DATA EXCHANGE ================\r\n"
#define APP_NFC_WRITER_SEPARATOR_BOT "==========================================\r\n"

#if defined(APP_NFC_WRITER_LED_EN)
#define APP_NFC_WRITER_LED_1         LED1
#define APP_NFC_WRITER_LED_2         LED2_PMOD1_GPIO9
#endif /* APP_NFC_WRITER_LED_EN */

/**
 * \brief NFC Writer application entry point.
 *
 * Runs the interactive command loop (read / erase / write / help) forever,
 * driving the NFC Writer SBB for each command.
 */
void app_nfc_writer_entry(void);

#ifdef __cplusplus
}
#endif

#endif /* Guard */
