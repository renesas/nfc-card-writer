/**
 * rs_nfc_reader.h
 *
 * NFC Reader SBB public API.
 *
 * This header is the single public include for the module. It provides:
 *   - Shared types and status codes (formerly rs_common.h)
 *   - NDEF parsing/decoding types and API (formerly rs_ndef_util.h)
 *   - NFC Reader configuration, result, and API
 */

#ifndef RS_NFC_READER_H
#define RS_NFC_READER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ptx_IOT_READER.h"

#ifdef __cplusplus
extern "C" {
#endif

/**********************************************************************************************************************
 * Common types, status codes, and callback convention
 **********************************************************************************************************************/

#define RS_COMMON_UNUSED(x) (void)(x)

typedef enum e_rs_status {
    RS_OK                  =   0,
    RS_ERR_TIMEOUT         =  -1,
    RS_ERR_CRED_INVALID    =  -2,
    RS_ERR_CONN_FAIL       =  -3,
    RS_ERR_NO_IP           =  -4,
    RS_ERR_NO_CLOUD        =  -5,
    RS_ERR_DEPENDENCY      =  -6,
    RS_ERR_INVALID_CFG     =  -7,
    RS_ERR_NOT_FOUND       =  -8,
    RS_ERR_BUFFER_OVERFLOW =  -9,

    /* Card / NDEF outcomes (NFC Reader SBB spec 3.4.2 and 3.2.2). Values
     * must stay in step with rs_common.h (pes/common/include in pes_il). */
    RS_ERR_CARD_ACTIVATION = -10,  /* card found but activation failed  */
    RS_ERR_CARD_COLLISION  = -11,  /* anticollision unresolved          */
    RS_ERR_CARD_UNSUPPORTED= -12,  /* card technology not supported     */
    RS_ERR_NDEF_UNSUPPORTED= -13,  /* card carries no readable NDEF     */
    RS_ERR_NDEF_TOO_LARGE  = -14,  /* NDEF exceeds max_ndef_bytes       */
    RS_ERR_READ_FAIL       = -15,  /* read aborted (e.g. card removed)  */
    RS_ERR_BUSY            = -16,  /* operation already in progress     */

    RS_ERR_INTERNAL        = -99,
} rs_status_t;

typedef void (* rs_callback_t)(rs_status_t status, void * p_context);

#ifndef RS_LOG
#define RS_LOG(fmt, ...)   /* default: silent */
#endif

/**********************************************************************************************************************
 * NDEF decoded record types
 *
 * NDEF decoding is performed by the module: rs_nfc_reader_Read() parses the
 * raw NDEF message into rs_nfc_card_result_t.decoded. Applications read
 * result->decoded directly — no decode call is exposed.
 **********************************************************************************************************************/

#define RS_NDEF_MAX_RECORDS        8U
#define RS_NDEF_MAX_TYPE_LEN      32U

typedef struct st_rs_ndef_record {
    uint8_t  tnf;
    uint8_t  type[RS_NDEF_MAX_TYPE_LEN];
    uint8_t  type_len;
    const uint8_t *payload;
    uint32_t payload_len;
    uint8_t  flags;
} rs_ndef_record_t;

typedef struct st_rs_ndef_decoded {
    rs_ndef_record_t records[RS_NDEF_MAX_RECORDS];
    uint8_t           record_count;
    bool              truncated;
} rs_ndef_decoded_t;

/**********************************************************************************************************************
 * Technology mask
 **********************************************************************************************************************/
typedef uint32_t rs_nfc_tech_mask_t;

#define RS_NFC_TECH_ISO14443A   (1UL << 0)
#define RS_NFC_TECH_ISO14443B   (1UL << 1)
#define RS_NFC_TECH_FELICA      (1UL << 2)
#define RS_NFC_TECH_ISO15693    (1UL << 3)
#define RS_NFC_TECH_NFC_FORUM   (1UL << 4)

#define RS_NFC_TECH_ALL  (RS_NFC_TECH_ISO14443A | \
                           RS_NFC_TECH_ISO14443B | \
                           RS_NFC_TECH_FELICA    | \
                           RS_NFC_TECH_ISO15693  | \
                           RS_NFC_TECH_NFC_FORUM)

/**********************************************************************************************************************
 * Named configuration values
 *
 * Cosmetic constants that give meaningful names to the raw literals used
 * when populating rs_nfc_reader_cfg_t. Prefer these over bare true / false
 * / 0 / UINT32_MAX / NULL at call sites for readability.
 **********************************************************************************************************************/

/* timeout_ms */
#define RS_NFC_TIMEOUT_INFINITE          (UINT32_MAX)   /* loop forever */
#define RS_NFC_TIMEOUT_DEFAULT_MS        (5000U)

/* retry_count */
#define RS_NFC_RETRY_DISABLED            (0U)

/* max_ndef_bytes — see RS_NFC_NDEF_MAX_BYTES for the absolute cap */

/* Boolean feature toggles (typed as bool in the struct). */
#define RS_NFC_NDEF_READ_ENABLED          (true)
#define RS_NFC_NDEF_READ_DISABLED         (false)

#define RS_NFC_RAW_EXCHANGE_ENABLED       (true)
#define RS_NFC_RAW_EXCHANGE_DISABLED      (false)

#define RS_NFC_DEP_VALIDATION_ENABLED     (true)
#define RS_NFC_DEP_VALIDATION_DISABLED    (false)

#define RS_NFC_CFG_VALIDATION_ENABLED     (true)
#define RS_NFC_CFG_VALIDATION_DISABLED    (false)

/* Callback / context slot: "no callback" / "no user context". */
#define RS_NFC_CALLBACK_NONE              (NULL)
#define RS_NFC_CONTEXT_NONE               (NULL)

#define RS_NFC_UID_MAX_BYTES              10U
#define RS_NFC_NDEF_MAX_BYTES             512U
/**********************************************************************************************************************
 * Card type (must precede result struct)
 **********************************************************************************************************************/
typedef enum e_rs_nfc_card_type {
    RS_NFC_CARD_TYPE_UNKNOWN = 0,
    RS_NFC_CARD_TYPE_ISO14443A,
    RS_NFC_CARD_TYPE_ISO14443B,
    RS_NFC_CARD_TYPE_FELICA,
    RS_NFC_CARD_TYPE_ISO15693,
    RS_NFC_CARD_TYPE_NFC_TAG_TYPE_2,
    RS_NFC_CARD_TYPE_NFC_TAG_TYPE_3,
    RS_NFC_CARD_TYPE_NFC_TAG_TYPE_4A,
    RS_NFC_CARD_TYPE_NFC_TAG_TYPE_4B,
    RS_NFC_CARD_TYPE_NFC_TAG_TYPE_5,
} rs_nfc_card_type_t;


/**********************************************************************************************************************
 * Callback typedefs (must precede rs_nfc_reader_cfg_t which uses them)
 **********************************************************************************************************************/

/**
 * Fired once when a non-blocking rs_nfc_reader_Read() completes
 * (timeout, fatal error, or rs_nfc_reader_Stop() was called).
 */
typedef void (* rs_nfc_callback_t)(rs_status_t status, void * p_context);

/**********************************************************************************************************************
 * Configuration
 **********************************************************************************************************************/
/**
 * Snapshot of the last raw protocol exchange performed on an activated card.
 * Populated only when cfg->run_raw_exchange = true and the active protocol
 * supports it (i.e. NOT ISO-DEP / UNDEFINED). The tx / rx pointers reference
 * SBB-internal static buffers and MUST NOT be retained past the
 * on_card_event callback (same contract as `summary`). Consumers must check
 * `valid` before using any other field.
 */
typedef struct st_rs_nfc_raw_exchange {
    bool           valid;
    rs_status_t    status;
    const uint8_t *tx;
    uint32_t       tx_len;
    const uint8_t *rx;
    uint32_t       rx_len;
} rs_nfc_raw_exchange_t;

 typedef struct st_rs_nfc_card_result {
    rs_nfc_card_type_t card_type;
    ptxIoTRd_CardProtocol_t  protocol;     /* active RF protocol */
    uint8_t uid[RS_NFC_UID_MAX_BYTES];
    uint8_t uid_len;
    bool ndef_present;
    uint8_t ndef_data[RS_NFC_NDEF_MAX_BYTES];
    uint16_t ndef_len;

    /* Decoded NDEF records, parsed from ndef_data by rs_nfc_reader_Read().
     * Each record's `payload` points into ndef_data above, so it is valid
     * only while this result is alive; do not use after a shallow copy. */
    rs_ndef_decoded_t decoded;

    int8_t rssi_dbm; /* optional, HAL may return 0 if unsupported */
    uint32_t read_time_ms;

    /* Extended card-info fields (populated by rs_ndef_read_card_info) */
    uint32_t    data_area_size;   /**< Tag capacity in bytes (from CC)       */
    bool        writeable;        /**< true if tag write-access is granted   */
    const char *tag_type_name;    /**< Human-readable tag type, e.g.
                                       "NFC Forum Type 2 Tag (T2T)".
                                       Points to a static string — do NOT free. */

    /* Last raw exchange (see rs_nfc_raw_exchange_t doc). Check
     * raw_exchange.valid before use. */
    rs_nfc_raw_exchange_t raw_exchange;
} rs_nfc_card_result_t;

/*
 * Fired by rs_nfc_reader_Read() each time a card is detected, activated
 * and (optionally) NDEF-read. The application MUST treat result/summary as
 * read-only and MUST NOT retain pointers past the call: both buffers are
 * reused on the next iteration of the read loop.
 *
 * status: with a non-NULL result, RS_OK means the card was read completely;
 * any other value is the NDEF-read outcome (no NDEF, unreadable or oversized
 * message) while card_type / uid are still valid. With result == NULL the
 * event is informational or an error (see summary).
 */
typedef void (* rs_nfc_card_event_cb_t)(rs_status_t                          status,
                                        const struct st_rs_nfc_card_result * result,
                                        const char                         * summary,
                                        void                               * p_context);

typedef struct st_rs_nfc_reader_cfg {
    /* RF technologies to enable during discovery (bitmask of
     * RS_NFC_TECH_* flags, e.g. RS_NFC_TECH_ALL). Must be non-zero. */
    rs_nfc_tech_mask_t tech_mask;

    /* Run duration of rs_nfc_reader_Read() in milliseconds.
     * Set to UINT32_MAX to loop forever (never return). */
    uint32_t timeout_ms;
    uint8_t retry_count;

    /* Read options */
    bool read_ndef;
    /* Upper bound (in bytes) for the NDEF payload copied into
     * result->ndef_data. Clamped internally to RS_NFC_NDEF_MAX_BYTES.
     * A value of 0 is rejected by the validator when read_ndef==true;
     * if validation is skipped (see cfg_valid_check_en), 0 is treated
     * as RS_NFC_NDEF_MAX_BYTES by the NDEF reader. */
    uint16_t max_ndef_bytes;

    /* Opt-in demo: after activation, perform a protocol-appropriate raw
     * frame exchange (T2T READ / T3T CHECK / T5T READ_SINGLE_BLOCK / NFC-DEP
     * SYMM). The TX/RX frames are exposed to the on_card_event callback via
     * the result->raw_exchange fields. No effect for ISO-DEP or when
     * on_card_event is NULL. Default: false. */
    bool run_raw_exchange;

    /* Non-blocking support:
     * callback == NULL -> blocking (Read blocks until done)
     * callback != NULL -> non-blocking (Read returns immediately,
     *                     spawns a static FreeRTOS task; callback fires
     *                     on completion). Only one non-blocking Read may
     *                     be active at a time.
     */
    rs_nfc_callback_t callback;
    void * p_context;

    /* Per-card event (fires once per detected/activated card during Read()).
     * When set, the orchestrator runs in continuous-loop mode and emits one
     * event per card until timeout_ms elapses. Works in both blocking and
     * non-blocking modes. */
    rs_nfc_card_event_cb_t   on_card_event;
    void                   * p_card_event_context;

    /* Optional runtime dependency validation */
    bool validate_dependencies;

    /* Gate for rs_nfc_reader_validate() inside rs_nfc_reader_Read().
     *   true  (recommended default): validate cfg fields on entry.
     *   false: skip validation entirely — the caller is responsible
     *          for supplying a well-formed cfg. A NULL cfg is still
     *          rejected regardless of this flag.
     * NOTE: because a memset(&cfg,0,sizeof(cfg)) zero-initialises this
     * field to false, callers MUST explicitly set it to true to keep
     * validation enabled. */
    bool cfg_valid_check_en;
} rs_nfc_reader_cfg_t;

/**********************************************************************************************************************
 * API
 **********************************************************************************************************************/

/**
 * Read NFC cards according to the supplied configuration.
 *
 * Blocking mode (cfg->callback == NULL):
 *   Blocks until timeout_ms elapses or a fatal error occurs.
 *
 * Non-blocking mode (cfg->callback != NULL):
 *   Spawns a dedicated static FreeRTOS task, returns RS_OK immediately.
 *   The callback fires once when the operation completes. Only one
 *   non-blocking Read() may be active at a time; a second call while
 *   a task is running returns RS_ERR_INTERNAL.
 *
 * In both modes, if on_card_event is set the orchestrator runs in
 * continuous-loop mode and fires one event per detected card.
 */
rs_status_t rs_nfc_reader_Read(const rs_nfc_reader_cfg_t * cfg,
                               rs_nfc_card_result_t      * result_out);

/**
 * Request graceful stop of a running Read (blocking or non-blocking).
 *
 * On success the read loop exits cleanly and, in non-blocking mode, the
 * operation-end callback fires with RS_OK. Any task blocked waiting for a
 * card is woken so it observes the stop request immediately rather than
 * sleeping until the next timeout or IRQ.
 *
 * The call is always safe (it never blocks and has no side effects on the
 * error paths), but it is not always successful — check the return value.
 *
 * @retval RS_OK             Stop request accepted; the loop will exit.
 * @retval RS_ERR_DEPENDENCY No Read() is currently in progress.
 * @retval RS_ERR_INTERNAL   Stop() was already called for the current
 *                           Read(); the earlier request is still pending.
 */
rs_status_t rs_nfc_reader_Stop(void);

/**********************************************************************************************************************
 * Wait state (optional)
 *
 * While the SBB waits for a card it records whether the reader raised its interrupt. rs_nfc_reader_GetWaitState()
 * turns that into a named state, so an application can tell "the reader never signalled" from "the reader signalled
 * but no card was reported". The SBB never prints: the application decides what to output and where.
 *
 * The state describes the most recent wait and is reset at the start of every wait. It also works while a wait is
 * still running, which matters in continuous mode with an infinite timeout, where Read() reports nothing.
 *
 * "Interrupt seen" means seen while the SBB was waiting; the SDK's own callback owns the line the rest of the time.
 * Set RS_NFC_WAIT_STATE_ENABLED to 0 to remove this feature (no code, no RAM).
 **********************************************************************************************************************/
#ifndef RS_NFC_WAIT_STATE_ENABLED
#define RS_NFC_WAIT_STATE_ENABLED 1
#endif

typedef enum e_rs_nfc_wait_state
{
    RS_NFC_WAIT_NONE = 0,               /* no wait has started yet                                          */
    RS_NFC_WAIT_NO_IRQ_YET,             /* waiting; no reader interrupt since this wait began               */
    RS_NFC_WAIT_IRQ_SEEN,               /* waiting; interrupt seen, no card reported yet                    */
    RS_NFC_WAIT_CARD,                   /* wait ended: card detected                                        */
    RS_NFC_WAIT_STOPPED,                /* reserved: a wait ended by rs_nfc_reader_Stop(); not reported by this adapter */
    RS_NFC_WAIT_TIMEOUT_NO_IRQ,         /* wait ended: timeout, no interrupt seen                           */
    RS_NFC_WAIT_TIMEOUT_IRQ_NO_CARD,    /* wait ended: timeout, interrupt seen but no card reported         */
    RS_NFC_WAIT_READER_ERROR            /* wait ended: the reader reported a fault or could not be read     */
} rs_nfc_wait_state_t;

#if RS_NFC_WAIT_STATE_ENABLED
/* Task context. Cheap: it only reads state the SBB already keeps. */
rs_nfc_wait_state_t rs_nfc_reader_GetWaitState(void);
#else
static inline rs_nfc_wait_state_t rs_nfc_reader_GetWaitState (void)
{
    return RS_NFC_WAIT_NONE;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* RS_NFC_READER_H */