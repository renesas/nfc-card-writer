/***********************************************************************************************************************
 * app_nfc_writer_wait_state.c - application example: why was no tag found?
 **********************************************************************************************************************/
#include "app_nfc_writer_wait_state.h"

#if APP_NFC_WAIT_STATE_EXAMPLE

const char * app_nfc_wait_state_text (rs_nfc_wait_state_t state)
{
    switch (state)
    {
        case RS_NFC_WAIT_NO_IRQ_YET:
            return "waiting, no reader interrupt seen yet";
        case RS_NFC_WAIT_IRQ_SEEN:
            return "waiting, interrupt seen but no card reported yet";
        case RS_NFC_WAIT_CARD:
            return "card detected";
        case RS_NFC_WAIT_STOPPED:
            return "stopped by the application";
        case RS_NFC_WAIT_TIMEOUT_NO_IRQ:
            return "timed out, no reader interrupt seen";
        case RS_NFC_WAIT_TIMEOUT_IRQ_NO_CARD:
            return "timed out, interrupt seen but no card reported";
        case RS_NFC_WAIT_READER_ERROR:
            return "the reader reported a fault or could not be read";
        default:
            return "no wait yet";
    }
}

#else

typedef int app_nfc_writer_wait_state_disabled_t;   /* keep the translation unit non-empty */

#endif
