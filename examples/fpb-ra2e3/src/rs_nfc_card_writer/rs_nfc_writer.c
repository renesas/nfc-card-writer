/**
 * rs_nfc_writer.c
 *
 * NFC Writer SBB helpers. The writer does NOT run discovery, activation, retry
 * or a read loop — the application drives the NFC Reader SBB
 * (rs_nfc_reader_Read) and calls these helpers from the reader's
 * on_card_event callback, while the tag is still activated. Each helper adds
 * NDEF erase / write / read-back verify on top of the card the reader has
 * just read.
 *
 * NO printing — diagnostics go through RS_LOG (the shared SBB logger, same as
 * the reader); status is returned to the caller.
 */

#include "rs_nfc_writer.h"
#include "rs_nfc_writer_ndef.h"

/***********************************************************************************************************************
 * API
 **********************************************************************************************************************/
rs_status_t rs_nfc_writer_erase (const rs_nfc_card_result_t * card)
{
    if (NULL == card)
    {
        RS_LOG("rs_nfc_writer_erase: NULL card");

        return RS_ERR_INVALID_CFG;
    }

    rs_status_t st = rs_nfc_writer_ndef_erase(card->protocol);

    if (RS_OK != st)
    {
        RS_LOG("rs_nfc_writer_erase: failed (protocol=%d, err=%d)",
               (int) card->protocol, (int) st);

        return st;
    }

    /* Read-back verify the tag is now empty (MRD read-back after write). */
    st = rs_nfc_writer_ndef_verify(card->protocol,
                                   RS_NFC_WRITE_PAYLOAD_NONE, RS_NFC_WRITE_LEN_NONE);

    if (RS_OK != st)
    {
        RS_LOG("rs_nfc_writer_erase: verify failed (protocol=%d, err=%d)",
               (int) card->protocol, (int) st);
    }

    return st;
}

rs_status_t rs_nfc_writer_write (const rs_nfc_card_result_t * card,
                                 const uint8_t              * ndef,
                                 uint16_t                     ndef_len)
{
    if ((NULL == card) || (NULL == ndef) ||
        (0u == ndef_len) || (ndef_len > RS_NFC_WRITE_MAX_BYTES))
    {
        RS_LOG("rs_nfc_writer_write: invalid args (len=%u)", (unsigned) ndef_len);

        return RS_ERR_INVALID_CFG;
    }

    /* Pre-flight capacity check: only when the reader reported the tag's usable
     * NDEF area (data_area_size > 0); 0 means "unknown", so skip the guard. */
    if ((card->data_area_size > 0u) && ((uint32_t) ndef_len > card->data_area_size))
    {
        RS_LOG("rs_nfc_writer_write: message too large (len=%u, capacity=%u)",
               (unsigned) ndef_len, (unsigned) card->data_area_size);

        return RS_ERR_BUFFER_OVERFLOW;
    }

    rs_status_t st = rs_nfc_writer_ndef_write(card->protocol, ndef, ndef_len);

    if (RS_OK != st)
    {
        RS_LOG("rs_nfc_writer_write: failed (protocol=%d, len=%u, err=%d)",
               (int) card->protocol, (unsigned) ndef_len, (int) st);

        return st;
    }

    /* Read-back verify the written message (MRD write-then-read-back). */
    st = rs_nfc_writer_ndef_verify(card->protocol, ndef, ndef_len);

    if (RS_OK != st)
    {
        RS_LOG("rs_nfc_writer_write: verify failed (protocol=%d, len=%u, err=%d)",
               (int) card->protocol, (unsigned) ndef_len, (int) st);
    }

    return st;
}

rs_status_t rs_nfc_writer_verify (const rs_nfc_card_result_t * card,
                                  const uint8_t              * ndef,
                                  uint16_t                     ndef_len)
{
    if (NULL == card)
    {
        RS_LOG("rs_nfc_writer_verify: NULL card");

        return RS_ERR_INVALID_CFG;
    }

    rs_status_t st = rs_nfc_writer_ndef_verify(card->protocol, ndef, ndef_len);

    if (RS_OK != st)
    {
        RS_LOG("rs_nfc_writer_verify: mismatch/failure (protocol=%d, err=%d)",
               (int) card->protocol, (int) st);
    }

    return st;
}

