/**
 * rs_nfc_writer_ndef.c
 *
 * NDEF message writing / erasing for NFC Forum Type 2 Tags (hand-rolled TLV
 * over raw ISO 14443-3 page writes) and Type 3 / Type 4 / Type 5 Tags (via the
 * PTX SDK's lean ptxNDEF_T3TOP / ptxNDEF_T4TOP / ptxNDEF_T5TOP components).
 * This is the write-side counterpart to the reader's rs_ndef_read.c and reuses
 * the exact same PTX105R backend accessors (rs_nfc_ptx_*), so the writer needs
 * no backend of its own.
 *
 * The card must already be activated (the caller runs inside the reader's
 * on_card_event callback, before deactivation). NO printing — status is
 * returned to the caller.
 */

#include "rs_nfc_writer.h"
#include "rs_nfc_writer_ndef.h"
#include "rs_nfc_ptx105r.h"
#include "ptx_IOT_READER.h"
#include "ptxNDEF_T3TOP.h"
#include "ptxNDEF_T4TOP.h"
#include "ptxNDEF_T5TOP.h"
#include <string.h>
#include <stdint.h>

/***********************************************************************************************************************
 * Constants
 **********************************************************************************************************************/
#define RX_BUF_SIZE       RS_NFC_PTX_RX_BUF_SIZE

/* Type 2 Tag raw write parameters (ISO 14443-3A). */
#define T2T_WRITE_CMD     0xA2u   /* WRITE command opcode                  */
#define T2T_READ_CMD      0x30u   /* READ command opcode (returns 16 bytes)*/
#define T2T_PAGE_BYTES    4u      /* a T2T page/block is 4 bytes           */
#define T2T_FIRST_BLOCK   4u      /* user data area starts at block 4      */
#define T2T_NDEF_TLV_TAG  0x03u   /* NDEF Message TLV tag                  */
#define T2T_TERM_TLV_TAG  0xFEu   /* Terminator TLV tag                    */

/***********************************************************************************************************************
 * Type 2 Tag NDEF Write — hand-rolled TLV over raw page writes
 **********************************************************************************************************************/

static rs_status_t write_t2t_ndef (const uint8_t * ndef, uint16_t ndef_len)
{
    /* NDEF Message TLV: 0x03 <len> <message ...> 0xFE (terminator).
     * Only the short (1-byte length) form is supported, matching
     * RS_NFC_WRITE_MAX_BYTES (< 255). */
    uint8_t  tlv[2u + RS_NFC_WRITE_MAX_BYTES + 1u];
    uint16_t tlv_len = 0u;

    if (ndef_len > RS_NFC_WRITE_MAX_BYTES)
    {
        return RS_ERR_INVALID_CFG;
    }

    tlv[tlv_len++] = T2T_NDEF_TLV_TAG;
    tlv[tlv_len++] = (uint8_t) ndef_len;

    if ((ndef_len > 0u) && (NULL != ndef))
    {
        (void) memcpy(&tlv[tlv_len], ndef, ndef_len);
        tlv_len = (uint16_t) (tlv_len + ndef_len);
    }

    tlv[tlv_len++] = T2T_TERM_TLV_TAG;

    /* Write the TLV to the tag one 4-byte page at a time, starting at block 4. */
    uint8_t  cmd[2u + T2T_PAGE_BYTES];
    uint8_t  rx[RX_BUF_SIZE];
    uint16_t offset = 0u;
    uint16_t block  = T2T_FIRST_BLOCK;

    while (offset < tlv_len)
    {
        uint16_t remaining = (uint16_t) (tlv_len - offset);
        uint8_t  chunk     = (remaining >= T2T_PAGE_BYTES) ? T2T_PAGE_BYTES
                                                           : (uint8_t) remaining;

        cmd[0] = T2T_WRITE_CMD;
        cmd[1] = (uint8_t) block;
        (void) memset(&cmd[2], 0, T2T_PAGE_BYTES);      /* pad short final page */
        (void) memcpy(&cmd[2], &tlv[offset], chunk);

        uint32_t    rx_len = RX_BUF_SIZE;
        rs_status_t st     = rs_nfc_ptx_data_exchange(cmd, sizeof(cmd), rx, &rx_len);

        if (RS_OK != st)
        {
            return RS_ERR_INTERNAL;
        }

        offset = (uint16_t) (offset + chunk);

        if (block >= 0xFFu)         /* single-byte block address exhausted */
        {
            break;
        }
        block++;
    }

    return RS_OK;
}

/***********************************************************************************************************************
 * Type 4 Tag NDEF Write — via PTX SDK ptxNDEF_T4TOP component
 **********************************************************************************************************************/

static rs_status_t write_t4t_ndef (const uint8_t * ndef, uint16_t ndef_len)
{
    rs_status_t st = rs_nfc_ptx_ndef_open();

    if (RS_OK != st)
    {
        return st;
    }

    struct ptxNDEF_T4TOP * t4t = rs_nfc_ptx_get_ndef_comp();

    /* CheckMessage discovers the NDEF file / capacity before writing. */
    ptxStatus_t ptx_st = ptxNDEF_T4TOpCheckMessage(t4t);

    if (ptxStatus_Success != ptx_st)
    {
        rs_nfc_ptx_ndef_close();

        return RS_ERR_NOT_FOUND;
    }

    uint32_t wlen = ((NULL != ndef) && (ndef_len > 0u)) ? (uint32_t) ndef_len : 0u;

    ptx_st = ptxNDEF_T4TOpWriteMessage(t4t, (uint8_t *) (uintptr_t) ndef, wlen);

    rs_nfc_ptx_ndef_close();

    return (ptxStatus_Success == ptx_st) ? RS_OK : RS_ERR_INTERNAL;
}

/***********************************************************************************************************************
 * Type 5 Tag NDEF Write — via PTX SDK ptxNDEF_T5TOP component
 **********************************************************************************************************************/

static rs_status_t write_t5t_ndef (const uint8_t * ndef, uint16_t ndef_len)
{
    rs_status_t st = rs_nfc_ptx_ndef_t5t_open();

    if (RS_OK != st)
    {
        return st;
    }

    struct ptxNDEF_T5TOP * t5t = rs_nfc_ptx_get_ndef_t5t_comp();

    ptxStatus_t ptx_st = ptxNDEF_T5TOpCheckMessage(t5t);

    if (ptxStatus_Success != ptx_st)
    {
        rs_nfc_ptx_ndef_t5t_close();

        return RS_ERR_NOT_FOUND;
    }

    uint32_t wlen = ((NULL != ndef) && (ndef_len > 0u)) ? (uint32_t) ndef_len : 0u;

    ptx_st = ptxNDEF_T5TOpWriteMessage(t5t, (uint8_t *) (uintptr_t) ndef, wlen);

    rs_nfc_ptx_ndef_t5t_close();

    return (ptxStatus_Success == ptx_st) ? RS_OK : RS_ERR_INTERNAL;
}

/***********************************************************************************************************************
 * Type 3 Tag NDEF Write — via PTX SDK ptxNDEF_T3TOP component
 **********************************************************************************************************************/

static rs_status_t write_t3t_ndef (const uint8_t * ndef, uint16_t ndef_len)
{
    rs_status_t st = rs_nfc_ptx_ndef_t3t_open();

    if (RS_OK != st)
    {
        return st;
    }

    struct ptxNDEF_T3TOP * t3t = rs_nfc_ptx_get_ndef_t3t_comp();

    ptxStatus_t ptx_st = ptxNDEF_T3TOpCheckMessage(t3t);

    if (ptxStatus_Success != ptx_st)
    {
        rs_nfc_ptx_ndef_t3t_close();

        return RS_ERR_NOT_FOUND;
    }

    uint32_t wlen = ((NULL != ndef) && (ndef_len > 0u)) ? (uint32_t) ndef_len : 0u;

    ptx_st = ptxNDEF_T3TOpWriteMessage(t3t, (uint8_t *) (uintptr_t) ndef, wlen);

    rs_nfc_ptx_ndef_t3t_close();

    return (ptxStatus_Success == ptx_st) ? RS_OK : RS_ERR_INTERNAL;
}

/***********************************************************************************************************************
 * Public (module-internal) dispatch
 **********************************************************************************************************************/

rs_status_t rs_nfc_writer_ndef_write (ptxIoTRd_CardProtocol_t protocol,
                                      const uint8_t         * ndef,
                                      uint16_t                ndef_len)
{
    switch (protocol)
    {
        case Prot_T2T:
            return write_t2t_ndef(ndef, ndef_len);

        case Prot_ISODEP:
            return write_t4t_ndef(ndef, ndef_len);

        case Prot_T5T:
            return write_t5t_ndef(ndef, ndef_len);

        case Prot_T3T:
            return write_t3t_ndef(ndef, ndef_len);

        case Prot_Undefined:
        default:
            return RS_ERR_INVALID_CFG;
    }
}

rs_status_t rs_nfc_writer_ndef_erase (ptxIoTRd_CardProtocol_t protocol)
{
    /* Erasing == writing a zero-length NDEF message. */
    return rs_nfc_writer_ndef_write(protocol, RS_NFC_WRITE_PAYLOAD_NONE, RS_NFC_WRITE_LEN_NONE);
}

/***********************************************************************************************************************
 * Read-back verification (MRD §5.5 step 6)
 **********************************************************************************************************************/

/* Read the NDEF message from a Type 4 Tag into buf; *len is in/out (capacity
 * in, message length out). */
static rs_status_t read_t4t_message (uint8_t * buf, uint32_t * len)
{
    rs_status_t st = rs_nfc_ptx_ndef_open();

    if (RS_OK != st)
    {
        return st;
    }

    struct ptxNDEF_T4TOP * t4t = rs_nfc_ptx_get_ndef_comp();

    ptxStatus_t ptx_st = ptxNDEF_T4TOpCheckMessage(t4t);

    if (ptxStatus_Success != ptx_st)
    {
        rs_nfc_ptx_ndef_close();

        return RS_ERR_NOT_FOUND;
    }

    ptx_st = ptxNDEF_T4TOpReadMessage(t4t, buf, len);

    rs_nfc_ptx_ndef_close();

    return (ptxStatus_Success == ptx_st) ? RS_OK : RS_ERR_INTERNAL;
}

/* Read the NDEF message from a Type 5 Tag into buf; *len is in/out. */
static rs_status_t read_t5t_message (uint8_t * buf, uint32_t * len)
{
    rs_status_t st = rs_nfc_ptx_ndef_t5t_open();

    if (RS_OK != st)
    {
        return st;
    }

    struct ptxNDEF_T5TOP * t5t = rs_nfc_ptx_get_ndef_t5t_comp();

    ptxStatus_t ptx_st = ptxNDEF_T5TOpCheckMessage(t5t);

    if (ptxStatus_Success != ptx_st)
    {
        rs_nfc_ptx_ndef_t5t_close();

        return RS_ERR_NOT_FOUND;
    }

    ptx_st = ptxNDEF_T5TOpReadMessage(t5t, buf, len);

    rs_nfc_ptx_ndef_t5t_close();

    return (ptxStatus_Success == ptx_st) ? RS_OK : RS_ERR_INTERNAL;
}

/* Read the NDEF message from a Type 3 Tag into buf; *len is in/out. */
static rs_status_t read_t3t_message (uint8_t * buf, uint32_t * len)
{
    rs_status_t st = rs_nfc_ptx_ndef_t3t_open();

    if (RS_OK != st)
    {
        return st;
    }

    struct ptxNDEF_T3TOP * t3t = rs_nfc_ptx_get_ndef_t3t_comp();

    ptxStatus_t ptx_st = ptxNDEF_T3TOpCheckMessage(t3t);

    if (ptxStatus_Success != ptx_st)
    {
        rs_nfc_ptx_ndef_t3t_close();

        return RS_ERR_NOT_FOUND;
    }

    ptx_st = ptxNDEF_T3TOpReadMessage(t3t, buf, len);

    rs_nfc_ptx_ndef_t3t_close();

    return (ptxStatus_Success == ptx_st) ? RS_OK : RS_ERR_INTERNAL;
}

/* Read `need` raw bytes of the Type 2 Tag user area (from block 4) into buf,
 * using the 0x30 READ command (16 bytes / 4 pages per response). */
static rs_status_t read_t2t_bytes (uint8_t * buf, uint16_t need)
{
    uint8_t  cmd[2];
    uint8_t  rx[RX_BUF_SIZE];
    uint16_t got   = 0u;
    uint16_t block = T2T_FIRST_BLOCK;

    while (got < need)
    {
        cmd[0] = T2T_READ_CMD;
        cmd[1] = (uint8_t) block;

        uint32_t    rx_len = RX_BUF_SIZE;
        rs_status_t st     = rs_nfc_ptx_data_exchange(cmd, sizeof(cmd), rx, &rx_len);

        if ((RS_OK != st) || (rx_len < 4u))
        {
            return RS_ERR_NOT_FOUND;
        }

        uint16_t take = (rx_len < 16u) ? (uint16_t) rx_len : 16u;

        if ((uint16_t) (got + take) > need)
        {
            take = (uint16_t) (need - got);
        }

        (void) memcpy(&buf[got], rx, take);
        got = (uint16_t) (got + take);

        if ((uint16_t) block + T2T_PAGE_BYTES > 0xFFu)
        {
            break;
        }

        block = (uint16_t) (block + T2T_PAGE_BYTES);
    }

    return (got >= need) ? RS_OK : RS_ERR_NOT_FOUND;
}

rs_status_t rs_nfc_writer_ndef_verify (ptxIoTRd_CardProtocol_t protocol,
                                       const uint8_t         * ndef,
                                       uint16_t                ndef_len)
{
    uint16_t want = ((NULL != ndef) && (ndef_len > 0u)) ? ndef_len : 0u;

    switch (protocol)
    {
        case Prot_T2T:
        {
            /* Rebuild the exact TLV that was written (03 <len> msg FE) and
             * compare it byte-for-byte against a read-back of the same pages. */
            uint8_t  expect[2u + RS_NFC_WRITE_MAX_BYTES + 1u];
            uint8_t  actual[sizeof(expect)];
            uint16_t elen = 0u;

            expect[elen++] = T2T_NDEF_TLV_TAG;
            expect[elen++] = (uint8_t) want;

            if (want > 0u)
            {
                (void) memcpy(&expect[elen], ndef, want);
                elen = (uint16_t) (elen + want);
            }

            expect[elen++] = T2T_TERM_TLV_TAG;

            rs_status_t st = read_t2t_bytes(actual, elen);

            if (RS_OK != st)
            {
                return st;
            }

            return (0 == memcmp(expect, actual, elen)) ? RS_OK : RS_ERR_INTERNAL;
        }

        case Prot_ISODEP:
        {
            uint8_t  actual[RS_NFC_WRITE_MAX_BYTES];
            uint32_t alen = sizeof(actual);

            rs_status_t st = read_t4t_message(actual, &alen);

            if (RS_OK != st)
            {
                return st;
            }

            if ((uint16_t) alen != want)
            {
                return RS_ERR_INTERNAL;
            }

            return ((0u == want) || (0 == memcmp(actual, ndef, want))) ? RS_OK : RS_ERR_INTERNAL;
        }

        case Prot_T5T:
        {
            uint8_t  actual[RS_NFC_WRITE_MAX_BYTES];
            uint32_t alen = sizeof(actual);

            rs_status_t st = read_t5t_message(actual, &alen);

            if (RS_OK != st)
            {
                return st;
            }

            if ((uint16_t) alen != want)
            {
                return RS_ERR_INTERNAL;
            }

            return ((0u == want) || (0 == memcmp(actual, ndef, want))) ? RS_OK : RS_ERR_INTERNAL;
        }

        case Prot_T3T:
        {
            uint8_t  actual[RS_NFC_WRITE_MAX_BYTES];
            uint32_t alen = sizeof(actual);

            rs_status_t st = read_t3t_message(actual, &alen);

            if (RS_OK != st)
            {
                return st;
            }

            if ((uint16_t) alen != want)
            {
                return RS_ERR_INTERNAL;
            }

            return ((0u == want) || (0 == memcmp(actual, ndef, want))) ? RS_OK : RS_ERR_INTERNAL;
        }

        case Prot_Undefined:
        default:
            return RS_ERR_INVALID_CFG;
    }
}
