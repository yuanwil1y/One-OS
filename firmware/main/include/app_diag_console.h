#pragma once

/*
 * Serial diagnostic console for the headless runtime.
 *
 * Starts the UART line reader. The console is transport only: it submits
 * requests to the application runtime and prints the resulting reports.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_diag_console_start(void);

#ifdef __cplusplus
}
#endif
