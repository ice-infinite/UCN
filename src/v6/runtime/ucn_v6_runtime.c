#include "../internal/ucn_v6_runtime_private.h"

#include <string.h>

typedef char runtime_owner_storage_must_fit[
    sizeof(struct ucn_v6_runtime_owner) <= UCN_V6_RUNTIME_OWNER_STORAGE_BYTES ?
        1 : -1];

static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) ++(*value);
}

#if UCN_V6_FEATURE_REALTIME_ENABLED
static bool event_key_equal(const ucn_v6_driver_event_key_t *left,
                            const ucn_v6_driver_event_key_t *right)
{
    return left != NULL && right != NULL && left->link_id == right->link_id &&
           left->link_generation == right->link_generation &&
           left->event_token == right->event_token;
}
static bool event_key_is_valid(const ucn_v6_driver_event_key_t *key)
{
    return key != NULL && key->link_id != 0U &&
           key->link_id <= UCN_V6_LINK_ID_MAX && key->link_generation != 0U &&
           key->link_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           key->event_token != 0U &&
           key->event_token <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}
#endif

static bool runtime_storage_is_valid(const ucn_v6_runtime_owner_t *runtime)
{
    return runtime != NULL && runtime->magic == UCN_V6_RUNTIME_MAGIC &&
           runtime->schema == UCN_V6_STORAGE_LAYOUT &&
           runtime->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           runtime->initialized && runtime->canary == UCN_V6_RUNTIME_CANARY;
}

static bool runtime_is_valid(const ucn_v6_runtime_owner_t *runtime)
{
    return runtime_storage_is_valid(runtime) && !runtime->stats.faulted
#if UCN_V6_FEATURE_REALTIME_ENABLED
           && !runtime->time_tx_active
#endif
        ;
}

static bool config_is_valid(const ucn_v6_runtime_config_t *config)
{
    if (config == NULL || config->runtime_instance_generation == 0U ||
        config->runtime_instance_generation >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD || config->adapter == NULL ||
        config->bootstrap == NULL || config->security == NULL ||
        config->capability == NULL || config->route == NULL ||
        config->metric == NULL || config->qos == NULL ||
        config->transfer == NULL || config->app.handle_ingress == NULL ||
        config->app.release_buffer == NULL) {
        return false;
    }
#if UCN_V6_FEATURE_REALTIME_ENABLED
    if (config->realtime == NULL) return false;
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    if (config->cluster == NULL) return false;
#endif
    return true;
}

static ucn_v6_result_t phase_noop(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    (void)now_us;
    if (!runtime_is_valid((ucn_v6_runtime_owner_t *)context) ||
        budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    return UCN_V6_OK;
}

static ucn_v6_result_t retry_at_next_tick(
    uint64_t now_us, ucn_v6_stack_phase_result_t *result)
{
    if (now_us == UINT64_MAX) return UCN_V6_ERR_EXHAUSTED;
    result->has_deadline = true;
    result->next_deadline_us = now_us + 1U;
    return UCN_V6_OK;
}

static ucn_v6_result_t phase_rx(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_runtime_ingress_disposition_t disposition =
        (ucn_v6_runtime_ingress_disposition_t)0;
    ucn_v6_result_t call_result;
    ucn_v6_result_t retire_result;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL ||
        runtime->ingress_active) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    call_result = ucn_v6_adapter_peek_rx(
        runtime->config.adapter, runtime->rx_frame,
        sizeof(runtime->rx_frame), &runtime->active_rx);
    if (call_result == UCN_V6_ERR_NOT_FOUND) return UCN_V6_OK;
    if (call_result != UCN_V6_OK) return call_result;
    runtime->ingress_active = true;
    runtime->callback_active = true;
    call_result = runtime->config.app.handle_ingress(
        runtime->config.app.context, runtime, now_us, runtime->rx_frame,
        runtime->active_rx.frame_length, &runtime->active_rx, &disposition);
    runtime->callback_active = false;
    runtime->ingress_active = false;
    if (call_result != UCN_V6_OK ||
        (disposition != UCN_V6_RUNTIME_INGRESS_CONSUMED &&
         disposition != UCN_V6_RUNTIME_INGRESS_DROP &&
         disposition != UCN_V6_RUNTIME_INGRESS_RETRY)) {
        runtime->stats.faulted = true;
        return call_result == UCN_V6_OK ? UCN_V6_ERR_STATE : call_result;
    }
    if (disposition == UCN_V6_RUNTIME_INGRESS_RETRY) {
        increment_saturated(&runtime->stats.rx_retried);
        return retry_at_next_tick(now_us, result);
    }
    retire_result = ucn_v6_adapter_retire_rx(
        runtime->config.adapter, &runtime->active_rx.key);
    if (retire_result != UCN_V6_OK) {
        runtime->stats.faulted = true;
        return retire_result;
    }
    if (disposition == UCN_V6_RUNTIME_INGRESS_CONSUMED) {
        increment_saturated(&runtime->stats.rx_consumed);
    } else {
        increment_saturated(&runtime->stats.rx_dropped);
    }
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_runtime_release_slot_t *find_release(
    ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    for (index = 0U; index < UCN_V6_RUNTIME_RELEASE_SLOTS; ++index) {
        if (runtime->releases[index].occupied) return &runtime->releases[index];
    }
    return NULL;
}

static ucn_v6_runtime_release_slot_t *find_release_token(
    ucn_v6_runtime_owner_t *runtime, uint64_t token)
{
    size_t index;
    for (index = 0U; index < UCN_V6_RUNTIME_RELEASE_SLOTS; ++index) {
        if (runtime->releases[index].occupied &&
            runtime->releases[index].buffer_token == token) {
            return &runtime->releases[index];
        }
    }
    return NULL;
}

static size_t free_release_slots(const ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    size_t count = 0U;
    for (index = 0U; index < UCN_V6_RUNTIME_RELEASE_SLOTS; ++index) {
        if (!runtime->releases[index].occupied) ++count;
    }
    return count;
}

static bool queue_release(
    ucn_v6_runtime_owner_t *runtime, uint64_t token,
    ucn_v6_result_t completion_result,
    const ucn_v6_driver_timestamp_t *timestamp)
{
    size_t index;
    if (token == 0U) return false;
    for (index = 0U; index < UCN_V6_RUNTIME_RELEASE_SLOTS; ++index) {
        ucn_v6_runtime_release_slot_t *slot = &runtime->releases[index];
        if (slot->occupied && slot->buffer_token == token) return false;
        if (!slot->occupied) {
            memset(slot, 0, sizeof(*slot));
            slot->occupied = true;
            slot->buffer_token = token;
            slot->result = completion_result;
            if (timestamp != NULL) slot->timestamp = *timestamp;
            return true;
        }
    }
    return false;
}

#if UCN_V6_FEATURE_REALTIME_ENABLED
static void record_time_tx_completion(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_driver_tx_completion_t *completion)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES; ++index) {
        ucn_v6_runtime_time_slot_t *slot = &runtime->time_slots[index];
        if (slot->occupied && slot->tx_bound && !slot->local_tx_complete &&
            event_key_equal(&slot->tx_key, &completion->key) &&
            completion->result == UCN_V6_OK && completion->timestamp.valid &&
            completion->timestamp.hardware &&
            completion->timestamp.timestamp_us != 0U &&
            completion->timestamp.uncertainty_us != 0U) {
            slot->local_tx.link_id = completion->key.link_id;
            slot->local_tx.link_generation = completion->key.link_generation;
            slot->local_tx.event_token = completion->key.event_token;
            slot->local_tx.timestamp_us = completion->timestamp.timestamp_us;
            slot->local_tx.uncertainty_us = completion->timestamp.uncertainty_us;
            slot->local_tx.hardware = true;
            slot->local_tx_complete = true;
            increment_saturated(
                &runtime->stats.realtime_tx_timestamps_captured);
            return;
        }
    }
}
#endif

static ucn_v6_result_t phase_tx_completion(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_runtime_release_slot_t *release;
    ucn_v6_driver_tx_completion_t completion;
    ucn_v6_result_t call_result;
    uint64_t retired_token = 0U;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    release = find_release(runtime);
    if (release != NULL) {
        runtime->callback_active = true;
        call_result = runtime->config.app.release_buffer(
            runtime->config.app.context, release->buffer_token,
            release->result, &release->timestamp);
        runtime->callback_active = false;
        if (call_result != UCN_V6_OK) return retry_at_next_tick(now_us, result);
        memset(release, 0, sizeof(*release));
        increment_saturated(&runtime->stats.released_buffers);
        result->work_done = 1U;
        result->has_more = true;
        return UCN_V6_OK;
    }
    memset(&completion, 0, sizeof(completion));
    call_result = ucn_v6_adapter_peek_tx_completion(runtime->config.adapter,
                                                    &completion);
    if (call_result == UCN_V6_ERR_NOT_FOUND) return UCN_V6_OK;
    if (call_result != UCN_V6_OK) return call_result;
    if (free_release_slots(runtime) == 0U ||
        !queue_release(runtime, completion.buffer_token, completion.result,
                       &completion.timestamp)) {
        return retry_at_next_tick(now_us, result);
    }
#if UCN_V6_FEATURE_REALTIME_ENABLED
    /* Commit T3 only after Adapter ownership has been retired below. */
#endif
    call_result = ucn_v6_adapter_retire_tx_completion(
        runtime->config.adapter, &completion.key, &retired_token);
    if (call_result != UCN_V6_OK || retired_token != completion.buffer_token) {
        ucn_v6_runtime_release_slot_t *queued =
            find_release_token(runtime, completion.buffer_token);
        if (queued != NULL) {
            memset(queued, 0, sizeof(*queued));
        }
        runtime->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
#if UCN_V6_FEATURE_REALTIME_ENABLED
    record_time_tx_completion(runtime, &completion);
#endif
    increment_saturated(&runtime->stats.tx_completions);
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t expire_time_slots(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us)
{
#if UCN_V6_FEATURE_REALTIME_ENABLED
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES; ++index) {
        ucn_v6_runtime_time_slot_t *slot = &runtime->time_slots[index];
        if (slot->occupied && now_us >= slot->deadline_us) {
            if (slot->tx_bound && !slot->local_tx_complete) {
                ucn_v6_result_t result = ucn_v6_adapter_cancel_tx(
                    runtime->config.adapter, &slot->tx_key);
                if (result != UCN_V6_OK && result != UCN_V6_ERR_NOT_FOUND) {
                    return result;
                }
            }
            memset(slot, 0, sizeof(*slot));
            increment_saturated(&runtime->stats.realtime_exchanges_expired);
        }
    }
#else
    (void)runtime;
    (void)now_us;
#endif
    return UCN_V6_OK;
}

static ucn_v6_result_t phase_timer(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    call_result = expire_time_slots(runtime, now_us);
    if (call_result != UCN_V6_OK) return call_result;
    (void)ucn_v6_bootstrap_expire(runtime->config.bootstrap, now_us);
    if (ucn_v6_capability_expire(runtime->config.capability, now_us) !=
            UCN_V6_OK ||
        ucn_v6_route_expire(runtime->config.route, now_us) != UCN_V6_OK ||
        ucn_v6_metric_expire(runtime->config.metric, now_us) != UCN_V6_OK ||
        ucn_v6_transfer_expire(runtime->config.transfer, now_us) !=
            UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
#if UCN_V6_FEATURE_REALTIME_ENABLED
    if (ucn_v6_realtime_step(runtime->config.realtime, now_us) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    if (ucn_v6_cluster_step(runtime->config.cluster, now_us) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
#endif
    result->work_done = 1U;
    return UCN_V6_OK;
}

static ucn_v6_result_t ack_completed_invalidation(
    ucn_v6_runtime_owner_t *runtime,
    ucn_v6_runtime_invalidation_source_t source,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_result_t ack_result;
    if (runtime->pending_source != source ||
        !runtime->invalidation_fanout_complete) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (source == UCN_V6_RUNTIME_INVALIDATION_ADAPTER) {
        ack_result = UCN_V6_OK;
    } else if (source == UCN_V6_RUNTIME_INVALIDATION_SECURITY) {
        ack_result = ucn_v6_security_invalidation_ack(
            runtime->config.security, &runtime->pending_invalidation);
    } else {
        ack_result = ucn_v6_capability_invalidation_ack(
            runtime->config.capability, &runtime->pending_invalidation);
    }
    if (ack_result != UCN_V6_OK) return ack_result;
    memset(&runtime->pending_invalidation, 0,
           sizeof(runtime->pending_invalidation));
    runtime->pending_source = UCN_V6_RUNTIME_INVALIDATION_NONE;
    runtime->invalidation_fanout_complete = false;
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t invalidation_phase(
    ucn_v6_runtime_owner_t *runtime,
    ucn_v6_runtime_invalidation_source_t source,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_result_t call_result;
    if (runtime->pending_source == source) {
        if (runtime->invalidation_fanout_complete) {
            return ack_completed_invalidation(runtime, source, result);
        }
        result->work_done = 1U;
        result->has_more = true;
        result->has_invalidation = true;
        result->invalidation = runtime->pending_invalidation;
        return UCN_V6_OK;
    }
    if (runtime->pending_source != UCN_V6_RUNTIME_INVALIDATION_NONE) {
        return UCN_V6_OK;
    }
    if (source == UCN_V6_RUNTIME_INVALIDATION_ADAPTER) {
        return UCN_V6_OK;
    } else if (source == UCN_V6_RUNTIME_INVALIDATION_SECURITY) {
        call_result = ucn_v6_security_invalidation_peek(
            runtime->config.security, &runtime->pending_invalidation);
    } else {
        call_result = ucn_v6_capability_invalidation_peek(
            runtime->config.capability, &runtime->pending_invalidation);
    }
    if (call_result == UCN_V6_ERR_NOT_FOUND) return UCN_V6_OK;
    if (call_result != UCN_V6_OK) return call_result;
    runtime->pending_source = source;
    runtime->invalidation_fanout_complete = false;
    result->work_done = 1U;
    result->has_more = true;
    result->has_invalidation = true;
    result->invalidation = runtime->pending_invalidation;
    return UCN_V6_OK;
}

static ucn_v6_result_t phase_security(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    (void)now_us;
    if (!runtime_is_valid((ucn_v6_runtime_owner_t *)context) ||
        budget == 0U || result == NULL) return UCN_V6_ERR_STATE;
    memset(result, 0, sizeof(*result));
    if (((ucn_v6_runtime_owner_t *)context)->pending_source ==
        UCN_V6_RUNTIME_INVALIDATION_ADAPTER) {
        return invalidation_phase((ucn_v6_runtime_owner_t *)context,
                                  UCN_V6_RUNTIME_INVALIDATION_ADAPTER,
                                  result);
    }
    return invalidation_phase((ucn_v6_runtime_owner_t *)context,
                              UCN_V6_RUNTIME_INVALIDATION_SECURITY, result);
}

static ucn_v6_result_t phase_capability(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    (void)now_us;
    if (!runtime_is_valid((ucn_v6_runtime_owner_t *)context) ||
        budget == 0U || result == NULL) return UCN_V6_ERR_STATE;
    memset(result, 0, sizeof(*result));
    return invalidation_phase((ucn_v6_runtime_owner_t *)context,
                              UCN_V6_RUNTIME_INVALIDATION_CAPABILITY, result);
}

static ucn_v6_result_t phase_qos_tx(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    bool submitted = false;
    ucn_v6_result_t call_result;
    (void)now_us;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    call_result = ucn_v6_adapter_service_tx(runtime->config.adapter,
                                            &submitted);
    if (call_result != UCN_V6_OK) return call_result;
    if (submitted) {
        result->work_done = 1U;
        result->has_more = true;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t invalidate_adapter(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    return runtime_is_valid((ucn_v6_runtime_owner_t *)context) &&
           ucn_v6_stack_invalidation_is_valid(invalidation) ?
               UCN_V6_OK : UCN_V6_ERR_STATE;
}

static ucn_v6_result_t invalidate_security(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    return invalidate_adapter(context, invalidation);
}

static ucn_v6_result_t invalidate_capability(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    if (!runtime_is_valid(runtime)) return UCN_V6_ERR_STATE;
    return ucn_v6_capability_apply_invalidation(runtime->config.capability,
                                                invalidation);
}

#if UCN_V6_FEATURE_REALTIME_ENABLED
static ucn_v6_result_t invalidate_realtime(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    if (!runtime_is_valid(runtime)) return UCN_V6_ERR_STATE;
    return ucn_v6_realtime_apply_invalidation(runtime->config.realtime,
                                              invalidation);
}
#endif

#if UCN_V6_FEATURE_CLUSTER_ENABLED
static ucn_v6_result_t invalidate_cluster(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    if (!runtime_is_valid(runtime)) return UCN_V6_ERR_STATE;
    return ucn_v6_cluster_apply_invalidation(runtime->config.cluster,
                                             invalidation);
}
#endif

static bool queue_retired_tokens(
    ucn_v6_runtime_owner_t *runtime, const uint64_t *tokens, size_t count)
{
    size_t index;
    if (count > free_release_slots(runtime)) return false;
    for (index = 0U; index < count; ++index) {
        if (!queue_release(runtime, tokens[index], UCN_V6_ERR_ACCESS, NULL)) {
            return false;
        }
    }
    return true;
}

static ucn_v6_result_t invalidate_transfer(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    uint64_t tokens[UCN_V6_CONFIG_TRANSFER_TX_SLOTS];
    ucn_v6_transfer_invalidation_result_t result;
    size_t count = 0U;
    size_t capacity;
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime)) return UCN_V6_ERR_STATE;
    capacity = free_release_slots(runtime);
    if (capacity > UCN_V6_CONFIG_TRANSFER_TX_SLOTS) {
        capacity = UCN_V6_CONFIG_TRANSFER_TX_SLOTS;
    }
    call_result = ucn_v6_transfer_apply_invalidation(
        runtime->config.transfer, invalidation, tokens, capacity, &count,
        &result);
    if (call_result != UCN_V6_OK) return call_result;
    return queue_retired_tokens(runtime, tokens, count) ?
               UCN_V6_OK : UCN_V6_ERR_STATE;
}

static ucn_v6_result_t invalidate_route(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    if (!runtime_is_valid(runtime)) return UCN_V6_ERR_STATE;
    return ucn_v6_route_apply_invalidation(runtime->config.route,
                                           invalidation);
}

static ucn_v6_result_t invalidate_qos(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    uint64_t tokens[UCN_V6_CONFIG_QOS_Q0_DEPTH +
                    UCN_V6_CONFIG_QOS_Q1_DEPTH +
                    UCN_V6_CONFIG_QOS_Q2_DEPTH +
                    UCN_V6_CONFIG_QOS_Q3_DEPTH +
                    UCN_V6_CONFIG_QOS_INFLIGHT];
    size_t count = 0U;
    size_t capacity;
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime)) return UCN_V6_ERR_STATE;
    capacity = free_release_slots(runtime);
    if (capacity > sizeof(tokens) / sizeof(tokens[0])) {
        capacity = sizeof(tokens) / sizeof(tokens[0]);
    }
    call_result = ucn_v6_qos_apply_invalidation(
        runtime->config.qos, invalidation, tokens, capacity, &count);
    if (call_result != UCN_V6_OK) return call_result;
    return queue_retired_tokens(runtime, tokens, count) ?
               UCN_V6_OK : UCN_V6_ERR_STATE;
}

static ucn_v6_result_t invalidate_endpoint(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_result_t result = UCN_V6_OK;
    if (!runtime_is_valid(runtime) ||
        runtime->pending_source == UCN_V6_RUNTIME_INVALIDATION_NONE ||
        memcmp(invalidation, &runtime->pending_invalidation,
               sizeof(*invalidation)) != 0) {
        return UCN_V6_ERR_STATE;
    }
    if (runtime->config.app.apply_endpoint_invalidation != NULL) {
        runtime->callback_active = true;
        result = runtime->config.app.apply_endpoint_invalidation(
            runtime->config.app.context, invalidation);
        runtime->callback_active = false;
    }
    if (result == UCN_V6_OK) {
        runtime->invalidation_fanout_complete = true;
        increment_saturated(&runtime->stats.invalidations);
    }
    return result;
}

ucn_v6_result_t ucn_v6_runtime_init_in_place(
    void *storage, size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_runtime_config_t *config,
    ucn_v6_runtime_owner_t **runtime_out)
{
    ucn_v6_runtime_owner_t *initialized;
    if (runtime_out == NULL || !config_is_valid(config) ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_RUNTIME_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     runtime_out, sizeof(*runtime_out)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     manifest, sizeof(*manifest)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config, sizeof(*config)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->adapter, 1U) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->bootstrap, 1U) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->security, 1U) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->capability, 1U) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->route, 1U) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->metric, 1U) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->qos, 1U) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->transfer, 1U) ||
#if UCN_V6_FEATURE_REALTIME_ENABLED
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->realtime, 1U) ||
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config->cluster, 1U) ||
#endif
        ucn_v6_memory_ranges_overlap(
            storage, storage_bytes, config->app.context,
            config->app.context != NULL ? 1U : 0U)) {
        return UCN_V6_ERR_CONFIG;
    }
    initialized = (ucn_v6_runtime_owner_t *)storage;
    memset(initialized, 0, sizeof(*initialized));
    initialized->magic = UCN_V6_RUNTIME_MAGIC;
    initialized->schema = UCN_V6_STORAGE_LAYOUT;
    initialized->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    initialized->config = *config;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    initialized->next_time_handle_cookie = 1U;
#endif
    initialized->initialized = true;
    initialized->canary = UCN_V6_RUNTIME_CANARY;
    *runtime_out = initialized;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_runtime_make_stack_hooks(
    ucn_v6_runtime_owner_t *runtime, ucn_v6_stack_hooks_t *hooks)
{
    ucn_v6_stack_hooks_t next;
    if (!runtime_is_valid(runtime) || hooks == NULL ||
        ucn_v6_memory_ranges_overlap(runtime, sizeof(*runtime), hooks,
                                     sizeof(*hooks))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&next, 0, sizeof(next));
    next.context = runtime;
    next.rx_ingress = phase_rx;
    next.tx_completion = phase_tx_completion;
    next.timer_expiry = phase_timer;
    next.persistence = phase_noop;
    next.hop_security = phase_security;
    next.e2e_security = phase_noop;
    next.capability = phase_capability;
    next.route_authority = phase_noop;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    next.realtime = phase_noop;
#endif
    next.operation = phase_noop;
    next.endpoint = phase_noop;
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    next.cluster = phase_noop;
#endif
    next.qos_tx = phase_qos_tx;
    next.invalidate_adapter = invalidate_adapter;
    next.invalidate_security = invalidate_security;
    next.invalidate_capability = invalidate_capability;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    next.invalidate_realtime = invalidate_realtime;
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    next.invalidate_cluster = invalidate_cluster;
#endif
    next.invalidate_transfer = invalidate_transfer;
    next.invalidate_route = invalidate_route;
    next.invalidate_qos = invalidate_qos;
    next.invalidate_endpoint = invalidate_endpoint;
    *hooks = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_runtime_reopen_link(
    ucn_v6_runtime_owner_t *runtime, uint16_t link_id,
    uint32_t *new_link_generation)
{
    uint64_t tokens[UCN_V6_CONFIG_ADAPTER_TX_SLOTS];
    size_t retired_count = 0U;
    size_t capacity;
    uint32_t new_generation = 0U;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || new_link_generation == NULL ||
        link_id == 0U || link_id == UINT16_MAX ||
        runtime->callback_active || runtime->ingress_active ||
        runtime->pending_source != UCN_V6_RUNTIME_INVALIDATION_NONE) {
        return UCN_V6_ERR_STATE;
    }
    capacity = free_release_slots(runtime);
    if (capacity > UCN_V6_CONFIG_ADAPTER_TX_SLOTS) {
        capacity = UCN_V6_CONFIG_ADAPTER_TX_SLOTS;
    }
    result = ucn_v6_adapter_reopen_link(
        runtime->config.adapter, link_id, tokens, capacity, &retired_count,
        &new_generation);
    if (result != UCN_V6_OK) return result;
    if (new_generation <= 1U ||
        new_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        !queue_retired_tokens(runtime, tokens, retired_count)) {
        runtime->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    memset(&runtime->pending_invalidation, 0,
           sizeof(runtime->pending_invalidation));
    runtime->pending_invalidation.type = UCN_V6_STACK_INVALIDATE_LINK;
    runtime->pending_invalidation.link_id = link_id;
    runtime->pending_invalidation.link_generation = new_generation - 1U;
    runtime->pending_source = UCN_V6_RUNTIME_INVALIDATION_ADAPTER;
    runtime->invalidation_fanout_complete = false;
    increment_saturated(&runtime->stats.link_reopens);
    *new_link_generation = new_generation;
    return UCN_V6_OK;
}

#if UCN_V6_FEATURE_REALTIME_ENABLED
static bool principal_equal(const ucn_v6_principal_t *left,
                            const ucn_v6_principal_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool binding_equal(const ucn_v6_binding_key_t *left,
                          const ucn_v6_binding_key_t *right)
{
    return left != NULL && right != NULL &&
           ucn_v6_binding_key_equal(left, right);
}

static ucn_v6_binding_key_t frame_source_binding(
    const ucn_v6_frame_t *frame)
{
    ucn_v6_binding_key_t value;
    value.realm_id = frame->realm_id;
    value.node_address = frame->source_address;
    value.binding_generation = frame->source_binding_generation;
    return value;
}

static ucn_v6_binding_key_t frame_destination_binding(
    const ucn_v6_frame_t *frame)
{
    ucn_v6_binding_key_t value;
    value.realm_id = frame->realm_id;
    value.node_address = frame->destination_address;
    value.binding_generation = frame->destination_binding_generation;
    return value;
}

static bool opened_time_control_is_valid(
    const ucn_v6_security_open_result_t *opened, uint16_t opcode)
{
    const uint8_t required_flags =
        UCN_V6_FLAG_PEER_HOP_CONTEXT | UCN_V6_FLAG_E2E_CONTEXT |
        UCN_V6_FLAG_PROTOCOL_CONTEXT | UCN_V6_FLAG_ROUTE_CONTEXT |
        UCN_V6_FLAG_PATH_CONTEXT;
    return opened != NULL && opened->frame.frame_type == UCN_V6_FRAME_CONTROL &&
           opened->frame.flags == required_flags &&
           opened->frame.protocol_opcode == opcode &&
           opened->frame.traffic_class == UCN_V6_TRAFFIC_Q0 &&
           opened->frame.delivery_guarantee == UCN_V6_DELIVERY_RELIABLE &&
           opened->frame.message.interaction_role ==
               UCN_V6_INTERACTION_ONE_WAY &&
           opened->frame.message.operation_id == 0U &&
           opened->hop_authenticated && opened->endpoint_authorized &&
           ucn_v6_principal_is_valid(&opened->authenticated_principal) &&
           ucn_v6_principal_is_valid(
               &opened->ingress_peer_session.principal) &&
           ucn_v6_binding_key_is_valid(&opened->ingress_peer_session.binding) &&
           opened->frame.session_generation != 0U &&
           opened->frame.session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool opened_time_control_has_valid_ingress(
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_driver_rx_view_t *rx, uint64_t now_us)
{
    return rx != NULL && event_key_is_valid(&rx->key) &&
           opened->ingress_link_instance_id == rx->key.link_id &&
           opened->ingress_link_instance_generation ==
               rx->key.link_generation &&
           opened->ingress_peer_session.session_generation != 0U &&
           opened->ingress_peer_session.session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           rx->timestamp.valid && rx->timestamp.hardware &&
           rx->timestamp.timestamp_us != 0U &&
           rx->timestamp.timestamp_us <= now_us &&
           rx->timestamp.uncertainty_us != 0U;
}

static bool reverse_ref_matches_sync(
    const ucn_v6_route_path_ref_t *reference,
    const ucn_v6_security_open_result_t *opened)
{
    ucn_v6_binding_key_t source = frame_source_binding(&opened->frame);
    ucn_v6_binding_key_t destination =
        frame_destination_binding(&opened->frame);
    return reference != NULL &&
           principal_equal(&reference->domain.destination_principal,
                           &opened->authenticated_principal) &&
           binding_equal(&reference->domain.origin_binding, &destination) &&
           binding_equal(&reference->domain.destination_binding, &source) &&
           reference->domain.origin_session_generation ==
               opened->frame.session_generation &&
           reference->domain.destination_session_generation ==
               opened->frame.session_generation;
}

static void build_inbound_forward_ref(
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_route_path_ref_t *reverse,
    ucn_v6_route_path_ref_t *forward)
{
    memset(forward, 0, sizeof(*forward));
    forward->domain.origin_principal = opened->authenticated_principal;
    forward->domain.origin_binding = frame_source_binding(&opened->frame);
    forward->domain.origin_session_generation =
        opened->frame.session_generation;
    forward->domain.destination_principal =
        reverse->domain.origin_principal;
    forward->domain.destination_binding =
        frame_destination_binding(&opened->frame);
    forward->domain.destination_session_generation =
        opened->frame.session_generation;
    forward->route_generation = opened->frame.route_generation;
    forward->path_id = opened->frame.path.path_id;
    forward->path_generation = opened->frame.path.path_generation;
}

static ucn_v6_address_class_t address_class_for_route(
    const ucn_v6_route_path_ref_t *reference)
{
    ucn_v6_address_class_t address_class;
    uint32_t maximum = reference->domain.origin_binding.node_address;
    if (reference->domain.destination_binding.node_address > maximum) {
        maximum = reference->domain.destination_binding.node_address;
    }
    for (address_class = UCN_V6_ADDRESS_CLASS_A0;
         address_class <= UCN_V6_ADDRESS_CLASS_A3;
         address_class = (ucn_v6_address_class_t)(address_class + 1)) {
        if (maximum <= ucn_v6_address_max_ordinary(address_class)) {
            return address_class;
        }
    }
    return (ucn_v6_address_class_t)-1;
}

static ucn_v6_result_t send_time_control(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_route_path_ref_t *reference,
    uint16_t opcode, const uint8_t *payload, size_t payload_length,
    uint64_t buffer_token, bool request_timestamp, uint64_t now_us,
    ucn_v6_driver_event_key_t *tx_key)
{
    ucn_v6_route_resolution_t resolution;
    ucn_v6_frame_t frame;
    ucn_v6_address_class_t address_class;
    size_t encoded_length = 0U;
    ucn_v6_result_t result;
    if (runtime->time_tx_active || reference == NULL || payload == NULL ||
        payload_length == 0U ||
        payload_length > sizeof(runtime->time_payload_work) ||
        buffer_token == 0U || tx_key == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = ucn_v6_route_resolve_ref(runtime->config.route, now_us,
                                      reference, &resolution);
    if (result != UCN_V6_OK) return result;
    if (!resolution.path.available ||
        !principal_equal(&resolution.path.next_hop.principal,
                         &resolution.dependency.session.principal) ||
        resolution.path.egress_link_id == 0U ||
        resolution.path.egress_link_generation == 0U) {
        return UCN_V6_ERR_STATE;
    }
    address_class = address_class_for_route(reference);
    if ((uint32_t)address_class > (uint32_t)UCN_V6_ADDRESS_CLASS_A3) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&frame, 0, sizeof(frame));
    frame.address_class = address_class;
    frame.frame_type = UCN_V6_FRAME_CONTROL;
    frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT | UCN_V6_FLAG_E2E_CONTEXT |
                  UCN_V6_FLAG_PROTOCOL_CONTEXT | UCN_V6_FLAG_ROUTE_CONTEXT |
                  UCN_V6_FLAG_PATH_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    frame.hop_limit = resolution.path.hop_count;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = reference->domain.origin_binding.realm_id;
    frame.source_address = reference->domain.origin_binding.node_address;
    frame.destination_address =
        reference->domain.destination_binding.node_address;
    frame.source_binding_generation =
        reference->domain.origin_binding.binding_generation;
    frame.destination_binding_generation =
        reference->domain.destination_binding.binding_generation;
    frame.session_generation =
        reference->domain.destination_session_generation;
    frame.protocol_opcode = opcode;
    frame.route_generation = reference->route_generation;
    frame.path.path_id = reference->path_id;
    frame.path.path_generation = reference->path_generation;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;

    runtime->time_tx_active = true;
    result = ucn_v6_security_protect_frame(
        runtime->config.security, now_us,
        &resolution.path.next_hop.principal,
        &reference->domain.destination_principal, &frame,
        runtime->time_payload_work, sizeof(runtime->time_payload_work),
        runtime->time_frame_work, sizeof(runtime->time_frame_work),
        runtime->time_encoded, sizeof(runtime->time_encoded),
        &encoded_length);
    if (result == UCN_V6_OK) {
        result = ucn_v6_adapter_enqueue_tx(
            runtime->config.adapter, resolution.path.egress_link_id,
            buffer_token, runtime->time_encoded, encoded_length,
            UCN_V6_TRAFFIC_Q0, request_timestamp, tx_key);
    }
    runtime->time_tx_active = false;
    return result;
}

static ucn_v6_runtime_time_slot_t *find_time_slot(
    ucn_v6_runtime_owner_t *runtime, ucn_v6_runtime_time_role_t role,
    uint16_t clock_domain_id, uint32_t domain_generation, uint32_t sequence)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES; ++index) {
        if (runtime->time_slots[index].occupied &&
            runtime->time_slots[index].role == role &&
            runtime->time_slots[index].clock_domain_id == clock_domain_id &&
            runtime->time_slots[index].domain_generation == domain_generation &&
            runtime->time_slots[index].sync_sequence == sequence) {
            return &runtime->time_slots[index];
        }
    }
    return NULL;
}

static ucn_v6_runtime_time_slot_t *find_free_time_slot(
    ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES; ++index) {
        if (!runtime->time_slots[index].occupied) return &runtime->time_slots[index];
    }
    return NULL;
}

static bool allocate_time_handle(
    ucn_v6_runtime_owner_t *runtime, ucn_v6_runtime_time_slot_t *slot,
    ucn_v6_runtime_time_handle_t *handle)
{
    size_t index;
    ucn_v6_runtime_time_handle_t value;
    if (handle == NULL || runtime->next_time_handle_cookie == 0U ||
        runtime->next_time_handle_cookie > UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return false;
    }
    index = (size_t)(slot - runtime->time_slots);
    slot->handle_cookie = runtime->next_time_handle_cookie++;
    value.opaque[0] = slot->handle_cookie;
    value.opaque[1] = UCN_V6_RUNTIME_CANARY ^ slot->handle_cookie ^
                      (uint64_t)(index + 1U) ^
                      runtime->config.runtime_instance_generation;
    *handle = value;
    return true;
}

static ucn_v6_runtime_time_slot_t *find_time_handle(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_runtime_time_handle_t *handle,
    ucn_v6_runtime_time_role_t role)
{
    size_t index;
    if (handle == NULL || handle->opaque[0] == 0U) return NULL;
    for (index = 0U; index < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES; ++index) {
        ucn_v6_runtime_time_slot_t *slot = &runtime->time_slots[index];
        if (slot->occupied && slot->role == role &&
            slot->handle_cookie == handle->opaque[0] &&
            handle->opaque[1] == (UCN_V6_RUNTIME_CANARY ^
                                  slot->handle_cookie ^
                                  (uint64_t)(index + 1U) ^
                                  runtime->config.runtime_instance_generation)) {
            return slot;
        }
    }
    return NULL;
}

ucn_v6_result_t ucn_v6_runtime_time_start_sync(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_route_path_ref_t *forward_route_ref,
    const ucn_v6_time_sync_announce_t *announce,
    uint64_t buffer_token, uint64_t now_us,
    ucn_v6_runtime_time_handle_t *handle)
{
    ucn_v6_runtime_time_slot_t *slot;
    ucn_v6_runtime_time_handle_t issued;
    uint8_t payload[UCN_V6_TIME_SYNC_ANNOUNCE_BYTES];
    ucn_v6_driver_event_key_t tx_key;
    uint64_t deadline;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || forward_route_ref == NULL ||
        announce == NULL || handle == NULL || buffer_token == 0U ||
        ucn_v6_memory_ranges_overlap(runtime, sizeof(*runtime), handle,
                                     sizeof(*handle)) ||
        UINT64_MAX - now_us < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGE_TIMEOUT_US ||
        ucn_v6_time_sync_announce_encode(announce, payload) != UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (find_time_slot(runtime, UCN_V6_RUNTIME_TIME_MASTER,
                       announce->clock_domain_id, announce->domain_generation,
                       announce->sync_sequence) != NULL) {
        return UCN_V6_ERR_REPLAY;
    }
    slot = find_free_time_slot(runtime);
    if (slot == NULL) return UCN_V6_ERR_NO_SPACE;
    deadline = now_us + UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGE_TIMEOUT_US;
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->role = UCN_V6_RUNTIME_TIME_MASTER;
    slot->clock_domain_id = announce->clock_domain_id;
    slot->domain_generation = announce->domain_generation;
    slot->sync_sequence = announce->sync_sequence;
    slot->deadline_us = deadline;
    slot->route_ref = *forward_route_ref;
    slot->remote_principal = forward_route_ref->domain.destination_principal;
    slot->local_binding = forward_route_ref->domain.origin_binding;
    slot->remote_binding = forward_route_ref->domain.destination_binding;
    slot->session_generation =
        forward_route_ref->domain.destination_session_generation;
    if (!allocate_time_handle(runtime, slot, &issued)) {
        memset(slot, 0, sizeof(*slot));
        runtime->stats.faulted = true;
        return UCN_V6_ERR_EXHAUSTED;
    }
    result = send_time_control(
        runtime, &slot->route_ref, UCN_V6_PROTOCOL_OPCODE_TIME_SYNC,
        payload, sizeof(payload), buffer_token, true, now_us, &tx_key);
    if (result != UCN_V6_OK) {
        memset(slot, 0, sizeof(*slot));
        return result;
    }
    slot->tx_key = tx_key;
    slot->tx_bound = true;
    *handle = issued;
    increment_saturated(&runtime->stats.realtime_exchanges_started);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_runtime_time_observe_sync(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_driver_rx_view_t *rx,
    const ucn_v6_route_path_ref_t *reverse_route_ref,
    uint64_t now_us,
    ucn_v6_runtime_time_handle_t *handle)
{
    ucn_v6_time_sync_announce_t announce;
    ucn_v6_route_resolution_t reverse_resolution;
    ucn_v6_runtime_time_slot_t *slot = NULL;
    ucn_v6_runtime_time_handle_t issued;
    size_t index;
    uint64_t deadline;
    if (!runtime_is_valid(runtime) || !runtime->ingress_active ||
        opened == NULL || rx == NULL || reverse_route_ref == NULL ||
        handle == NULL ||
        ucn_v6_memory_ranges_overlap(runtime, sizeof(*runtime), handle,
                                     sizeof(*handle)) ||
        memcmp(rx, &runtime->active_rx, sizeof(*rx)) != 0 ||
        !opened_time_control_is_valid(
            opened, UCN_V6_PROTOCOL_OPCODE_TIME_SYNC) ||
        !opened_time_control_has_valid_ingress(opened, rx, now_us) ||
        !reverse_ref_matches_sync(reverse_route_ref, opened) ||
        ucn_v6_time_sync_announce_decode(
            opened->frame.payload, opened->frame.payload_length,
            &announce) != UCN_V6_OK ||
        UINT64_MAX - now_us <
            UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGE_TIMEOUT_US) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (ucn_v6_route_resolve_ref(runtime->config.route, now_us,
                                 reverse_route_ref, &reverse_resolution) !=
            UCN_V6_OK ||
        !reverse_resolution.path.available) {
        return UCN_V6_ERR_STATE;
    }
    deadline = now_us + UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGE_TIMEOUT_US;
    for (index = 0U; index < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES; ++index) {
        ucn_v6_runtime_time_slot_t *candidate = &runtime->time_slots[index];
        if (candidate->occupied &&
            candidate->role == UCN_V6_RUNTIME_TIME_MEMBER &&
            candidate->clock_domain_id == announce.clock_domain_id) {
            if (announce.sync_sequence <= candidate->sync_sequence) {
                return UCN_V6_ERR_REPLAY;
            }
            if (candidate->tx_bound && !candidate->local_tx_complete &&
                ucn_v6_adapter_cancel_tx(runtime->config.adapter,
                                         &candidate->tx_key) != UCN_V6_OK) {
                return UCN_V6_ERR_STATE;
            }
            memset(candidate, 0, sizeof(*candidate));
        }
        if (slot == NULL && !candidate->occupied) slot = candidate;
    }
    if (slot == NULL) return UCN_V6_ERR_NO_SPACE;
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->role = UCN_V6_RUNTIME_TIME_MEMBER;
    slot->clock_domain_id = announce.clock_domain_id;
    slot->domain_generation = announce.domain_generation;
    slot->sync_sequence = announce.sync_sequence;
    slot->deadline_us = deadline;
    slot->route_ref = *reverse_route_ref;
    build_inbound_forward_ref(opened, reverse_route_ref,
                              &slot->inbound_forward_route_ref);
    slot->remote_principal = opened->authenticated_principal;
    slot->local_binding = frame_destination_binding(&opened->frame);
    slot->remote_binding = frame_source_binding(&opened->frame);
    slot->session_generation = opened->frame.session_generation;
    slot->local_rx.link_id = rx->key.link_id;
    slot->local_rx.link_generation = rx->key.link_generation;
    slot->local_rx.event_token = rx->key.event_token;
    slot->local_rx.timestamp_us = rx->timestamp.timestamp_us;
    slot->local_rx.uncertainty_us = rx->timestamp.uncertainty_us;
    slot->local_rx.hardware = true;
    if (!allocate_time_handle(runtime, slot, &issued)) {
        memset(slot, 0, sizeof(*slot));
        runtime->stats.faulted = true;
        return UCN_V6_ERR_EXHAUSTED;
    }
    *handle = issued;
    increment_saturated(&runtime->stats.realtime_exchanges_started);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_runtime_time_send_delay_request(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_runtime_time_handle_t *handle,
    uint64_t buffer_token, uint64_t now_us)
{
    ucn_v6_runtime_time_slot_t *slot;
    ucn_v6_time_sync_announce_t request;
    uint8_t payload[UCN_V6_TIME_SYNC_ANNOUNCE_BYTES];
    ucn_v6_driver_event_key_t tx_key;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || handle == NULL || buffer_token == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_time_handle(runtime, handle, UCN_V6_RUNTIME_TIME_MEMBER);
    if (slot == NULL) return UCN_V6_ERR_NOT_FOUND;
    if (now_us >= slot->deadline_us || slot->tx_bound) {
        return slot->tx_bound ? UCN_V6_ERR_REPLAY : UCN_V6_ERR_TIMEOUT;
    }
    request.clock_domain_id = slot->clock_domain_id;
    request.domain_generation = slot->domain_generation;
    request.sync_sequence = slot->sync_sequence;
    if (ucn_v6_time_sync_announce_encode(&request, payload) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    result = send_time_control(
        runtime, &slot->route_ref,
        UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_REQUEST,
        payload, sizeof(payload), buffer_token, true, now_us, &tx_key);
    if (result != UCN_V6_OK) return result;
    slot->tx_key = tx_key;
    slot->tx_bound = true;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_runtime_time_respond_delay_request(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_driver_rx_view_t *rx,
    uint64_t buffer_token, uint64_t now_us)
{
    ucn_v6_time_sync_announce_t request;
    ucn_v6_runtime_time_slot_t *slot;
    ucn_v6_binding_key_t source;
    ucn_v6_binding_key_t destination;
    uint8_t payload[UCN_V6_TIME_SYNC_RESPONSE_BYTES];
    ucn_v6_driver_event_key_t ignored_key;
    bool first_response;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || !runtime->ingress_active ||
        opened == NULL || rx == NULL || buffer_token == 0U ||
        memcmp(rx, &runtime->active_rx, sizeof(*rx)) != 0 ||
        !opened_time_control_is_valid(
            opened, UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_REQUEST) ||
        !opened_time_control_has_valid_ingress(opened, rx, now_us) ||
        ucn_v6_time_sync_announce_decode(
            opened->frame.payload, opened->frame.payload_length,
            &request) != UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_time_slot(runtime, UCN_V6_RUNTIME_TIME_MASTER,
                          request.clock_domain_id,
                          request.domain_generation,
                          request.sync_sequence);
    if (slot == NULL || !slot->local_tx_complete ||
        now_us >= slot->deadline_us) {
        return UCN_V6_ERR_STATE;
    }
    source = frame_source_binding(&opened->frame);
    destination = frame_destination_binding(&opened->frame);
    if (!principal_equal(&opened->authenticated_principal,
                         &slot->remote_principal) ||
        !binding_equal(&source, &slot->remote_binding) ||
        !binding_equal(&destination, &slot->local_binding) ||
        opened->frame.session_generation != slot->session_generation ||
        opened->frame.realm_id != slot->local_binding.realm_id) {
        return UCN_V6_ERR_ACCESS;
    }
    first_response = !slot->response_semantic_frozen;
    if (first_response) {
        memset(&slot->frozen_response, 0, sizeof(slot->frozen_response));
        slot->frozen_response.clock_domain_id = slot->clock_domain_id;
        slot->frozen_response.domain_generation = slot->domain_generation;
        slot->frozen_response.sync_sequence = slot->sync_sequence;
        slot->frozen_response.t1_master_tx_us =
            slot->local_tx.timestamp_us;
        slot->frozen_response.t4_master_rx_us = rx->timestamp.timestamp_us;
        slot->frozen_response.t1_uncertainty_us =
            slot->local_tx.uncertainty_us;
        slot->frozen_response.t4_uncertainty_us =
            rx->timestamp.uncertainty_us;
        slot->inbound_forward_route_ref.domain.origin_principal =
            opened->authenticated_principal;
        slot->inbound_forward_route_ref.domain.origin_binding = source;
        slot->inbound_forward_route_ref.domain.origin_session_generation =
            opened->frame.session_generation;
        slot->inbound_forward_route_ref.domain.destination_principal =
            slot->route_ref.domain.origin_principal;
        slot->inbound_forward_route_ref.domain.destination_binding = destination;
        slot->inbound_forward_route_ref.domain.destination_session_generation =
            opened->frame.session_generation;
        slot->inbound_forward_route_ref.route_generation =
            opened->frame.route_generation;
        slot->inbound_forward_route_ref.path_id = opened->frame.path.path_id;
        slot->inbound_forward_route_ref.path_generation =
            opened->frame.path.path_generation;
        slot->local_rx.link_id = rx->key.link_id;
        slot->local_rx.link_generation = rx->key.link_generation;
        slot->local_rx.event_token = rx->key.event_token;
        slot->local_rx.timestamp_us = rx->timestamp.timestamp_us;
        slot->local_rx.uncertainty_us = rx->timestamp.uncertainty_us;
        slot->local_rx.hardware = true;
        slot->response_semantic_frozen = true;
    } else if (opened->frame.route_generation !=
                   slot->inbound_forward_route_ref.route_generation ||
               opened->frame.path.path_id !=
                   slot->inbound_forward_route_ref.path_id ||
               opened->frame.path.path_generation !=
                   slot->inbound_forward_route_ref.path_generation ||
               rx->key.link_id != slot->local_rx.link_id ||
               rx->key.link_generation != slot->local_rx.link_generation) {
        return UCN_V6_ERR_REPLAY;
    }
    if (ucn_v6_time_sync_response_encode(&slot->frozen_response, payload) !=
        UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    result = send_time_control(
        runtime, &slot->route_ref,
        UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE,
        payload, sizeof(payload), buffer_token, false, now_us, &ignored_key);
    if (result == UCN_V6_OK && !slot->response_sent) {
        slot->response_sent = true;
        increment_saturated(&runtime->stats.realtime_exchanges_completed);
    }
    return result;
}

ucn_v6_result_t ucn_v6_runtime_time_complete(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_driver_rx_view_t *rx,
    uint64_t now_us)
{
    ucn_v6_time_sync_response_t response;
    ucn_v6_time_sync_observation_t observation;
    ucn_v6_runtime_time_slot_t *slot;
    ucn_v6_binding_key_t source;
    ucn_v6_binding_key_t destination;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || !runtime->ingress_active ||
        opened == NULL || rx == NULL ||
        memcmp(rx, &runtime->active_rx, sizeof(*rx)) != 0 ||
        ucn_v6_time_sync_response_decode(
            opened->frame.payload, opened->frame.payload_length,
            &response) != UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_time_slot(runtime, UCN_V6_RUNTIME_TIME_MEMBER,
                          response.clock_domain_id,
                          response.domain_generation,
                          response.sync_sequence);
    source = frame_source_binding(&opened->frame);
    destination = frame_destination_binding(&opened->frame);
    if (slot == NULL || !slot->local_tx_complete ||
        slot->clock_domain_id != response.clock_domain_id ||
        slot->domain_generation != response.domain_generation ||
        now_us >= slot->deadline_us ||
        !opened_time_control_is_valid(
            opened, UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE) ||
        !principal_equal(&opened->authenticated_principal,
                         &slot->remote_principal) ||
        !binding_equal(&source, &slot->remote_binding) ||
        !binding_equal(&destination, &slot->local_binding) ||
        opened->frame.session_generation != slot->session_generation ||
        opened->frame.route_generation !=
            slot->inbound_forward_route_ref.route_generation ||
        opened->frame.path.path_id !=
            slot->inbound_forward_route_ref.path_id ||
        opened->frame.path.path_generation !=
            slot->inbound_forward_route_ref.path_generation ||
        opened->ingress_link_instance_id != slot->local_rx.link_id ||
        opened->ingress_link_instance_generation !=
            slot->local_rx.link_generation ||
        !opened_time_control_has_valid_ingress(opened, rx, now_us)) {
        return UCN_V6_ERR_STATE;
    }
    memset(&observation, 0, sizeof(observation));
    observation.sync_sequence = slot->sync_sequence;
    observation.forward_route_ref = slot->inbound_forward_route_ref;
    observation.reverse_route_ref = slot->route_ref;
    observation.t2_member_rx = slot->local_rx;
    observation.t3_member_tx = slot->local_tx;
    result = ucn_v6_realtime_ingest_exchange(
        runtime->config.realtime, opened, &observation, now_us);
    if (result == UCN_V6_OK) {
        memset(slot, 0, sizeof(*slot));
        increment_saturated(&runtime->stats.realtime_exchanges_completed);
    }
    return result;
}
#endif

ucn_v6_result_t ucn_v6_runtime_copy_view(
    const ucn_v6_runtime_owner_t *runtime, ucn_v6_runtime_view_t *view)
{
    if (!runtime_storage_is_valid(runtime) || view == NULL ||
        ucn_v6_memory_ranges_overlap(runtime, sizeof(*runtime), view,
                                     sizeof(*view))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *view = runtime->stats;
    return UCN_V6_OK;
}
