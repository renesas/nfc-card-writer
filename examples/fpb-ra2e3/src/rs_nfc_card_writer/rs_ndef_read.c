/**
 * rs_ndef_read.c
 *
 * NDEF message reading for NFC Forum Type 2 Tags (hand-rolled, small
 * footprint) and Type 3/Type 4/Type 5 Tags (via the PTX SDK's lean
 * ptxNDEF_T3TOP / ptxNDEF_T4TOP / ptxNDEF_T5TOP components). We use
 * the individual tag-type NDEF-OP components instead of the generic
 * ptxNDEF dispatcher (ptxNDEF.c) which unconditionally links all four
 * tag-type operation components (~13 KB flash).
 *
 * Also contains the module-internal NDEF message record parser
 * (rs_ndef_decode_message) which fills rs_nfc_card_result_t.decoded.
 *
 * NO printing — all results go into the caller's rs_nfc_card_result_t.
 */

#include "rs_nfc_reader.h"
#include "rs_nfc_ptx105r.h"
#include "ptx_IOT_READER.h"
#include "ptxNDEF_T3TOP.h"
#include "ptxNDEF_T4TOP.h"
#include "ptxNDEF_T5TOP.h"
#include <string.h>

/***********************************************************************************************************************
 * Constants
 **********************************************************************************************************************/
#define RX_BUF_SIZE   RS_NFC_PTX_RX_BUF_SIZE
#define TX_BUF_SIZE   RS_NFC_PTX_TX_BUF_SIZE

/* NDEF message decoder — module-internal; the parsed records are exposed to
 * the application via rs_nfc_card_result_t.decoded. */
static rs_status_t rs_ndef_decode_message(const uint8_t     * msg,
                                          uint32_t            len,
                                          rs_ndef_decoded_t * out);

/***********************************************************************************************************************
 * Type 4 Tag NDEF Read — via PTX SDK ptxNDEF_T4TOP component
 **********************************************************************************************************************/

static rs_status_t read_t4t_ndef (rs_nfc_card_result_t * res, uint32_t cap)
{
    res->tag_type_name = "ISO-DEP (Type 4 Tag / ISO 14443-4)";

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
        res->ndef_present   = false;
        res->ndef_len       = 0;

        return RS_ERR_NDEF_UNSUPPORTED;
    }

    /* Ask the SDK for one byte more than the caller's cap so an oversized
     * message is detectable instead of being silently clamped to the cap
     * (spec 3.4.2 F-03 / 3.2.2 T-19). res->ndef_data is always
     * RS_NFC_NDEF_MAX_BYTES, so the probe byte is in bounds whenever
     * cap < RS_NFC_NDEF_MAX_BYTES. */
    uint32_t msg_len = (cap < (uint32_t) RS_NFC_NDEF_MAX_BYTES) ? (cap + 1u) : cap;
    ptx_st = ptxNDEF_T4TOpReadMessage(t4t, res->ndef_data, &msg_len);

    if (ptxStatus_Success == ptx_st)
    {
        if (msg_len > cap)
        {
            res->ndef_present = false;
            res->ndef_len     = 0;
            st                = RS_ERR_NDEF_TOO_LARGE;
        }
        else
        {
            res->ndef_len     = (uint16_t) msg_len;
            res->ndef_present = (msg_len > 0u);
        }
    }
    else
    {
        /* The SDK could not read the message off an NDEF-formatted card.
         * Propagate it (spec 3.4.2 F-15) instead of reporting success with
         * an empty result, which hid read failures from the application. */
        res->ndef_present = false;
        res->ndef_len     = 0;
        st                = RS_ERR_READ_FAIL;
    }

    /* Extract CC metadata from the SDK T4TOP component */
    res->data_area_size = t4t->CCParams.NDEFFileSize;
    res->writeable      = (0x00u == t4t->CCParams.NDEFAccessWrite);

    rs_nfc_ptx_ndef_close();

    return st;  /* RS_ERR_READ_FAIL if the SDK read failed */
}

/***********************************************************************************************************************
 * Type 5 Tag NDEF Read — via PTX SDK ptxNDEF_T5TOP component
 **********************************************************************************************************************/

static rs_status_t read_t5t_ndef (rs_nfc_card_result_t * res, uint32_t cap)
{
    rs_status_t st = rs_nfc_ptx_ndef_t5t_open();

    res->tag_type_name = "NFC Forum Type 5 Tag (T5T/ISO 15693)";

    if (RS_OK != st)
    {
        return st;
    }

    struct ptxNDEF_T5TOP * t5t = rs_nfc_ptx_get_ndef_t5t_comp();

    ptxStatus_t ptx_st = ptxNDEF_T5TOpCheckMessage(t5t);

    if (ptxStatus_Success != ptx_st)
    {
        rs_nfc_ptx_ndef_t5t_close();
        res->ndef_present   = false;
        res->ndef_len       = 0;

        return RS_ERR_NDEF_UNSUPPORTED;
    }

    /* Ask the SDK for one byte more than the caller's cap so an oversized
     * message is detectable instead of being silently clamped to the cap
     * (spec 3.4.2 F-03 / 3.2.2 T-19). res->ndef_data is always
     * RS_NFC_NDEF_MAX_BYTES, so the probe byte is in bounds whenever
     * cap < RS_NFC_NDEF_MAX_BYTES. */
    uint32_t msg_len = (cap < (uint32_t) RS_NFC_NDEF_MAX_BYTES) ? (cap + 1u) : cap;
    ptx_st = ptxNDEF_T5TOpReadMessage(t5t, res->ndef_data, &msg_len);

    if (ptxStatus_Success == ptx_st)
    {
        if (msg_len > cap)
        {
            res->ndef_present = false;
            res->ndef_len     = 0;
            st                = RS_ERR_NDEF_TOO_LARGE;
        }
        else
        {
            res->ndef_len     = (uint16_t) msg_len;
            res->ndef_present = (msg_len > 0u);
        }
    }
    else
    {
        /* The SDK could not read the message off an NDEF-formatted card.
         * Propagate it (spec 3.4.2 F-15) instead of reporting success with
         * an empty result, which hid read failures from the application. */
        res->ndef_present = false;
        res->ndef_len     = 0;
        st                = RS_ERR_READ_FAIL;
    }

    /* Extract CC metadata from the SDK T5TOP component */
    res->data_area_size = (uint32_t) t5t->CCParams.MLEN;
    res->writeable      = (0x00u == t5t->CCParams.WriteAccess);

    rs_nfc_ptx_ndef_t5t_close();

    return st;  /* RS_ERR_READ_FAIL if the SDK read failed */
}

/***********************************************************************************************************************
 * Type 3 Tag NDEF Read — via PTX SDK ptxNDEF_T3TOP component
 **********************************************************************************************************************/

static rs_status_t read_t3t_ndef (rs_nfc_card_result_t * res, uint32_t cap)
{
    res->tag_type_name = "NFC Forum Type 3 Tag (T3T/FeliCa)";

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
        res->ndef_present   = false;
        res->ndef_len       = 0;

        return RS_ERR_NDEF_UNSUPPORTED;
    }

    /* Ask the SDK for one byte more than the caller's cap so an oversized
     * message is detectable instead of being silently clamped to the cap
     * (spec 3.4.2 F-03 / 3.2.2 T-19). res->ndef_data is always
     * RS_NFC_NDEF_MAX_BYTES, so the probe byte is in bounds whenever
     * cap < RS_NFC_NDEF_MAX_BYTES. */
    uint32_t msg_len = (cap < (uint32_t) RS_NFC_NDEF_MAX_BYTES) ? (cap + 1u) : cap;
    ptx_st = ptxNDEF_T3TOpReadMessage(t3t, res->ndef_data, &msg_len);

    if (ptxStatus_Success == ptx_st)
    {
        if (msg_len > cap)
        {
            res->ndef_present = false;
            res->ndef_len     = 0;
            st                = RS_ERR_NDEF_TOO_LARGE;
        }
        else
        {
            res->ndef_len     = (uint16_t) msg_len;
            res->ndef_present = (msg_len > 0u);
        }
    }
    else
    {
        /* The SDK could not read the message off an NDEF-formatted card.
         * Propagate it (spec 3.4.2 F-15) instead of reporting success with
         * an empty result, which hid read failures from the application. */
        res->ndef_present = false;
        res->ndef_len     = 0;
        st                = RS_ERR_READ_FAIL;
    }

    /* Extract CC metadata from the SDK T3TOP component.
     * NmaxB is the maximum number of 16-byte blocks available for NDEF data.
     * RWFlag: 0x00 = read-only, non-zero = read/write. */
    res->data_area_size = (uint32_t) t3t->CCParams.NmaxB * PTX_T3T_BLOCK_SIZE;
    res->writeable      = (0x00u != t3t->CCParams.RWFlag);

    rs_nfc_ptx_ndef_t3t_close();

    return st;  /* RS_ERR_READ_FAIL if the SDK read failed */
}

/***********************************************************************************************************************
 * Type 2 Tag NDEF Read — hand-rolled (small footprint, proven)
 **********************************************************************************************************************/
static rs_status_t read_t2t_ndef (rs_nfc_card_result_t * res, uint32_t ndef_cap)
{
    uint8_t rx[RX_BUF_SIZE];
    uint8_t cmd[2];
    uint32_t rx_len;
    uint8_t  data_buf[TX_BUF_SIZE];   /* accumulate TLV area here */

    /* READ block 3 -> CC (response = blocks 3..6, 16 bytes) */
    cmd[0] = 0x30; cmd[1] = 0x03;
    rx_len = RX_BUF_SIZE;
    rs_status_t st = rs_nfc_ptx_data_exchange(cmd, 2u, rx, &rx_len);

    if ((RS_OK != st) || (rx_len < 4u))
    {
        return RS_ERR_NDEF_UNSUPPORTED;
    }

    /* CC: [0]=magic(0xE1) [1]=version [2]=size(x8) [3]=access */
    if (0xE1u != rx[0])
    {
        return RS_ERR_NDEF_UNSUPPORTED;  /* not NDEF formatted */
    }

    uint32_t data_area  = (uint32_t) rx[2] * 8u;
    uint8_t  wa_nibble  = (uint8_t) (rx[3] & 0x0Fu);

    res->data_area_size = data_area;
    res->writeable      = (0x00u == wa_nibble);
    res->tag_type_name  = "NFC Forum Type 2 Tag (T2T)";

    /* Read the data area (starting at block 4) */
    uint32_t buf_cap = (data_area > (uint32_t) TX_BUF_SIZE)
                       ? (uint32_t) TX_BUF_SIZE : data_area;

    if (0u == buf_cap)
    {
        buf_cap = (uint32_t) TX_BUF_SIZE;
    }

    uint32_t got   = 0;
    uint8_t  block = 4u;

    while (got < buf_cap)
    {
        cmd[0] = 0x30; cmd[1] = block;
        rx_len = RX_BUF_SIZE;
        st = rs_nfc_ptx_data_exchange(cmd, 2u, rx, &rx_len);

        if ((RS_OK != st) || (rx_len < 4u))
        {
            break;
        }

        uint32_t take = (rx_len < 16u) ? rx_len : 16u;

        if ((got + take) > buf_cap)
        {
            take = buf_cap - got;
        }

        (void)memcpy(&data_buf[got], rx, take);
        got += take;

        if ((uint32_t) block + 4u > 0xFFu)
        {
            break;
        }

        block = (uint8_t) (block + 4u);
    }

    /* Walk TLV area, locate NDEF Message TLV (tag 0x03) */
    uint32_t p = 0;

    while (p < got)
    {
        uint8_t t = data_buf[p++];

        if (0x00u == t)
        {
            continue;     /* NULL TLV       */
        }

        if (0xFEu == t)
        {
            break;        /* Terminator TLV */
        }

        if (p >= got)
        {
            break;
        }

        uint32_t l = data_buf[p++];

        if (0xFFu == l)  /* 3-byte length form */
        {
            if ((p + 2u) > got)
            {
                break;
            }

            l = ((uint32_t) data_buf[p] << 8) | data_buf[p + 1u];
            p += 2u;
        }

        if (0x03u == t)  /* NDEF Message TLV */
        {
            if ((p + l) > got)
            {
                l = got - p;
            }

            /* An NDEF message larger than the caller's cap is an error, not
             * a silent truncation: the application would otherwise parse a
             * half message and never know (spec 3.4.2 F-03 / 3.2.2 T-19). */
            if (l > ndef_cap)
            {
                res->ndef_present = false;
                res->ndef_len     = 0;

                return RS_ERR_NDEF_TOO_LARGE;
            }

            (void)memcpy(res->ndef_data, &data_buf[p], l);
            res->ndef_len     = (uint16_t) l;
            res->ndef_present = (l > 0u);

            return RS_OK;
        }

        p += l;  /* skip Lock/Memory/other TLVs */
    }

    /* No NDEF TLV found: the tag is not NDEF-bearing (spec 3.2.2 T-18). */
    res->ndef_present = false;
    res->ndef_len     = 0;

    return RS_ERR_NDEF_UNSUPPORTED;
}

/***********************************************************************************************************************
 * Public API
 **********************************************************************************************************************/

rs_status_t rs_ndef_read_card_info (ptxIoTRd_CardProtocol_t protocol,
                                    rs_nfc_card_result_t * result,
                                    uint32_t               max_ndef_bytes)
{
    if (NULL == result)
    {
        return RS_ERR_INVALID_CFG;
    }

    /* Clear the extended fields */
    result->ndef_present   = false;
    result->ndef_len       = 0;
    result->data_area_size = 0;
    result->writeable      = false;
    result->tag_type_name  = NULL;
    (void)memset(&result->decoded, 0, sizeof(result->decoded));

    /* Clamp the caller-supplied cap to the on-stack / result buffer size.
     * 0 (unspecified) falls back to the max. */
    uint32_t cap = ((0u == max_ndef_bytes) || (max_ndef_bytes > RS_NFC_NDEF_MAX_BYTES))
                   ? (uint32_t) RS_NFC_NDEF_MAX_BYTES
                   : max_ndef_bytes;

    rs_status_t st;

    switch (protocol)
    {
        case Prot_ISODEP: st = read_t4t_ndef(result, cap); break;
        case Prot_T2T:    st = read_t2t_ndef(result, cap); break;
        case Prot_T5T:    st = read_t5t_ndef(result, cap); break;
        case Prot_T3T:    st = read_t3t_ndef(result, cap); break;

        case Prot_NFCDEP:
            result->tag_type_name = "NFC-DEP (Peer-to-Peer)";
            return RS_OK;

        default:
            result->tag_type_name = "Unknown";
            return RS_OK;
    }

    /* Parse the raw NDEF message into records for the application. */
    if (result->ndef_present && (result->ndef_len > 0u))
    {
        (void)rs_ndef_decode_message(result->ndef_data,
                                     (uint32_t) result->ndef_len,
                                     &result->decoded);
    }

    return st;
}

/***********************************************************************************************************************
 * NDEF message decoder
 **********************************************************************************************************************/

static rs_status_t rs_ndef_decode_message (const uint8_t     * msg,
                                           uint32_t            len,
                                           rs_ndef_decoded_t * out)
{
    uint32_t pos = 0;
    uint8_t hdr;
    bool mb;
    bool me;
    bool sr;
    bool il;
    uint8_t type_len;
    uint32_t payload_len;
    uint8_t copy_type;
    uint8_t id_len;

    if (NULL == out)
    {
        return RS_ERR_INVALID_CFG;
    }

    (void)memset(out, 0, sizeof(*out));

    if ((NULL == msg) || (0u == len))
    {
        return RS_ERR_NOT_FOUND;
    }

    while (pos < len)
    {
        if (out->record_count >= RS_NDEF_MAX_RECORDS)
        {
            out->truncated = true;
            break;
        }

        rs_ndef_record_t * rec = &out->records[out->record_count];
        hdr = msg[pos++];
        rec->flags = hdr;
        rec->tnf   = (uint8_t) (hdr & 0x07u);

        mb = (0u != (hdr & 0x80u));
        me = (0u != (hdr & 0x40u));
        sr = (0u != (hdr & 0x10u));
        il = (0u != (hdr & 0x08u));
        (void)mb;

        if (pos >= len)
        {
            break;
        }

        type_len = msg[pos++];

        if (sr)
        {
            if (pos >= len)
            {
                break;
            }

            payload_len = (uint32_t) msg[pos++];
        }
        else
        {
            if ((pos + 4u) > len)
            {
                break;
            }

            payload_len = ((uint32_t) msg[pos] << 24) |
                          ((uint32_t) msg[pos + 1u] << 16) |
                          ((uint32_t) msg[pos + 2u] << 8) |
                          ((uint32_t) msg[pos + 3u]);
            pos += 4u;
        }

        id_len = 0;

        if (il)
        {
            if (pos >= len)
            {
                break;
            }

            id_len = msg[pos++];
        }

        if ((pos + type_len) > len)
        {
            break;
        }

        copy_type = (type_len <= RS_NDEF_MAX_TYPE_LEN)
                            ? type_len : RS_NDEF_MAX_TYPE_LEN;
        (void)memcpy(rec->type, &msg[pos], copy_type);
        rec->type_len = type_len;
        pos += type_len;

        if ((pos + id_len) > len)
        {
            break;
        }

        pos += id_len;

        if ((pos + payload_len) > len)
        {
            break;
        }

        rec->payload     = &msg[pos];
        rec->payload_len = payload_len;
        pos += payload_len;

        out->record_count++;

        if (me)
        {
            break;
        }
    }

    return RS_OK;
}
