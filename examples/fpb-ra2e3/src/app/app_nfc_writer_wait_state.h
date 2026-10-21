/***********************************************************************************************************************
 * app_nfc_writer_wait_state.h - application example: why was no tag found?
 *
 * The reader module reports what it sees while it waits for a tag (rs_nfc_reader_GetWaitState()). It prints nothing.
 * This example is the application side: it turns the state into text, and the CLI adds that text to its
 * "No tag (timeout)" line. Output goes through APP_LOG(), which maps to console_io.
 **********************************************************************************************************************/
#ifndef APP_NFC_WRITER_WAIT_STATE_H
#define APP_NFC_WRITER_WAIT_STATE_H

#include "rs_nfc_reader.h"
#include "app_nfc_writer_log.h"

/* 1 = the CLI prints the reason after "No tag (timeout)". Off by default: it adds roughly 0.4 KB of strings and code (estimate). */
#ifndef APP_NFC_WAIT_STATE_EXAMPLE
#define APP_NFC_WAIT_STATE_EXAMPLE 0
#endif

/* Plain-language text for a wait state. The reader module carries no text; the wording is the application's. */
const char * app_nfc_wait_state_text(rs_nfc_wait_state_t state);

#endif /* APP_NFC_WRITER_WAIT_STATE_H */
