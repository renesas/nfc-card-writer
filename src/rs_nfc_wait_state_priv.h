/***********************************************************************************************************************
 * rs_nfc_wait_state_priv.h - SBB-internal hooks for the wait state (not part of the public API)
 *
 * The adapter calls these hooks around the interrupt-driven wait for a card. Everything is a plain store, so it is
 * safe in the reader interrupt callback. With RS_NFC_WAIT_STATE_ENABLED == 0 the hooks expand to nothing.
 **********************************************************************************************************************/
#ifndef RS_NFC_WAIT_STATE_PRIV_H
#define RS_NFC_WAIT_STATE_PRIV_H

#include <stdbool.h>
#include <stdint.h>
#include "rs_nfc_reader.h"

typedef enum e_rs_nfc_wait_phase
{
    RS_NFC_WAIT_PHASE_NEVER = 0,    /* no wait has started                                         */
    RS_NFC_WAIT_PHASE_RUNNING,      /* waiting for the reader interrupt                            */
    RS_NFC_WAIT_PHASE_CARD,         /* ended: card detected                                        */
    RS_NFC_WAIT_PHASE_STOPPED,      /* reserved: ended by rs_nfc_reader_Stop(); not set by this adapter        */
    RS_NFC_WAIT_PHASE_TIMEOUT,      /* ended: timeout                                              */
    RS_NFC_WAIT_PHASE_ERROR         /* ended: the reader reported a fault or could not be read     */
} rs_nfc_wait_phase_t;

#if RS_NFC_WAIT_STATE_ENABLED

/* Invariant: BEGIN clears the flag before the reader IRQ is armed, so the ISR is its only writer: no lock needed. */
extern volatile uint8_t g_rs_nfc_wait_phase;
extern volatile bool    g_rs_nfc_wait_irq_seen;

/* Start of a wait. Call before the reader interrupt callback is armed, so the ISR is the only writer afterwards. */
#define RS_NFC_WAIT_BEGIN()    do { g_rs_nfc_wait_irq_seen = false; g_rs_nfc_wait_phase = (uint8_t) RS_NFC_WAIT_PHASE_RUNNING; } while (0)

/* A reader interrupt arrived while the SBB was waiting. Interrupt-safe. */
#define RS_NFC_WAIT_IRQ()      do { g_rs_nfc_wait_irq_seen = true; } while (0)

/* The wait ended. */
#define RS_NFC_WAIT_END(phase) do { g_rs_nfc_wait_phase = (uint8_t) (phase); } while (0)

#else

#define RS_NFC_WAIT_BEGIN()    do { } while (0)
#define RS_NFC_WAIT_IRQ()      do { } while (0)
#define RS_NFC_WAIT_END(phase) do { (void) (phase); } while (0)

#endif

#endif /* RS_NFC_WAIT_STATE_PRIV_H */
