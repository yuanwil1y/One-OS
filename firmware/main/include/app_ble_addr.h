#pragma once

/*
 * BLE device addresses, in the one order this application uses.
 *
 * WHY THIS FILE EXISTS
 *
 * A BLE address is six bytes and there are two reasonable orders to hold them in,
 * and both are already in this tree:
 *
 *   - the CONTROLLER order, which is what the radio reports and what it must be
 *     given back: `addr[0]` is the least significant byte. NimBLE reports a peer
 *     in this order (ble_addr_t::val), so it is the order that arrives from a scan
 *     and the order ble_gap_connect() expects;
 *   - the DISPLAY order, or "wire order", which is what every user-facing surface
 *     prints: `addr[0]` is the most significant byte, so the address reads as
 *     `c4:99:4c:1a:2b:3d`. This is what a person reads off the device's label, what
 *     nRF Connect shows, and what a recognition recipe's BLE_PUBLIC_ADDRESS key
 *     means.
 *
 * Holding controller order in the application is a real defect, not a cosmetic
 * one: it makes `app_device`'s generated device id print the address backwards
 * (`3d2b1a4c99c4` for a device labelled `c4:99:4c:1a:2b:3d`), which is both
 * confusing to an operator reading the serial console and wrong against the
 * database, whose BLE_PUBLIC_ADDRESS rule is a match key. It also turns any
 * comparison between a scanned address and an address written down by a human into
 * a comparison of two different encodings.
 *
 * THE RULE
 *
 *   Everything in this application above the radio boundary holds DISPLAY order.
 *
 * A controller-order address is converted exactly once, where it enters the
 * application - app_scan_native.c, immediately after the scan report - and is never
 * converted again. app_ble_gatt.h documents the same convention for the GATT
 * session's peer, and because the GATT path is handed an address that came from
 * scan evidence, the conversion must NOT be repeated there: converting twice is
 * the same as not converting, so a second reversal would silently restore the bug
 * while every unit test of the conversion still passed.
 *
 * That is why app_ble_addr_wire_to_controller() exists as well: the radio side is
 * the one place display order has to be turned back into controller order, and the
 * round trip (wire -> controller -> wire) is what a host test can pin without a
 * radio. A test that only checked one direction could not tell a correct
 * implementation from one that reverses twice.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Controller order -> display order.
 *
 * `addr` and `out` may be the same buffer, so "convert where the address enters
 * the application" is a single call with no temporary at the call site. Returns
 * out, or NULL when either argument is NULL. Both must hold six bytes.
 *
 * This is the only conversion applied to a scanned address: converting twice is
 * the same as not converting, so a second reversal would restore the original bug
 * while every test of this function still passed. See app_ble_native's handoff
 * test for the check that the scan path applies it exactly once.
 */
uint8_t *app_ble_addr_to_wire(const uint8_t *addr, uint8_t out[6]);

/*
 * Display order -> controller order.
 *
 * Same aliasing rule, same return value. This is what a caller needs before
 * handing an address to the radio, and it is the inverse of app_ble_addr_to_wire():
 * applying both returns the original bytes.
 */
uint8_t *app_ble_addr_to_controller(const uint8_t *addr, uint8_t out[6]);

/*
 * Are two addresses the same, in either order?
 *
 * Deliberately order-insensitive. Addresses reaching this function should all be
 * in display order, but an order-insensitive comparison means a caller that
 * forgot to convert - or converted twice - is caught as "these are the same
 * device" rather than silently producing two device records for one thing. The
 * cost is that two addresses which are byte-reverses of each other cannot be told
 * apart, which for a 48-bit random address is not a case worth a wrong merge for.
 *
 * A NULL argument is never equal to anything, including another NULL.
 */
bool app_ble_addr_equal(const uint8_t *a, const uint8_t *b);

/*
 * Is this a usable address?
 *
 * A zero address is not an identity: a controller that failed to report one leaves
 * zeros, and every such device would otherwise be merged into one record. This is
 * the check app_scan_tracker already applies to observations.
 */
bool app_ble_addr_is_usable(const uint8_t *addr);

/*
 * Format into `out` as `aa:bb:cc:dd:ee:ff`, lowercase, for a console line or a
 * diagnostic. `out` needs 18 bytes. Returns the written length (17), or 0 when the
 * buffer is too small - a truncated address presented as a complete one is worse
 * than none.
 */
size_t app_ble_addr_format(const uint8_t *addr, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
