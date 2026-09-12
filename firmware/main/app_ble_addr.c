/*
 * BLE device addresses. See app_ble_addr.h for the convention and why it is one
 * file.
 *
 * This module is deliberately three tiny functions with no state: the failure it
 * exists to prevent is an address being converted zero times or twice, and a
 * stateless pair of inverses is the only shape where "converted exactly once" can
 * be checked by a host test.
 */

#include "app_ble_addr.h"

uint8_t *app_ble_addr_to_wire(const uint8_t *addr, uint8_t out[6])
{
    uint8_t reversed[6];

    if (addr == NULL || out == NULL) {
        return NULL;
    }
    /*
     * Reversed into a local first, so `addr` and `out` may be the same buffer. A
     * swap-in-place loop would be wrong here, not merely awkward: writing out[0]
     * overwrites the byte that the later index reads, so the result would be a
     * rotation of the address rather than its reverse - and it would be correct
     * only for palindromes, which is exactly the address a test is least likely to
     * use. The whole conversion is six bytes, so the temporary costs nothing.
     */
    for (size_t i = 0u; i < 6u; ++i) {
        reversed[i] = addr[5u - i];
    }
    for (size_t i = 0u; i < 6u; ++i) {
        out[i] = reversed[i];
    }
    return out;
}

uint8_t *app_ble_addr_to_controller(const uint8_t *addr, uint8_t out[6])
{
    /* A byte reversal is its own inverse, and saying so here rather than writing
     * the loop twice is what keeps the two directions from drifting apart. */
    return app_ble_addr_to_wire(addr, out);
}

bool app_ble_addr_equal(const uint8_t *a, const uint8_t *b)
{
    bool forward = true;
    bool reversed = true;

    if (a == NULL || b == NULL) {
        return false;
    }
    for (size_t i = 0u; i < 6u; ++i) {
        if (a[i] != b[i]) {
            forward = false;
        }
        if (a[i] != b[5u - i]) {
            reversed = false;
        }
    }
    return forward || reversed;
}

bool app_ble_addr_is_usable(const uint8_t *addr)
{
    if (addr == NULL) {
        return false;
    }
    for (size_t i = 0u; i < 6u; ++i) {
        if (addr[i] != 0u) {
            return true;
        }
    }
    return false;
}

size_t app_ble_addr_format(const uint8_t *addr, char *out, size_t out_size)
{
    static const char digits[] = "0123456789abcdef";
    size_t used = 0u;

    if (addr == NULL || out == NULL || out_size == 0u) {
        return 0u;
    }
    out[0] = '\0';
    /* 17 characters plus the terminator; anything less cannot hold the result. */
    if (out_size < 18u) {
        return 0u;
    }
    for (size_t i = 0u; i < 6u; ++i) {
        if (i != 0u) {
            out[used++] = ':';
        }
        out[used++] = digits[(addr[i] >> 4) & 0x0Fu];
        out[used++] = digits[addr[i] & 0x0Fu];
    }
    out[used] = '\0';
    return used;
}
