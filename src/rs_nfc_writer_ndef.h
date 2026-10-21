/**
 * rs_nfc_writer_ndef.h
 *
 * Module-internal NDEF erase/write entry points. Not part of the public API
 * (rs_nfc_writer.h). Implemented in rs_nfc_writer_ndef.c; called by the
 * orchestrator trampoline in rs_nfc_writer.c.
 */

#ifndef RS_NFC_WRITER_NDEF_H
#define RS_NFC_WRITER_NDEF_H

#include "rs_nfc_writer.h"   /* rs_status_t + ptxIoTRd_CardProtocol_t */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write an NDEF message to the currently-activated tag. Dispatches on protocol
 * (T2T / T3T-FeliCa / T4T-ISO-DEP / T5T). A zero-length message (ndef == NULL
 * or ndef_len == 0) erases the tag's NDEF content.
 */
rs_status_t rs_nfc_writer_ndef_write(ptxIoTRd_CardProtocol_t protocol,
                                     const uint8_t         * ndef,
                                     uint16_t                ndef_len);

/** Erase the tag's NDEF content (write a zero-length NDEF message). */
rs_status_t rs_nfc_writer_ndef_erase(ptxIoTRd_CardProtocol_t protocol);

/**
 * Read the tag's NDEF content back and compare it against the message that was
 * just written (MRD §5.5 step 6). ndef == NULL / ndef_len == 0 verifies an
 * erase (the tag must read back empty). Dispatches on protocol (T2T / T3T /
 * T4T / T5T).
 *
 * @return RS_OK if the read-back matches, RS_ERR_NOT_FOUND if the tag could
 *         not be re-read, RS_ERR_INTERNAL on a content mismatch, or
 *         RS_ERR_INVALID_CFG for an unsupported protocol.
 */
rs_status_t rs_nfc_writer_ndef_verify(ptxIoTRd_CardProtocol_t protocol,
                                      const uint8_t         * ndef,
                                      uint16_t                ndef_len);

#ifdef __cplusplus
}
#endif

#endif /* RS_NFC_WRITER_NDEF_H */
