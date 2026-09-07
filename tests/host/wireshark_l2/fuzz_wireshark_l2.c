#include "wireshark_l2.h"

#include <stddef.h>
#include <stdint.h>

/*
 * libFuzzer/AFL-compatible pure-parser entry point. The parser APIs perform no
 * I/O, radio access, heap allocation, or cross-family calls.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    wireshark_ble_adv_t ble;
    wireshark_wifi_ie_summary_t ies;
    wireshark_wifi_mgmt_t mgmt;

    (void)wireshark_ble_adv_parse(data, size, &ble);
    (void)wireshark_wifi_ie_parse(data, size, &ies);
    (void)wireshark_wifi_mgmt_parse(data, size, &mgmt);
    return 0;
}
