#include "theengs_l2.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const theengs_value_t *find_value(const theengs_decoded_values_t *out,
                                         theengs_property_id_t id,
                                         uint8_t instance)
{
    for (uint8_t i = 0; i < out->value_count; ++i) {
        if (out->values[i].property_id == id && out->values[i].instance == instance) return &out->values[i];
    }
    return NULL;
}

static void expect_float(const theengs_decoded_values_t *out, theengs_property_id_t id,
                         uint8_t instance, float expected, float tolerance)
{
    const theengs_value_t *value = find_value(out, id, instance);
    assert(value != NULL && value->type == THEENGS_VALUE_FLOAT);
    assert(fabsf(value->value.f32 - expected) <= tolerance);
}

static void expect_u32(const theengs_decoded_values_t *out, theengs_property_id_t id,
                       uint8_t instance, uint32_t expected)
{
    const theengs_value_t *value = find_value(out, id, instance);
    assert(value != NULL && value->type == THEENGS_VALUE_UINT32 && value->value.u32 == expected);
}

static void expect_i32(const theengs_decoded_values_t *out, theengs_property_id_t id, int32_t expected)
{
    const theengs_value_t *value = find_value(out, id, 0);
    assert(value != NULL && value->type == THEENGS_VALUE_INT32 && value->value.i32 == expected);
}

static void test_ruuvi_valid(void)
{
    const uint8_t payload[] = {0x05,0x12,0xfc,0x53,0x94,0xc3,0x7c,0x00,0x04,0xff,0xfc,0x04,
                               0x0c,0xac,0x36,0x42,0x00,0xcd,0xcb,0xb8,0x33,0x4c,0x88,0x4f};
    theengs_parsed_adv_t adv = {.manufacturer_data = payload, .manufacturer_data_len = sizeof(payload)};
    theengs_decoded_values_t out;
    assert(theengs_decode_by_id(THEENGS_DECODER_RUUVI_RAW_V2, &adv, &out) == THEENGS_STATUS_OK);
    assert(out.value_count == 10u && !out.incomplete);
    expect_float(&out, THEENGS_PROPERTY_TEMPERATURE_C, 0, 24.3f, 0.001f);
    expect_float(&out, THEENGS_PROPERTY_HUMIDITY_PERCENT, 0, 53.49f, 0.001f);
    expect_u32(&out, THEENGS_PROPERTY_PRESSURE_PA, 0, 100044u);
    expect_i32(&out, THEENGS_PROPERTY_ACCEL_X_MG, 4);
    expect_i32(&out, THEENGS_PROPERTY_ACCEL_Y_MG, -4);
    expect_i32(&out, THEENGS_PROPERTY_ACCEL_Z_MG, 1036);
    expect_u32(&out, THEENGS_PROPERTY_VOLTAGE_MV, 0, 2977u);
    expect_i32(&out, THEENGS_PROPERTY_TX_POWER_DBM, 4);
    expect_u32(&out, THEENGS_PROPERTY_MOVEMENT_COUNT, 0, 66u);
    expect_u32(&out, THEENGS_PROPERTY_MEASUREMENT_SEQUENCE, 0, 205u);
}

static void test_ruuvi_invalid_and_bounds(void)
{
    const uint8_t invalid[] = {0x05,0x80,0x00,0xff,0xff,0xff,0xff,0x80,0x00,0x80,0x00,0x80,
                               0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    theengs_parsed_adv_t adv = {.manufacturer_data = invalid, .manufacturer_data_len = sizeof(invalid)};
    theengs_decoded_values_t out;
    assert(theengs_decode_by_id(THEENGS_DECODER_RUUVI_RAW_V2, &adv, &out) == THEENGS_STATUS_OK);
    assert(out.value_count == 0u);
    adv.manufacturer_data_len = sizeof(invalid) - 1u;
    assert(theengs_decode_by_id(THEENGS_DECODER_RUUVI_RAW_V2, &adv, &out) == THEENGS_STATUS_TRUNCATED_INPUT);
    uint8_t wrong[sizeof(invalid)]; memcpy(wrong, invalid, sizeof(wrong)); wrong[0] = 0x03u;
    adv.manufacturer_data = wrong; adv.manufacturer_data_len = sizeof(wrong);
    assert(theengs_decode_by_id(THEENGS_DECODER_RUUVI_RAW_V2, &adv, &out) == THEENGS_STATUS_UNSUPPORTED_PAYLOAD);
}

static void test_bthome_example(void)
{
    const uint8_t payload[] = {0x40,0x02,0xc4,0x09,0x03,0xbf,0x13};
    theengs_parsed_adv_t adv = {.service_data = payload, .service_data_len = sizeof(payload)};
    theengs_decoded_values_t out;
    assert(theengs_decode_by_id(THEENGS_DECODER_BTHOME_V2, &adv, &out) == THEENGS_STATUS_OK);
    assert(out.value_count == 2u);
    expect_float(&out, THEENGS_PROPERTY_TEMPERATURE_C, 0, 25.0f, 0.001f);
    expect_float(&out, THEENGS_PROPERTY_HUMIDITY_PERCENT, 0, 50.55f, 0.001f);
}

static void test_bthome_rich_and_duplicates(void)
{
    const uint8_t payload[] = {0x40,0x00,0x09,0x01,0x61,0x02,0xc4,0x09,0x03,0xbf,0x13,
        0x04,0x13,0x8a,0x01,0x05,0x13,0x8a,0x14,0x0c,0x02,0x0c,0x11,0x01,
        0x12,0xe2,0x04,0x14,0x02,0x0c,0x21,0x01,0x2e,0x23,0x2f,0x23,0x3a,0x04};
    theengs_parsed_adv_t adv = {.service_data = payload, .service_data_len = sizeof(payload)};
    theengs_decoded_values_t out;
    assert(theengs_decode_by_id(THEENGS_DECODER_BTHOME_V2, &adv, &out) == THEENGS_STATUS_OK);
    assert(out.value_count == 14u);
    expect_u32(&out, THEENGS_PROPERTY_PACKET_ID, 0, 9u);
    expect_u32(&out, THEENGS_PROPERTY_BATTERY_PERCENT, 0, 97u);
    expect_u32(&out, THEENGS_PROPERTY_PRESSURE_PA, 0, 100883u);
    expect_float(&out, THEENGS_PROPERTY_ILLUMINANCE_LUX, 0, 13460.67f, 0.01f);
    expect_u32(&out, THEENGS_PROPERTY_VOLTAGE_MV, 0, 3074u);
    const theengs_value_t *opening = find_value(&out, THEENGS_PROPERTY_OPENING, 0);
    assert(opening != NULL && opening->type == THEENGS_VALUE_BOOL && opening->value.boolean);
    expect_u32(&out, THEENGS_PROPERTY_CO2_PPM, 0, 1250u);
    expect_float(&out, THEENGS_PROPERTY_MOISTURE_PERCENT, 0, 30.74f, 0.001f);
    const theengs_value_t *motion = find_value(&out, THEENGS_PROPERTY_MOTION, 0);
    assert(motion != NULL && motion->type == THEENGS_VALUE_BOOL && motion->value.boolean);
    expect_float(&out, THEENGS_PROPERTY_HUMIDITY_PERCENT, 1, 35.0f, 0.001f);
    expect_float(&out, THEENGS_PROPERTY_MOISTURE_PERCENT, 1, 35.0f, 0.001f);
    const theengs_value_t *button = find_value(&out, THEENGS_PROPERTY_BUTTON_EVENT, 0);
    assert(button != NULL && button->type == THEENGS_VALUE_ENUM && button->value.enumeration == THEENGS_BUTTON_EVENT_LONG_PRESS);
}

static void test_bthome_fail_closed(void)
{
    theengs_decoded_values_t out;
    const uint8_t encrypted[] = {0x41,0x01,0x64};
    theengs_parsed_adv_t adv = {.service_data = encrypted, .service_data_len = sizeof(encrypted)};
    assert(theengs_decode_by_id(THEENGS_DECODER_BTHOME_V2, &adv, &out) == THEENGS_STATUS_UNSUPPORTED_PAYLOAD);
    const uint8_t truncated[] = {0x40,0x02,0x34}; adv.service_data = truncated; adv.service_data_len = sizeof(truncated);
    assert(theengs_decode_by_id(THEENGS_DECODER_BTHOME_V2, &adv, &out) == THEENGS_STATUS_TRUNCATED_INPUT && out.incomplete);
    const uint8_t partial[] = {0x40,0x01,0x64,0x7f}; adv.service_data = partial; adv.service_data_len = sizeof(partial);
    assert(theengs_decode_by_id(THEENGS_DECODER_BTHOME_V2, &adv, &out) == THEENGS_STATUS_PARTIAL && out.incomplete);
    expect_u32(&out, THEENGS_PROPERTY_BATTERY_PERCENT, 0, 100u);
}

static void test_result_bound(void)
{
    uint8_t payload[1u + 17u * 2u]; payload[0] = 0x40u;
    for (size_t i = 0; i < 17u; ++i) { payload[1u + i * 2u] = 0x01u; payload[2u + i * 2u] = (uint8_t)i; }
    theengs_parsed_adv_t adv = {.service_data = payload, .service_data_len = sizeof(payload)};
    theengs_decoded_values_t out;
    assert(theengs_decode_by_id(THEENGS_DECODER_BTHOME_V2, &adv, &out) == THEENGS_STATUS_RESULT_FULL);
    assert(out.value_count == THEENGS_MAX_VALUES && out.incomplete);
    expect_u32(&out, THEENGS_PROPERTY_BATTERY_PERCENT, 15, 15u);
}

static void test_malformed_sweep(void)
{
    uint8_t bytes[64]; uint32_t state = 0x31415926u; theengs_decoded_values_t out;
    for (size_t len = 0; len <= sizeof(bytes); ++len) {
        for (size_t i = 0; i < sizeof(bytes); ++i) { state = state * 1664525u + 1013904223u; bytes[i] = (uint8_t)(state >> 24); }
        theengs_parsed_adv_t adv = {.service_data = bytes, .service_data_len = len,
                                    .manufacturer_data = bytes, .manufacturer_data_len = len};
        (void)theengs_decode_by_id(THEENGS_DECODER_RUUVI_RAW_V2, &adv, &out); assert(out.value_count <= THEENGS_MAX_VALUES);
        (void)theengs_decode_by_id(THEENGS_DECODER_BTHOME_V2, &adv, &out); assert(out.value_count <= THEENGS_MAX_VALUES);
    }
}

static void test_metadata_and_dispatch(void)
{
    theengs_model_info_t model; theengs_property_info_t prop; theengs_parsed_adv_t empty = {0}; theengs_decoded_values_t out;
    assert(theengs_model_info(THEENGS_DECODER_RUUVI_RAW_V2, &model) == THEENGS_STATUS_OK);
    assert(strcmp(model.model_id, "RUUVI_RAW_V2") == 0 && model.input_kind == THEENGS_INPUT_MANUFACTURER_DATA);
    assert(theengs_property_info(THEENGS_DECODER_BTHOME_V2, THEENGS_PROPERTY_BUTTON_EVENT, &prop) == THEENGS_STATUS_OK);
    assert(prop.value_type == THEENGS_VALUE_ENUM);
    assert(theengs_property_info(THEENGS_DECODER_RUUVI_RAW_V2, THEENGS_PROPERTY_BUTTON_EVENT, &prop) == THEENGS_STATUS_NOT_FOUND);
    assert(theengs_model_info(0xffffu, &model) == THEENGS_STATUS_UNKNOWN_DECODER);
    assert(theengs_decode_by_id(0xffffu, &empty, &out) == THEENGS_STATUS_UNKNOWN_DECODER);
}

int main(void)
{
    test_ruuvi_valid();
    test_ruuvi_invalid_and_bounds();
    test_bthome_example();
    test_bthome_rich_and_duplicates();
    test_bthome_fail_closed();
    test_result_bound();
    test_malformed_sweep();
    test_metadata_and_dispatch();
    puts("theengs_l2 host tests: PASS");
    return 0;
}
