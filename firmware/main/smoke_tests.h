#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SMOKE_TEST_WIFI = 0,
    SMOKE_TEST_BLE,
    SMOKE_TEST_I154,
    SMOKE_TEST_SD_RW,
    SMOKE_TEST_SD_FORMAT,
    SMOKE_TEST_COUNT,
} smoke_test_kind_t;

typedef struct {
    smoke_test_kind_t kind;
    esp_err_t err;
    uint32_t primary;
    uint32_t secondary;
    char detail[64];
} smoke_test_result_t;

esp_err_t smoke_test_run(smoke_test_kind_t kind, smoke_test_result_t *out_result);

#ifdef __cplusplus
}
#endif
