/*
 * app_nfc_writer_log.c
 *
 * Implementation of the application log helper. See app_nfc_writer_log.h.
 *
 * Notes:
 *  - Log output goes through APP_LOG (console_io over the SCI UART);
 *    this module only provides the card-info / buffer print helpers.
 */
#include "app_nfc_writer_log.h"
#include "hal_data.h"
#include "r_ioport.h"
#include "ptxCOMMON.h"


/*
 * ####################################################################################################################
 * API IMPLEMENTATION
 * ####################################################################################################################
 */

void app_nfc_writer_log_print_buffer (uint8_t  * buffer,
                                      uint32_t   bufferOffset,
                                      uint32_t   bufferLength,
                                      uint8_t    addNewLine,
                                      uint8_t    printASCII)
{
    uint32_t i;
    uint8_t character_to_print;

    if (NULL != buffer)
    {
        if (0 != bufferLength)
        {
            for (i = 0; (i < bufferLength) && (i < (uint32_t) TX_BUFFER_SIZE); i++)
            {
                if ((i > 0) && ((i % (LINE_LENGTH - 5) == 0)))
                {
                    APP_LOG("\r\n     ");
                }

                if (0 == printASCII)
                {
                    APP_LOG("%02X", (uint8_t) buffer[i + bufferOffset]);
                }
                else
                {
                    character_to_print = (uint8_t) buffer[i + bufferOffset];

                    if (character_to_print < 0x20)
                    {
                        APP_LOG(".");
                    }
                    else
                    {
                        APP_LOG("%c", character_to_print);
                    }
                }
            }

            if (0 != addNewLine)
            {
                APP_LOG("\r\n");
            }
        }
    }
}

/*
 * ####################################################################################################################
 * NDEF RECORD DECODER (human-readable record content)
 * ####################################################################################################################
 */

/* Human-readable NDEF TNF (Type Name Format) names. */
static const char * ndef_tnf_name (uint8_t tnf)
{
    switch (tnf)
    {
        case 0x00u: return "Empty";
        case 0x01u: return "Well-known";
        case 0x02u: return "MIME";
        case 0x03u: return "Absolute URI";
        case 0x04u: return "External";
        case 0x05u: return "Unknown";
        case 0x06u: return "Unchanged";
        default:    return "Reserved";
    }
}

/* NFC Forum URI-record abbreviation prefixes (indexed by the identifier byte). */
static const char * const NDEF_URI_PREFIX[] = {
    "", "http://www.", "https://www.", "http://", "https://", "tel:",
    "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://",
    "sftp://", "smb://", "nfs://", "ftp://", "dav://", "news:",
    "telnet://", "imap:", "rtsp://", "urn:", "pop:", "sip:", "sips:",
    "tftp:", "btspp://", "btl2cap://", "btgoep://", "tcpobex://",
    "irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:",
    "urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:"
};

static void print_printable (const uint8_t * p, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        uint8_t c = p[i];
        APP_LOG("%c", ((c >= 0x20u) && (c < 0x7Fu)) ? (int) c : '.');
    }
}

/* Local single-char NDEF type check (e.g. "T" text, "U" URI). */
static bool ndef_type_is (const rs_ndef_record_t * rec, char c)
{
    return (1u == rec->type_len) && ((char) rec->type[0] == c);
}

/* Print the records already decoded by the module (result->decoded). */
static void print_ndef_records (const rs_nfc_card_result_t * result)
{
    const rs_ndef_decoded_t * decoded = &result->decoded;

    if (0u == decoded->record_count)
    {
        APP_LOG("Records  : none\r\n");
        return;
    }

    APP_LOG("Records  : %u%s\r\n",
           (unsigned) decoded->record_count,
           decoded->truncated ? " (truncated)" : "");

    APP_LOG(app_nfc_writer_log_COL_BRIGHT_CYAN);

    for (uint8_t i = 0; i < decoded->record_count; i++)
    {
        const rs_ndef_record_t * rec = &decoded->records[i];

        APP_LOG("[%u] TNF=0x%02X (%s) Type='", (unsigned) i,
               (unsigned) rec->tnf, ndef_tnf_name(rec->tnf));
        print_printable(rec->type, rec->type_len);
        APP_LOG("'  Payload=%u bytes\r\n", (unsigned) rec->payload_len);

        /* Well-known Text record: [status][lang][UTF-8 text] */
        if ((0x01u == rec->tnf) &&
            ndef_type_is(rec, 'T') &&
            (rec->payload_len >= 1u))
        {
            uint8_t status   = rec->payload[0];
            uint8_t lang_len = (uint8_t) (status & 0x3Fu);

            if ((uint32_t) lang_len + 1u <= rec->payload_len)
            {
                const uint8_t * txt = &rec->payload[1u + lang_len];
                uint32_t txt_len = rec->payload_len - 1u - lang_len;

                APP_LOG("  Text: \"");
                print_printable(txt, txt_len);
                APP_LOG("\"\r\n");
            }
        }
        /* Well-known URI record: [prefix-id][URI tail] */
        else if ((0x01u == rec->tnf) &&
                 ndef_type_is(rec, 'U') &&
                 (rec->payload_len >= 1u))
        {
            uint8_t id = rec->payload[0];

            APP_LOG("  URI : ");

            if (id < (uint8_t) (sizeof(NDEF_URI_PREFIX) / sizeof(NDEF_URI_PREFIX[0])))
            {
                APP_LOG("%s", NDEF_URI_PREFIX[id]);
            }

            print_printable(&rec->payload[1], rec->payload_len - 1u);
            APP_LOG("\r\n");
        }
        /* Anything else: show a compact printable preview of the payload. */
        else if (rec->payload_len > 0u)
        {
            APP_LOG("  Data: ");
            print_printable(rec->payload, rec->payload_len);
            APP_LOG("\r\n" app_nfc_writer_log_COL_RESET);
        }
    }
}

/*
 * ####################################################################################################################
 * APPLICATION-LEVEL CARD-INFO PRINTER
 * ####################################################################################################################
 *
 * Reads fields from rs_nfc_card_result_t and formats a human-readable block
 * via APP_LOG (console_io).  Pure I/O - no LED or board
 * interaction; the caller is responsible for any visual feedback (blink, etc.).
 */
void app_nfc_writer_log_print_card_info (const rs_nfc_card_result_t * result)
{
    const char * rf_tech;

    if (NULL == result)
    {
        return;
    }

    switch (result->card_type)
    {
        case RS_NFC_CARD_TYPE_ISO14443A:
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_2:
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_4A:
            rf_tech = "TYPE A";
            break;

        case RS_NFC_CARD_TYPE_ISO14443B:
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_4B:
            rf_tech = "TYPE B";
            break;

        case RS_NFC_CARD_TYPE_FELICA:
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_3:
            rf_tech = "TYPE F";
            break;

        case RS_NFC_CARD_TYPE_ISO15693:
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_5:
            rf_tech = "TYPE V";
            break;

        default:
            rf_tech = "UNKNOWN";
            break;
    }

    APP_LOG("RF tech  : "app_nfc_writer_log_COL_BRIGHT_CYAN "%s\r\n" app_nfc_writer_log_COL_RESET, rf_tech);

    /* Tag Type */
    APP_LOG("Tag type : %s\r\n",
           (NULL != result->tag_type_name)
               ? result->tag_type_name : "Unknown");

    /* UID */
    APP_LOG("UID      : ");

    if (0u == result->uid_len)
    {
        APP_LOG("N/A");
    }
    else
    {
        for (uint8_t i = 0; i < result->uid_len; i++)
        {
            if (i)
            {
                APP_LOG(":");
            }

            APP_LOG("%02X", result->uid[i]);
        }
    }

    APP_LOG("\r\n");

    /* Size / Writeable */
    if (result->data_area_size > 0u)
    {
        APP_LOG("Size     : %u B\r\n",
               (unsigned) result->data_area_size);
        APP_LOG("Writeable: %s\r\n",
               result->writeable ? "Yes" : "No");
    }
    else
    {
        APP_LOG("Size     : N/A\r\n");
        APP_LOG("Writeable: N/A\r\n");
    }

    /* NDEF records */
    if (result->ndef_present && (result->ndef_len > 0u))
    {
        APP_LOG("NDEF     : %u B\r\n", (unsigned) result->ndef_len);
        APP_LOG("Raw      :");

        for (uint32_t k = 0u; k < result->ndef_len; k++)
        {
            if ((k > 0u) && (0u == (k % 16u)))
            {
                APP_LOG("\r\n          ");
            }
            APP_LOG(" %02X", result->ndef_data[k]);
        }
        APP_LOG("\r\n");

        /* Decode the NDEF message into individual records and print their
         * human-readable content (Text / URI / raw payload preview). */
        print_ndef_records(result);
    }
    else
    {
        APP_LOG("Records  : none\r\n");
    }
}
