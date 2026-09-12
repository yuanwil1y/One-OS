/* Host-test stub for the ESP-IDF hardware RNG.
 *
 * The firmware seeds Noise ephemeral keys from esp_fill_random(). Host tests
 * must be deterministic, so this stub produces a reproducible byte stream.
 * It is NOT a random number generator and is never compiled into firmware.
 * Tests that need real entropy properties pin the ephemeral key explicitly
 * through esphome_noise_set_fixed_ephemeral(). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);
#ifdef __cplusplus
}
#endif
