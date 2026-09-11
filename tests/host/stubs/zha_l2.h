#pragma once

/*
 * Host-build stub for zha_l2.
 *
 * See tests/host/stubs/theengs_l2.h for why this exists. Values match
 * firmware/components/zha_l2/include/zha_l2.h; only what the application's
 * quirk-availability query needs is declared.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZHA_MAX_NAME_LEN 32u

#define ZHA_QUIRK_ID_IKEA_VALLHORN 0xCE5181FFu
#define ZHA_QUIRK_ID_SONOFF_BUTTON 0x82538874u

typedef enum {
    ZHA_STATUS_OK = 0,
    ZHA_STATUS_INVALID_ARG,
    ZHA_STATUS_NOT_FOUND,
    ZHA_STATUS_NO_SPACE,
    ZHA_STATUS_TRUNCATED,
    ZHA_STATUS_UNSUPPORTED,
    ZHA_STATUS_RANGE,
} zha_status_t;

typedef struct {
    const char *project;
    const char *revision;
    const char *path;
    const char *license;
    const char *canonical_key;
} zha_quirk_provenance_t;

typedef struct {
    uint32_t quirk_id;
    const char *label;
    zha_quirk_provenance_t provenance;
} zha_quirk_info_t;

zha_status_t zha_quirk_get_info(uint32_t quirk_id, zha_quirk_info_t *out);
