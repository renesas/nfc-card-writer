/***********************************************************************************************************************
 * rs_nfc_wait_state.c - NFC SBB wait state
 *
 * Answers one question for the application: what is the SBB seeing while it waits for a card? The reader interrupt
 * runs in an ISR where nothing can be printed, so the SBB records one fact there (an interrupt arrived) and turns it
 * into a named state on request. The application decides what to print and where.
 **********************************************************************************************************************/
#include "rs_nfc_reader.h"
#include "rs_nfc_wait_state_priv.h"

#if RS_NFC_WAIT_STATE_ENABLED

/* g_rs_nfc_wait_irq_seen is written by the reader IRQ callback (and cleared at the start of a wait); the phase is
 * written by the task that waits. Readers may see the two a moment apart, which is harmless for diagnostics. */
/* Invariant: BEGIN clears the flag before the reader IRQ is armed, so the ISR is its only writer: no lock needed. */
volatile uint8_t g_rs_nfc_wait_phase    = (uint8_t) RS_NFC_WAIT_PHASE_NEVER;
volatile bool    g_rs_nfc_wait_irq_seen = false;

rs_nfc_wait_state_t rs_nfc_reader_GetWaitState (void)
{
    bool    irq_seen = g_rs_nfc_wait_irq_seen;
    uint8_t phase    = g_rs_nfc_wait_phase;

    switch (phase)
    {
        case RS_NFC_WAIT_PHASE_RUNNING:
            return irq_seen ? RS_NFC_WAIT_IRQ_SEEN : RS_NFC_WAIT_NO_IRQ_YET;

        case RS_NFC_WAIT_PHASE_CARD:
            return RS_NFC_WAIT_CARD;

        case RS_NFC_WAIT_PHASE_STOPPED:
            return RS_NFC_WAIT_STOPPED;

        case RS_NFC_WAIT_PHASE_TIMEOUT:
            return irq_seen ? RS_NFC_WAIT_TIMEOUT_IRQ_NO_CARD : RS_NFC_WAIT_TIMEOUT_NO_IRQ;

        case RS_NFC_WAIT_PHASE_ERROR:
            return RS_NFC_WAIT_READER_ERROR;

        default:
            return RS_NFC_WAIT_NONE;
    }
}

#else

typedef int rs_nfc_wait_state_disabled_t;   /* keep the translation unit non-empty */

#endif
