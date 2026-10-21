/**
 * rs_nfc_writer.h
 *
 * NFC Writer SBB public API.
 *
 * This header is the single public include for the module. The NFC Writer
 * builds ON TOP of the NFC Reader SBB (rs_nfc_reader.h): it reuses the reader's
 * discovery/activation/read machinery and adds NDEF erase and write on the
 * card that the reader has just activated.
 *
 * The writer does NOT own a discovery/read loop. The application drives the
 * NFC Reader SBB (rs_nfc_reader_Read) directly and, inside the reader's
 * on_card_event callback — while the tag is still activated — calls the
 * writer helpers to act on the just-read card:
 *
 *   rs_nfc_writer_erase()   erase the NDEF message, then read-back verify empty
 *   rs_nfc_writer_write()   write a raw NDEF message, then read-back verify it
 *   rs_nfc_writer_verify()  read the tag back and confirm it matches (standalone)
 *
 * write() and erase() perform the read-back verify internally, so a returned
 * RS_OK means the operation was applied AND confirmed on the tag. verify() is
 * also exposed on its own for checking a tag you did not just write.
 *
 * Each helper takes the reader's rs_nfc_card_result_t (the active card); the
 * target tag type is taken from result->protocol. The caller supplies the
 * NDEF message as raw bytes; the module is payload-agnostic and does not build
 * records for you.
 *
 * Usage caveats
 * -------------
 * 1. Card pointer lifetime. The rs_nfc_card_result_t is only valid for the
 *    duration of the on_card_event callback, while the tag is ACTIVATED. Do
 *    NOT cache it and call a writer helper later: by then the reader has
 *    deactivated the tag, so the write/erase/verify has no selected card and
 *    fails (typically RS_ERR_NOT_FOUND). Always call inside the callback.
 *
 * 2. Capacity guard is best-effort. When the reader has reported the tag's
 *    usable NDEF area (card->data_area_size > 0), write() rejects an oversized
 *    message up front with RS_ERR_BUFFER_OVERFLOW. If the capacity is unknown
 *    (data_area_size == 0) the guard is skipped: an over-large message may
 *    then write but fail the internal read-back verify with RS_ERR_INTERNAL.
 */

#ifndef RS_NFC_WRITER_H
#define RS_NFC_WRITER_H

/* Reuse the reader's shared types: rs_status_t, rs_nfc_card_result_t,
 * rs_ndef_* decode types, rs_nfc_tech_mask_t, rs_nfc_card_type_t, the
 * RS_NFC_* named config constants, and ptxIoTRd_CardProtocol_t. */
#include "rs_nfc_reader.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**********************************************************************************************************************
 * Named configuration values (writer-specific; reader constants come in via
 * rs_nfc_reader.h — e.g. RS_NFC_TIMEOUT_INFINITE, RS_NFC_TECH_ALL).
 **********************************************************************************************************************/

/* Upper bound on a single NDEF write payload. T2T is the tightest tag type
 * (hand-rolled TLV writer); keep caller payloads at or below this. */
#define RS_NFC_WRITE_MAX_BYTES           248U

/* ndef_write_data / ndef_write_len when no payload is supplied (READ,
 * READ_ERASE). */
#define RS_NFC_WRITE_PAYLOAD_NONE        (NULL)
#define RS_NFC_WRITE_LEN_NONE            (0U)

/**********************************************************************************************************************
 * API
 *
 * All three helpers act on a card the NFC Reader SBB has just activated and
 * read. Call them from the reader's on_card_event callback, while the tag is
 * still activated; the target tag type is taken from card->protocol.
 *
 * Supported tag types: Type 2 (T2T), Type 3 (T3T/FeliCa), Type 4 (ISO-DEP/T4T)
 * and Type 5 (T5T).
 **********************************************************************************************************************/

/**
 * Erase the activated card's NDEF message (writes a zero-length NDEF message)
 * and read-back verifies the tag is now empty.
 *
 * Call from on_card_event while the tag is ACTIVATED; do not cache `card` and
 * call later (see "Usage caveats" above).
 *
 * @param[in] card  Active card result from the reader (must be non-NULL).
 * @return RS_OK on success (erased and verified empty), RS_ERR_INVALID_CFG for
 *         a NULL/unsupported card, or an rs_status_t error from the write or
 *         the read-back verify.
 */
rs_status_t rs_nfc_writer_erase(const rs_nfc_card_result_t * card);

/**
 * Write a raw NDEF message to the activated card and read-back verify it.
 *
 * Call from on_card_event while the tag is ACTIVATED; do not cache `card` and
 * call later (see "Usage caveats" above). If the reader reported the tag
 * capacity (card->data_area_size > 0), an over-large message is rejected up
 * front with RS_ERR_BUFFER_OVERFLOW; otherwise the read-back verify is the
 * final guard.
 *
 * @param[in] card      Active card result from the reader (must be non-NULL).
 * @param[in] ndef      Raw NDEF message bytes (must be non-NULL).
 * @param[in] ndef_len  Message length, 1..RS_NFC_WRITE_MAX_BYTES.
 * @return RS_OK on success (written and verified), RS_ERR_INVALID_CFG for bad
 *         arguments / unsupported tag, RS_ERR_BUFFER_OVERFLOW if the message
 *         exceeds a known tag capacity, or an rs_status_t error from the write
 *         or the read-back verify.
 */
rs_status_t rs_nfc_writer_write(const rs_nfc_card_result_t * card,
                                const uint8_t              * ndef,
                                uint16_t                     ndef_len);

/**
 * Read the activated card's NDEF message back and confirm it matches the
 * supplied message (MRD §5.5 step 6). ndef == NULL / ndef_len == 0 verifies an
 * erase (the tag must read back empty).
 *
 * @param[in] card      Active card result from the reader (must be non-NULL).
 * @param[in] ndef      Expected NDEF message bytes (NULL for an erase).
 * @param[in] ndef_len  Expected length (0 for an erase).
 * @return RS_OK if the read-back matches, RS_ERR_NOT_FOUND if the tag could
 *         not be re-read, RS_ERR_INTERNAL on a content mismatch, or
 *         RS_ERR_INVALID_CFG for a NULL/unsupported card.
 */
rs_status_t rs_nfc_writer_verify(const rs_nfc_card_result_t * card,
                                 const uint8_t              * ndef,
                                 uint16_t                     ndef_len);

#ifdef __cplusplus
}
#endif

#endif /* RS_NFC_WRITER_H */
