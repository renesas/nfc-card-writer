/**
 * rs_nfc_ptx105r_board.h
 *
 * Integrator-supplied board symbols used by rs_nfc_ptx105r.c.
 */

#ifndef RS_NFC_PTX105R_BOARD_H
#define RS_NFC_PTX105R_BOARD_H

#include "hal_data.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void ptxPLAT_GPIO_IsrCallback(external_irq_callback_args_t *p_args);
extern const external_irq_instance_t g_ext_irq;

#ifdef __cplusplus
}
#endif

#endif /* RS_NFC_PTX105R_BOARD_H */
