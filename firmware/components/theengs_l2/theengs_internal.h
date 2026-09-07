#ifndef THEENGS_INTERNAL_H
#define THEENGS_INTERNAL_H

#include "theengs_l2.h"

theengs_status_t theengs_decode_ruuvi_raw_v2(const theengs_parsed_adv_t *adv,
                                             theengs_decoded_values_t *out);
theengs_status_t theengs_decode_bthome_v2(const theengs_parsed_adv_t *adv,
                                         theengs_decoded_values_t *out);

theengs_status_t theengs_append_float(theengs_decoded_values_t *out,
                                      theengs_property_id_t property_id,
                                      float value);
theengs_status_t theengs_append_i32(theengs_decoded_values_t *out,
                                    theengs_property_id_t property_id,
                                    int32_t value);
theengs_status_t theengs_append_u32(theengs_decoded_values_t *out,
                                    theengs_property_id_t property_id,
                                    uint32_t value);
theengs_status_t theengs_append_bool(theengs_decoded_values_t *out,
                                     theengs_property_id_t property_id,
                                     bool value);
theengs_status_t theengs_append_enum(theengs_decoded_values_t *out,
                                     theengs_property_id_t property_id,
                                     uint32_t value);

#endif
