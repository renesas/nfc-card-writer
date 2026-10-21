#include "new_thread0.h"
#include "app_nfc_writer.h"

/* New Thread entry function */
/* pvParameters contains TaskHandle_t */
void new_thread0_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED (pvParameters);

    /* Start the NFC card writer application. This runs the interactive CLI
     * (read / erase / write) forever, driving the NFC Writer SBB for each
     * command; it does not return. */
    app_nfc_writer_entry();

    /* Should not reach here; suspend if it does. */
    vTaskSuspend(NULL);
}
