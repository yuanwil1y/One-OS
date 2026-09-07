#include "zigpy_l2.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned commissioning_on;
    unsigned commissioning_off;
    unsigned node_req;
    unsigned active_req;
    unsigned simple_req;
    unsigned identity_req;
    unsigned read_req;
    unsigned write_req;
    unsigned command_req;
    unsigned reporting_req;
    unsigned cancel_req;
    uint8_t last_endpoint;
    bool last_identity_manufacturer;
    bool last_identity_model;
} fake_backend_t;

static zigpy_status_t fake_commissioning(void *opaque, bool enabled, uint8_t duration)
{
    fake_backend_t *b = opaque;
    if (enabled) {
        assert(duration > 0 && duration != 0xFF);
        b->commissioning_on++;
    } else {
        assert(duration == 0);
        b->commissioning_off++;
    }
    return ZIGPY_STATUS_OK;
}

static zigpy_status_t fake_node(void *opaque, uint32_t id,
                                const zigpy_device_ref_t *device)
{
    fake_backend_t *b = opaque;
    assert(id != 0);
    assert(device->ieee != 0);
    b->node_req++;
    return ZIGPY_STATUS_OK;
}

static zigpy_status_t fake_active(void *opaque, uint32_t id,
                                  const zigpy_device_ref_t *device)
{
    fake_backend_t *b = opaque;
    assert(id != 0);
    assert(device->nwk == 0x1234);
    b->active_req++;
    return ZIGPY_STATUS_OK;
}

static zigpy_status_t fake_simple(void *opaque, uint32_t id,
                                  const zigpy_device_ref_t *device,
                                  uint8_t endpoint)
{
    fake_backend_t *b = opaque;
    (void)device;
    assert(id != 0);
    b->simple_req++;
    b->last_endpoint = endpoint;
    return ZIGPY_STATUS_OK;
}

static zigpy_status_t fake_identity(void *opaque, uint32_t id,
                                    const zigpy_device_ref_t *device,
                                    uint8_t endpoint, bool manufacturer, bool model)
{
    fake_backend_t *b = opaque;
    (void)device;
    assert(id != 0);
    assert(endpoint == 1);
    b->identity_req++;
    b->last_identity_manufacturer = manufacturer;
    b->last_identity_model = model;
    return ZIGPY_STATUS_OK;
}

static zigpy_status_t fake_read(void *opaque, uint32_t id,
                                const zigpy_attr_path_t *path)
{
    fake_backend_t *b = opaque;
    assert(id != 0 && path->attribute_id == 0x0000);
    b->read_req++;
    return ZIGPY_STATUS_OK;
}

static zigpy_status_t fake_write(void *opaque, uint32_t id,
                                 const zigpy_attr_path_t *path,
                                 const uint8_t *value, size_t len)
{
    fake_backend_t *b = opaque;
    assert(id != 0 && path->cluster_id == 0x0006);
    assert(len == 1 && value[0] == 1);
    b->write_req++;
    return ZIGPY_STATUS_OK;
}

static zigpy_status_t fake_command(void *opaque, uint32_t id,
                                   const zigpy_command_path_t *path,
                                   const uint8_t *payload, size_t len)
{
    fake_backend_t *b = opaque;
    assert(id != 0 && path->command_id == 0x02);
    assert(len == 0 && payload != NULL);
    b->command_req++;
    return ZIGPY_STATUS_OK;
}

static zigpy_status_t fake_reporting(void *opaque, uint32_t id,
                                     const zigpy_reporting_request_t *req)
{
    fake_backend_t *b = opaque;
    assert(id != 0 && req->max_interval_s == 300);
    b->reporting_req++;
    return ZIGPY_STATUS_OK;
}

static void fake_cancel(void *opaque, uint32_t token, bool interview)
{
    fake_backend_t *b = opaque;
    assert(token != 0);
    (void)interview;
    b->cancel_req++;
}

static zigpy_backend_ops_t fake_ops(void)
{
    zigpy_backend_ops_t ops = {
        .commissioning_set = fake_commissioning,
        .node_desc_request = fake_node,
        .active_ep_request = fake_active,
        .simple_desc_request = fake_simple,
        .basic_identity_request = fake_identity,
        .attr_read_request = fake_read,
        .attr_write_request = fake_write,
        .command_request = fake_command,
        .reporting_request = fake_reporting,
        .cancel_request = fake_cancel,
    };
    return ops;
}

static void test_commissioning(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);
    assert(zigpy_commissioning_start(&ctx, 10, 1000) == ZIGPY_STATUS_OK);
    assert(zigpy_commissioning_is_active(&ctx));
    assert(zigpy_commissioning_start(&ctx, 10, 1000) == ZIGPY_STATUS_BUSY);
    zigpy_poll(&ctx, 10999);
    assert(zigpy_commissioning_is_active(&ctx));
    zigpy_poll(&ctx, 11000);
    assert(!zigpy_commissioning_is_active(&ctx));
    assert(b.commissioning_on == 1 && b.commissioning_off == 1);
    assert(zigpy_commissioning_start(&ctx, 0xFF, 0) == ZIGPY_STATUS_INVALID_ARG);
}

static void test_interview(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();
    zigpy_device_ref_t dev = {.ieee = 0x00124B0001ABCDEFULL, .nwk = 0x1234};
    zigpy_node_descriptor_t node = {.logical_type = 2, .mac_capability_flags = 0x80,
                                    .manufacturer_code = 0x117C};
    uint8_t eps[] = {1, 2};
    uint16_t ep1_in[] = {0x0000, 0x0006, 0xFC81};
    uint16_t ep1_out[] = {0x0019};
    uint16_t ep2_in[] = {0x0006};
    uint32_t id;
    zigpy_interview_status_t st;
    zigpy_device_snapshot_t snap;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_begin(&ctx, &dev, &id) == ZIGPY_STATUS_OK);
    assert(b.node_req == 1);
    assert(zigpy_interview_node_desc_complete(&ctx, id, ZIGPY_STATUS_OK, &node) ==
           ZIGPY_STATUS_OK);
    assert(b.active_req == 1);
    assert(zigpy_interview_active_ep_complete(&ctx, id, ZIGPY_STATUS_OK, eps, 2) ==
           ZIGPY_STATUS_OK);
    assert(b.simple_req == 1 && b.last_endpoint == 1);
    assert(zigpy_interview_simple_desc_complete(&ctx, id, 1, ZIGPY_STATUS_OK,
                                                0x0104, 0x0402, ep1_in, 3,
                                                ep1_out, 1) == ZIGPY_STATUS_OK);
    assert(b.simple_req == 2 && b.last_endpoint == 2);
    assert(zigpy_interview_simple_desc_complete(&ctx, id, 2, ZIGPY_STATUS_OK,
                                                0x0104, 0x0100, ep2_in, 1,
                                                NULL, 0) == ZIGPY_STATUS_OK);
    assert(b.identity_req == 1);
    assert(b.last_identity_manufacturer && b.last_identity_model);
    assert(zigpy_interview_basic_identity_complete(&ctx, id, ZIGPY_STATUS_OK,
                                                   "IKEA of Sweden",
                                                   "VALLHORN Wireless Motion Sensor") ==
           ZIGPY_STATUS_OK);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_DONE && st.status == ZIGPY_STATUS_OK);
    assert(st.complete_mask == (ZIGPY_INTERVIEW_COMPLETE_NODE_DESC |
                                ZIGPY_INTERVIEW_COMPLETE_ACTIVE_EP |
                                ZIGPY_INTERVIEW_COMPLETE_SIMPLE_DESC |
                                ZIGPY_INTERVIEW_COMPLETE_IDENTITY));
    assert(zigpy_interview_get_snapshot(&ctx, &snap) == ZIGPY_STATUS_OK);
    assert(snap.endpoint_count == 2);
    assert(strcmp(snap.manufacturer, "IKEA of Sweden") == 0);
    assert(strcmp(snap.model, "VALLHORN Wireless Motion Sensor") == 0);

    assert(zigpy_interview_node_desc_complete(&ctx, id, ZIGPY_STATUS_OK, &node) ==
           ZIGPY_STATUS_STALE);
}

static void test_identity_fallback_and_partial(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();
    zigpy_device_ref_t dev = {.ieee = 1, .nwk = 0x1234};
    zigpy_node_descriptor_t node = {0};
    uint8_t ep = 1;
    uint16_t in[] = {0x0000};
    uint32_t id;
    zigpy_interview_status_t st;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_begin(&ctx, &dev, &id) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_node_desc_complete(&ctx, id, ZIGPY_STATUS_OK, &node) ==
           ZIGPY_STATUS_OK);
    assert(zigpy_interview_active_ep_complete(&ctx, id, ZIGPY_STATUS_OK, &ep, 1) ==
           ZIGPY_STATUS_OK);
    assert(zigpy_interview_simple_desc_complete(&ctx, id, 1, ZIGPY_STATUS_OK,
                                                0x0104, 0, in, 1, NULL, 0) ==
           ZIGPY_STATUS_OK);
    assert(b.identity_req == 1);
    assert(zigpy_interview_basic_identity_complete(&ctx, id, ZIGPY_STATUS_TIMEOUT,
                                                   NULL, NULL) == ZIGPY_STATUS_OK);
    assert(b.identity_req == 2 && b.last_identity_manufacturer &&
           !b.last_identity_model);
    assert(zigpy_interview_basic_identity_complete(&ctx, id, ZIGPY_STATUS_OK,
                                                   "Vendor", NULL) ==
           ZIGPY_STATUS_OK);
    assert(b.identity_req == 3 && !b.last_identity_manufacturer &&
           b.last_identity_model);
    assert(zigpy_interview_basic_identity_complete(&ctx, id, ZIGPY_STATUS_OK,
                                                   NULL, "Model") ==
           ZIGPY_STATUS_OK);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_DONE && st.status == ZIGPY_STATUS_PARTIAL);
}

static void test_transactions(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();
    zigpy_attr_path_t attr = {
        .device = {.ieee = 1, .nwk = 0x1234},
        .endpoint_id = 1,
        .cluster_id = 0x0006,
        .attribute_id = 0x0000,
    };
    zigpy_command_path_t command = {
        .device = {.ieee = 1, .nwk = 0x1234},
        .endpoint_id = 1,
        .cluster_id = 0x0006,
        .command_id = 0x02,
    };
    zigpy_reporting_request_t reporting = {
        .path = attr,
        .min_interval_s = 1,
        .max_interval_s = 300,
        .reportable_change = 1,
    };
    zigpy_transaction_result_t result;
    uint8_t one = 1;
    uint8_t read_value[] = {0x42, 0x43};
    uint32_t read_id, write_id, cmd_id, report_id;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);

    attr.attribute_id = 0x0000;
    assert(zigpy_attr_read_async(&ctx, &attr, 100, 2, 1000, &read_id) ==
           ZIGPY_STATUS_OK);
    assert(b.read_req == 1);
    zigpy_poll(&ctx, 1100);
    assert(b.read_req == 2);
    zigpy_poll(&ctx, 1200);
    assert(b.read_req == 3);
    assert(zigpy_transaction_complete(&ctx, read_id, ZIGPY_STATUS_OK,
                                      read_value, sizeof(read_value)) ==
           ZIGPY_STATUS_OK);
    assert(zigpy_transaction_get_result(&ctx, read_id, &result) == ZIGPY_STATUS_OK);
    assert(result.state == ZIGPY_TX_COMPLETE && result.status == ZIGPY_STATUS_OK);
    assert(result.attempts == 3 && result.value_len == 2 && result.value[0] == 0x42);
    assert(zigpy_transaction_release(&ctx, read_id) == ZIGPY_STATUS_OK);

    assert(zigpy_attr_write_async(&ctx, &attr, &one, 1, 100, 0, 0, &write_id) ==
           ZIGPY_STATUS_OK);
    assert(zigpy_transaction_complete(&ctx, write_id, ZIGPY_STATUS_OK, NULL, 0) ==
           ZIGPY_STATUS_OK);
    assert(b.write_req == 1);

    assert(zigpy_command_invoke_async(&ctx, &command, NULL, 0, 100, 0, 0, &cmd_id) ==
           ZIGPY_STATUS_OK);
    assert(b.command_req == 1);
    assert(zigpy_transaction_cancel(&ctx, cmd_id) == ZIGPY_STATUS_OK);
    assert(zigpy_transaction_complete(&ctx, cmd_id, ZIGPY_STATUS_OK, NULL, 0) ==
           ZIGPY_STATUS_STALE);

    assert(zigpy_reporting_configure_async(&ctx, &reporting, 100, 0, 0, &report_id) ==
           ZIGPY_STATUS_OK);
    assert(b.reporting_req == 1);
    zigpy_poll(&ctx, 100);
    assert(zigpy_transaction_get_result(&ctx, report_id, &result) == ZIGPY_STATUS_OK);
    assert(result.status == ZIGPY_STATUS_TIMEOUT);
    assert(b.cancel_req == 1);
}

static void test_interview_cancel(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();
    zigpy_device_ref_t dev = {.ieee = 1, .nwk = 0x1234};
    uint32_t id;
    zigpy_interview_status_t st;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_begin(&ctx, &dev, &id) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_cancel(&ctx, id) == ZIGPY_STATUS_OK);
    assert(b.cancel_req == 1);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_CANCELLED &&
           st.status == ZIGPY_STATUS_CANCELLED);
}

int main(void)
{
    test_commissioning();
    test_interview();
    test_identity_fallback_and_partial();
    test_transactions();
    test_interview_cancel();
    puts("zigpy_l2 host tests passed");
    return 0;
}
