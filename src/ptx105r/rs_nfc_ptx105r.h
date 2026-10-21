/**
 * rs_nfc_ptx105r.h
 *
 * Hardware Abstraction Layer for the NFC Reader SBB.
 * Isolates the SBB business logic from the vendor-specific NFC stack
 * (currently: Renesas PTX105R, calling the PTX NFC SDK's ptxIoTRd_*
 * functions directly — no RM_NFC_READER_PTX FSP wrapper).
 *
 * Each function below is implemented directly by the PTX105R backend
 * (rs_nfc_ptx105r.c) and called directly by name — no function-
 * pointer vtable indirection.
 */

#ifndef RS_NFC_PTX105R_H
#define RS_NFC_PTX105R_H

#include "rs_nfc_reader.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declarations — full definitions in ptxNDEF_T3TOP.h / ptxNDEF_T4TOP.h /
 * ptxNDEF_T5TOP.h (PTX SDK). Do NOT typedef here to avoid collision with the
 * SDK headers' own typedefs. We use the lean T3T/T4T/T5T NDEF-OP components
 * (not the generic ptxNDEF dispatcher) — see rs_nfc_ptx105r.c for why. */
struct ptxNDEF_T3TOP;
struct ptxNDEF_T4TOP;
struct ptxNDEF_T5TOP;

#ifdef __cplusplus
extern "C" {
#endif

/**********************************************************************************************************************
 * Discovery status
 **********************************************************************************************************************/
typedef enum e_rs_nfc_ptx_disc_status {
    RS_NFC_DISC_NO_CARD = 0,
    RS_NFC_DISC_CARD_ACTIVE,
    RS_NFC_DISC_RUNNING,
    RS_NFC_DISC_DONE,
} rs_nfc_ptx_disc_status_t;

/**********************************************************************************************************************
 * Activated card info (filled after activation)
 **********************************************************************************************************************/
#define RS_NFC_PTX_RX_BUF_SIZE   300U
#define RS_NFC_PTX_TX_BUF_SIZE   280U

typedef struct st_rs_nfc_ptx_card_info {
    rs_nfc_card_type_t card_type;
    ptxIoTRd_CardProtocol_t  protocol;
    uint8_t             uid[RS_NFC_UID_MAX_BYTES];
    uint8_t             uid_len;
} rs_nfc_ptx_card_info_t;

/**********************************************************************************************************************
 * PTX functions
 **********************************************************************************************************************/
rs_status_t rs_nfc_ptx_open(void);
rs_status_t rs_nfc_ptx_close(void);
bool rs_nfc_ptx_is_open(void);
rs_status_t rs_nfc_ptx_configure_discovery(rs_nfc_tech_mask_t tech_mask);
rs_status_t rs_nfc_ptx_start_discovery(void);
rs_status_t rs_nfc_ptx_stop_discovery(void);
rs_status_t rs_nfc_ptx_wait_for_card(uint32_t               timeout_ms,
                                      rs_nfc_ptx_disc_status_t * out_status);
rs_status_t rs_nfc_ptx_activate_card(rs_nfc_ptx_card_info_t * card_info);
rs_status_t rs_nfc_ptx_get_card_type(rs_nfc_card_type_t * out_type);
rs_status_t rs_nfc_ptx_get_uid(uint8_t * uid, uint8_t * uid_len);
void rs_nfc_ptx_sleep(uint32_t ms);
rs_status_t rs_nfc_ptx_data_exchange(const uint8_t * tx,
                                     uint32_t        tx_len,
                                     uint8_t       * rx,
                                     uint32_t      * rx_len);
rs_status_t rs_nfc_ptx_deactivate(void);
rs_status_t rs_nfc_ptx_get_system_state(uint8_t * out_state);
rs_status_t rs_nfc_ptx_get_last_rf_error(uint8_t * out_err);
void rs_nfc_ptx_wake_waiting_task(void);

/** Open the SDK T4T NDEF component. Call after activating an ISO-DEP
 *  (Type 4 Tag) card, before rs_ndef_read_card_info/WriteNDEF. */
rs_status_t rs_nfc_ptx_ndef_open(void);
/** Close the SDK T4T NDEF component (call after NDEF operations are done). */
void rs_nfc_ptx_ndef_close(void);
/** Get a pointer to the static ptxNDEF_T4TOP_t instance. Valid after ndef_open(). */
struct ptxNDEF_T4TOP * rs_nfc_ptx_get_ndef_comp(void);

/** Open the SDK T3T NDEF component. Call after activating a T3T
 *  (FeliCa / Type 3 Tag) card, before rs_ndef_read_card_info.
 *  Reads NFCID2 and MRTI timing values from the active card registry. */
rs_status_t rs_nfc_ptx_ndef_t3t_open(void);
/** Close the SDK T3T NDEF component (call after NDEF operations are done). */
void rs_nfc_ptx_ndef_t3t_close(void);
/** Get a pointer to the static ptxNDEF_T3TOP_t instance. Valid after ndef_t3t_open(). */
struct ptxNDEF_T3TOP * rs_nfc_ptx_get_ndef_t3t_comp(void);

/** Open the SDK T5T NDEF component. Call after activating a T5T
 *  (ISO 15693) card, before rs_ndef_read_card_info/WriteNDEF. */
rs_status_t rs_nfc_ptx_ndef_t5t_open(void);
/** Close the SDK T5T NDEF component (call after NDEF operations are done). */
void rs_nfc_ptx_ndef_t5t_close(void);
/** Get a pointer to the static ptxNDEF_T5TOP_t instance. Valid after ndef_t5t_open(). */
struct ptxNDEF_T5TOP * rs_nfc_ptx_get_ndef_t5t_comp(void);

#ifdef __cplusplus
}
#endif

#endif /* RS_NFC_PTX105R_H */