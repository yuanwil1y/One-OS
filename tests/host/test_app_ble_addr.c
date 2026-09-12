/*
 * BLE address byte order.
 *
 * The defect this pins: the radio reports a peer's address least-significant byte
 * first (NimBLE's ble_addr_t::val), while every user-facing surface prints it most
 * significant byte first. Holding controller order in the application makes
 * app_device's generated device id read backwards and makes a comparison against a
 * database's BLE_PUBLIC_ADDRESS key a comparison of two different encodings.
 *
 * The dangerous failure is not "no conversion" but "conversion applied twice",
 * which is byte-for-byte identical to no conversion at all - so every test of the
 * conversion in isolation still passes. The vectors below are chosen so that
 * double-conversion cannot survive:
 *
 *   1. Both directions are pinned against an address written the way a real
 *      controller reports it (least significant first) and the way it is printed
 *      (most significant first), so the module is anchored to the outside world
 *      rather than to itself.
 *   2. The round trip is checked in both orders.
 *   3. A non-palindromic address is used throughout. A palindrome would make the
 *      conversion a no-op and hide exactly this bug.
 *
 * The host cannot observe the radio, so what is proven here is the convention and
 * both of its directions. Whether the scan path applies it exactly once is proven
 * by app_ble_native's scan-to-GATT handoff test; whether a real controller accepts
 * the result is item 5b.2 of docs/hardware-acceptance.md.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_ble_addr.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/*
 * A real-shaped public address, both ways round.
 *
 * CONTROLLER is what a BLE controller reports for the device whose label reads
 * WIRE: the same six bytes, least significant first. This is the pair the whole
 * module is anchored to.
 */
static const uint8_t WIRE[6] = {0xc4u, 0x99u, 0x4cu, 0x1au, 0x2bu, 0x3du};
static const uint8_t CONTROLLER[6] = {0x3du, 0x2bu, 0x1au, 0x4cu, 0x99u, 0xc4u};
/* The same shape as a random static address: still not a palindrome. */
static const uint8_t WIRE_RANDOM[6] = {0x7au, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
static const uint8_t CONTROLLER_RANDOM[6] = {0x55u, 0x44u, 0x33u, 0x22u, 0x11u, 0x7au};

static void test_both_directions_are_pinned(void)
{
    uint8_t out[6];

    memset(out, 0, sizeof(out));
    CHECK(app_ble_addr_to_wire(CONTROLLER, out) == out, "to_wire returns its output");
    CHECK(memcmp(out, WIRE, 6) == 0, "controller -> wire produced the wrong bytes");

    memset(out, 0, sizeof(out));
    CHECK(app_ble_addr_to_controller(WIRE, out) == out, "to_controller returns its output");
    CHECK(memcmp(out, CONTROLLER, 6) == 0, "wire -> controller produced the wrong bytes");

    /* A second, differently shaped address: one vector can be satisfied by a
     * implementation that happens to work for those bytes. */
    CHECK(memcmp(app_ble_addr_to_wire(CONTROLLER_RANDOM, out), WIRE_RANDOM, 6) == 0,
          "the random-address vector did not convert");
    CHECK(memcmp(app_ble_addr_to_controller(WIRE_RANDOM, out), CONTROLLER_RANDOM, 6) == 0,
          "and did not convert back");
}

/*
 * The round trip, in both orders.
 *
 * This is the property a caller relies on: an address can go out to the radio and
 * come back unchanged. It is also the property that fails loudly if one direction
 * is ever "fixed" independently of the other.
 */
static void test_the_round_trip(void)
{
    uint8_t controller[6];
    uint8_t back[6];

    app_ble_addr_to_controller(WIRE, controller);
    app_ble_addr_to_wire(controller, back);
    CHECK(memcmp(back, WIRE, 6) == 0, "wire -> controller -> wire changed the address");

    app_ble_addr_to_wire(CONTROLLER, back);
    app_ble_addr_to_controller(back, controller);
    CHECK(memcmp(controller, CONTROLLER, 6) == 0,
          "controller -> wire -> controller changed the address");

    /* Twice in the same direction returns the original, which is the fact that
     * makes "converted twice" invisible: it is why the handoff test exists. */
    app_ble_addr_to_wire(WIRE, back);
    app_ble_addr_to_wire(back, controller);
    CHECK(memcmp(controller, WIRE, 6) == 0,
          "reversing twice must be the identity - that is what makes a double "
          "conversion indistinguishable from none");
}

/* In-place conversion is what the call site uses, so it is what is tested. A swap
 * loop written directly into `out` would corrupt this case while still passing a
 * palindrome, which is why the vectors are non-palindromic. */
static void test_conversion_may_be_in_place(void)
{
    uint8_t buffer[6];
    uint8_t separate[6];

    memcpy(buffer, CONTROLLER, 6);
    CHECK(app_ble_addr_to_wire(buffer, buffer) == buffer, "in-place to_wire did not return out");
    CHECK(memcmp(buffer, WIRE, 6) == 0, "in-place conversion produced the wrong bytes");

    app_ble_addr_to_wire(CONTROLLER, separate);
    CHECK(memcmp(buffer, separate, 6) == 0, "in-place and out-of-place disagree");

    memcpy(buffer, WIRE, 6);
    app_ble_addr_to_controller(buffer, buffer);
    CHECK(memcmp(buffer, CONTROLLER, 6) == 0, "in-place reverse conversion is wrong");

    /* Every rotation of the address must be distinguishable from its reverse, or a
     * buggy in-place loop could pass by accident. */
    {
        uint8_t rotated[6];

        memcpy(rotated, WIRE, 6);
        for (size_t shift = 1u; shift < 6u; ++shift) {
            for (size_t i = 0u; i < 6u; ++i) {
                rotated[i] = WIRE[(i + shift) % 6u];
            }
            CHECK(memcmp(rotated, WIRE, 6) != 0,
                  "rotation by %u is identical to the address, so this test cannot detect a "
                  "rotation bug", (unsigned)shift);
        }
    }
}

/* A NULL argument must not be dereferenced; the console calls these on evidence
 * that may be absent. */
static void test_null_arguments(void)
{
    uint8_t out[6];

    CHECK(app_ble_addr_to_wire(NULL, out) == NULL, "to_wire(NULL) did not return NULL");
    CHECK(app_ble_addr_to_controller(NULL, out) == NULL, "to_controller(NULL) did not return NULL");
    CHECK(!app_ble_addr_equal(NULL, WIRE), "NULL equals an address");
    CHECK(!app_ble_addr_equal(WIRE, NULL), "an address equals NULL");
    CHECK(!app_ble_addr_equal(NULL, NULL), "NULL equals NULL");
    CHECK(!app_ble_addr_is_usable(NULL), "NULL is usable");
    CHECK(app_ble_addr_format(NULL, (char *)out, sizeof(out)) == 0u, "format(NULL) wrote");
}

/*
 * Comparison accepts either order.
 *
 * The point is not convenience: an address that was converted zero times or twice
 * must still be recognised as the same device, so a mistake produces one device
 * record rather than two for one thing. Two different addresses must still differ.
 */
static void test_comparison_accepts_either_order(void)
{
    CHECK(app_ble_addr_equal(WIRE, WIRE), "an address does not equal itself");
    CHECK(app_ble_addr_equal(WIRE, CONTROLLER), "the two orders of one address differ");
    CHECK(app_ble_addr_equal(CONTROLLER, WIRE), "and the comparison is not one-way");
    CHECK(!app_ble_addr_equal(WIRE, WIRE_RANDOM), "two different addresses are equal");
    CHECK(!app_ble_addr_equal(CONTROLLER, CONTROLLER_RANDOM), "two different orders are equal");

    /* Near misses: one byte apart, including in the middle, so a comparison that
     * only checked the ends would be caught. */
    {
        uint8_t near[6];

        memcpy(near, WIRE, 6);
        near[2] ^= 0x01u;
        CHECK(!app_ble_addr_equal(WIRE, near), "a one-byte difference was accepted");
        memcpy(near, WIRE, 6);
        near[5] ^= 0x01u;
        CHECK(!app_ble_addr_equal(WIRE, near), "a difference in the last byte was accepted");
    }
}

static void test_usable_identity(void)
{
    static const uint8_t zero[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    static const uint8_t one_byte[6] = {0u, 0u, 0u, 0u, 0u, 0x01u};

    CHECK(!app_ble_addr_is_usable(zero), "the all-zero address is usable as an identity");
    CHECK(app_ble_addr_is_usable(one_byte), "a single non-zero byte is not usable");
    CHECK(app_ble_addr_is_usable(WIRE), "a real address is not usable");
}

/*
 * Formatting, which is what an operator compares against nRF Connect.
 *
 * The output must be exactly `aa:bb:cc:dd:ee:ff` lowercase: 17 bytes and no
 * truncation. A short buffer is refused rather than filled with half an address,
 * because half an address that looks complete is worse than none.
 */
static void test_formatting(void)
{
    char out[32];

    CHECK(app_ble_addr_format(WIRE, out, sizeof(out)) == 17u, "wrong length: %u",
          (unsigned)app_ble_addr_format(WIRE, out, sizeof(out)));
    CHECK(strcmp(out, "c4:99:4c:1a:2b:3d") == 0, "formatted as '%s'", out);

    CHECK(app_ble_addr_format(CONTROLLER, out, sizeof(out)) == 17u, "wrong length");
    CHECK(strcmp(out, "3d:2b:1a:4c:99:c4") == 0,
          "the controller order must print reversed, got '%s'", out);

    /* An address with a leading zero byte keeps both digits: it is a common
     * mistake to drop it, and it changes the address. */
    {
        static const uint8_t leading_zero[6] = {0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u};

        CHECK(app_ble_addr_format(leading_zero, out, sizeof(out)) == 17u, "wrong length");
        CHECK(strcmp(out, "00:11:22:33:44:55") == 0, "a leading zero byte was dropped: '%s'",
              out);
    }

    /* A buffer that cannot hold the result reports 0. */
    {
        char tiny[17];

        memset(tiny, 0x5A, sizeof(tiny));
        CHECK(app_ble_addr_format(WIRE, tiny, sizeof(tiny)) == 0u,
              "a 17-byte buffer was accepted for a 17-character address plus terminator");
        CHECK(tiny[0] == '\0', "a refused format left text behind");
    }
    {
        char exact[18];

        CHECK(app_ble_addr_format(WIRE, exact, sizeof(exact)) == 17u,
              "an exactly sized buffer was refused");
        CHECK(strcmp(exact, "c4:99:4c:1a:2b:3d") == 0, "the exact buffer holds '%s'", exact);
    }
}

int main(void)
{
    printf("ble address order tests\n");

    test_both_directions_are_pinned();
    test_the_round_trip();
    test_conversion_may_be_in_place();
    test_null_arguments();
    test_comparison_accepts_either_order();
    test_usable_identity();
    test_formatting();

    printf("app_ble_addr: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
