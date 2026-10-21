/**
 * rs_nfc_reader.c
 *
 * NFC Reader SBB — main orchestrator (blocking + non-blocking).
 *
 * Owns the full FSP-driven discovery/activation/read state machine. The
 * application interacts exclusively through the SBB public API:
 *
 *   - rs_nfc_reader_Read()         : run the interrupt-driven detect/read loop
 *   - rs_nfc_reader_Stop()         : request graceful stop
 *
 * Operating modes (selected via cfg fields):
 *
 *   1. Blocking event-loop (callback==NULL, on_card_event!=NULL)
 *   2. Blocking single-shot (callback==NULL, on_card_event==NULL)
 *   3. Non-blocking (callback!=NULL): spawns a static FreeRTOS task that
 *      runs mode 1 or 2 internally. Read() returns RS_OK immediately.
 */

#include "rs_nfc_reader.h"
#include "rs_nfc_ptx105r.h"
#include <string.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

/***********************************************************************************************************************
 * Constants
 **********************************************************************************************************************/
#define DEFAULT_TIMEOUT_MS       5000U
#define DEFAULT_RETRY_COUNT      0U
#define SUMMARY_BUF_SIZE         128U

/* IRQ-only wait: the task blocks until the reader IRQ fires, Stop() sends
 * a task notification, or the caller-supplied timeout_ms expires. No
 * periodic host-side wake is used; the system-state / RF-warning
 * accessors are re-read only when the task is woken by one of those
 * events. */

/* Async worker task configuration (static allocation — no heap) Do not reduce below 4096. */
#define ASYNC_TASK_STACK_WORDS   (4096U / sizeof(StackType_t))
#define ASYNC_TASK_PRIORITY      1U
#define ASYNC_TASK_NAME          "RS_NFC"

/* PTX system/RF status raw codes */
#define PTX_SYS_STATUS_OK                       0x00u
#define PTX_RF_ERR_WARNING_PA_OVERCURRENT_LIMIT 0x06u

/* Static buffers for the optional per-card raw demo exchange.
 * Sized to match the frames built inside rs_nfc_reader_raw_exchange
 * (largest = T5T READ_SINGLE_BLOCK = 11 bytes TX; RX capped by HAL).
 * Only one Read() runs at a time (async re-entrancy is guarded by
 * g_async_ctx.active), so a single set of static buffers is safe.
 * Compiled only with RS_NFC_BUILD_RAW_EXCHANGE, like their sole user, so a
 * build without the raw exchange carries no unused 580-byte RAM buffers. */
#if RS_NFC_BUILD_RAW_EXCHANGE
#define RAW_TX_BUF_SIZE  280U
#define RAW_RX_BUF_SIZE  300U
static uint8_t g_raw_tx_buf[RAW_TX_BUF_SIZE];
static uint8_t g_raw_rx_buf[RAW_RX_BUF_SIZE];
#endif

/***********************************************************************************************************************
 * Per-call orchestrator state
 **********************************************************************************************************************/
typedef enum e_rs_nfc_reader_loop_state {
    LOOP_WAIT_FOR_ACTIVATION = 0,
    LOOP_DATA_EVENT,
    LOOP_DEACTIVATE,
    LOOP_SYSTEM_ERROR,
} rs_nfc_reader_loop_state_t;

/***********************************************************************************************************************
 * Stop-requested flag + reader-active flag
 *
 * g_reader_active is true between the entry of rs_nfc_reader_Read() and
 * the completion of the corresponding operation (blocking return, or async
 * worker self-delete). Stop() uses it to reject calls when nothing is
 * running.
 **********************************************************************************************************************/
static volatile bool g_stop_requested = false;
static volatile bool g_reader_active  = false;

static bool rs_nfc_reader_is_stop_requested (void)
{
    return g_stop_requested;
}

/***********************************************************************************************************************
 * Non-blocking async context (static allocation)
 **********************************************************************************************************************/
typedef struct st_rs_nfc_reader_async_ctx {
    rs_nfc_reader_cfg_t    cfg;            /* deep copy of caller cfg  */
    rs_nfc_card_result_t * p_result_out;   /* caller's result pointer  */
    volatile bool          active;         /* re-entrancy guard        */
} rs_nfc_reader_async_ctx_t;

static rs_nfc_reader_async_ctx_t  g_async_ctx;
static StaticTask_t         g_async_task_tcb;
static StackType_t          g_async_task_stack[ASYNC_TASK_STACK_WORDS];
static TaskHandle_t         g_async_task_handle = NULL;

/***********************************************************************************************************************
 * Forward declarations
 **********************************************************************************************************************/
static rs_status_t rs_nfc_read_blocking(const rs_nfc_reader_cfg_t * cfg,
                                         rs_nfc_card_result_t      * result_out);
static rs_status_t run_event_loop(const rs_nfc_reader_cfg_t * cfg,
                                   rs_nfc_card_result_t      * result_out);

/* Internal helpers (previously public API — now module-private). */
static rs_status_t rs_nfc_reader_validate(const rs_nfc_reader_cfg_t * cfg);

#if RS_NFC_BUILD_RAW_EXCHANGE
static rs_status_t rs_nfc_reader_raw_exchange(ptxIoTRd_CardProtocol_t protocol,
                                              const uint8_t     * uid,
                                              uint8_t             uid_len,
                                              uint8_t           * tx,
                                              uint32_t          * tx_len,
                                              uint8_t           * rx,
                                              uint32_t          * rx_len);
#endif

/* Module-private helpers (no external linkage). */
static rs_status_t rs_nfc_reader_detect_wait(uint32_t               timeout_ms,
                                             rs_nfc_ptx_disc_status_t * out_status);
static bool rs_nfc_reader_is_stop_requested(void);
static uint32_t rs_nfc_reader_card_summary_build(const rs_nfc_card_result_t * res,
                                                 char                       * buf,
                                                 uint32_t                     buf_size);

/* Single-attempt read + retry wrapper + dependency check (reader-private). */
static rs_status_t rs_nfc_reader_try_once(const void * cfg, void * result_out);
static rs_status_t rs_nfc_reader_retry(const void * cfg,
                                       void       * result_out,
                                       uint8_t      max_retries);
static rs_status_t rs_nfc_reader_validate_deps(void);

/* Defined in rs_ndef_read.c — reads CC/NDEF/tag metadata into the result
 * (and fills result->decoded). Module-internal; not part of the public API. */
rs_status_t rs_ndef_read_card_info(ptxIoTRd_CardProtocol_t protocol,
                                   rs_nfc_card_result_t * result,
                                   uint32_t               max_ndef_bytes);

/***********************************************************************************************************************
 * Card detection (merged from rs_nfc_detect.c)
 *
 * Wait for a card-discovery event, the timeout to expire, or a stop
 * request. Interrupt-driven — the task blocks on the reader IRQ. Uses only
 * the rs_nfc_ptx API.
 **********************************************************************************************************************/
static rs_status_t rs_nfc_reader_detect_wait (uint32_t timeout_ms, rs_nfc_ptx_disc_status_t * out_status)
{
    rs_status_t st = RS_OK;

    if (NULL == out_status)
    {
        return RS_ERR_INVALID_CFG;
    }

    /* Early exit on stop request */
    if (rs_nfc_reader_is_stop_requested())
    {
        *out_status = RS_NFC_DISC_NO_CARD;

        return RS_OK;
    }

    /* System-health check is now performed internally by
     * rs_nfc_ptx_wait_for_card. */

    /* Block (zero-CPU) until the reader's IRQ line signals an event or
     * the timeout elapses. If rs_nfc_reader_Stop() is called while
     * we are blocked, it sends a task notification to wake us
     * immediately so we can observe the stop flag. */
    st = rs_nfc_ptx_wait_for_card(timeout_ms, out_status);

    if (RS_OK != st)
    {
        return st;
    }

    if ((RS_NFC_DISC_CARD_ACTIVE == *out_status) ||
        (RS_NFC_DISC_DONE        == *out_status))
    {
        return RS_OK;  /* card found or discovery done */
    }

    /* Check stop again — we may have been woken by Stop(). */
    if (rs_nfc_reader_is_stop_requested())
    {
        *out_status = RS_NFC_DISC_NO_CARD;

        return RS_OK;
    }

    *out_status = RS_NFC_DISC_NO_CARD;

    return RS_ERR_TIMEOUT;
}

/***********************************************************************************************************************
 * Single-attempt (used by retry wrapper & single-shot)
 **********************************************************************************************************************/
static rs_status_t rs_nfc_reader_try_once (const void * cfg_raw, void * result_raw)
{
    const rs_nfc_reader_cfg_t * cfg = (const rs_nfc_reader_cfg_t *) cfg_raw;
    rs_nfc_card_result_t      * res = (rs_nfc_card_result_t *) result_raw;
    rs_status_t st;
    bool want_ndef;
    uint32_t timeout = (NULL != cfg) ? cfg->timeout_ms : DEFAULT_TIMEOUT_MS;

    /* 1. Wait for a card */
    rs_nfc_ptx_disc_status_t disc = RS_NFC_DISC_NO_CARD;
    st = rs_nfc_reader_detect_wait(timeout, &disc);

    if (RS_OK != st)
    {
        return st;
    }

    /* If stop was requested during the wait, detect_wait returns RS_OK
     * with NO_CARD — treat as a clean exit. */
    if (g_stop_requested || (RS_NFC_DISC_NO_CARD == disc))
    {
        return RS_OK;
    }

    /* 2. Activate the card */
    rs_nfc_ptx_card_info_t card_info;
    st = rs_nfc_ptx_activate_card(&card_info);

    if (RS_OK != st)
    {
        return st;
    }

    /* 3. Fill basic result fields */
    if (NULL != res)
    {
        res->card_type = card_info.card_type;
        res->protocol  = card_info.protocol;
        (void)memcpy(res->uid, card_info.uid, card_info.uid_len);
        res->uid_len      = card_info.uid_len;
        res->ndef_present = false;
        res->ndef_len     = 0;
        res->rssi_dbm     = 0;
        res->read_time_ms = 0;

        /* 4. Optionally read NDEF (richer read via rs_ndef_read_card_info,
         * which also populates data_area_size / writeable / tag_type_name). */
        want_ndef = (NULL != cfg) ? cfg->read_ndef : true;

        if (want_ndef)
        {
            uint32_t ndef_cap = (NULL != cfg) ? (uint32_t) cfg->max_ndef_bytes : (uint32_t) RS_NFC_NDEF_MAX_BYTES;

            /* The NDEF outcome is part of the read result: an oversized
             * message, an unreadable one, or a card with no NDEF at all must
             * reach the caller (spec 3.4.2 F-03 / F-15, 3.2.2 T-18 / T-19).
             * Card identity has already been filled in above, so the caller
             * still sees card_type / uid even on these failures. */
            rs_status_t ndef_st = rs_ndef_read_card_info(res->protocol, res, ndef_cap);

            if (RS_OK != ndef_st)
            {
                return ndef_st;
            }
        }
    }

    return RS_OK;
}

/***********************************************************************************************************************
 * Retry wrapper
 *
 * Attempts a single-shot read up to (max_retries + 1) times, deactivating +
 * re-discovering between attempts.
 **********************************************************************************************************************/
static rs_status_t rs_nfc_reader_retry (const void * cfg, void * result_out, uint8_t max_retries)
{
    rs_status_t st = RS_ERR_TIMEOUT;

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++)
    {
        st = rs_nfc_reader_try_once(cfg, result_out);

        if (RS_OK == st)
        {
            break;
        }

        /* Deactivate + re-discover between retries */
        (void)rs_nfc_ptx_deactivate();
    }

    return st;
}

/***********************************************************************************************************************
 * Runtime dependency validation
 *
 * Checks that the PTX SDK backend has been opened successfully.
 **********************************************************************************************************************/
static rs_status_t rs_nfc_reader_validate_deps (void)
{
    /* rs_nfc_ptx_is_open() reflects whether rs_nfc_ptx_open() (which
     * initializes the PTX SDK IoT-Reader context directly) has succeeded. */
    if (!rs_nfc_ptx_is_open())
    {
        return RS_ERR_DEPENDENCY;
    }

    return RS_OK;
}

/***********************************************************************************************************************
 * Event-loop mode
 **********************************************************************************************************************/
static rs_status_t run_event_loop (const rs_nfc_reader_cfg_t * cfg,
                                   rs_nfc_card_result_t      * result_out)
{
    rs_nfc_card_result_t   local_res;
    rs_nfc_card_result_t * res = (NULL != result_out) ? result_out : &local_res;
    char                    summary[SUMMARY_BUF_SIZE];
    rs_nfc_reader_loop_state_t state         = LOOP_WAIT_FOR_ACTIVATION;
    uint32_t                elapsed_ms    = 0u;
    const bool              loop_forever  = (UINT32_MAX == cfg->timeout_ms);
    uint8_t sys_state;
    uint8_t last_rf_err;

    while (loop_forever || (elapsed_ms < cfg->timeout_ms))
    {
        /* Stop requested? Exit cleanly. */
        if (g_stop_requested)
        {
            return RS_OK;
        }

        /* Critical system-error watchdog */
        sys_state = PTX_SYS_STATUS_OK;

        if (RS_OK == rs_nfc_ptx_get_system_state(&sys_state))
        {
            if (PTX_SYS_STATUS_OK != sys_state)
            {
                state = LOOP_SYSTEM_ERROR;
            }
        }

        /* PA overcurrent / other RF warning notifications */
        last_rf_err = 0u;
        (void)rs_nfc_ptx_get_last_rf_error(&last_rf_err);

        if ((PTX_RF_ERR_WARNING_PA_OVERCURRENT_LIMIT == last_rf_err) &&
            (NULL != cfg->on_card_event))
        {
            cfg->on_card_event(RS_OK, NULL,
                               "WARN: PA overcurrent limiter activated",
                               cfg->p_card_event_context);
        }

        switch (state)
        {
            case LOOP_WAIT_FOR_ACTIVATION:
            {
                /* IRQ-only wait: blocks at 0% CPU until the reader IRQ
                 * fires, Stop() sends a task notification, or the full
                 * remaining timeout expires. No periodic host wake. */
                uint32_t wait_ms = loop_forever
                                    ? UINT32_MAX
                                    : (cfg->timeout_ms - elapsed_ms);

                rs_nfc_ptx_disc_status_t disc = RS_NFC_DISC_NO_CARD;

                if (RS_OK != rs_nfc_ptx_wait_for_card(wait_ms, &disc))
                {
                    state = LOOP_DEACTIVATE;
                    break;
                }

                if ((RS_NFC_DISC_CARD_ACTIVE == disc) ||
                    (RS_NFC_DISC_DONE        == disc))
                {
                    state = LOOP_DATA_EVENT;
                }
                else if (!loop_forever)
                {
                    /* No card within the budget: end the finite wait. When a
                     * card WAS found, fall through to LOOP_DATA_EVENT to
                     * process it (Stop() from the callback ends the loop). */
                    elapsed_ms = cfg->timeout_ms;
                }

                break;
            }

            case LOOP_DATA_EVENT:
            {
                rs_nfc_ptx_card_info_t info;
                rs_status_t st = rs_nfc_ptx_activate_card(&info);

                if (RS_OK == st)
                {
                    (void)memset(res, 0, sizeof(*res));
                    res->card_type = info.card_type;
                    res->protocol  = info.protocol;
                    (void)memcpy(res->uid, info.uid, info.uid_len);
                    res->uid_len = info.uid_len;

                    /* Optionally read NDEF (richer read via
                     * rs_ndef_read_card_info, which also populates
                     * data_area_size / writeable / tag_type_name). The NDEF
                     * outcome (no NDEF, unreadable, oversized) is reported to
                     * the application as the event status; card identity is
                     * already filled in, so the result is still delivered. */
                    rs_status_t ndef_st = RS_OK;

                    if (cfg->read_ndef)
                    {
                        ndef_st = rs_ndef_read_card_info(res->protocol, res, (uint32_t) cfg->max_ndef_bytes);
                    }

                    /* Optional raw demo exchange — result exposed to app
                     * via res->raw_exchange so the app does not need to call
                     * rs_nfc_reader_raw_exchange itself. Prior memset() has
                     * already zero-initialised res->raw_exchange. */
#if RS_NFC_BUILD_RAW_EXCHANGE
                    if (cfg->run_raw_exchange &&
                        (Prot_ISODEP    != res->protocol) &&
                        (Prot_Undefined != res->protocol))
                    {
                        uint32_t tx_len = 0u;
                        uint32_t rx_len = RAW_RX_BUF_SIZE;
                        rs_status_t rst = rs_nfc_reader_raw_exchange(
                                              res->protocol,
                                              res->uid, res->uid_len,
                                              g_raw_tx_buf, &tx_len,
                                              g_raw_rx_buf, &rx_len);
                        res->raw_exchange.valid  = true;
                        res->raw_exchange.status = rst;
                        res->raw_exchange.tx     = g_raw_tx_buf;
                        res->raw_exchange.tx_len = tx_len;
                        res->raw_exchange.rx     = g_raw_rx_buf;
                        res->raw_exchange.rx_len = (RS_OK == rst) ? rx_len : 0u;
                    }
#endif

                    /* Build summary string and fire per-card event */
                    (void)rs_nfc_reader_card_summary_build(res, summary, sizeof(summary));

                    if (NULL != cfg->on_card_event)
                    {
                        cfg->on_card_event(ndef_st, res, summary, cfg->p_card_event_context);
                    }
                }
                else
                {
                    if (NULL != cfg->on_card_event)
                    {
                        cfg->on_card_event(st, NULL, "card activate failed",
                                           cfg->p_card_event_context);
                    }
                }

                state = LOOP_DEACTIVATE;
                break;
            }

            case LOOP_DEACTIVATE:
            {
                (void)rs_nfc_ptx_deactivate();    /* restart discovery */
                state = LOOP_WAIT_FOR_ACTIVATION;
                break;
            }

            case LOOP_SYSTEM_ERROR:
            {
                if (NULL != cfg->on_card_event)
                {
                    cfg->on_card_event(RS_ERR_INTERNAL, NULL,
                       "ERROR: critical system-error (overcurrent/temperature)",
                       cfg->p_card_event_context);
                }

                return RS_ERR_INTERNAL;
            }

            default:
                break;
        }
    }

    return RS_OK; /* timed out without a fatal error */
}

/***********************************************************************************************************************
 * Blocking core
 **********************************************************************************************************************/
static rs_status_t rs_nfc_read_blocking (const rs_nfc_reader_cfg_t * cfg,
                                         rs_nfc_card_result_t      * result_out)
{
    rs_status_t st;

    if (NULL != result_out)
    {
        (void)memset(result_out, 0, sizeof(*result_out));
    }

    st = rs_nfc_ptx_open();

    if (RS_OK != st)
    {
        return st;
    }

    st = rs_nfc_ptx_configure_discovery(cfg->tech_mask);

    if (RS_OK != st)
    {
        (void)rs_nfc_ptx_close();

        return st;
    }

    st = rs_nfc_ptx_start_discovery();

    if (RS_OK != st)
    {
        (void)rs_nfc_ptx_close();

        return st;
    }

    /* Mode selection */
    if (NULL != cfg->on_card_event)
    {
        st = run_event_loop(cfg, result_out);
    }
    else if (cfg->retry_count > 0u)
    {
        st = rs_nfc_reader_retry(cfg, result_out, cfg->retry_count);
    }
    else
    {
        st = rs_nfc_reader_try_once(cfg, result_out);
    }

    (void)rs_nfc_ptx_deactivate();
    (void)rs_nfc_ptx_close();

    return st;
}

/***********************************************************************************************************************
 * Async worker task function
 **********************************************************************************************************************/
static void rs_nfc_async_worker (void * pvParameters)
{
    (void)pvParameters;
    rs_nfc_reader_async_ctx_t * ctx = &g_async_ctx;

    /* Run full blocking flow inside this dedicated task. */
    rs_status_t st = rs_nfc_read_blocking(&ctx->cfg, ctx->p_result_out);

    /* Fire the operation-end callback. */
    if (NULL != ctx->cfg.callback)
    {
        ctx->cfg.callback(st, ctx->cfg.p_context);
    }

    /* Mark context inactive and self-delete. */
    ctx->active = false;
    g_reader_active = false;
    g_async_task_handle = NULL;
    vTaskDelete(NULL);
}

/***********************************************************************************************************************
 * Public API
 **********************************************************************************************************************/

static rs_status_t rs_nfc_reader_validate (const rs_nfc_reader_cfg_t * cfg)
{
    rs_status_t dep_st = RS_OK;

    if (NULL == cfg)
    {
        return RS_ERR_INVALID_CFG;
    }

    if (0u == cfg->tech_mask)
    {
        return RS_ERR_INVALID_CFG;
    }

    if (0u == cfg->timeout_ms)
    {
        return RS_ERR_INVALID_CFG;
    }

    if (cfg->read_ndef)
    {
        if ((0u == cfg->max_ndef_bytes) || (cfg->max_ndef_bytes > RS_NFC_NDEF_MAX_BYTES))
        {
            return RS_ERR_INVALID_CFG;
        }
    }

    /* Non-blocking callback is now accepted (no longer rejected). */
    if (cfg->validate_dependencies)
    {
        dep_st = rs_nfc_reader_validate_deps();

        if (RS_OK != dep_st)
        {
            return dep_st;
        }
    }

    return RS_OK;
}

rs_status_t rs_nfc_reader_Read (const rs_nfc_reader_cfg_t * cfg,
                                rs_nfc_card_result_t      * result_out)
{
    rs_status_t st;

    /* NULL cfg is always rejected, regardless of cfg_valid_check_en. */
    if (NULL == cfg)
    {
        return RS_ERR_INVALID_CFG;
    }

    /* Optional configuration validation (gated by cfg_valid_check_en). */
    if (cfg->cfg_valid_check_en)
    {
        st = rs_nfc_reader_validate(cfg);

        if (RS_OK != st)
        {
            return st;
        }
    }

    /* Non-blocking path */
    if (NULL != cfg->callback)
    {
        /* Re-entrancy guard: only one async Read() at a time. */
        if (g_async_ctx.active)
        {
            return RS_ERR_INTERNAL;
        }

        /* Deep-copy config into static context. */
        (void)memcpy(&g_async_ctx.cfg, cfg, sizeof(*cfg));
        g_async_ctx.p_result_out = result_out;
        g_async_ctx.active       = true;
        g_stop_requested         = false;
        g_reader_active          = true;

        /* Create async worker (static allocation — no heap). */
        g_async_task_handle = xTaskCreateStatic(
            rs_nfc_async_worker,
            ASYNC_TASK_NAME,
            ASYNC_TASK_STACK_WORDS,
            NULL,
            ASYNC_TASK_PRIORITY,
            g_async_task_stack,
            &g_async_task_tcb
        );

        if (NULL == g_async_task_handle)
        {
            g_async_ctx.active = false;
            g_reader_active    = false;

            return RS_ERR_INTERNAL;
        }

        /* returns immediately */
        return RS_OK;
    }

    /* Blocking path */
    g_stop_requested = false;
    g_reader_active  = true;

    st = rs_nfc_read_blocking(cfg, result_out);

    g_reader_active = false;

    return st;
}

rs_status_t rs_nfc_reader_Stop (void)
{
    /* Nothing to stop — no Read() is currently in progress. */
    if (!g_reader_active)
    {
        return RS_ERR_DEPENDENCY;
    }

    /* Stop() already invoked for the current Read(); reject the double
     * request so callers can distinguish the first successful stop from
     * subsequent no-op calls. */
    if (g_stop_requested)
    {
        return RS_ERR_INTERNAL;
    }

    g_stop_requested = true;
    /* Wake any task blocked in rs_nfc_ptx_wait_for_card() so it can
     * observe the stop flag immediately instead of sleeping until the
     * next timeout expiry or IRQ event. */
    rs_nfc_ptx_wake_waiting_task();

    return RS_OK;
}

/***********************************************************************************************************************
 * Card summary
 **********************************************************************************************************************/

static const char HEX_DIGITS[] = "0123456789ABCDEF";

static uint32_t copy_str (char * dst, uint32_t dst_size, uint32_t off, const char * s)
{
    while ((NULL != s) && ('\0' != *s) && (off + 1u < dst_size))
    {
        dst[off++] = *s++;
    }

    return off;
}

static uint32_t copy_hex_byte (char * dst, uint32_t dst_size, uint32_t off, uint8_t b)
{
    if (off + 2u >= dst_size)
    {
        return off;
    }

    dst[off++] = HEX_DIGITS[(b >> 4) & 0x0Fu];
    dst[off++] = HEX_DIGITS[b & 0x0Fu];

    return off;
}

static uint32_t copy_uint (char * dst, uint32_t dst_size, uint32_t off, uint32_t v)
{
    char tmp[11];
    uint32_t n = 0;

    if (0u == v)
    {
        if (off + 1u < dst_size)
        {
            dst[off++] = '0';
        }

        return off;
    }

    while (v > 0u && n < sizeof(tmp))
    {
        tmp[n++] = (char) ('0' + (v % 10u));
        v /= 10u;
    }

    while (n > 0u && off + 1u < dst_size)
    {
        dst[off++] = tmp[--n];
    }

    return off;
}

static const char * card_type_name (rs_nfc_card_type_t t)
{
    switch (t)
    {
        case RS_NFC_CARD_TYPE_ISO14443A:
            return "ISO14443A";
        case RS_NFC_CARD_TYPE_ISO14443B:
            return "ISO14443B";
        case RS_NFC_CARD_TYPE_FELICA:
            return "FeliCa";
        case RS_NFC_CARD_TYPE_ISO15693:
            return "ISO15693";
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_2:
            return "NFC-T2T";
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_3:
            return "NFC-T3T";
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_4A:
            return "NFC-T4A";
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_4B:
            return "NFC-T4B";
        case RS_NFC_CARD_TYPE_NFC_TAG_TYPE_5:
            return "NFC-T5T";
        default:
            return "Unknown";
    }
}

static uint32_t rs_nfc_reader_card_summary_build (const rs_nfc_card_result_t * res,
                                                  char                       * buf,
                                                  uint32_t                     buf_size)
{
    if ((NULL == buf) || (0u == buf_size))
    {
        return 0u;
    }

    if (NULL == res)
    {
        buf[0] = '\0';

        return 0u;
    }

    uint32_t off = 0u;

    off = copy_str(buf, buf_size, off, "CARD DETECTED! type=");
    off = copy_str(buf, buf_size, off, card_type_name(res->card_type));
    off = copy_str(buf, buf_size, off, " UID=");

    for (uint8_t i = 0; i < res->uid_len; i++)
    {
        off = copy_hex_byte(buf, buf_size, off, res->uid[i]);
    }

    if (res->ndef_present)
    {
        off = copy_str(buf, buf_size, off, " NDEF=");
        off = copy_uint(buf, buf_size, off, (uint32_t) res->ndef_len);
        off = copy_str(buf, buf_size, off, "B");
    }

    if (off < buf_size)
    {
        buf[off] = '\0';
    }
    else
    {
        buf[buf_size - 1u] = '\0';
        off = buf_size - 1u;
    }

    return off;
}

/***********************************************************************************************************************
 * Raw exchange
 **********************************************************************************************************************/
#if RS_NFC_BUILD_RAW_EXCHANGE
static rs_status_t rs_nfc_reader_raw_exchange (ptxIoTRd_CardProtocol_t protocol,
                                               const uint8_t     * uid,
                                               uint8_t             uid_len,
                                               uint8_t           * tx,
                                               uint32_t          * tx_len,
                                               uint8_t           * rx,
                                               uint32_t          * rx_len)
{
    if ((NULL == tx) || (NULL == tx_len) || (NULL == rx) || (NULL == rx_len))
    {
        return RS_ERR_INVALID_CFG;
    }

    uint32_t frame_len = 0;

    switch (protocol)
    {
        case Prot_T2T:
        {
            tx[0] = 0x30u;
            tx[1] = 0x00u;
            frame_len = 2u;
            break;
        }

        case Prot_T3T:
        {
            static const uint8_t t3t_tail[] = {
                0x01, 0x0B, 0x00, 0x01, 0x80, 0x00
            };

            tx[0] = 0x06;

            if ((NULL != uid) && (uid_len >= 8u))
            {
                (void)memcpy(&tx[1], uid, 8u);
            }
            else
            {
                (void)memset(&tx[1], 0, 8u);
            }

            (void)memcpy(&tx[9], t3t_tail, sizeof(t3t_tail));
            frame_len = 1u + 8u + (uint32_t) sizeof(t3t_tail);
            break;
        }

        case Prot_T5T:
        {
            tx[0] = 0x22u;
            tx[1] = 0x20u;

            if ((NULL != uid) && (uid_len >= 8u))
            {
                for (uint8_t i = 0; i < 8u; i++)
                {
                    tx[2u + i] = uid[7u - i];
                }
            }
            else
            {
                (void)memset(&tx[2], 0, 8u);
            }

            tx[10] = 0x00u;
            frame_len = 11u;
            break;
        }

        case Prot_NFCDEP:
        {
            tx[0] = 0x00u;
            tx[1] = 0x00u;
            frame_len = 2u;
            break;
        }

        default:
        {
            return RS_ERR_INVALID_CFG;
        }
    }

    *tx_len = frame_len;

    return rs_nfc_ptx_data_exchange(tx, frame_len, rx, rx_len);
}
#endif
