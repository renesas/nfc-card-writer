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
    File        : app_nfc_writer.c

    Description : Interactive NFC card writer demo for PTX1xxR NFC Platform.
                  A tiny CLI over the console_io UART selects one of
                  three operations, drives the NFC Reader SBB and applies
                  the NFC Writer SBB helpers to the activated tag:

                      read            read the tag's NDEF message
                      erase           read, then erase the NDEF message
                      write <text>    read, erase, then write an NDEF Text
                                      record built from <text>
                      help            show the command menu

                  All NFC protocol logic lives in the NFC Writer / Reader SBBs
                  modules (src/rs_nfc_card_writer); this app only parses
                  commands, builds the NDEF payload, drives the reader and
                  prints results.
*/

/*
 * ####################################################################################################################
 * INCLUDES
 * ####################################################################################################################
 */
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"
#include "app_nfc_writer.h"
#include "app_nfc_writer_log.h"
#include "app_nfc_writer_wait_state.h"
#include "rs_nfc_writer.h"

/*
 * ####################################################################################################################
 * DEFINES / TYPES
 * ####################################################################################################################
 */

/* Longest command line the CLI will read (including the trailing NUL). */
#define APP_CLI_LINE_MAX          256u

/* How long a single command waits for a tag before giving up (ms). */
#define APP_CLI_CARD_TIMEOUT_MS   15000u

/* Upper bound on the user text for a `write` command. The NDEF Text record
 * adds a 7-byte overhead (see build_ndef_text_record), so keep text within
 * the module's per-write cap. */
#define APP_TEXT_MAX              (RS_NFC_WRITE_MAX_BYTES - 7u)

/* Which write-side operation a CLI command performs on the activated tag. */
typedef enum e_app_op {
    APP_OP_READ = 0,   /* read the NDEF message only (no modification) */
    APP_OP_ERASE,      /* read, then erase the NDEF message           */
    APP_OP_WRITE,      /* read, erase, then write ndef                */
} app_op_t;

/* Per-command state shared with the card-event callback. */
typedef struct st_app_cmd_ctx {
    app_op_t        op;
    const uint8_t * ndef;            /* write payload (APP_OP_WRITE only)  */
    uint16_t        ndef_len;
    const char *    text;            /* the same payload as typed, for the
                                      * confirmation line (APP_OP_WRITE only) */
    volatile bool   card_handled;    /* set once the first tag is processed */
} app_cmd_ctx_t;

/*
 * ####################################################################################################################
 * LED UTILITIES
 * ####################################################################################################################
 */
#if defined(APP_NFC_WRITER_LED_EN)
static const bsp_io_port_pin_t led_pins[] =
{
    APP_NFC_WRITER_LED_1,
    APP_NFC_WRITER_LED_2,
};

static const uint32_t LED_COUNT = (uint32_t)(sizeof(led_pins) / sizeof(led_pins[0]));

static void app_nfc_writer_led_set_all (bsp_io_level_t level)
{
    for (uint32_t i = 0; i < LED_COUNT; i++)
    {
        (void)g_ioport.p_api->pinWrite(g_ioport.p_ctrl, led_pins[i], level);
    }
}
#endif /* APP_NFC_WRITER_LED_EN */

/*
 * ####################################################################################################################
 * NDEF TEXT RECORD BUILDER
 * ####################################################################################################################
 */

/*
 * Build a single NFC Forum "Text" NDEF record (RTD_TEXT, language "en",
 * UTF-8) into out_buf. The record is a complete NDEF message:
 *
 *   D1 01 <plen> 54 02 'e' 'n' <text...>
 *    |  |    |    |   |   \---/   \-----/
 *    |  |    |    |   |    lang    text
 *    |  |    |    |   status byte (UTF-8, lang len = 2)
 *    |  |    |    type = 'T'
 *    |  |    payload length = 3 + text_len
 *    |  type length = 1
 *    header: MB=1 ME=1 SR=1 TNF=0x01 (NFC Forum well-known)
 *
 * Returns the total message length, or 0 if text is too long for out_cap.
 */
static uint16_t build_ndef_text_record (const char * text,
                                        uint8_t    * out_buf,
                                        uint16_t     out_cap)
{
    size_t text_len = (NULL != text) ? strlen(text) : 0u;

    if (text_len > APP_TEXT_MAX)
    {
        return 0u;
    }

    uint16_t payload_len = (uint16_t)(3u + text_len);   /* status + "en" + text */
    uint16_t total       = (uint16_t)(4u + payload_len);/* 4-byte header + payload */

    if (total > out_cap)
    {
        return 0u;
    }

    uint16_t i = 0u;
    out_buf[i++] = 0xD1u;                 /* MB|ME|SR|TNF=0x01                */
    out_buf[i++] = 0x01u;                 /* type length                      */
    out_buf[i++] = (uint8_t)payload_len;  /* payload length (short record)    */
    out_buf[i++] = 0x54u;                 /* 'T' — RTD_TEXT                   */
    out_buf[i++] = 0x02u;                 /* status: UTF-8, lang length = 2   */
    out_buf[i++] = 'e';
    out_buf[i++] = 'n';

    if (text_len > 0u)
    {
        (void)memcpy(&out_buf[i], text, text_len);
        i = (uint16_t)(i + text_len);
    }

    return total;
}

/*
 * ####################################################################################################################
 * CARD EVENT CALLBACK
 * ####################################################################################################################
 */
static void on_nfc_card_event (rs_status_t                  status,
                               const rs_nfc_card_result_t * result,
                               const char                 * summary,
                               void                       * p_context)
{
    app_cmd_ctx_t * ctx = (app_cmd_ctx_t *)p_context;

    /* Informational / warning / fatal events carry no result. */
    if (NULL == result)
    {
        if (NULL != summary)
        {
            APP_LOG("%s\r\n", summary);
        }
        return;
    }

#if defined(APP_NFC_WRITER_LED_EN)
    app_nfc_writer_led_set_all(BSP_IO_LEVEL_HIGH);
#endif

    APP_LOG(app_nfc_writer_log_COL_BRIGHT_GREEN "\r\n%s" app_nfc_writer_log_COL_RESET "\r\n",
           (NULL != summary) ? summary : "CARD DETECTED!");

    /* The reader has already read the tag; print what was on it. */
    app_nfc_writer_log_print_card_info(result);

    if (NULL != ctx)
    {
        /* Perform the write-side of the selected op on the still-activated tag. */
        switch (ctx->op)
        {
            case APP_OP_ERASE:
            {
                rs_status_t est = rs_nfc_writer_erase(result);

                if (RS_OK == est)
                {
                    APP_LOG("-> Erased\r\n");
                }
                else
                {
                    APP_LOG("-> Erase FAILED (%d)\r\n", (int)est);
                }
                break;
            }

            case APP_OP_WRITE:
            {
                /* rs_nfc_writer_write() writes and read-back verifies internally. */
                rs_status_t wst = rs_nfc_writer_write(result, ctx->ndef, ctx->ndef_len);

                if (RS_OK == wst)
                {
                    /* The card dump printed above comes from the reader's
                     * pre-write read, so it shows what the tag held before
                     * this write. Echo the new text here. */
                    APP_LOG("-> Written + verified (%u B): \"%s\"\r\n",
                           (unsigned)ctx->ndef_len, ctx->text);
                }
                else
                {
                    APP_LOG("-> Write FAILED (%d)\r\n", (int)wst);
                }
                break;
            }

            case APP_OP_READ:
            default:
                break;
        }

        /* One-shot per command: stop the loop after the first tag so the CLI
         * returns to the prompt. */
        ctx->card_handled = true;
        (void)rs_nfc_reader_Stop();
    }

    (void)status;

#if defined(APP_NFC_WRITER_LED_EN)
    app_nfc_writer_led_set_all(BSP_IO_LEVEL_LOW);
#endif
}

/*
 * ####################################################################################################################
 * COMMAND EXECUTION
 * ####################################################################################################################
 */

/* Run one operation and block until a tag is handled or the timeout elapses.
 * `text` is the write payload as the user typed it, used only for the
 * confirmation line; NULL for read and erase. */
static void app_run_op (app_op_t op,
                        const uint8_t * ndef, uint16_t ndef_len,
                        const char * text)
{
    app_cmd_ctx_t ctx;
    ctx.op           = op;
    ctx.ndef         = ndef;
    ctx.ndef_len     = ndef_len;
    ctx.text         = text;
    ctx.card_handled = false;

    /* Drive the NFC Reader SBB directly (blocking). The write-side of the op
     * is applied via the NFC Writer SBB helpers inside on_nfc_card_event, while
     * the tag is still activated. */
    rs_nfc_reader_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tech_mask             = RS_NFC_TECH_ALL;
    cfg.timeout_ms            = APP_CLI_CARD_TIMEOUT_MS;
    cfg.retry_count           = 0u;
    cfg.read_ndef             = true;
    cfg.max_ndef_bytes        = RS_NFC_NDEF_MAX_BYTES;
    cfg.run_raw_exchange      = false;
    cfg.callback              = NULL;                 /* blocking */
    cfg.p_context             = NULL;
    cfg.on_card_event         = on_nfc_card_event;
    cfg.p_card_event_context  = &ctx;
    cfg.validate_dependencies = false;
    cfg.cfg_valid_check_en    = true;

    APP_LOG("\r\nPresent a tag...\r\n");

    rs_status_t st = rs_nfc_reader_Read(&cfg, NULL);

    if (RS_OK != st)
    {
        APP_LOG("Start FAILED (%d)\r\n", (int)st);
    }
    else if (!ctx.card_handled)
    {
#if APP_NFC_WAIT_STATE_EXAMPLE
        APP_LOG("No tag (timeout): %s\r\n", app_nfc_wait_state_text(rs_nfc_reader_GetWaitState()));
#else
        APP_LOG("No tag (timeout)\r\n");
#endif
    }
}

/*
 * ####################################################################################################################
 * CLI
 * ####################################################################################################################
 */
static void app_print_menu (void)
{
    APP_LOG("\r\n"
           "NFC Card Writer\r\n"
           "  read         read the tag\r\n"
           "  erase        erase the tag\r\n"
           "  write <text> write a Text record\r\n"
           "  help         this menu\r\n");
}

/* FSP error that stopped the console; read it with the debugger. */
static volatile fsp_err_t s_console_err = FSP_SUCCESS;

/* The console UART could not be initialised, so there is no CLI and nothing
 * can be printed. Keep the error for the debugger, blink the LEDs to show the
 * failure and stop here instead of retrying. Does not return. */
static void app_console_failed (fsp_err_t err)
{
    s_console_err = err;

    for (;;)
    {
#if defined(APP_NFC_WRITER_LED_EN)
        app_nfc_writer_led_set_all(BSP_IO_LEVEL_HIGH);
        vTaskDelay(pdMS_TO_TICKS(100u));
        app_nfc_writer_led_set_all(BSP_IO_LEVEL_LOW);
        vTaskDelay(pdMS_TO_TICKS(100u));
#else
        vTaskSuspend(NULL);
#endif
    }
}

/* Read one line of input from the console_io UART into buf. Blocks until a
 * newline is received. Handles CR/LF and simple backspace (BS or DEL). If
 * input was lost, the line is rejected: an error is printed, everything up to
 * the next Enter is discarded and an empty line is returned. Returns the
 * length (excluding the terminating NUL). */
static size_t app_read_line (char * buf, size_t cap)
{
    static bool s_last_was_cr = false;
    size_t      len           = 0u;
    bool        discard       = false;

    for (;;)
    {
        int c = console_io_getc();

        if (CONSOLE_IO_RX_LOST == c)
        {
            /* Characters (possibly Enter) are missing from this line. */
            if (!discard)
            {
                APP_LOG("\r\nInput lost (RX overflow) - line discarded, press Enter\r\n");
            }
            discard       = true;
            len           = 0u;
            s_last_was_cr = false;
            continue;
        }

        if (c < 0)
        {
            app_console_failed(console_io_open());   /* does not return */
        }

        /* Terminals send CR, LF or CR+LF for Enter: treat the LF of a CR+LF
         * pair as part of the previous Enter, not as a new empty line. */
        if (('\n' == c) && s_last_was_cr)
        {
            s_last_was_cr = false;
            continue;
        }

        s_last_was_cr = ('\r' == c);

        if (('\n' == c) || ('\r' == c))
        {
            if (!discard)
            {
                APP_LOG("\r\n");
            }
            break;
        }

        if (discard)
        {
            continue;                            /* rest of a rejected line */
        }

        if (('\b' == c) || (0x7F == c))          /* backspace / delete */
        {
            if (len > 0u)
            {
                len--;
                APP_LOG("\b \b");
            }
            continue;
        }

        if (len < (cap - 1u))
        {
            buf[len++] = (char)c;
            console_io_putc((char)c);            /* local echo */
        }
    }

    buf[len] = '\0';
    return len;
}

/* Advance past leading spaces/tabs. */
static char * app_skip_ws (char * s)
{
    while ((' ' == *s) || ('\t' == *s))
    {
        s++;
    }
    return s;
}

static void app_handle_command (char * line)
{
    char * cmd = app_skip_ws(line);

    if ('\0' == *cmd)
    {
        return;                                   /* empty line */
    }

    if (0 == strncmp(cmd, "read", 4) &&
        ('\0' == cmd[4] || ' ' == cmd[4]))
    {
        app_run_op(APP_OP_READ, RS_NFC_WRITE_PAYLOAD_NONE, RS_NFC_WRITE_LEN_NONE, NULL);
    }
    else if (0 == strncmp(cmd, "erase", 5) &&
             ('\0' == cmd[5] || ' ' == cmd[5]))
    {
        app_run_op(APP_OP_ERASE, RS_NFC_WRITE_PAYLOAD_NONE, RS_NFC_WRITE_LEN_NONE, NULL);
    }
    else if (0 == strncmp(cmd, "write", 5) &&
             ('\0' == cmd[5] || ' ' == cmd[5]))
    {
        char * text = app_skip_ws(&cmd[5]);

        /* Strip one pair of surrounding double quotes if present. */
        size_t tlen = strlen(text);
        if ((tlen >= 2u) && ('"' == text[0]) && ('"' == text[tlen - 1u]))
        {
            text[tlen - 1u] = '\0';
            text++;
        }

        if ('\0' == *text)
        {
            APP_LOG("usage: write <text>\r\n");
            return;
        }

        static uint8_t ndef_buf[RS_NFC_WRITE_MAX_BYTES];
        uint16_t       ndef_len = build_ndef_text_record(text, ndef_buf, sizeof(ndef_buf));

        if (0u == ndef_len)
        {
            APP_LOG("Too long (max %u)\r\n", (unsigned)APP_TEXT_MAX);
            return;
        }

        app_run_op(APP_OP_WRITE, ndef_buf, ndef_len, text);
    }
    else if ((0 == strcmp(cmd, "help")) ||
             (0 == strcmp(cmd, "menu")) ||
             (0 == strcmp(cmd, "?")))
    {
        app_print_menu();
    }
    else
    {
        APP_LOG("Unknown. Try 'help'\r\n");
    }
}

/*
 * ####################################################################################################################
 * APPLICATION ENTRY POINT
 * ####################################################################################################################
 */
void app_nfc_writer_entry (void)
{
    static char line[APP_CLI_LINE_MAX];

    fsp_err_t err = console_io_open();

    if (FSP_SUCCESS != err)
    {
        app_console_failed(err);                 /* does not return */
    }

    APP_LOG("\r\nNFC Card Writer ready\r\n");
    app_print_menu();

    for (;;)
    {
        APP_LOG("nfc> ");
        (void)app_read_line(line, sizeof(line));
        app_handle_command(line);
    }
}
