#include "zha_l2.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static zha_device_view_t vallhorn_view(void)
{
    zha_device_view_t view = {0};
    view.endpoint_count = 1;
    view.endpoints[0].endpoint_id = 1;
    view.endpoints[0].profile_id = 0x0104;
    view.endpoints[0].device_id = 0x0402;
    view.endpoints[0].server_cluster_count = 2;
    view.endpoints[0].server_clusters[0] = 0x0000;
    view.endpoints[0].server_clusters[1] = 0xFC81;
    return view;
}

static void test_provenance_and_apply(void)
{
    zha_quirk_info_t info;
    zha_device_view_t view = vallhorn_view();
    zha_quirked_device_t applied;

    assert(zha_quirk_get_info(ZHA_QUIRK_ID_IKEA_VALLHORN, &info) == ZHA_STATUS_OK);
    assert(strcmp(info.provenance.license, "Apache-2.0") == 0);
    assert(strcmp(info.provenance.revision,
                  "6a3822c1348f9e40ad243eb881084a1c075da277") == 0);
    assert(strstr(info.provenance.path, "vallhorn.py") != NULL);

    assert(zha_quirk_apply_by_id(ZHA_QUIRK_ID_IKEA_VALLHORN, &view, &applied) ==
           ZHA_STATUS_OK);
    assert(applied.quirk_id == ZHA_QUIRK_ID_IKEA_VALLHORN);
    assert(applied.patch_count == 1);
    assert(applied.applied_patches[0].kind == ZHA_PATCH_REPLACE_CLUSTER);
    assert(applied.view.endpoints[0].server_clusters[1] == 0xFC81);

    view.endpoints[0].server_cluster_count = 1;
    assert(zha_quirk_apply_by_id(ZHA_QUIRK_ID_IKEA_VALLHORN, &view, &applied) ==
           ZHA_STATUS_NOT_FOUND);
    assert(zha_quirk_apply_by_id(0xDEADBEEFu, &view, &applied) == ZHA_STATUS_NOT_FOUND);
}

static void test_capabilities(void)
{
    zha_capability_t cap;

    assert(zha_capability_enumerate(ZHA_QUIRK_ID_IKEA_VALLHORN, 0, &cap) ==
           ZHA_STATUS_OK);
    assert(cap.kind == ZHA_CAP_SWITCH && cap.cluster_id == 0xFC81 &&
           cap.attribute_id == 0x0000 && cap.writable);
    assert(strcmp(cap.name, "on_only_when_dark") == 0);

    assert(zha_capability_enumerate(ZHA_QUIRK_ID_IKEA_VALLHORN, 1, &cap) ==
           ZHA_STATUS_OK);
    assert(cap.kind == ZHA_CAP_NUMBER && cap.min_value == 10 &&
           cap.max_value == 65534 && strcmp(cap.unit, "s") == 0);

    assert(zha_capability_enumerate(ZHA_QUIRK_ID_IKEA_VALLHORN, 2, &cap) ==
           ZHA_STATUS_NOT_FOUND);
    assert(zha_capability_enumerate(ZHA_QUIRK_ID_SONOFF_BUTTON, 0, &cap) ==
           ZHA_STATUS_OK);
    assert(cap.kind == ZHA_CAP_ACTION);
}

static void test_transforms(void)
{
    zha_transform_t t = {
        .kind = ZHA_TRANSFORM_SCALE_OFFSET,
        .multiplier = 2,
        .divisor = 1,
        .offset = 5,
    };
    int64_t value;

    assert(zha_transform_decode(&t, 10, &value) == ZHA_STATUS_OK && value == 25);
    assert(zha_transform_encode(&t, 25, &value) == ZHA_STATUS_OK && value == 10);

    t.kind = ZHA_TRANSFORM_BITMASK_BOOL;
    t.mask = 0x04;
    assert(zha_transform_decode(&t, 0x05, &value) == ZHA_STATUS_OK && value == 1);
    assert(zha_transform_encode(&t, 1, &value) == ZHA_STATUS_OK && value == 0x04);

    t.kind = ZHA_TRANSFORM_CLAMP;
    t.min_value = 10;
    t.max_value = 20;
    assert(zha_transform_decode(&t, 100, &value) == ZHA_STATUS_OK && value == 20);
    assert(zha_transform_encode(&t, 9, &value) == ZHA_STATUS_RANGE);
}

static void test_actions(void)
{
    zha_action_event_t event = {.endpoint_id = 1, .cluster_id = 0x0006};
    zha_action_t action;

    event.command_id = 0x02;
    assert(zha_action_decode(ZHA_QUIRK_ID_SONOFF_BUTTON, &event, &action) ==
           ZHA_STATUS_OK);
    assert(action.kind == ZHA_ACTION_SHORT_PRESS && action.button == 1);

    event.command_id = 0x01;
    assert(zha_action_decode(ZHA_QUIRK_ID_SONOFF_BUTTON, &event, &action) ==
           ZHA_STATUS_OK);
    assert(action.kind == ZHA_ACTION_DOUBLE_PRESS);

    event.command_id = 0x00;
    assert(zha_action_decode(ZHA_QUIRK_ID_SONOFF_BUTTON, &event, &action) ==
           ZHA_STATUS_OK);
    assert(action.kind == ZHA_ACTION_LONG_PRESS);

    event.command_id = 0x99;
    assert(zha_action_decode(ZHA_QUIRK_ID_SONOFF_BUTTON, &event, &action) ==
           ZHA_STATUS_NOT_FOUND);
    assert(zha_action_decode(ZHA_QUIRK_ID_IKEA_VALLHORN, &event, &action) ==
           ZHA_STATUS_NOT_FOUND);
}

int main(void)
{
    test_provenance_and_apply();
    test_capabilities();
    test_transforms();
    test_actions();
    puts("zha_l2 host tests passed");
    return 0;
}
