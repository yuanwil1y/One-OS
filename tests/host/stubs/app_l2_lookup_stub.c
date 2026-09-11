/*
 * Host-build definitions for the two Level-2 lookups the application makes
 * through tests/host/stubs/theengs_l2.h and tests/host/stubs/zha_l2.h.
 *
 * The application asks each family which decoder/quirk ids it carries, so a
 * Device DB profile naming an id this firmware does not have is refused instead
 * of producing an Entity nothing can fill. The firmware links the real families
 * for that question.
 *
 * The host groups app_device and app_device_db do not otherwise build those
 * components, so this file supplies the same answers for the ids the fixture
 * corpus uses. It is a test double: it is not part of the firmware build, and
 * any test that depends on the answer states the expectation explicitly.
 */

#include "theengs_l2.h"
#include "zha_l2.h"

#include <string.h>

theengs_status_t theengs_model_info(theengs_decoder_id_t decoder_id,
                                    theengs_model_info_t *out_info)
{
    if (out_info == NULL) {
        return THEENGS_STATUS_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));

    switch (decoder_id) {
    case THEENGS_DECODER_RUUVI_RAW_V2:
        out_info->decoder_id = decoder_id;
        out_info->input_kind = THEENGS_INPUT_MANUFACTURER_DATA;
        out_info->property_count = 8u;
        memcpy(out_info->model_id, "ruuvitag", 8u);
        memcpy(out_info->model_name, "RuuviTag RAWv2", 14u);
        return THEENGS_STATUS_OK;
    case THEENGS_DECODER_BTHOME_V2:
        out_info->decoder_id = decoder_id;
        out_info->input_kind = THEENGS_INPUT_SERVICE_DATA;
        out_info->property_count = 18u;
        memcpy(out_info->model_id, "bthome", 6u);
        memcpy(out_info->model_name, "BTHome v2", 9u);
        return THEENGS_STATUS_OK;
    default:
        return THEENGS_STATUS_UNKNOWN_DECODER;
    }
}

zha_status_t zha_quirk_get_info(uint32_t quirk_id, zha_quirk_info_t *out)
{
    if (out == NULL) {
        return ZHA_STATUS_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    switch (quirk_id) {
    case ZHA_QUIRK_ID_IKEA_VALLHORN:
        out->quirk_id = quirk_id;
        out->label = "ikea_vallhorn";
        return ZHA_STATUS_OK;
    case ZHA_QUIRK_ID_SONOFF_BUTTON:
        out->quirk_id = quirk_id;
        out->label = "sonoff_button";
        return ZHA_STATUS_OK;
    default:
        return ZHA_STATUS_NOT_FOUND;
    }
}
