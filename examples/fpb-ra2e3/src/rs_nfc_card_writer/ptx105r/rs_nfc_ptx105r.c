/**
 * rs_nfc_ptx105r.c
 *
 * Implementation for the Renesas PTX105R NFC reader, calling the Renesas
 * PTX NFC SDK (ptxIoTRd_* / ptx_IOT_READER.h) directly instead of going
 * through the RM_NFC_READER_PTX FSP wrapper. Peripheral (SPI/GPIO/Timer)
 * bring-up still uses the ptxPLAT_* /ptxPERIPH_* platform-glue functions
 * that are also used internally by the FSP wrapper, pointed at the
 * FSP-generated peripheral instances in g_nfc_reader_ptx0_cfg.
 *
 * Each function is a direct, non-static entry point declared in
 * rs_nfc_ptx105r.h and called by name from the rest of the NFC Card
 * Reader SBB — no function-pointer vtable indirection.
 *
 * There is no local state-machine (open/idle/discovered/activated) here:
 * we rely on the PTX SDK's own error handling/return codes instead of
 * re-implementing the FSP wrapper's defensive state checks.
 */

#include "rs_nfc_ptx105r.h"
#include "rs_nfc_ptx105r_board.h"
#include "rs_nfc_wait_state_priv.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_data.h"
/* PTX NFC SDK — used directly instead of the RM_NFC_READER_PTX FSP API.
 * NOTE: We intentionally use the lean ptxNDEF_T3TOP/T4TOP/T5TOP (Type-3/
 * Type-4/Type-5 Tag NDEF Operation) components instead of the generic
 * ptxNDEF dispatcher. The generic ptxNDEF_Open() unconditionally links
 * T2TOP+T3TOP+T4TOP+T5TOP (~13 KB flash) which does not fit this MCU's
 * flash budget. T2T NDEF read/write remains hand-rolled
 * (rs_ndef_read.c/rs_ndef_write.c). */
#include "ptx_IOT_READER.h"
#include "ptxNDEF_T3TOP.h"
#include "ptxNDEF_T4TOP.h"
#include "ptxNDEF_T5TOP.h"
#include "ptxPLAT_GPIO.h"
#include "ptxPLAT_SPI.h"
#include "ptxPLAT_TIMER.h"
#include "ptxPERIPH_APPTIMER.h"

/***********************************************************************************************************************
 * Module-local constants
 **********************************************************************************************************************/
#define PTX105R_ZERO                  (0)
#define PTX105R_SHUTDOWN_TEMP         (223U)
#define PTX105R_TIMEOUT_RAW           (200U) /* Application-timeout for raw-protocol exchanges */

/* Only perform temperature-sensor calibration once per power cycle */
static bool g_start_temp_calibration = true;

/* Tracks whether rs_nfc_ptx_open() succeeded (replaces FSP ctrl->open) */
static bool g_ptx_opened = false;

/***********************************************************************************************************************
 * SDK T4T NDEF component — static allocation (shared TX/RX buffers).
 * Lean alternative to the generic ptxNDEF_t dispatcher (see note above).
 **********************************************************************************************************************/
static ptxNDEF_T4TOP_t  g_t4t_ndef_comp;
static uint8_t          g_ndef_tx_buf[RS_NFC_PTX_TX_BUF_SIZE];
static uint8_t          g_ndef_rx_buf[RS_NFC_PTX_RX_BUF_SIZE];

/***********************************************************************************************************************
 * SDK T3T NDEF component — static allocation. Shares the TX/RX buffers
 * above. NFCID2 and MRTI are extracted from the active card at open time.
 **********************************************************************************************************************/
static ptxNDEF_T3TOP_t  g_t3t_ndef_comp;

/***********************************************************************************************************************
 * SDK T5T NDEF component — static allocation. Shares the TX/RX buffers
 * above; needs a small dedicated work buffer (block-sized scratch area).
 **********************************************************************************************************************/
#define T5T_WORK_BUF_SIZE  32U
static ptxNDEF_T5TOP_t  g_t5t_ndef_comp;
static uint8_t          g_t5t_work_buf[T5T_WORK_BUF_SIZE];

/***********************************************************************************************************************
 * Interrupt-driven wait support
 **********************************************************************************************************************/

static volatile TaskHandle_t g_waiting_task = NULL;

static void ptx105r_irq_wake_cb (external_irq_callback_args_t * p_args)
{
    RS_COMMON_UNUSED(p_args);

    /* Record only; nothing can be printed from an ISR. */
    RS_NFC_WAIT_IRQ();

    if (NULL != g_waiting_task)
    {
        BaseType_t higher_prio_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(g_waiting_task, &higher_prio_task_woken);
        portYIELD_FROM_ISR(higher_prio_task_woken);
    }
}

/***********************************************************************************************************************
 * Internal helpers
 **********************************************************************************************************************/

static rs_nfc_card_type_t map_card_type (ptxIoTRd_CardParams_t * card,
                                         ptxIoTRd_CardProtocol_t prot)
{
    if (NULL == card)
    {
        return RS_NFC_CARD_TYPE_UNKNOWN;
    }

    switch (card->TechType)
    {
        case Tech_TypeA:
            switch (prot)
            {
                case Prot_T2T:    return RS_NFC_CARD_TYPE_NFC_TAG_TYPE_2;
                case Prot_ISODEP: return RS_NFC_CARD_TYPE_NFC_TAG_TYPE_4A;
                default:          return RS_NFC_CARD_TYPE_ISO14443A;
            }
        case Tech_TypeB:
            return (Prot_ISODEP == prot) ? RS_NFC_CARD_TYPE_NFC_TAG_TYPE_4B
                                         : RS_NFC_CARD_TYPE_ISO14443B;
        case Tech_TypeF:
            return (Prot_T3T == prot) ? RS_NFC_CARD_TYPE_NFC_TAG_TYPE_3
                                      : RS_NFC_CARD_TYPE_FELICA;
        case Tech_TypeV:
            return (Prot_T5T == prot) ? RS_NFC_CARD_TYPE_NFC_TAG_TYPE_5
                                      : RS_NFC_CARD_TYPE_ISO15693;
        default:
            return RS_NFC_CARD_TYPE_UNKNOWN;
    }
}

static ptxIoTRd_CardProtocol_t choose_protocol (ptxIoTRd_CardParams_t * card)
{
    if (NULL == card)
    {
        return Prot_Undefined;
    }

    switch (card->TechType)
    {
        case Tech_TypeA:
            if (0u != (card->TechParams.CardAParams.SEL_RES & 0x40u))
                return Prot_NFCDEP;
            if (0u != (card->TechParams.CardAParams.SEL_RES & 0x20u))
                return Prot_ISODEP;
            return Prot_T2T;
        case Tech_TypeB:
            if (0u != (card->TechParams.CardBParams.SENSB_RES[10] & 0x01u))
                return Prot_ISODEP;
            return Prot_Undefined;
        case Tech_TypeF:
            if ((0x01u == card->TechParams.CardFParams.SENSF_RES[0]) &&
                (0xFEu == card->TechParams.CardFParams.SENSF_RES[1]))
                return Prot_NFCDEP;
            return Prot_T3T;
        case Tech_TypeV:
            return Prot_T5T;
        case Tech_TypeExtension:
            return Prot_Extension;
        default:
            return Prot_Undefined;
    }
}

static void extract_uid (ptxIoTRd_CardParams_t * card, uint8_t * uid, uint8_t * uid_len)
{
    uint8_t len = 0;
    *uid_len = 0;

    if (NULL == card)
    {
        return;
    }

    switch (card->TechType)
    {
        case Tech_TypeA:
        {
            len = card->TechParams.CardAParams.NFCID1_LEN;

            if (len > RS_NFC_UID_MAX_BYTES)
            {
                len = RS_NFC_UID_MAX_BYTES;
            }

            memcpy(uid, card->TechParams.CardAParams.NFCID1, len);
            *uid_len = len;
            break;
        }
        case Tech_TypeB:
            memcpy(uid, &card->TechParams.CardBParams.SENSB_RES[1], 4u);
            *uid_len = 4u;
            break;
        case Tech_TypeF:
            memcpy(uid, &card->TechParams.CardFParams.SENSF_RES[2], 8u);
            *uid_len = 8u;
            break;
        case Tech_TypeV:
            for (uint8_t i = 0; i < 8u; i++)
            {
                uid[i] = card->TechParams.CardVParams.UID[7u - i];
            }
            *uid_len = 8u;
            break;
        default:
            break;
    }
}

static rs_status_t ptx105r_discover_status (rs_nfc_ptx_disc_status_t * out_status)
{
    if (NULL == out_status)
    {
        return RS_ERR_INVALID_CFG;
    }

    uint8_t raw = 0;
    ptxStatus_t st = ptxIoTRd_Get_Status_Info(g_nfc_reader_ptx0_cfg.iot_reader_context,
                                              StatusType_Discover, &raw);

    if (ptxStatus_Success != st)
    {
        return RS_ERR_INTERNAL;
    }

    switch (raw)
    {
        case RF_DISCOVER_STATUS_CARD_ACTIVE:     *out_status = RS_NFC_DISC_CARD_ACTIVE; break;
        case RF_DISCOVER_STATUS_DISCOVER_RUNNING:*out_status = RS_NFC_DISC_RUNNING;     break;
        case RF_DISCOVER_STATUS_DISCOVER_DONE:   *out_status = RS_NFC_DISC_DONE;        break;
        default:                                 *out_status = RS_NFC_DISC_NO_CARD;     break;
    }

    return RS_OK;
}

static rs_status_t ptx105r_system_check (void)
{
    uint8_t state = 0;
    ptxStatus_t st = ptxIoTRd_Get_Status_Info(g_nfc_reader_ptx0_cfg.iot_reader_context,
                                              StatusType_System, &state);

    if (ptxStatus_Success != st)
    {
        return RS_ERR_INTERNAL;
    }

    return (PTX_SYSTEM_STATUS_OK == state) ? RS_OK : RS_ERR_INTERNAL;
}

static ptxIoTRd_CardRegistry_t * g_active_reg = NULL;

/***********************************************************************************************************************
 * PTX SDK function implementations
 **********************************************************************************************************************/

rs_status_t rs_nfc_ptx_open (void)
{
    nfc_reader_ptx_cfg_t const * p_cfg = &g_nfc_reader_ptx0_cfg;

    /* Define IoT Reader parameters (mirrors former RM_NFC_READER_PTX_Open) */
    ptxIoTRd_InitPars_t            init_params;
    ptxIoTRd_TempSense_Params_t    temp_sensor;
    ptxIoTRd_ComInterface_Params_t com_interface;

    memset(&init_params, PTX105R_ZERO, sizeof(init_params));
    memset(&temp_sensor, PTX105R_ZERO, sizeof(temp_sensor));
    memset(&com_interface, PTX105R_ZERO, sizeof(com_interface));

    /* Temperature-sensor calibration is only performed once per power cycle */
    if (true == g_start_temp_calibration)
    {
        temp_sensor.Calibrate = p_cfg->temp_sensor_calibrate;
        temp_sensor.Tambient  = p_cfg->temp_sensor_ambient;
        temp_sensor.Tshutdown = p_cfg->temp_sensor_shutdown;
        g_start_temp_calibration = false;
    }
    else
    {
        temp_sensor.Tshutdown = PTX105R_SHUTDOWN_TEMP;
    }

    init_params.TemperatureSensor = &temp_sensor;
    init_params.ComInterface      = &com_interface;

    /* Bring up the low-level peripherals (SPI, GPIO/IRQ, timers) required
     * by the PTX SDK. These are the same platform-glue calls the FSP
     * wrapper used to make internally. */
    if (ptxStatus_Success != ptxPLAT_GPIO_Open(p_cfg->p_gpio_context, p_cfg->p_irq_context, p_cfg->interrupt_pin))
    {
        return RS_ERR_INTERNAL;
    }
    if (ptxStatus_Success != ptxPLAT_TIMER_Open(p_cfg->p_timer_context))
    {
        return RS_ERR_INTERNAL;
    }
    if (ptxStatus_Success != ptxPERIPH_APPTIMER_Open(p_cfg->p_app_timer))
    {
        return RS_ERR_INTERNAL;
    }
    if (ptxStatus_Success != ptxPLAT_SPI_Open(p_cfg->p_comms_instance_ctrl, p_cfg->p_gpio_context))
    {
        return RS_ERR_INTERNAL;
    }

    /* Initiate the IoT-Reader System (PTX SDK) */
    ptxStatus_t st = ptxIoTRd_Init(p_cfg->iot_reader_context, &init_params);
    if (ptxStatus_Success != st)
    {
        /* Retry once after a Deinit, mirrors the FSP wrapper's recovery path */
        (void)ptxIoTRd_Deinit(p_cfg->iot_reader_context);
        st = ptxIoTRd_Init(p_cfg->iot_reader_context, &init_params);
    }

    if (ptxStatus_Success != st)
    {
        return RS_ERR_INTERNAL;
    }

    g_ptx_opened = true;
    return RS_OK;
}

rs_status_t rs_nfc_ptx_close(void)
{
    g_active_reg = NULL;
    ptxStatus_t st = ptxIoTRd_Deinit(g_nfc_reader_ptx0_cfg.iot_reader_context);
    g_ptx_opened = false;
    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

bool rs_nfc_ptx_is_open(void)
{
    return g_ptx_opened;
}

rs_status_t rs_nfc_ptx_configure_discovery(rs_nfc_tech_mask_t tech_mask)
{
    RS_COMMON_UNUSED(tech_mask);
    return RS_OK;
}

rs_status_t rs_nfc_ptx_start_discovery(void)
{
    nfc_reader_ptx_cfg_t const * p_cfg = &g_nfc_reader_ptx0_cfg;

    ptxIoTRd_DiscConfig_t disc_config;
    (void)memset(&disc_config, PTX105R_ZERO, sizeof(disc_config));

    disc_config.PollTypeA            = p_cfg->poll_type_a;
    disc_config.PollTypeB            = p_cfg->poll_type_b;
    disc_config.PollTypeF212         = p_cfg->poll_type_f;
    disc_config.PollTypeV            = p_cfg->poll_type_v;
    disc_config.IdleTime             = p_cfg->idle_time_ms;
    disc_config.PollTypeADeviceLimit = p_cfg->device_limit;
    disc_config.PollTypeBDeviceLimit = p_cfg->device_limit;
    disc_config.PollTypeVDeviceLimit = p_cfg->device_limit;
    disc_config.PollTypeFDeviceLimit = p_cfg->device_limit;
    disc_config.Discover_Mode        = p_cfg->discover_mode;

    if (!(disc_config.PollTypeA || disc_config.PollTypeB ||
          disc_config.PollTypeF212 || disc_config.PollTypeV))
    {
        return RS_ERR_INVALID_CFG;
    }

    ptxStatus_t st = ptxIoTRd_Initiate_Discovery(p_cfg->iot_reader_context, &disc_config);
    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

rs_status_t rs_nfc_ptx_stop_discovery(void)
{
    ptxStatus_t st = ptxIoTRd_Reader_Deactivation(g_nfc_reader_ptx0_cfg.iot_reader_context,
                                                  PTX_IOTRD_RF_DEACTIVATION_TYPE_IDLE);
    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

rs_status_t rs_nfc_ptx_wait_for_card(uint32_t timeout_ms, rs_nfc_ptx_disc_status_t *out_status)
{
    if (NULL == out_status) { return RS_ERR_INVALID_CFG; }
    *out_status = RS_NFC_DISC_NO_CARD;

    rs_status_t sys = ptx105r_system_check();
    if (RS_OK != sys)
    {
        /* Do not leave the previous wait's state in place after a reader fault. */
        RS_NFC_WAIT_END(RS_NFC_WAIT_PHASE_ERROR);
        return sys;
    }

    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        R_BSP_SoftwareDelay(timeout_ms, BSP_DELAY_UNITS_MILLISECONDS);
        return ptx105r_discover_status(out_status);
    }

    TickType_t remaining_ticks = (UINT32_MAX == timeout_ms)
                                  ? portMAX_DELAY
                                  : pdMS_TO_TICKS(timeout_ms);
    TickType_t start_tick      = xTaskGetTickCount();

    /* Before the reader IRQ callback is armed, so the ISR is the only writer from here on. */
    RS_NFC_WAIT_BEGIN();

    g_waiting_task = xTaskGetCurrentTaskHandle();

    (void)g_ext_irq.p_api->callbackSet(g_ext_irq.p_ctrl, ptx105r_irq_wake_cb, NULL, NULL);

    (void)ulTaskNotifyTake(pdTRUE, 0);

    rs_status_t st = RS_OK;

    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, remaining_ticks);

        (void)g_ext_irq.p_api->callbackSet(g_ext_irq.p_ctrl, ptxPLAT_GPIO_IsrCallback, NULL, NULL);

        st = ptx105r_discover_status(out_status);
        if (RS_OK != st)
        {
            RS_NFC_WAIT_END(RS_NFC_WAIT_PHASE_ERROR);
            break;
        }

        /* Only an activated (or fully-discovered) card ends the wait. A
         * DISCOVER_RUNNING status means the reader is still scanning, so keep
         * waiting for the next IRQ; returning early on RUNNING makes a
         * finite-timeout caller report a false "timed out". */
        if ((RS_NFC_DISC_CARD_ACTIVE == *out_status) ||
            (RS_NFC_DISC_DONE        == *out_status))
        {
            RS_NFC_WAIT_END(RS_NFC_WAIT_PHASE_CARD);
            break;
        }

        if (UINT32_MAX != timeout_ms)
        {
            TickType_t total   = pdMS_TO_TICKS(timeout_ms);
            TickType_t elapsed = xTaskGetTickCount() - start_tick;
            if (elapsed >= total)
            {
                RS_NFC_WAIT_END(RS_NFC_WAIT_PHASE_TIMEOUT);
                break;
            }
            remaining_ticks = total - elapsed;
        }

        g_waiting_task = xTaskGetCurrentTaskHandle();
        (void)g_ext_irq.p_api->callbackSet(g_ext_irq.p_ctrl, ptx105r_irq_wake_cb, NULL, NULL);
    }

    g_waiting_task = NULL;
    return st;
}

rs_status_t rs_nfc_ptx_activate_card(rs_nfc_ptx_card_info_t *card_info)
{
    if (NULL == card_info) { return RS_ERR_INVALID_CFG; }
    (void)memset(card_info, 0, sizeof(*card_info));

    ptxIoTRd_t *iot_rd = g_nfc_reader_ptx0_cfg.iot_reader_context;

    ptxIoTRd_CardRegistry_t *reg = NULL;
    ptxStatus_t st = ptxIoTRd_Get_Card_Registry(iot_rd, &reg);
    if ((ptxStatus_Success != st) || (NULL == reg)) { return RS_ERR_INTERNAL; }

    g_active_reg = reg;

    if (NULL != reg->ActiveCard)
    {
        card_info->card_type = map_card_type(reg->ActiveCard, reg->ActiveCardProtType);
        card_info->protocol  = reg->ActiveCardProtType;
        extract_uid(reg->ActiveCard, card_info->uid, &card_info->uid_len);
        return RS_OK;
    }

    if (0u == reg->NrCards) { return RS_ERR_NOT_FOUND; }

    ptxIoTRd_CardProtocol_t prot = choose_protocol(&reg->Cards[0]);
    st = ptxIoTRd_Activate_Card(iot_rd, &reg->Cards[0], prot);
    if (ptxStatus_Success != st) { return RS_ERR_INTERNAL; }

    card_info->card_type = map_card_type(reg->ActiveCard, reg->ActiveCardProtType);
    card_info->protocol  = reg->ActiveCardProtType;
    extract_uid(reg->ActiveCard, card_info->uid, &card_info->uid_len);
    return RS_OK;
}

rs_status_t rs_nfc_ptx_get_card_type(rs_nfc_card_type_t *out_type)
{
    if (NULL == out_type) { return RS_ERR_INVALID_CFG; }

    if (NULL == g_active_reg || NULL == g_active_reg->ActiveCard)
    {
        *out_type = RS_NFC_CARD_TYPE_UNKNOWN;
        return RS_ERR_NOT_FOUND;
    }

    *out_type = map_card_type(g_active_reg->ActiveCard, g_active_reg->ActiveCardProtType);
    return RS_OK;
}

rs_status_t rs_nfc_ptx_get_uid(uint8_t *uid, uint8_t *uid_len)
{
    if ((NULL == uid) || (NULL == uid_len)) { return RS_ERR_INVALID_CFG; }

    if (NULL == g_active_reg || NULL == g_active_reg->ActiveCard)
    {
        *uid_len = 0;
        return RS_ERR_NOT_FOUND;
    }

    extract_uid(g_active_reg->ActiveCard, uid, uid_len);
    return RS_OK;
}

void rs_nfc_ptx_sleep(uint32_t ms)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    else
    {
        R_BSP_SoftwareDelay(ms, BSP_DELAY_UNITS_MILLISECONDS);
    }
}

rs_status_t rs_nfc_ptx_data_exchange(const uint8_t *tx, uint32_t tx_len,
                                       uint8_t *rx, uint32_t *rx_len)
{
    if ((NULL == tx) || (NULL == rx) || (NULL == rx_len)) { return RS_ERR_INVALID_CFG; }

    ptxStatus_t st = ptxIoTRd_Data_Exchange(g_nfc_reader_ptx0_cfg.iot_reader_context,
                                            (uint8_t *)(uintptr_t)tx, tx_len,
                                            rx, rx_len, PTX105R_TIMEOUT_RAW);

    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

rs_status_t rs_nfc_ptx_deactivate(void)
{
    ptxStatus_t st = ptxIoTRd_Reader_Deactivation(g_nfc_reader_ptx0_cfg.iot_reader_context,
                                                  PTX_IOTRD_RF_DEACTIVATION_TYPE_DISCOVER);
    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

rs_status_t rs_nfc_ptx_get_system_state(uint8_t *out_state)
{
    if (NULL == out_state) { return RS_ERR_INVALID_CFG; }
    ptxStatus_t st = ptxIoTRd_Get_Status_Info(g_nfc_reader_ptx0_cfg.iot_reader_context,
                                              StatusType_System, out_state);
    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

rs_status_t rs_nfc_ptx_get_last_rf_error(uint8_t *out_err)
{
    if (NULL == out_err) { return RS_ERR_INVALID_CFG; }
    ptxStatus_t st = ptxIoTRd_Get_Status_Info(g_nfc_reader_ptx0_cfg.iot_reader_context,
                                              StatusType_LastRFError, out_err);
    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

void rs_nfc_ptx_wake_waiting_task(void)
{
    TaskHandle_t task = g_waiting_task;
    if (NULL != task)
    {
        (void)xTaskNotifyGive(task);
    }
}

/***********************************************************************************************************************
 * SDK T4T NDEF component lifecycle (lean — see include-block note above)
 *
 * Call rs_nfc_ptx_ndef_open() once per card activation (protocol ==
 * Prot_ISODEP), and rs_nfc_ptx_ndef_close() after NDEF
 * operations are finished.
 **********************************************************************************************************************/

rs_status_t rs_nfc_ptx_ndef_open(void)
{
    ptxNDEF_T4TOP_InitParams_t params;
    (void)memset(&params, 0, sizeof(params));

    params.RxBuffer                  = g_ndef_rx_buf;
    params.RxBufferSize              = sizeof(g_ndef_rx_buf);
    params.T4TInitParams.IotRd       = g_nfc_reader_ptx0_cfg.iot_reader_context;
    params.T4TInitParams.TxBuffer    = g_ndef_tx_buf;
    params.T4TInitParams.TxBufferSize = sizeof(g_ndef_tx_buf);

    ptxStatus_t st = ptxNDEF_T4TOpOpen(&g_t4t_ndef_comp, &params);
    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

void rs_nfc_ptx_ndef_close(void)
{
    (void)ptxNDEF_T4TOpClose(&g_t4t_ndef_comp);
}

ptxNDEF_T4TOP_t * rs_nfc_ptx_get_ndef_comp(void)
{
    return &g_t4t_ndef_comp;
}

/***********************************************************************************************************************
 * SDK T5T NDEF component lifecycle (lean — see include-block note above)
 *
 * Call rs_nfc_ptx_ndef_t5t_open() once per card activation (protocol ==
 * Prot_T5T), and rs_nfc_ptx_ndef_t5t_close() after NDEF
 * operations are finished.
 **********************************************************************************************************************/

rs_status_t rs_nfc_ptx_ndef_t5t_open(void)
{
    ptxNDEF_T5TOP_InitParams_t params;
    (void)memset(&params, 0, sizeof(params));

    params.RxBuffer                   = g_ndef_rx_buf;
    params.RxBufferSize               = sizeof(g_ndef_rx_buf);
    params.WorkBuffer                 = g_t5t_work_buf;
    params.WorkBufferSize             = sizeof(g_t5t_work_buf);
    params.T5TInitParams.IotRd        = g_nfc_reader_ptx0_cfg.iot_reader_context;
    params.T5TInitParams.TxBuffer     = g_ndef_tx_buf;
    params.T5TInitParams.TxBufferSize = sizeof(g_ndef_tx_buf);

    /* Non-addressed mode: correct for the common single-tag-in-field
     * scenario. (Multi-tag V-type addressing would need the active card's
     * UID here — omitted to save flash on this tight-budget MCU.) */

    ptxStatus_t st = ptxNDEF_T5TOpOpen(&g_t5t_ndef_comp, &params);
    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

void rs_nfc_ptx_ndef_t5t_close(void)
{
    (void)ptxNDEF_T5TOpClose(&g_t5t_ndef_comp);
}

ptxNDEF_T5TOP_t * rs_nfc_ptx_get_ndef_t5t_comp(void)
{
    return &g_t5t_ndef_comp;
}

/***********************************************************************************************************************
 * SDK T3T NDEF component lifecycle
 *
 * Call rs_nfc_ptx_ndef_t3t_open() once per card activation (protocol ==
 * Prot_T3T), and rs_nfc_ptx_ndef_t3t_close() after NDEF
 * operations are finished. NFCID2 and MRTI timing parameters are read
 * from the active card's SENSF_RES — the card must already be activated.
 **********************************************************************************************************************/

rs_status_t rs_nfc_ptx_ndef_t3t_open (void)
{
    /* Guard: need an activated FeliCa / T3T card in the registry */
    if ((NULL == g_active_reg) || (NULL == g_active_reg->ActiveCard))
    {
        return RS_ERR_NOT_FOUND;
    }

    ptxNDEF_T3TOP_InitParams_t params;
    (void)memset(&params, 0, sizeof(params));

    params.RxBuffer                    = g_ndef_rx_buf;
    params.RxBufferSize                = sizeof(g_ndef_rx_buf);
    params.T3TInitParams.IotRd         = g_nfc_reader_ptx0_cfg.iot_reader_context;
    params.T3TInitParams.TxBuffer      = g_ndef_tx_buf;
    params.T3TInitParams.TxBufferSize  = sizeof(g_ndef_tx_buf);

    /* NFCID2 starts at SENSF_RES[2] (8 bytes) */
    params.T3TInitParams.NFCID2        = &g_active_reg->ActiveCard->TechParams.CardFParams.SENSF_RES[2];
    params.T3TInitParams.NFCID2Len     = g_active_reg->ActiveCard->TechParams.CardFParams.SENSF_RES_LEN;
    /* MRTI_Check and MRTI_Update are at SENSF_RES[15] and SENSF_RES[16] */
    params.T3TInitParams.MRTI_Check    = g_active_reg->ActiveCard->TechParams.CardFParams.SENSF_RES[15];
    params.T3TInitParams.MRTI_Update   = g_active_reg->ActiveCard->TechParams.CardFParams.SENSF_RES[16];

    ptxStatus_t st = ptxNDEF_T3TOpOpen(&g_t3t_ndef_comp, &params);

    return (ptxStatus_Success == st) ? RS_OK : RS_ERR_INTERNAL;
}

void rs_nfc_ptx_ndef_t3t_close (void)
{
    (void)ptxNDEF_T3TOpClose(&g_t3t_ndef_comp);
}

ptxNDEF_T3TOP_t * rs_nfc_ptx_get_ndef_t3t_comp (void)
{
    return &g_t3t_ndef_comp;
}