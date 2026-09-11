#pragma once

/*
 * Host-build stub for theengs_l2.
 *
 * WHY THIS EXISTS
 * ---------------
 * firmware/main/app_recognition.c asks the Theengs family which decoder ids it
 * actually supports, because "does this firmware carry this decoder" must be
 * answered by the family that owns the id space rather than by a duplicated list
 * in the application. Including the real header is therefore correct in the
 * firmware build.
 *
 * The family's C sources are not part of the host test build of app_device and
 * app_device_db (they would drag in the whole component for one lookup), so this
 * header reproduces ONLY the declarations those groups need, with the same
 * values as firmware/components/theengs_l2/include/theengs_l2.h.
 *
 * It is a test double, never a firmware input: the firmware build resolves
 * "theengs_l2.h" to the real component header because tests/host/stubs is not on
 * its include path. tools/local/run-host-tests.ps1 additionally asserts that the
 * two declarations agree, so this file cannot silently drift.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define THEENGS_MODEL_ID_MAX 24u
#define THEENGS_MODEL_NAME_MAX 32u

typedef uint16_t theengs_decoder_id_t;

enum {
    THEENGS_DECODER_RUUVI_RAW_V2 = 0x0001u,
    THEENGS_DECODER_BTHOME_V2 = 0x0002u,
};

typedef enum {
    THEENGS_STATUS_OK = 0,
    THEENGS_STATUS_PARTIAL = 1,
    THEENGS_STATUS_INVALID_ARGUMENT = -1,
    THEENGS_STATUS_UNKNOWN_DECODER = -2,
    THEENGS_STATUS_TRUNCATED_INPUT = -3,
    THEENGS_STATUS_UNSUPPORTED_PAYLOAD = -4,
    THEENGS_STATUS_RESULT_FULL = -5,
    THEENGS_STATUS_NOT_FOUND = -6,
} theengs_status_t;

typedef enum {
    THEENGS_INPUT_MANUFACTURER_DATA = 1,
    THEENGS_INPUT_SERVICE_DATA = 2,
} theengs_input_kind_t;

typedef struct {
    theengs_decoder_id_t decoder_id;
    theengs_input_kind_t input_kind;
    uint8_t property_count;
    char model_id[THEENGS_MODEL_ID_MAX];
    char model_name[THEENGS_MODEL_NAME_MAX];
} theengs_model_info_t;

theengs_status_t theengs_model_info(theengs_decoder_id_t decoder_id,
                                    theengs_model_info_t *out_info);
