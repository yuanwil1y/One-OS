#include "zigpy_l2.h"

#include <string.h>

#define ZIGPY_BASIC_CLUSTER_ID 0x0000u

static uint32_t next_token(zigpy_ctx_t *ctx)
{
    ctx->next_token++;
    if (ctx->next_token == 0u) {
        ctx->next_token = 1u;
    }
    return ctx->next_token;
}

static bool interview_is_active(const zigpy_ctx_t *ctx)
{
    return ctx->interview.phase >= ZIGPY_INTERVIEW_NODE_DESC &&
           ctx->interview.phase <= ZIGPY_INTERVIEW_BASIC_IDENTITY;
}

static void interview_finish(zigpy_ctx_t *ctx, zigpy_status_t status)
{
    ctx->interview.status = status;
    ctx->interview.complete_mask = ctx->snapshot.complete_mask;
    ctx->interview.truncated_mask = ctx->snapshot.truncated_mask;
    ctx->interview.phase = (status == ZIGPY_STATUS_CANCELLED)
                               ? ZIGPY_INTERVIEW_CANCELLED
                               : ((status == ZIGPY_STATUS_OK ||
                                   status == ZIGPY_STATUS_PARTIAL ||
                                   status == ZIGPY_STATUS_TRUNCATED)
                                      ? ZIGPY_INTERVIEW_DONE
                                      : ZIGPY_INTERVIEW_FAILED);

    /* Deadlines only apply while a phase is outstanding. */
    ctx->interview.phase_deadline_ms = 0u;
    ctx->interview.overall_deadline_ms = 0u;

    /* Preserve the last interview that produced usable device information, so a
     * failed or timed-out re-interview cannot destroy existing knowledge. */
    if (ctx->interview.phase == ZIGPY_INTERVIEW_DONE) {
        ctx->last_good_snapshot = ctx->snapshot;
        ctx->has_last_good_snapshot = true;
    }
}

static void copy_identity(char *dst, uint32_t *truncated_mask, const char *src)
{
    size_t len;

    if (src == NULL) {
        return;
    }
    len = strlen(src);
    if (len > ZIGPY_MAX_IDENTITY_LEN) {
        len = ZIGPY_MAX_IDENTITY_LEN;
        *truncated_mask |= ZIGPY_INTERVIEW_TRUNC_IDENTITY;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static zigpy_status_t request_next_simple_desc(zigpy_ctx_t *ctx)
{
    zigpy_status_t status;
    uint8_t endpoint_id;

    if (ctx->simple_index >= ctx->snapshot.endpoint_count) {
        size_t i;

        if (ctx->simple_success_count == ctx->snapshot.endpoint_count) {
            ctx->snapshot.complete_mask |= ZIGPY_INTERVIEW_COMPLETE_SIMPLE_DESC;
        }
        ctx->interview.completed_endpoints = ctx->simple_index;
        ctx->interview.complete_mask = ctx->snapshot.complete_mask;
        ctx->interview.truncated_mask = ctx->snapshot.truncated_mask;

        ctx->identity_endpoint = 0u;
        for (i = 0; i < ctx->snapshot.endpoint_count; ++i) {
            const zigpy_endpoint_snapshot_t *ep = &ctx->snapshot.endpoints[i];
            size_t j;
            if (ep->descriptor_status != ZIGPY_STATUS_OK) {
                continue;
            }
            for (j = 0; j < ep->input_cluster_count; ++j) {
                if (ep->input_clusters[j] == ZIGPY_BASIC_CLUSTER_ID) {
                    ctx->identity_endpoint = ep->endpoint_id;
                    break;
                }
            }
            if (ctx->identity_endpoint != 0u) {
                break;
            }
        }

        if (ctx->identity_endpoint == 0u ||
            ctx->backend.basic_identity_request == NULL) {
            ctx->snapshot.complete_mask |= ZIGPY_INTERVIEW_COMPLETE_IDENTITY;
            ctx->interview.complete_mask = ctx->snapshot.complete_mask;
            interview_finish(ctx,
                             ctx->simple_success_count == ctx->snapshot.endpoint_count
                                 ? (ctx->snapshot.truncated_mask != 0u
                                        ? ZIGPY_STATUS_TRUNCATED
                                        : ZIGPY_STATUS_OK)
                                 : ZIGPY_STATUS_PARTIAL);
            return ZIGPY_STATUS_OK;
        }

        ctx->interview.phase = ZIGPY_INTERVIEW_BASIC_IDENTITY;
        ctx->identity_stage = 0u;
        ctx->identity_had_error = false;
        status = ctx->backend.basic_identity_request(
            ctx->backend_ctx, ctx->interview.interview_id, &ctx->snapshot.device,
            ctx->identity_endpoint, true, true);
        if (status != ZIGPY_STATUS_OK) {
            ctx->identity_stage = 1u;
            ctx->identity_had_error = true;
            status = ctx->backend.basic_identity_request(
                ctx->backend_ctx, ctx->interview.interview_id,
                &ctx->snapshot.device, ctx->identity_endpoint, true, false);
            if (status != ZIGPY_STATUS_OK) {
                interview_finish(ctx, ZIGPY_STATUS_PARTIAL);
            }
        }
        return status;
    }

    endpoint_id = ctx->snapshot.endpoints[ctx->simple_index].endpoint_id;
    status = ctx->backend.simple_desc_request(
        ctx->backend_ctx, ctx->interview.interview_id, &ctx->snapshot.device,
        endpoint_id);
    if (status != ZIGPY_STATUS_OK) {
        ctx->snapshot.endpoints[ctx->simple_index].descriptor_status = status;
        ctx->simple_index++;
        ctx->interview.completed_endpoints = ctx->simple_index;
        return request_next_simple_desc(ctx);
    }
    return ZIGPY_STATUS_OK;
}

static zigpy_transaction_slot_t *find_transaction(zigpy_ctx_t *ctx, uint32_t id)
{
    size_t i;
    for (i = 0; i < ZIGPY_MAX_TRANSACTIONS; ++i) {
        if (ctx->transactions[i].result.state != ZIGPY_TX_FREE &&
            ctx->transactions[i].result.transaction_id == id) {
            return &ctx->transactions[i];
        }
    }
    return NULL;
}

static const zigpy_transaction_slot_t *find_transaction_const(const zigpy_ctx_t *ctx,
                                                               uint32_t id)
{
    size_t i;
    for (i = 0; i < ZIGPY_MAX_TRANSACTIONS; ++i) {
        if (ctx->transactions[i].result.state != ZIGPY_TX_FREE &&
            ctx->transactions[i].result.transaction_id == id) {
            return &ctx->transactions[i];
        }
    }
    return NULL;
}

static zigpy_transaction_slot_t *allocate_transaction(zigpy_ctx_t *ctx)
{
    size_t i;
    for (i = 0; i < ZIGPY_MAX_TRANSACTIONS; ++i) {
        if (ctx->transactions[i].result.state == ZIGPY_TX_FREE) {
            memset(&ctx->transactions[i], 0, sizeof(ctx->transactions[i]));
            return &ctx->transactions[i];
        }
    }
    return NULL;
}

static zigpy_status_t send_transaction(zigpy_ctx_t *ctx,
                                       zigpy_transaction_slot_t *slot)
{
    switch (slot->result.kind) {
    case ZIGPY_TX_ATTR_READ:
        if (ctx->backend.attr_read_request == NULL) {
            return ZIGPY_STATUS_BACKEND_ERROR;
        }
        return ctx->backend.attr_read_request(ctx->backend_ctx,
                                              slot->result.transaction_id,
                                              &slot->request.read);
    case ZIGPY_TX_ATTR_WRITE:
        if (ctx->backend.attr_write_request == NULL) {
            return ZIGPY_STATUS_BACKEND_ERROR;
        }
        return ctx->backend.attr_write_request(
            ctx->backend_ctx, slot->result.transaction_id, &slot->request.write.path,
            slot->request.write.value, slot->request.write.value_len);
    case ZIGPY_TX_COMMAND:
        if (ctx->backend.command_request == NULL) {
            return ZIGPY_STATUS_BACKEND_ERROR;
        }
        return ctx->backend.command_request(
            ctx->backend_ctx, slot->result.transaction_id,
            &slot->request.command.path, slot->request.command.payload,
            slot->request.command.payload_len);
    case ZIGPY_TX_REPORTING:
        if (ctx->backend.reporting_request == NULL) {
            return ZIGPY_STATUS_BACKEND_ERROR;
        }
        return ctx->backend.reporting_request(ctx->backend_ctx,
                                              slot->result.transaction_id,
                                              &slot->request.reporting);
    default:
        return ZIGPY_STATUS_INVALID_ARG;
    }
}

static zigpy_status_t start_transaction(zigpy_ctx_t *ctx,
                                        zigpy_transaction_slot_t *slot,
                                        zigpy_transaction_kind_t kind,
                                        uint32_t timeout_ms, uint8_t retries,
                                        uint32_t now_ms, uint32_t *out_id)
{
    zigpy_status_t status;

    if (timeout_ms == 0u || out_id == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    slot->result.transaction_id = next_token(ctx);
    slot->result.kind = kind;
    slot->result.state = ZIGPY_TX_PENDING;
    slot->result.status = ZIGPY_STATUS_BUSY;
    slot->result.attempts = 1u;
    slot->timeout_ms = timeout_ms;
    slot->deadline_ms = now_ms + timeout_ms;
    /* `attempts` is uint8_t and starts at 1, so a retry limit of 255 could never
     * be exceeded by that counter: it would wrap to 0 and poll forever. Clamp
     * the limit into the range the counter can actually reach. */
    slot->max_retries = (retries > ZIGPY_MAX_RETRIES)
                            ? (uint8_t)ZIGPY_MAX_RETRIES
                            : retries;
    *out_id = slot->result.transaction_id;

    status = send_transaction(ctx, slot);
    if (status != ZIGPY_STATUS_OK) {
        slot->result.state = ZIGPY_TX_COMPLETE;
        slot->result.status = status;
        return status;
    }
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_init(zigpy_ctx_t *ctx, const zigpy_backend_ops_t *backend,
                          void *backend_ctx)
{
    if (ctx == NULL || backend == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->backend = *backend;
    ctx->backend_ctx = backend_ctx;
    ctx->next_token = 1u;
    ctx->interview.phase = ZIGPY_INTERVIEW_IDLE;
    ctx->interview.status = ZIGPY_STATUS_NOT_FOUND;
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_commissioning_start(zigpy_ctx_t *ctx, uint8_t duration_s,
                                         uint32_t now_ms)
{
    zigpy_status_t status;
    if (ctx == NULL || duration_s == 0u || duration_s == 0xFFu ||
        ctx->backend.commissioning_set == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    if (ctx->commissioning_active) {
        return ZIGPY_STATUS_BUSY;
    }
    status = ctx->backend.commissioning_set(ctx->backend_ctx, true, duration_s);
    if (status != ZIGPY_STATUS_OK) {
        return status;
    }
    ctx->commissioning_active = true;
    ctx->commissioning_deadline_ms = now_ms + ((uint32_t)duration_s * 1000u);
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_commissioning_stop(zigpy_ctx_t *ctx)
{
    zigpy_status_t status;
    if (ctx == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    if (!ctx->commissioning_active) {
        return ZIGPY_STATUS_OK;
    }
    if (ctx->backend.commissioning_set == NULL) {
        return ZIGPY_STATUS_BACKEND_ERROR;
    }
    status = ctx->backend.commissioning_set(ctx->backend_ctx, false, 0u);
    if (status == ZIGPY_STATUS_OK) {
        ctx->commissioning_active = false;
        ctx->commissioning_deadline_ms = 0u;
    }
    return status;
}

bool zigpy_commissioning_is_active(const zigpy_ctx_t *ctx)
{
    return ctx != NULL && ctx->commissioning_active;
}

zigpy_status_t zigpy_interview_begin(zigpy_ctx_t *ctx,
                                     const zigpy_device_ref_t *device,
                                     uint32_t *out_interview_id)
{
    /* Default deadlines; see zigpy_interview_begin_ex for the bounded form. */
    return zigpy_interview_begin_ex(ctx, device, NULL, 0u, out_interview_id);
}

zigpy_status_t zigpy_interview_begin_ex(zigpy_ctx_t *ctx,
                                        const zigpy_device_ref_t *device,
                                        const zigpy_interview_config_t *config,
                                        uint32_t now_ms,
                                        uint32_t *out_interview_id)
{
    zigpy_status_t status;
    uint32_t id;
    uint32_t phase_timeout;
    uint32_t overall_timeout;

    if (ctx == NULL || device == NULL || out_interview_id == NULL ||
        ctx->backend.node_desc_request == NULL ||
        ctx->backend.active_ep_request == NULL ||
        ctx->backend.simple_desc_request == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    if (interview_is_active(ctx)) {
        return ZIGPY_STATUS_BUSY;
    }

    phase_timeout = (config != NULL && config->phase_timeout_ms != 0u)
                        ? config->phase_timeout_ms
                        : ZIGPY_INTERVIEW_DEFAULT_PHASE_TIMEOUT_MS;
    overall_timeout = (config != NULL && config->overall_timeout_ms != 0u)
                          ? config->overall_timeout_ms
                          : ZIGPY_INTERVIEW_DEFAULT_OVERALL_TIMEOUT_MS;

    memset(&ctx->snapshot, 0, sizeof(ctx->snapshot));
    memset(&ctx->interview, 0, sizeof(ctx->interview));
    ctx->snapshot.device = *device;
    id = next_token(ctx);
    ctx->interview.interview_id = id;
    ctx->interview.phase = ZIGPY_INTERVIEW_NODE_DESC;
    ctx->interview.status = ZIGPY_STATUS_BUSY;
    ctx->interview_phase_timeout_ms = phase_timeout;
    ctx->interview_overall_timeout_ms = overall_timeout;
    ctx->interview_phase_started_ms = now_ms;
    ctx->interview.phase_deadline_ms = now_ms + phase_timeout;
    ctx->interview.overall_deadline_ms = now_ms + overall_timeout;
    ctx->simple_index = 0u;
    ctx->simple_success_count = 0u;
    ctx->identity_endpoint = 0u;
    ctx->identity_stage = 0u;
    ctx->identity_had_error = false;
    *out_interview_id = id;

    status = ctx->backend.node_desc_request(ctx->backend_ctx, id, device);
    if (status != ZIGPY_STATUS_OK) {
        interview_finish(ctx, status);
    }
    return status;
}

zigpy_status_t zigpy_interview_cancel(zigpy_ctx_t *ctx, uint32_t interview_id)
{
    if (ctx == NULL || ctx->interview.interview_id != interview_id) {
        return ZIGPY_STATUS_NOT_FOUND;
    }
    if (!interview_is_active(ctx)) {
        return ZIGPY_STATUS_STALE;
    }
    if (ctx->backend.cancel_request != NULL) {
        ctx->backend.cancel_request(ctx->backend_ctx, interview_id, true);
    }
    interview_finish(ctx, ZIGPY_STATUS_CANCELLED);
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_interview_get_status(const zigpy_ctx_t *ctx,
                                          zigpy_interview_status_t *out)
{
    if (ctx == NULL || out == NULL || ctx->interview.interview_id == 0u) {
        return ZIGPY_STATUS_NOT_FOUND;
    }
    *out = ctx->interview;
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_interview_get_snapshot(const zigpy_ctx_t *ctx,
                                            zigpy_device_snapshot_t *out)
{
    if (ctx == NULL || out == NULL || ctx->interview.interview_id == 0u) {
        return ZIGPY_STATUS_NOT_FOUND;
    }
    *out = ctx->snapshot;
    if (ctx->interview.phase == ZIGPY_INTERVIEW_DONE) {
        return ctx->interview.status;
    }
    if (ctx->interview.phase == ZIGPY_INTERVIEW_CANCELLED) {
        return ZIGPY_STATUS_CANCELLED;
    }
    if (ctx->interview.phase == ZIGPY_INTERVIEW_FAILED) {
        return ctx->interview.status;
    }
    return ZIGPY_STATUS_PARTIAL;
}

zigpy_status_t zigpy_interview_get_last_good_snapshot(
    const zigpy_ctx_t *ctx, zigpy_device_snapshot_t *out)
{
    if (ctx == NULL || out == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    if (!ctx->has_last_good_snapshot) {
        return ZIGPY_STATUS_NOT_FOUND;
    }
    *out = ctx->last_good_snapshot;
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_interview_node_desc_complete(
    zigpy_ctx_t *ctx, uint32_t interview_id, zigpy_status_t status,
    const zigpy_node_descriptor_t *descriptor)
{
    if (ctx == NULL || ctx->interview.interview_id != interview_id ||
        ctx->interview.phase != ZIGPY_INTERVIEW_NODE_DESC) {
        return ZIGPY_STATUS_STALE;
    }
    if (status != ZIGPY_STATUS_OK || descriptor == NULL) {
        interview_finish(ctx, status == ZIGPY_STATUS_OK ? ZIGPY_STATUS_BACKEND_ERROR
                                                       : status);
        return ctx->interview.status;
    }

    ctx->snapshot.node_descriptor = *descriptor;
    ctx->snapshot.complete_mask |= ZIGPY_INTERVIEW_COMPLETE_NODE_DESC;
    ctx->interview.complete_mask = ctx->snapshot.complete_mask;
    ctx->interview.phase = ZIGPY_INTERVIEW_ACTIVE_EP;
    status = ctx->backend.active_ep_request(ctx->backend_ctx, interview_id,
                                            &ctx->snapshot.device);
    if (status != ZIGPY_STATUS_OK) {
        interview_finish(ctx, status);
    }
    return status;
}

zigpy_status_t zigpy_interview_active_ep_complete(
    zigpy_ctx_t *ctx, uint32_t interview_id, zigpy_status_t status,
    const uint8_t *endpoint_ids, size_t endpoint_count)
{
    size_t copy_count;
    size_t i;

    if (ctx == NULL || ctx->interview.interview_id != interview_id ||
        ctx->interview.phase != ZIGPY_INTERVIEW_ACTIVE_EP) {
        return ZIGPY_STATUS_STALE;
    }
    if (status != ZIGPY_STATUS_OK || (endpoint_count > 0u && endpoint_ids == NULL)) {
        interview_finish(ctx, status == ZIGPY_STATUS_OK ? ZIGPY_STATUS_BACKEND_ERROR
                                                       : status);
        return ctx->interview.status;
    }

    copy_count = endpoint_count;
    if (copy_count > ZIGPY_MAX_ENDPOINTS) {
        copy_count = ZIGPY_MAX_ENDPOINTS;
        ctx->snapshot.truncated_mask |= ZIGPY_INTERVIEW_TRUNC_ENDPOINTS;
    }
    ctx->snapshot.endpoint_count = (uint8_t)copy_count;
    for (i = 0; i < copy_count; ++i) {
        ctx->snapshot.endpoints[i].endpoint_id = endpoint_ids[i];
        ctx->snapshot.endpoints[i].descriptor_status = ZIGPY_STATUS_BUSY;
    }
    ctx->snapshot.complete_mask |= ZIGPY_INTERVIEW_COMPLETE_ACTIVE_EP;
    ctx->interview.complete_mask = ctx->snapshot.complete_mask;
    ctx->interview.truncated_mask = ctx->snapshot.truncated_mask;
    ctx->interview.total_endpoints = (uint8_t)copy_count;
    ctx->interview.phase = ZIGPY_INTERVIEW_SIMPLE_DESC;
    ctx->simple_index = 0u;
    ctx->simple_success_count = 0u;

    return request_next_simple_desc(ctx);
}

zigpy_status_t zigpy_interview_simple_desc_complete(
    zigpy_ctx_t *ctx, uint32_t interview_id, uint8_t endpoint_id,
    zigpy_status_t status, uint16_t profile_id, uint16_t device_id,
    const uint16_t *input_clusters, size_t input_cluster_count,
    const uint16_t *output_clusters, size_t output_cluster_count)
{
    zigpy_endpoint_snapshot_t *ep;
    size_t in_count;
    size_t out_count;

    if (ctx == NULL || ctx->interview.interview_id != interview_id ||
        ctx->interview.phase != ZIGPY_INTERVIEW_SIMPLE_DESC ||
        ctx->simple_index >= ctx->snapshot.endpoint_count) {
        return ZIGPY_STATUS_STALE;
    }
    ep = &ctx->snapshot.endpoints[ctx->simple_index];
    if (ep->endpoint_id != endpoint_id) {
        return ZIGPY_STATUS_STALE;
    }

    ep->descriptor_status = status;
    if (status == ZIGPY_STATUS_OK) {
        ep->profile_id = profile_id;
        ep->device_id = device_id;
        in_count = input_cluster_count;
        out_count = output_cluster_count;
        if (in_count > ZIGPY_MAX_CLUSTERS_PER_ENDPOINT) {
            in_count = ZIGPY_MAX_CLUSTERS_PER_ENDPOINT;
            ctx->snapshot.truncated_mask |= ZIGPY_INTERVIEW_TRUNC_CLUSTERS;
        }
        if (out_count > ZIGPY_MAX_CLUSTERS_PER_ENDPOINT) {
            out_count = ZIGPY_MAX_CLUSTERS_PER_ENDPOINT;
            ctx->snapshot.truncated_mask |= ZIGPY_INTERVIEW_TRUNC_CLUSTERS;
        }
        if ((in_count > 0u && input_clusters == NULL) ||
            (out_count > 0u && output_clusters == NULL)) {
            ep->descriptor_status = ZIGPY_STATUS_BACKEND_ERROR;
        } else {
            ep->input_cluster_count = (uint8_t)in_count;
            ep->output_cluster_count = (uint8_t)out_count;
            if (in_count > 0u) {
                memcpy(ep->input_clusters, input_clusters,
                       in_count * sizeof(input_clusters[0]));
            }
            if (out_count > 0u) {
                memcpy(ep->output_clusters, output_clusters,
                       out_count * sizeof(output_clusters[0]));
            }
            ctx->simple_success_count++;
        }
    }

    ctx->simple_index++;
    ctx->interview.completed_endpoints = ctx->simple_index;
    ctx->interview.truncated_mask = ctx->snapshot.truncated_mask;
    return request_next_simple_desc(ctx);
}

zigpy_status_t zigpy_interview_basic_identity_complete(
    zigpy_ctx_t *ctx, uint32_t interview_id, zigpy_status_t status,
    const char *manufacturer, const char *model)
{
    zigpy_status_t req_status;

    if (ctx == NULL || ctx->interview.interview_id != interview_id ||
        ctx->interview.phase != ZIGPY_INTERVIEW_BASIC_IDENTITY) {
        return ZIGPY_STATUS_STALE;
    }

    if (ctx->identity_stage == 0u) {
        if (status == ZIGPY_STATUS_OK) {
            copy_identity(ctx->snapshot.manufacturer, &ctx->snapshot.truncated_mask,
                          manufacturer);
            copy_identity(ctx->snapshot.model, &ctx->snapshot.truncated_mask, model);
        }
        if (status == ZIGPY_STATUS_OK && manufacturer != NULL && model != NULL) {
            ctx->snapshot.complete_mask |= ZIGPY_INTERVIEW_COMPLETE_IDENTITY;
            ctx->interview.complete_mask = ctx->snapshot.complete_mask;
            ctx->interview.truncated_mask = ctx->snapshot.truncated_mask;
            interview_finish(ctx,
                             ctx->simple_success_count == ctx->snapshot.endpoint_count
                                 ? (ctx->snapshot.truncated_mask != 0u
                                        ? ZIGPY_STATUS_TRUNCATED
                                        : ZIGPY_STATUS_OK)
                                 : ZIGPY_STATUS_PARTIAL);
            return ZIGPY_STATUS_OK;
        }

        ctx->identity_had_error = (status != ZIGPY_STATUS_OK);
        ctx->identity_stage = 1u;
        req_status = ctx->backend.basic_identity_request(
            ctx->backend_ctx, interview_id, &ctx->snapshot.device,
            ctx->identity_endpoint, true, false);
        if (req_status != ZIGPY_STATUS_OK) {
            ctx->identity_had_error = true;
            ctx->identity_stage = 2u;
            req_status = ctx->backend.basic_identity_request(
                ctx->backend_ctx, interview_id, &ctx->snapshot.device,
                ctx->identity_endpoint, false, true);
            if (req_status != ZIGPY_STATUS_OK) {
                ctx->snapshot.complete_mask |= ZIGPY_INTERVIEW_COMPLETE_IDENTITY;
                interview_finish(ctx, ZIGPY_STATUS_PARTIAL);
            }
        }
        return req_status;
    }

    if (ctx->identity_stage == 1u) {
        if (status == ZIGPY_STATUS_OK) {
            copy_identity(ctx->snapshot.manufacturer, &ctx->snapshot.truncated_mask,
                          manufacturer);
        } else {
            ctx->identity_had_error = true;
        }
        ctx->identity_stage = 2u;
        req_status = ctx->backend.basic_identity_request(
            ctx->backend_ctx, interview_id, &ctx->snapshot.device,
            ctx->identity_endpoint, false, true);
        if (req_status != ZIGPY_STATUS_OK) {
            ctx->identity_had_error = true;
            ctx->snapshot.complete_mask |= ZIGPY_INTERVIEW_COMPLETE_IDENTITY;
            interview_finish(ctx, ZIGPY_STATUS_PARTIAL);
        }
        return req_status;
    }

    if (status == ZIGPY_STATUS_OK) {
        copy_identity(ctx->snapshot.model, &ctx->snapshot.truncated_mask, model);
    } else {
        ctx->identity_had_error = true;
    }
    ctx->snapshot.complete_mask |= ZIGPY_INTERVIEW_COMPLETE_IDENTITY;
    ctx->interview.complete_mask = ctx->snapshot.complete_mask;
    ctx->interview.truncated_mask = ctx->snapshot.truncated_mask;
    interview_finish(ctx,
                     (ctx->identity_had_error ||
                      ctx->simple_success_count != ctx->snapshot.endpoint_count)
                         ? ZIGPY_STATUS_PARTIAL
                         : (ctx->snapshot.truncated_mask != 0u
                                ? ZIGPY_STATUS_TRUNCATED
                                : ZIGPY_STATUS_OK));
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_attr_read_async(zigpy_ctx_t *ctx,
                                     const zigpy_attr_path_t *path,
                                     uint32_t timeout_ms, uint8_t retries,
                                     uint32_t now_ms, uint32_t *out_transaction_id)
{
    zigpy_transaction_slot_t *slot;
    if (ctx == NULL || path == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    slot = allocate_transaction(ctx);
    if (slot == NULL) {
        return ZIGPY_STATUS_NO_SPACE;
    }
    slot->request.read = *path;
    return start_transaction(ctx, slot, ZIGPY_TX_ATTR_READ, timeout_ms, retries,
                             now_ms, out_transaction_id);
}

zigpy_status_t zigpy_attr_write_async(zigpy_ctx_t *ctx,
                                      const zigpy_attr_path_t *path,
                                      const uint8_t *value, size_t value_len,
                                      uint32_t timeout_ms, uint8_t retries,
                                      uint32_t now_ms, uint32_t *out_transaction_id)
{
    zigpy_transaction_slot_t *slot;
    if (ctx == NULL || path == NULL || (value_len > 0u && value == NULL) ||
        value_len > ZIGPY_MAX_VALUE_SIZE) {
        return value_len > ZIGPY_MAX_VALUE_SIZE ? ZIGPY_STATUS_TRUNCATED
                                                : ZIGPY_STATUS_INVALID_ARG;
    }
    slot = allocate_transaction(ctx);
    if (slot == NULL) {
        return ZIGPY_STATUS_NO_SPACE;
    }
    slot->request.write.path = *path;
    slot->request.write.value_len = value_len;
    if (value_len > 0u) {
        memcpy(slot->request.write.value, value, value_len);
    }
    return start_transaction(ctx, slot, ZIGPY_TX_ATTR_WRITE, timeout_ms, retries,
                             now_ms, out_transaction_id);
}

zigpy_status_t zigpy_command_invoke_async(zigpy_ctx_t *ctx,
                                          const zigpy_command_path_t *path,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          uint32_t timeout_ms, uint8_t retries,
                                          uint32_t now_ms,
                                          uint32_t *out_transaction_id)
{
    zigpy_transaction_slot_t *slot;
    if (ctx == NULL || path == NULL || (payload_len > 0u && payload == NULL) ||
        payload_len > ZIGPY_MAX_VALUE_SIZE) {
        return payload_len > ZIGPY_MAX_VALUE_SIZE ? ZIGPY_STATUS_TRUNCATED
                                                  : ZIGPY_STATUS_INVALID_ARG;
    }
    slot = allocate_transaction(ctx);
    if (slot == NULL) {
        return ZIGPY_STATUS_NO_SPACE;
    }
    slot->request.command.path = *path;
    slot->request.command.payload_len = payload_len;
    if (payload_len > 0u) {
        memcpy(slot->request.command.payload, payload, payload_len);
    }
    return start_transaction(ctx, slot, ZIGPY_TX_COMMAND, timeout_ms, retries,
                             now_ms, out_transaction_id);
}

zigpy_status_t zigpy_reporting_configure_async(
    zigpy_ctx_t *ctx, const zigpy_reporting_request_t *request,
    uint32_t timeout_ms, uint8_t retries, uint32_t now_ms,
    uint32_t *out_transaction_id)
{
    zigpy_transaction_slot_t *slot;
    if (ctx == NULL || request == NULL || request->max_interval_s == 0u ||
        request->min_interval_s > request->max_interval_s) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    slot = allocate_transaction(ctx);
    if (slot == NULL) {
        return ZIGPY_STATUS_NO_SPACE;
    }
    slot->request.reporting = *request;
    return start_transaction(ctx, slot, ZIGPY_TX_REPORTING, timeout_ms, retries,
                             now_ms, out_transaction_id);
}

zigpy_status_t zigpy_transaction_cancel(zigpy_ctx_t *ctx,
                                        uint32_t transaction_id)
{
    zigpy_transaction_slot_t *slot;
    if (ctx == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    slot = find_transaction(ctx, transaction_id);
    if (slot == NULL) {
        return ZIGPY_STATUS_NOT_FOUND;
    }
    if (slot->result.state != ZIGPY_TX_PENDING) {
        return ZIGPY_STATUS_STALE;
    }
    if (ctx->backend.cancel_request != NULL) {
        ctx->backend.cancel_request(ctx->backend_ctx, transaction_id, false);
    }
    slot->result.state = ZIGPY_TX_CANCELLED;
    slot->result.status = ZIGPY_STATUS_CANCELLED;
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_transaction_complete(zigpy_ctx_t *ctx,
                                          uint32_t transaction_id,
                                          zigpy_status_t status,
                                          const uint8_t *value,
                                          size_t value_len)
{
    zigpy_transaction_slot_t *slot;
    size_t copy_len;

    if (ctx == NULL || (value_len > 0u && value == NULL)) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    slot = find_transaction(ctx, transaction_id);
    if (slot == NULL) {
        return ZIGPY_STATUS_NOT_FOUND;
    }
    if (slot->result.state != ZIGPY_TX_PENDING) {
        return ZIGPY_STATUS_STALE;
    }

    copy_len = value_len;
    if (copy_len > ZIGPY_MAX_VALUE_SIZE) {
        copy_len = ZIGPY_MAX_VALUE_SIZE;
        status = ZIGPY_STATUS_TRUNCATED;
    }
    if (copy_len > 0u) {
        memcpy(slot->result.value, value, copy_len);
    }
    slot->result.value_len = copy_len;
    slot->result.status = status;
    slot->result.state = ZIGPY_TX_COMPLETE;
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_transaction_get_result(const zigpy_ctx_t *ctx,
                                            uint32_t transaction_id,
                                            zigpy_transaction_result_t *out)
{
    const zigpy_transaction_slot_t *slot;
    if (ctx == NULL || out == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    slot = find_transaction_const(ctx, transaction_id);
    if (slot == NULL) {
        return ZIGPY_STATUS_NOT_FOUND;
    }
    *out = slot->result;
    return ZIGPY_STATUS_OK;
}

zigpy_status_t zigpy_transaction_release(zigpy_ctx_t *ctx,
                                         uint32_t transaction_id)
{
    zigpy_transaction_slot_t *slot;
    if (ctx == NULL) {
        return ZIGPY_STATUS_INVALID_ARG;
    }
    slot = find_transaction(ctx, transaction_id);
    if (slot == NULL) {
        return ZIGPY_STATUS_NOT_FOUND;
    }
    if (slot->result.state == ZIGPY_TX_PENDING) {
        return ZIGPY_STATUS_BUSY;
    }
    memset(slot, 0, sizeof(*slot));
    return ZIGPY_STATUS_OK;
}

/*
 * Enforce interview deadlines.
 *
 * Every interview phase only advances when the backend invokes the matching
 * zigpy_interview_*_complete callback. A backend that never calls back (silent
 * or sleepy device, dropped frame, backend fault) previously left the interview
 * active forever, which also blocked all later interviews with ZIGPY_STATUS_BUSY
 * because interview_is_active() stayed true.
 *
 * Returns true when the interview was finished by this call.
 */
static bool poll_interview_deadlines(zigpy_ctx_t *ctx, uint32_t now_ms)
{
    if (!interview_is_active(ctx)) {
        return false;
    }

    /* The phase deadline is refreshed whenever the phase actually changes, so a
     * long multi-endpoint interview is bounded per step rather than in total. */
    if (ctx->interview.phase != ctx->interview_deadline_phase) {
        ctx->interview_deadline_phase = ctx->interview.phase;
        ctx->interview_phase_started_ms = now_ms;
        ctx->interview.phase_deadline_ms =
            (ctx->interview_phase_timeout_ms != 0u)
                ? now_ms + ctx->interview_phase_timeout_ms
                : 0u;
    }

    /*
     * An interview that has already been asked to stop still needs a finite
     * exit: the cancel is asynchronous, so allow the phase deadline that was
     * already running to expire, then close the interview out.
     */
    bool phase_expired = ctx->interview.phase_deadline_ms != 0u &&
                         (int32_t)(now_ms - ctx->interview.phase_deadline_ms) >= 0;
    bool overall_expired = ctx->interview.overall_deadline_ms != 0u &&
                           (int32_t)(now_ms - ctx->interview.overall_deadline_ms) >= 0;

    if (!phase_expired && !overall_expired) {
        return false;
    }

    if (ctx->backend.cancel_request != NULL) {
        ctx->backend.cancel_request(ctx->backend_ctx,
                                    ctx->interview.interview_id, true);
    }

    /* PARTIAL when some usable evidence was already collected; FAILED when the
     * device produced nothing. Either way this is a finite, reported outcome,
     * never a silent hang. */
    if (ctx->snapshot.complete_mask != 0u) {
        interview_finish(ctx, ZIGPY_STATUS_PARTIAL);
    } else {
        interview_finish(ctx, ZIGPY_STATUS_TIMEOUT);
    }
    return true;
}

void zigpy_poll(zigpy_ctx_t *ctx, uint32_t now_ms)
{
    size_t i;
    if (ctx == NULL) {
        return;
    }
    if (ctx->commissioning_active &&
        (int32_t)(now_ms - ctx->commissioning_deadline_ms) >= 0) {
        (void)zigpy_commissioning_stop(ctx);
    }

    (void)poll_interview_deadlines(ctx, now_ms);

    for (i = 0; i < ZIGPY_MAX_TRANSACTIONS; ++i) {
        zigpy_transaction_slot_t *slot = &ctx->transactions[i];
        zigpy_status_t status;
        if (slot->result.state != ZIGPY_TX_PENDING) {
            continue;
        }
        if ((int32_t)(now_ms - slot->deadline_ms) < 0) {
            continue;
        }
        /* Retries are bounded: start_transaction clamps max_retries so that
         * `attempts` (uint8_t, starting at 1) can always exceed it. The counter
         * therefore cannot wrap and this loop always terminates. */
        if (slot->result.attempts <= slot->max_retries) {
            slot->result.attempts++;
            slot->deadline_ms = now_ms + slot->timeout_ms;
            status = send_transaction(ctx, slot);
            if (status != ZIGPY_STATUS_OK) {
                slot->result.state = ZIGPY_TX_COMPLETE;
                slot->result.status = status;
            }
        } else {
            slot->result.state = ZIGPY_TX_COMPLETE;
            slot->result.status = ZIGPY_STATUS_TIMEOUT;
        }
    }
}
