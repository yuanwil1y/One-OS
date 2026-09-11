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

/*
 * Regression: an interview whose backend never calls back must not stay active
 * forever. Previously zigpy_poll only handled commissioning and transaction
 * deadlines, so a silent backend left the interview pending indefinitely and
 * every later interview failed with ZIGPY_STATUS_BUSY.
 */
static void test_interview_phase_timeout_without_callback(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();
    zigpy_device_ref_t dev = {.ieee = 1, .nwk = 0x1234};
    zigpy_interview_config_t cfg = {.phase_timeout_ms = 1000,
                                    .overall_timeout_ms = 10000};
    uint32_t id = 0;
    uint32_t second_id = 0;
    zigpy_interview_status_t st;
    zigpy_device_snapshot_t snap;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_begin_ex(&ctx, &dev, &cfg, 0u, &id) == ZIGPY_STATUS_OK);
    assert(b.node_req == 1);

    /* Just before the deadline the interview is still legitimately waiting. */
    zigpy_poll(&ctx, 999);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_NODE_DESC);

    /* A second interview cannot start while one is genuinely in flight. */
    assert(zigpy_interview_begin_ex(&ctx, &dev, &cfg, 999u, &second_id) ==
           ZIGPY_STATUS_BUSY);

    /* At the deadline the component must close the interview out. */
    zigpy_poll(&ctx, 1000);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_FAILED);
    assert(st.status == ZIGPY_STATUS_TIMEOUT);
    /* The silent backend was told to cancel, and no device data was invented. */
    assert(b.cancel_req == 1);
    assert(zigpy_interview_get_snapshot(&ctx, &snap) == ZIGPY_STATUS_TIMEOUT);
    assert(snap.complete_mask == 0u);
    assert(snap.endpoint_count == 0u);

    /* Deadlines are cleared once the interview is terminal. */
    assert(st.phase_deadline_ms == 0u && st.overall_deadline_ms == 0u);

    /* Crucially, the next interview is no longer blocked. */
    assert(zigpy_interview_begin_ex(&ctx, &dev, &cfg, 2000u, &second_id) ==
           ZIGPY_STATUS_OK);
    assert(second_id != id);
    assert(b.node_req == 2);
    assert(zigpy_interview_cancel(&ctx, second_id) == ZIGPY_STATUS_OK);
}

/*
 * Regression: a backend that answers the first step and then goes silent must
 * end as PARTIAL (usable evidence retained) rather than hanging, and the
 * evidence collected before the timeout must survive.
 */
static void test_interview_partial_timeout_keeps_evidence(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();
    zigpy_device_ref_t dev = {.ieee = 0xABCD, .nwk = 0x1234};
    zigpy_node_descriptor_t node = {.logical_type = 2, .manufacturer_code = 0x117C};
    zigpy_interview_config_t cfg = {.phase_timeout_ms = 500,
                                    .overall_timeout_ms = 4000};
    uint32_t id = 0;
    zigpy_interview_status_t st;
    zigpy_device_snapshot_t snap;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_begin_ex(&ctx, &dev, &cfg, 0u, &id) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_node_desc_complete(&ctx, id, ZIGPY_STATUS_OK, &node) ==
           ZIGPY_STATUS_OK);

    /* Phase advanced to ACTIVE_EP; the deadline refreshes for the new phase. */
    zigpy_poll(&ctx, 1000);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_ACTIVE_EP);

    zigpy_poll(&ctx, 1500);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_DONE);
    assert(st.status == ZIGPY_STATUS_PARTIAL);
    assert((st.complete_mask & ZIGPY_INTERVIEW_COMPLETE_NODE_DESC) != 0u);

    /* The node descriptor gathered before the timeout is still readable. */
    assert(zigpy_interview_get_snapshot(&ctx, &snap) == ZIGPY_STATUS_PARTIAL);
    assert((snap.complete_mask & ZIGPY_INTERVIEW_COMPLETE_NODE_DESC) != 0u);
    assert(snap.node_descriptor.manufacturer_code == 0x117C);
}

/*
 * Regression: the overall deadline must bound a slow interview even when each
 * phase keeps arriving just in time.
 */
static void test_interview_overall_deadline(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();
    zigpy_device_ref_t dev = {.ieee = 1, .nwk = 0x1234};
    zigpy_node_descriptor_t node = {0};
    zigpy_interview_config_t cfg = {.phase_timeout_ms = 1000,
                                    .overall_timeout_ms = 2500};
    uint32_t id = 0;
    zigpy_interview_status_t st;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_begin_ex(&ctx, &dev, &cfg, 0u, &id) == ZIGPY_STATUS_OK);
    /* Answer at t=900: inside the phase deadline, and refresh the phase. */
    assert(zigpy_interview_node_desc_complete(&ctx, id, ZIGPY_STATUS_OK, &node) ==
           ZIGPY_STATUS_OK);
    zigpy_poll(&ctx, 900);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_ACTIVE_EP);

    /* A per-phase answer at t=1800 is still "in time" for that phase, but the
     * overall interview budget is nearly gone. */
    zigpy_poll(&ctx, 1800);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_ACTIVE_EP);

    /* Past the overall deadline the interview must terminate regardless. */
    zigpy_poll(&ctx, 2500);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_DONE);
    assert(st.status == ZIGPY_STATUS_PARTIAL);
}

/*
 * Regression: retries == 255 used to make the uint8_t attempt counter wrap, so
 * the retry condition stayed true and the transaction retried forever.
 */
static void test_retry_limit_does_not_wrap(void)
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
    zigpy_transaction_result_t result;
    uint32_t id;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);

    /* Ask for the maximum representable retry count. */
    assert(zigpy_attr_read_async(&ctx, &attr, 100, 255, 0, &id) == ZIGPY_STATUS_OK);
    assert(b.read_req == 1);

    /* The stored limit must be clamped into the reachable range. */
    assert(zigpy_transaction_get_result(&ctx, id, &result) == ZIGPY_STATUS_OK);
    assert(result.attempts == 1u);
    {
        /*
         * The initial attempt was already sent. Each further poll that finds
         * the deadline passed consumes one retry, so the transaction must
         * finish after exactly ZIGPY_MAX_RETRIES more timeouts. The bound is
         * what matters: with the wrap-around bug this loop never terminated
         * because the condition `attempts <= max_retries` stayed true forever.
         */
        uint32_t now = 0u;
        unsigned retry_polls = 0u;
        unsigned guard = 0u;
        for (;;) {
            assert(guard++ < 1000u); /* fails loudly instead of hanging */
            now += 200u;             /* always past the 100 ms deadline */
            zigpy_poll(&ctx, now);
            assert(zigpy_transaction_get_result(&ctx, id, &result) ==
                   ZIGPY_STATUS_OK);
            if (result.state != ZIGPY_TX_PENDING) {
                break;
            }
            ++retry_polls;
        }
        assert(result.state == ZIGPY_TX_COMPLETE);
        assert(result.status == ZIGPY_STATUS_TIMEOUT);
        /* Exactly the clamped retry budget was consumed, and no more. */
        assert(retry_polls == ZIGPY_MAX_RETRIES);
        assert(result.attempts == (uint8_t)(ZIGPY_MAX_RETRIES + 1u));
        assert(b.read_req == (unsigned)ZIGPY_MAX_RETRIES + 1u);
    }

    assert(zigpy_transaction_release(&ctx, id) == ZIGPY_STATUS_OK);
}

/* A bounded retry count must still work exactly as before. */
static void test_retry_limit_bounded_case_unchanged(void)
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
    zigpy_transaction_result_t result;
    uint32_t id;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);
    assert(zigpy_attr_read_async(&ctx, &attr, 100, 2, 0, &id) == ZIGPY_STATUS_OK);
    assert(b.read_req == 1);

    zigpy_poll(&ctx, 100);
    assert(b.read_req == 2);
    zigpy_poll(&ctx, 200);
    assert(b.read_req == 3);
    /* Attempts exhausted: the fourth poll must report a timeout, not retry. */
    zigpy_poll(&ctx, 300);
    assert(b.read_req == 3);
    assert(zigpy_transaction_get_result(&ctx, id, &result) == ZIGPY_STATUS_OK);
    assert(result.state == ZIGPY_TX_COMPLETE);
    assert(result.status == ZIGPY_STATUS_TIMEOUT);
    assert(result.attempts == 3u);
}

/*
 * Regression: starting a re-interview used to wipe the snapshot immediately, so
 * a failed re-interview destroyed already-known device information.
 */
static void test_last_good_snapshot_survives_failed_reinterview(void)
{
    zigpy_ctx_t ctx;
    fake_backend_t b = {0};
    zigpy_backend_ops_t ops = fake_ops();
    zigpy_device_ref_t dev = {.ieee = 0x00124B0001ABCDEFULL, .nwk = 0x1234};
    zigpy_node_descriptor_t node = {.logical_type = 2, .manufacturer_code = 0x117C};
    uint8_t eps[] = {1};
    uint16_t ep_in[] = {0x0000, 0x0006};
    zigpy_interview_config_t cfg = {.phase_timeout_ms = 500,
                                    .overall_timeout_ms = 2000};
    uint32_t id = 0;
    uint32_t second = 0;
    zigpy_interview_status_t st;
    zigpy_device_snapshot_t good;

    assert(zigpy_init(&ctx, &ops, &b) == ZIGPY_STATUS_OK);

    /* Before any successful interview there is no known-good data. */
    assert(zigpy_interview_get_last_good_snapshot(&ctx, &good) ==
           ZIGPY_STATUS_NOT_FOUND);

    /* Complete one full interview. */
    assert(zigpy_interview_begin_ex(&ctx, &dev, &cfg, 0u, &id) == ZIGPY_STATUS_OK);
    assert(zigpy_interview_node_desc_complete(&ctx, id, ZIGPY_STATUS_OK, &node) ==
           ZIGPY_STATUS_OK);
    assert(zigpy_interview_active_ep_complete(&ctx, id, ZIGPY_STATUS_OK, eps, 1) ==
           ZIGPY_STATUS_OK);
    assert(zigpy_interview_simple_desc_complete(&ctx, id, 1, ZIGPY_STATUS_OK,
                                                0x0104, 0x0402, ep_in, 2, NULL, 0) ==
           ZIGPY_STATUS_OK);
    assert(zigpy_interview_basic_identity_complete(&ctx, id, ZIGPY_STATUS_OK,
                                                   "IKEA of Sweden",
                                                   "VALLHORN") == ZIGPY_STATUS_OK);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_DONE && st.status == ZIGPY_STATUS_OK);

    assert(zigpy_interview_get_last_good_snapshot(&ctx, &good) == ZIGPY_STATUS_OK);
    assert(good.endpoint_count == 1u);
    assert(strcmp(good.manufacturer, "IKEA of Sweden") == 0);
    assert(strcmp(good.model, "VALLHORN") == 0);

    /* Now start a re-interview that times out with no callbacks at all. */
    assert(zigpy_interview_begin_ex(&ctx, &dev, &cfg, 10000u, &second) ==
           ZIGPY_STATUS_OK);
    zigpy_poll(&ctx, 50000u);
    assert(zigpy_interview_get_status(&ctx, &st) == ZIGPY_STATUS_OK);
    assert(st.phase == ZIGPY_INTERVIEW_FAILED);

    /* The current snapshot is empty, but the known-good one is recoverable. */
    assert(zigpy_interview_get_last_good_snapshot(&ctx, &good) == ZIGPY_STATUS_OK);
    assert(good.endpoint_count == 1u);
    assert(strcmp(good.manufacturer, "IKEA of Sweden") == 0);
    assert(strcmp(good.model, "VALLHORN") == 0);
    assert((good.complete_mask & ZIGPY_INTERVIEW_COMPLETE_IDENTITY) != 0u);
}

int main(void)
{
    test_commissioning();
    test_interview();
    test_identity_fallback_and_partial();
    test_transactions();
    test_interview_cancel();
    test_interview_phase_timeout_without_callback();
    test_interview_partial_timeout_keeps_evidence();
    test_interview_overall_deadline();
    test_retry_limit_does_not_wrap();
    test_retry_limit_bounded_case_unchanged();
    test_last_good_snapshot_survives_failed_reinterview();
    puts("zigpy_l2 host tests passed");
    return 0;
}
