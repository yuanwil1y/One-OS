#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t smoke_gui_init(void);
void smoke_gui_poll(void);

#ifdef __cplusplus
}
#endif
