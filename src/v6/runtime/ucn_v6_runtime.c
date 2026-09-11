#include "../internal/ucn_v6_runtime_private.h"

#include <string.h>

typedef char runtime_owner_storage_must_fit[
    sizeof(struct ucn_v6_runtime_owner) <= UCN_V6_RUNTIME_OWNER_STORAGE_BYTES ?
        1 : -1];

static bool runtime_principal_equal(
    const ucn_v6_principal_t *left,
    const ucn_v6_principal_t *right);
static bool runtime_binding_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right);
static bool runtime_session_equal(
    const ucn_v6_session_key_t *left,
    const ucn_v6_session_key_t *right);
static bool runtime_session_is_valid(
    const ucn_v6_session_key_t *session);
static ucn_v6_address_class_t address_class_for_route(
    const ucn_v6_route_path_ref_t *reference);
static ucn_v6_result_t runtime_enqueue_exact_frame(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_runtime_send_request_t *request, bool release_to_app,
    const ucn_v6_route_path_ref_t *reference,
    ucn_v6_runtime_send_result_t *result_out);
static ucn_v6_result_t runtime_enqueue_frame_kind(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_runtime_send_request_t *request, bool release_to_app,
    ucn_v6_runtime_tx_security_kind_t security_kind,
    ucn_v6_runtime_send_result_t *result_out);
static ucn_v6_result_t select_reverse_route(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_route_path_ref_t *reference);
static ucn_v6_result_t enqueue_peer_discovery_reply(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_security_open_result_t *opened, uint16_t opcode,
    ucn_v6_traffic_class_t traffic_class,
    const uint8_t *payload, size_t payload_length);
#if UCN_V6_FEATURE_REALTIME_ENABLED
static ucn_v6_result_t phase_realtime(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result);
#endif

static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) ++(*value);
}

static bool event_key_is_valid(const ucn_v6_driver_event_key_t *key)
{
    return key != NULL && key->link_id != 0U &&
           key->link_id <= UCN_V6_LINK_ID_MAX && key->link_generation != 0U &&
           key->link_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           key->event_token != 0U &&
           key->event_token <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}
#if UCN_V6_FEATURE_REALTIME_ENABLED
static bool event_key_equal(const ucn_v6_driver_event_key_t *left,
                            const ucn_v6_driver_event_key_t *right)
{
    return left != NULL && right != NULL && left->link_id == right->link_id &&
           left->link_generation == right->link_generation &&
           left->event_token == right->event_token;
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
    return runtime_storage_is_valid(runtime) && !runtime->stats.faulted;
}

static bool config_is_valid(const ucn_v6_runtime_config_t *config)
{
    if (config == NULL || config->runtime_instance_generation == 0U ||
        config->runtime_instance_generation >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        config->relay_route_policy < UCN_V6_ROUTE_POLICY_PINNED ||
        config->relay_route_policy >
            UCN_V6_ROUTE_POLICY_WEIGHTED_MULTIPATH ||
        config->relay_residence_bound_us == 0U ||
        config->relay_transmit_bound_us == 0U ||
        UINT64_MAX - config->relay_residence_bound_us <
            config->relay_transmit_bound_us || config->adapter == NULL ||
        config->transfer_maximum_credit == 0U ||
        config->bootstrap == NULL || config->security == NULL ||
        config->capability == NULL || config->route == NULL ||
        config->metric == NULL || config->qos == NULL ||
        config->transfer == NULL ||
        config->bootstrap_ops.process_ingress == NULL ||
        config->bootstrap_ops.build_join_commit == NULL ||
        config->app.handle_authenticated_ingress == NULL ||
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
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    if (runtime->ingress_active ||
        runtime->pending_source != UCN_V6_RUNTIME_INVALIDATION_NONE) {
        return UCN_V6_OK;
    }
    call_result = ucn_v6_adapter_peek_rx(
        runtime->config.adapter, runtime->rx_frame,
        sizeof(runtime->rx_frame), &runtime->active_rx);
    if (call_result == UCN_V6_ERR_NOT_FOUND) return UCN_V6_OK;
    if (call_result != UCN_V6_OK) return call_result;
    runtime->ingress_active = true;
    runtime->rx_phase = UCN_V6_RUNTIME_RX_RAW;
    runtime->rx_disposition =
        (ucn_v6_runtime_ingress_disposition_t)0;
    memset(&runtime->opened_rx, 0, sizeof(runtime->opened_rx));
    result->work_done = 1U;
    result->has_more = true;
    (void)now_us;
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

static ucn_v6_runtime_tx_slot_t *find_tx_slot(
    ucn_v6_runtime_owner_t *runtime, uint64_t buffer_token)
{
    size_t index;
    for (index = 0U; index < UCN_V6_RUNTIME_TX_SLOTS; ++index) {
        if (runtime->tx_slots[index].occupied &&
            runtime->tx_slots[index].buffer_token == buffer_token) {
            return &runtime->tx_slots[index];
        }
    }
    return NULL;
}

static ucn_v6_runtime_tx_slot_t *find_free_tx_slot(
    ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    for (index = 0U; index < UCN_V6_RUNTIME_TX_SLOTS; ++index) {
        if (!runtime->tx_slots[index].occupied) {
            return &runtime->tx_slots[index];
        }
    }
    return NULL;
}

static ucn_v6_runtime_transfer_slot_t *find_transfer_slot(
    ucn_v6_runtime_owner_t *runtime, uint64_t message_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        if (runtime->transfer_slots[index].occupied &&
            runtime->transfer_slots[index].message_id == message_id) {
            return &runtime->transfer_slots[index];
        }
    }
    return NULL;
}

static ucn_v6_runtime_transfer_slot_t *find_free_transfer_slot(
    ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        if (!runtime->transfer_slots[index].occupied) {
            return &runtime->transfer_slots[index];
        }
    }
    return NULL;
}

static void clear_transfer_fragment_marker(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_runtime_tx_slot_t *slot)
{
    ucn_v6_runtime_transfer_slot_t *transfer;
    if (runtime == NULL || slot == NULL || !slot->transfer_fragment) return;
    transfer = find_transfer_slot(runtime, slot->transfer_message_id);
    if (transfer != NULL) transfer->fragment_queued = false;
}

static ucn_v6_result_t allocate_internal_buffer_token(
    ucn_v6_runtime_owner_t *runtime, uint64_t *buffer_token)
{
    uint64_t token;
    if (runtime == NULL || buffer_token == NULL ||
        runtime->next_internal_buffer_token <= (uint64_t)INT64_MAX) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    token = runtime->next_internal_buffer_token;
    --runtime->next_internal_buffer_token;
    *buffer_token = token;
    return UCN_V6_OK;
}

static bool packet_local_failure(ucn_v6_result_t result)
{
    return result == UCN_V6_ERR_MALFORMED ||
           result == UCN_V6_ERR_SECURITY ||
           result == UCN_V6_ERR_ACCESS ||
           result == UCN_V6_ERR_REPLAY ||
           result == UCN_V6_ERR_NOT_FOUND ||
           result == UCN_V6_ERR_NO_SPACE ||
           result == UCN_V6_ERR_EXHAUSTED ||
           result == UCN_V6_ERR_TIMEOUT ||
           result == UCN_V6_ERR_ARGUMENT;
}

static const ucn_v6_runtime_qos_inflight_t *qos_inflight_find_const(
    const ucn_v6_runtime_owner_t *runtime, uint64_t buffer_token)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (runtime->qos_inflight[index].buffer_token == buffer_token) {
            return &runtime->qos_inflight[index];
        }
    }
    return NULL;
}

static bool qos_inflight_add(
    ucn_v6_runtime_owner_t *runtime, uint64_t buffer_token,
    bool release_to_app, bool bootstrap,
    const ucn_v6_driver_event_key_t *adapter_key,
    const ucn_v6_session_key_t *next_hop_parent,
    const ucn_v6_session_key_t *endpoint_parent)
{
    size_t index;
    if (buffer_token == 0U || !event_key_is_valid(adapter_key) ||
        next_hop_parent == NULL || endpoint_parent == NULL ||
        qos_inflight_find_const(runtime, buffer_token) != NULL) {
        return false;
    }
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (runtime->qos_inflight[index].buffer_token == 0U) {
            runtime->qos_inflight[index].buffer_token = buffer_token;
            runtime->qos_inflight[index].release_to_app = release_to_app;
            runtime->qos_inflight[index].bootstrap = bootstrap;
            runtime->qos_inflight[index].adapter_key = *adapter_key;
            runtime->qos_inflight[index].next_hop_parent = *next_hop_parent;
            runtime->qos_inflight[index].endpoint_parent = *endpoint_parent;
            return true;
        }
    }
    return false;
}

static bool qos_inflight_has_free(const ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (runtime->qos_inflight[index].buffer_token == 0U) return true;
    }
    return false;
}

static void qos_inflight_remove(
    ucn_v6_runtime_owner_t *runtime, uint64_t buffer_token)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (runtime->qos_inflight[index].buffer_token == buffer_token) {
            memset(&runtime->qos_inflight[index], 0,
                   sizeof(runtime->qos_inflight[index]));
            return;
        }
    }
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
        if (!slot->occupied || !slot->tx_bound || slot->local_tx_complete ||
            slot->tx_buffer_token != completion->buffer_token ||
            !event_key_equal(&slot->tx_key, &completion->key)) {
            continue;
        }
        if (completion->result == UCN_V6_OK && completion->timestamp.valid &&
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
        } else {
            memset(slot, 0, sizeof(*slot));
            increment_saturated(&runtime->stats.realtime_exchanges_expired);
        }
        return;
    }
}

static void bind_time_tx_submission(
    ucn_v6_runtime_owner_t *runtime, uint64_t buffer_token,
    const ucn_v6_driver_event_key_t *key)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES; ++index) {
        ucn_v6_runtime_time_slot_t *slot = &runtime->time_slots[index];
        if (slot->occupied && slot->tx_queued && !slot->tx_bound &&
            slot->tx_buffer_token == buffer_token) {
            slot->tx_key = *key;
            slot->tx_bound = true;
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
    const ucn_v6_runtime_qos_inflight_t *qos_marker;
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
    qos_marker = qos_inflight_find_const(runtime, completion.buffer_token);
    if ((qos_marker == NULL || qos_marker->release_to_app) &&
        (free_release_slots(runtime) == 0U ||
         !queue_release(runtime, completion.buffer_token, completion.result,
                        &completion.timestamp))) {
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
    if (qos_marker != NULL && !qos_marker->bootstrap) {
        call_result = ucn_v6_qos_record_completion(
            runtime->config.qos, completion.buffer_token,
            UCN_V6_QOS_COMPLETION_PHYSICAL_COMPLETED);
        if (call_result == UCN_V6_OK) {
            call_result = ucn_v6_qos_retire_completion(
                runtime->config.qos, completion.buffer_token);
        }
        if (call_result != UCN_V6_OK) {
            runtime->stats.faulted = true;
            return call_result;
        }
    }
    if (qos_marker != NULL) {
        if (qos_marker->bootstrap && completion.result == UCN_V6_OK) {
            increment_saturated(&runtime->stats.bootstrap_frames_sent);
        }
        qos_inflight_remove(runtime, completion.buffer_token);
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
            if (slot->tx_queued && !slot->tx_bound) {
                ucn_v6_runtime_tx_slot_t *queued =
                    find_tx_slot(runtime, slot->tx_buffer_token);
                ucn_v6_result_t cancel = ucn_v6_qos_cancel_queued(
                    runtime->config.qos, slot->tx_buffer_token);
                if (cancel != UCN_V6_OK || queued == NULL) return UCN_V6_ERR_STATE;
                memset(queued, 0, sizeof(*queued));
            } else if (slot->tx_bound && !slot->local_tx_complete) {
                ucn_v6_result_t cancel = ucn_v6_adapter_cancel_tx(
                    runtime->config.adapter, &slot->tx_key);
                if (cancel != UCN_V6_OK &&
                    cancel != UCN_V6_ERR_NOT_FOUND &&
                    cancel != UCN_V6_ERR_REPLAY) {
                    return cancel;
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
    {
        size_t index;
        size_t expired = ucn_v6_bootstrap_expire(
            runtime->config.bootstrap, now_us);
        for (index = 0U;
             index < UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U; ++index) {
            ucn_v6_bootstrap_reassembly_t *slot =
                &runtime->bootstrap_reassembly[index];
            if (slot->occupied && now_us >= slot->deadline_us) {
                (void)ucn_v6_bootstrap_reassembly_reset(slot);
                ++expired;
            }
        }
        for (index = 0U; index < UCN_V6_RUNTIME_BOOTSTRAP_TX_SLOTS;
             ++index) {
            ucn_v6_runtime_bootstrap_tx_slot_t *slot =
                &runtime->bootstrap_tx[index];
            if (slot->occupied && now_us >= slot->deadline_us) {
                memset(slot, 0, sizeof(*slot));
                ++expired;
            }
        }
        for (index = 0U;
             index < UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U; ++index) {
            ucn_v6_runtime_bootstrap_initiation_t *slot =
                &runtime->bootstrap_initiations[index];
            if (slot->occupied && now_us >= slot->deadline_us) {
                memset(slot, 0, sizeof(*slot));
                ++expired;
            }
        }
        while (expired != 0U) {
            increment_saturated(
                &runtime->stats.bootstrap_objects_expired);
            --expired;
        }
    }
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

static ucn_v6_bootstrap_event_t runtime_bootstrap_event_from_opcode(
    uint16_t opcode)
{
    switch (opcode) {
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_IDENTITY_CHALLENGE:
        return UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_IDENTITY_RESPONSE:
        return UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_ADDRESS_OFFER:
        return UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_DEVICE_COMMIT:
        return UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_FINAL_COMMIT:
        return UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_ABORT:
        return UCN_V6_BOOTSTRAP_EVENT_ABORT;
    default:
        return (ucn_v6_bootstrap_event_t)0;
    }
}

static ucn_v6_bootstrap_phase_t runtime_bootstrap_event_phase(
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_flow_t flow,
    ucn_v6_bootstrap_phase_t current_phase)
{
    switch (event) {
    case UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF:
        return UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED;
    case UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF:
        return UCN_V6_BOOTSTRAP_DEVICE_VERIFIED;
    case UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER:
        return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ?
                   UCN_V6_BOOTSTRAP_ADDRESS_OFFERED :
                   UCN_V6_BOOTSTRAP_EMPTY;
    case UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT:
        return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ?
                   UCN_V6_BOOTSTRAP_DEVICE_COMMITTED :
                   UCN_V6_BOOTSTRAP_EMPTY;
    case UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE:
        return UCN_V6_BOOTSTRAP_FINAL_DURABLE;
    case UCN_V6_BOOTSTRAP_EVENT_ABORT:
        return current_phase;
    default:
        return UCN_V6_BOOTSTRAP_EMPTY;
    }
}

static ucn_v6_bootstrap_event_t runtime_bootstrap_expected_response(
    const ucn_v6_runtime_bootstrap_ingress_t *ingress)
{
    if (ingress == NULL) return (ucn_v6_bootstrap_event_t)0;
    if (ingress->kind == UCN_V6_RUNTIME_BOOTSTRAP_HELLO_COOKIE) {
        return UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF;
    }
    if (ingress->kind != UCN_V6_RUNTIME_BOOTSTRAP_EVENT) {
        return (ucn_v6_bootstrap_event_t)0;
    }
    switch (ingress->event) {
    case UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF:
        return UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF;
    case UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF:
        return ingress->pending.flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ?
                   UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER :
                   UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE;
    case UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER:
        return UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT;
    case UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT:
        return UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE;
    default:
        return (ucn_v6_bootstrap_event_t)0;
    }
}

static ucn_v6_runtime_bootstrap_tx_slot_t *find_bootstrap_tx_free(
    ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    for (index = 0U; index < UCN_V6_RUNTIME_BOOTSTRAP_TX_SLOTS; ++index) {
        if (!runtime->bootstrap_tx[index].occupied) {
            return &runtime->bootstrap_tx[index];
        }
    }
    return NULL;
}

static size_t bootstrap_tx_free_count(
    const ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    size_t count = 0U;
    for (index = 0U; index < UCN_V6_RUNTIME_BOOTSTRAP_TX_SLOTS; ++index) {
        if (!runtime->bootstrap_tx[index].occupied) ++count;
    }
    return count;
}

static ucn_v6_runtime_bootstrap_tx_slot_t *find_bootstrap_tx_pending(
    ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    for (index = 0U; index < UCN_V6_RUNTIME_BOOTSTRAP_TX_SLOTS; ++index) {
        if (runtime->bootstrap_tx[index].occupied) {
            return &runtime->bootstrap_tx[index];
        }
    }
    return NULL;
}

static ucn_v6_runtime_bootstrap_tx_slot_t *find_bootstrap_tx_token(
    ucn_v6_runtime_owner_t *runtime, uint64_t token)
{
    size_t index;
    for (index = 0U; index < UCN_V6_RUNTIME_BOOTSTRAP_TX_SLOTS; ++index) {
        if (runtime->bootstrap_tx[index].occupied &&
            runtime->bootstrap_tx[index].buffer_token == token) {
            return &runtime->bootstrap_tx[index];
        }
    }
    return NULL;
}

static ucn_v6_runtime_bootstrap_initiation_t *find_bootstrap_initiation_free(
    ucn_v6_runtime_owner_t *runtime)
{
    size_t index;
    for (index = 0U;
         index < UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U; ++index) {
        if (!runtime->bootstrap_initiations[index].occupied) {
            return &runtime->bootstrap_initiations[index];
        }
    }
    return NULL;
}

static ucn_v6_runtime_bootstrap_initiation_t *find_bootstrap_initiation(
    ucn_v6_runtime_owner_t *runtime,
    uint16_t link_id,
    uint32_t link_generation,
    uint32_t local_peer_discriminator,
    ucn_v6_bootstrap_flow_t flow,
    uint64_t transaction_id)
{
    size_t index;
    for (index = 0U;
         index < UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U; ++index) {
        ucn_v6_runtime_bootstrap_initiation_t *slot =
            &runtime->bootstrap_initiations[index];
        if (slot->occupied && slot->link_id == link_id &&
            slot->link_generation == link_generation &&
            slot->local_peer_discriminator == local_peer_discriminator &&
            slot->hello.flow == flow &&
            slot->hello.transaction_id == transaction_id) {
            return slot;
        }
    }
    return NULL;
}

static ucn_v6_bootstrap_reassembly_t *find_bootstrap_reassembly(
    ucn_v6_runtime_owner_t *runtime,
    uint16_t opcode,
    const ucn_v6_bootstrap_key_t *key,
    bool allow_free)
{
    size_t index;
    ucn_v6_bootstrap_reassembly_t *free_slot = NULL;
    for (index = 0U;
         index < UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U; ++index) {
        ucn_v6_bootstrap_reassembly_t *slot =
            &runtime->bootstrap_reassembly[index];
        if (!slot->occupied) {
            if (free_slot == NULL) free_slot = slot;
            continue;
        }
        if (slot->protocol_opcode == opcode &&
            slot->key.ingress_link_id == key->ingress_link_id &&
            slot->key.ingress_link_generation ==
                key->ingress_link_generation &&
            slot->key.local_peer_discriminator ==
                key->local_peer_discriminator &&
            slot->key.transaction_id == key->transaction_id &&
            memcmp(slot->key.identity_digest.bytes,
                   key->identity_digest.bytes,
                   sizeof(key->identity_digest.bytes)) == 0) {
            return slot;
        }
    }
    return allow_free ? free_slot : NULL;
}

static bool runtime_bootstrap_source_is_reauth(
    const ucn_v6_frame_t *frame)
{
    return frame->source_address != 0U &&
           frame->source_binding_generation != 0U;
}

static ucn_v6_result_t queue_bootstrap_payload(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_frame_t *ingress_frame,
    uint16_t link_id,
    uint32_t link_generation,
    uint16_t opcode,
    const uint8_t *payload,
    size_t payload_length,
    uint64_t deadline_us,
    uint64_t *queued_token)
{
    ucn_v6_runtime_bootstrap_tx_slot_t *slot;
    ucn_v6_principal_t local_principal;
    ucn_v6_binding_key_t local_binding;
    ucn_v6_frame_t frame;
    size_t encoded_length = 0U;
    uint64_t token = 0U;
    ucn_v6_result_t result;
    (void)local_principal;
    if (!runtime_is_valid(runtime) || ingress_frame == NULL ||
        payload == NULL || payload_length == 0U ||
        payload_length > UINT16_MAX || deadline_us == 0U ||
        queued_token == NULL ||
        link_id == 0U || link_id > UCN_V6_LINK_ID_MAX ||
        link_generation == 0U ||
        link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_bootstrap_tx_free(runtime);
    if (slot == NULL) return UCN_V6_ERR_NO_SPACE;
    memset(&frame, 0, sizeof(frame));
    frame.address_class = ingress_frame->address_class;
    frame.frame_type = UCN_V6_FRAME_BOOTSTRAP;
    frame.flags = UCN_V6_FLAG_PROTOCOL_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_V6_DELIVERY_BEST_EFFORT;
    frame.hop_limit = 1U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = ingress_frame->realm_id;
    frame.destination_address =
        ucn_v6_address_max_ordinary(frame.address_class) + 1U;
    if (runtime_bootstrap_source_is_reauth(ingress_frame)) {
        memset(&local_principal, 0, sizeof(local_principal));
        memset(&local_binding, 0, sizeof(local_binding));
        result = ucn_v6_security_copy_local_identity(
            runtime->config.security, &local_principal, &local_binding);
        if (result != UCN_V6_OK) return result;
        if (local_binding.realm_id != frame.realm_id) {
            return UCN_V6_ERR_SECURITY;
        }
        frame.source_address = local_binding.node_address;
        frame.source_binding_generation =
            local_binding.binding_generation;
    }
    frame.protocol_opcode = opcode;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;
    result = allocate_internal_buffer_token(runtime, &token);
    if (result != UCN_V6_OK) return result;
    memset(slot, 0, sizeof(*slot));
    result = ucn_v6_wire_encode(
        &frame, slot->frame, sizeof(slot->frame), &encoded_length);
    if (result != UCN_V6_OK) {
        memset(slot, 0, sizeof(*slot));
        return result;
    }
    if (encoded_length == 0U || encoded_length > UINT16_MAX) {
        memset(slot, 0, sizeof(*slot));
        return UCN_V6_ERR_STATE;
    }
    slot->occupied = true;
    slot->buffer_token = token;
    slot->link_id = link_id;
    slot->link_generation = link_generation;
    slot->protocol_opcode = opcode;
    slot->frame_length = (uint16_t)encoded_length;
    slot->deadline_us = deadline_us;
    *queued_token = token;
    return UCN_V6_OK;
}

static void rollback_bootstrap_tokens(
    ucn_v6_runtime_owner_t *runtime,
    const uint64_t tokens[UCN_V6_BOOTSTRAP_MAX_FRAGMENTS],
    uint8_t token_count)
{
    uint8_t index;
    for (index = 0U; index < token_count; ++index) {
        ucn_v6_runtime_bootstrap_tx_slot_t *slot =
            find_bootstrap_tx_token(runtime, tokens[index]);
        if (slot != NULL) memset(slot, 0, sizeof(*slot));
    }
}

static ucn_v6_result_t queue_bootstrap_event(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_frame_t *ingress_frame,
    uint16_t link_id,
    uint32_t link_generation,
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_phase_t phase,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_bootstrap_evidence_t *evidence,
    uint64_t deadline_us)
{
    ucn_v6_bootstrap_fragment_t fragment;
    uint8_t fragment_payload[UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES +
                             UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES];
    uint64_t queued_tokens[UCN_V6_BOOTSTRAP_MAX_FRAGMENTS] = {0};
    uint16_t opcode = ucn_v6_bootstrap_event_opcode(event);
    size_t logical_length = 0U;
    size_t fragment_payload_length = 0U;
    uint8_t fragment_count;
    uint8_t index;
    uint8_t queued_count = 0U;
    ucn_v6_result_t result;
    if (opcode == 0U || event == UCN_V6_BOOTSTRAP_EVENT_COOKIE ||
        transcript == NULL || evidence == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = ucn_v6_bootstrap_logical_encode(
        event, phase, transcript, evidence, runtime->tx_payload_work,
        sizeof(runtime->tx_payload_work), &logical_length);
    if (result != UCN_V6_OK) return result;
    fragment_count = (uint8_t)((logical_length +
                                UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES - 1U) /
                               UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES);
    if (fragment_count == 0U ||
        fragment_count > UCN_V6_BOOTSTRAP_MAX_FRAGMENTS ||
        bootstrap_tx_free_count(runtime) < fragment_count) {
        return UCN_V6_ERR_NO_SPACE;
    }
    for (index = 0U; index < fragment_count; ++index) {
        size_t offset = (size_t)index *
                        UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES;
        size_t remaining = logical_length - offset;
        size_t part = remaining > UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES ?
                          UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES : remaining;
        memset(&fragment, 0, sizeof(fragment));
        fragment.flow = transcript->flow;
        fragment.phase = phase;
        fragment.fragment_index = index;
        fragment.fragment_count = fragment_count;
        fragment.total_length = (uint16_t)logical_length;
        fragment.fragment_offset = (uint16_t)offset;
        fragment.fragment_length = (uint16_t)part;
        fragment.transaction_id = transcript->transaction_id;
        fragment.identity_digest =
            transcript->joining_device_identity_digest;
        memcpy(fragment.data, &runtime->tx_payload_work[offset], part);
        result = ucn_v6_bootstrap_fragment_encode(
            &fragment, fragment_payload, sizeof(fragment_payload),
            &fragment_payload_length);
        if (result == UCN_V6_OK) {
            result = queue_bootstrap_payload(
                runtime, ingress_frame, link_id, link_generation, opcode,
                fragment_payload, fragment_payload_length, deadline_us,
                &queued_tokens[queued_count]);
        }
        if (result != UCN_V6_OK) {
            rollback_bootstrap_tokens(runtime, queued_tokens, queued_count);
            return result;
        }
        ++queued_count;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t call_bootstrap_policy(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us)
{
    ucn_v6_result_t result;
    memset(&runtime->bootstrap_action_work, 0,
           sizeof(runtime->bootstrap_action_work));
    runtime->callback_active = true;
    result = runtime->config.bootstrap_ops.process_ingress(
        runtime->config.bootstrap_ops.context, now_us,
        &runtime->bootstrap_ingress_work,
        &runtime->bootstrap_action_work);
    runtime->callback_active = false;
    return result;
}

static ucn_v6_result_t commit_bootstrap_session(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    uint64_t now_us)
{
    ucn_v6_result_t result;
    memset(&runtime->bootstrap_commit_work, 0,
           sizeof(runtime->bootstrap_commit_work));
    runtime->callback_active = true;
    result = runtime->config.bootstrap_ops.build_join_commit(
        runtime->config.bootstrap_ops.context, now_us, key, transcript,
        &runtime->bootstrap_commit_work);
    runtime->callback_active = false;
    if (result != UCN_V6_OK) return result;
    result = ucn_v6_security_commit_join(
        runtime->config.security, runtime->config.bootstrap, key, now_us,
        &runtime->bootstrap_commit_work);
    if (result == UCN_V6_OK) {
        increment_saturated(&runtime->stats.bootstrap_sessions_committed);
    }
    return result;
}

static ucn_v6_result_t queue_bootstrap_cookie_challenge(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_frame_t *frame,
    uint64_t now_us)
{
    size_t length = 0U;
    uint64_t token = 0U;
    uint64_t deadline;
    ucn_v6_result_t result;
    if (now_us > UINT64_MAX -
                     UCN_V6_CONFIG_RUNTIME_BOOTSTRAP_TX_TIMEOUT_US) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    deadline = now_us + UCN_V6_CONFIG_RUNTIME_BOOTSTRAP_TX_TIMEOUT_US;
    result = ucn_v6_bootstrap_cookie_challenge_encode(
        &runtime->bootstrap_action_work.cookie_challenge,
        runtime->tx_payload_work);
    if (result != UCN_V6_OK) return result;
    length = UCN_V6_BOOTSTRAP_COOKIE_CHALLENGE_BYTES;
    return queue_bootstrap_payload(
        runtime, frame, runtime->active_rx.key.link_id,
        runtime->active_rx.key.link_generation,
        UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE,
        runtime->tx_payload_work, length, deadline, &token);
}

static ucn_v6_result_t queue_bootstrap_hello_cookie(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_frame_t *frame,
    const ucn_v6_bootstrap_pending_t *pending)
{
    size_t length = 0U;
    uint64_t token = 0U;
    ucn_v6_result_t result;
    result = ucn_v6_bootstrap_hello_cookie_encode(
        &runtime->bootstrap_action_work.hello_cookie,
        runtime->tx_payload_work, sizeof(runtime->tx_payload_work),
        &length);
    if (result != UCN_V6_OK) return result;
    return queue_bootstrap_payload(
        runtime, frame, runtime->active_rx.key.link_id,
        runtime->active_rx.key.link_generation,
        UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_HELLO_COOKIE,
        runtime->tx_payload_work, length, pending->deadline_us, &token);
}

static ucn_v6_result_t bootstrap_open_from_action(
    ucn_v6_runtime_owner_t *runtime,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_evidence_t *cookie,
    uint64_t now_us,
    ucn_v6_bootstrap_pending_t *pending)
{
    const ucn_v6_binding_key_t *existing = NULL;
    ucn_v6_result_t result;
    if (!runtime->bootstrap_action_work.open_pending ||
        pending == NULL) {
        return UCN_V6_ERR_STATE;
    }
    if (flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH) {
        if (!runtime->bootstrap_action_work.has_existing_binding) {
            return UCN_V6_ERR_STATE;
        }
        existing = &runtime->bootstrap_action_work.existing_binding;
    } else if (runtime->bootstrap_action_work.has_existing_binding) {
        return UCN_V6_ERR_STATE;
    }
    result = ucn_v6_bootstrap_open_after_cookie(
        runtime->config.bootstrap, flow, key,
        &runtime->bootstrap_action_work.open_transcript, existing,
        cookie, now_us);
    if (result != UCN_V6_OK) return result;
    memset(pending, 0, sizeof(*pending));
    return ucn_v6_bootstrap_copy_pending(
        runtime->config.bootstrap, flow, key, pending);
}

static ucn_v6_result_t apply_bootstrap_event_action(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_frame_t *frame,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_pending_t *pending,
    uint64_t now_us)
{
    ucn_v6_bootstrap_event_t expected =
        runtime_bootstrap_expected_response(
            &runtime->bootstrap_ingress_work);
    ucn_v6_bootstrap_phase_t response_phase = UCN_V6_BOOTSTRAP_EMPTY;
    bool final_incoming =
        runtime->bootstrap_ingress_work.kind ==
            UCN_V6_RUNTIME_BOOTSTRAP_EVENT &&
        runtime->bootstrap_ingress_work.event ==
            UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE;
    bool final_outgoing = expected == UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE;
    const ucn_v6_bootstrap_transcript_t *final_transcript = NULL;
    ucn_v6_result_t result;

    if (runtime->bootstrap_action_work.open_pending ||
        (expected == (ucn_v6_bootstrap_event_t)0 &&
         runtime->bootstrap_action_work.response_kind !=
             UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_NONE) ||
        (expected != (ucn_v6_bootstrap_event_t)0 &&
         (runtime->bootstrap_action_work.response_kind !=
              UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_EVENT ||
          runtime->bootstrap_action_work.response_event != expected)) ||
        runtime->bootstrap_action_work.commit_session !=
            (final_incoming || final_outgoing)) {
        return UCN_V6_ERR_STATE;
    }
    if (runtime->bootstrap_ingress_work.kind ==
        UCN_V6_RUNTIME_BOOTSTRAP_EVENT) {
        result = ucn_v6_bootstrap_advance(
            runtime->config.bootstrap, pending->flow, key,
            &runtime->bootstrap_ingress_work.value.authenticated_event
                 .transcript,
            runtime->bootstrap_ingress_work.event,
            &runtime->bootstrap_ingress_work.value.authenticated_event
                 .evidence,
            now_us);
        if (result != UCN_V6_OK) return result;
    }
    if (expected != (ucn_v6_bootstrap_event_t)0) {
        response_phase = runtime_bootstrap_event_phase(
            expected, pending->flow, pending->phase);
        if (response_phase == UCN_V6_BOOTSTRAP_EMPTY) {
            return UCN_V6_ERR_STATE;
        }
        result = ucn_v6_bootstrap_advance(
            runtime->config.bootstrap, pending->flow, key,
            &runtime->bootstrap_action_work.response_transcript,
            expected, &runtime->bootstrap_action_work.response_evidence,
            now_us);
        if (result != UCN_V6_OK) return result;
    }
    if (final_incoming) {
        final_transcript =
            &runtime->bootstrap_ingress_work.value.authenticated_event
                 .transcript;
    } else if (final_outgoing) {
        final_transcript =
            &runtime->bootstrap_action_work.response_transcript;
    }
    if (final_transcript != NULL) {
        result = commit_bootstrap_session(runtime, key, final_transcript,
                                          now_us);
        if (result != UCN_V6_OK) return result;
    }
    if (expected != (ucn_v6_bootstrap_event_t)0) {
        result = queue_bootstrap_event(
            runtime, frame, runtime->active_rx.key.link_id,
            runtime->active_rx.key.link_generation, expected,
            response_phase,
            &runtime->bootstrap_action_work.response_transcript,
            &runtime->bootstrap_action_work.response_evidence,
            pending->deadline_us);
        if (result != UCN_V6_OK) return result;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t handle_bootstrap_ingress(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_frame_t *frame,
    uint64_t now_us,
    bool *consumed)
{
    ucn_v6_runtime_bootstrap_ingress_t *ingress =
        &runtime->bootstrap_ingress_work;
    ucn_v6_runtime_bootstrap_action_t *action =
        &runtime->bootstrap_action_work;
    ucn_v6_bootstrap_pending_t pending;
    ucn_v6_bootstrap_fragment_t fragment;
    ucn_v6_bootstrap_reassembly_t *reassembly;
    const uint8_t *logical = NULL;
    size_t logical_length = 0U;
    bool complete = false;
    ucn_v6_result_t result;

    if (runtime == NULL || frame == NULL || consumed == NULL ||
        frame->frame_type != UCN_V6_FRAME_BOOTSTRAP) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *consumed = false;
    memset(ingress, 0, sizeof(*ingress));
    ingress->address_class = frame->address_class;
    ingress->realm_id = frame->realm_id;
    if (runtime_bootstrap_source_is_reauth(frame)) {
        ingress->source_binding.realm_id = frame->realm_id;
        ingress->source_binding.node_address = frame->source_address;
        ingress->source_binding.binding_generation =
            frame->source_binding_generation;
    }

    if (frame->protocol_opcode == UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_HELLO) {
        ingress->kind = UCN_V6_RUNTIME_BOOTSTRAP_HELLO;
        result = ucn_v6_bootstrap_hello_decode(
            frame->payload, frame->payload_length, &ingress->value.hello);
        if (result != UCN_V6_OK) return result;
        result = ucn_v6_bootstrap_admit_initial_hello(
            runtime->config.bootstrap, runtime->active_rx.key.link_id,
            runtime->active_rx.key.link_generation, now_us,
            runtime->active_rx.frame_length,
            runtime->active_rx.frame_length);
        if (result != UCN_V6_OK) return result;
        result = call_bootstrap_policy(runtime, now_us);
        if (result != UCN_V6_OK) return result;
        if (action->open_pending || action->commit_session ||
            action->response_kind !=
                UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_COOKIE_CHALLENGE ||
            action->cookie_challenge.flow != ingress->value.hello.flow ||
            action->cookie_challenge.transaction_id !=
                ingress->value.hello.transaction_id) {
            return UCN_V6_ERR_STATE;
        }
        result = queue_bootstrap_cookie_challenge(runtime, frame, now_us);
    } else if (frame->protocol_opcode ==
               UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE) {
        ucn_v6_runtime_bootstrap_initiation_t *initiation;
        ingress->kind = UCN_V6_RUNTIME_BOOTSTRAP_COOKIE_CHALLENGE;
        result = ucn_v6_bootstrap_cookie_challenge_decode(
            frame->payload, frame->payload_length,
            &ingress->value.cookie_challenge);
        if (result != UCN_V6_OK) return result;
        initiation = find_bootstrap_initiation(
            runtime, runtime->active_rx.key.link_id,
            runtime->active_rx.key.link_generation,
            runtime->active_rx.local_peer_discriminator,
            ingress->value.cookie_challenge.flow,
            ingress->value.cookie_challenge.transaction_id);
        if (initiation == NULL || now_us >= initiation->deadline_us ||
            initiation->realm_id != frame->realm_id ||
            initiation->address_class != frame->address_class) {
            return UCN_V6_ERR_REPLAY;
        }
        result = call_bootstrap_policy(runtime, now_us);
        if (result != UCN_V6_OK) return result;
        if (!action->open_pending || action->commit_session ||
            action->response_kind !=
                UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_HELLO_COOKIE ||
            action->hello_cookie.flow !=
                ingress->value.cookie_challenge.flow ||
            action->hello_cookie.transaction_id !=
                ingress->value.cookie_challenge.transaction_id ||
            action->hello_cookie.device_nonce !=
                initiation->hello.device_nonce ||
            !runtime_principal_equal(
                &action->hello_cookie.identity_digest,
                &initiation->hello.identity_digest) ||
            action->open_transcript.flow != initiation->hello.flow ||
            action->open_transcript.device_nonce !=
                initiation->hello.device_nonce ||
            action->open_transcript.transaction_id !=
                initiation->hello.transaction_id ||
            !runtime_principal_equal(
                &action->open_transcript.joining_device_identity_digest,
                &initiation->hello.identity_digest)) {
            return UCN_V6_ERR_STATE;
        }
        memset(&ingress->key, 0, sizeof(ingress->key));
        ingress->key.ingress_link_id = runtime->active_rx.key.link_id;
        ingress->key.ingress_link_generation =
            runtime->active_rx.key.link_generation;
        ingress->key.local_peer_discriminator =
            runtime->active_rx.local_peer_discriminator;
        ingress->key.identity_digest = action->hello_cookie.identity_digest;
        ingress->key.transaction_id = action->hello_cookie.transaction_id;
        memset(&pending, 0, sizeof(pending));
        result = bootstrap_open_from_action(
            runtime, action->hello_cookie.flow, &ingress->key,
            &action->hello_cookie.cookie_evidence, now_us, &pending);
        if (result == UCN_V6_OK) {
            result = queue_bootstrap_hello_cookie(runtime, frame, &pending);
        }
    } else if (frame->protocol_opcode ==
               UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_HELLO_COOKIE) {
        ingress->kind = UCN_V6_RUNTIME_BOOTSTRAP_HELLO_COOKIE;
        result = ucn_v6_bootstrap_hello_cookie_decode(
            frame->payload, frame->payload_length,
            &ingress->value.hello_cookie);
        if (result != UCN_V6_OK) return result;
        ingress->key.ingress_link_id = runtime->active_rx.key.link_id;
        ingress->key.ingress_link_generation =
            runtime->active_rx.key.link_generation;
        ingress->key.local_peer_discriminator =
            runtime->active_rx.local_peer_discriminator;
        ingress->key.identity_digest =
            ingress->value.hello_cookie.identity_digest;
        ingress->key.transaction_id =
            ingress->value.hello_cookie.transaction_id;
        result = call_bootstrap_policy(runtime, now_us);
        if (result != UCN_V6_OK) return result;
        if (!action->open_pending || action->commit_session ||
            action->response_kind !=
                UCN_V6_RUNTIME_BOOTSTRAP_RESPONSE_EVENT ||
            action->response_event !=
                UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF) {
            return UCN_V6_ERR_STATE;
        }
        memset(&pending, 0, sizeof(pending));
        result = bootstrap_open_from_action(
            runtime, ingress->value.hello_cookie.flow, &ingress->key,
            &ingress->value.hello_cookie.cookie_evidence,
            now_us, &pending);
        if (result == UCN_V6_OK) {
            action->open_pending = false;
            ingress->pending = pending;
            result = apply_bootstrap_event_action(
                runtime, frame, &ingress->key, &pending, now_us);
        }
    } else {
        ingress->kind = UCN_V6_RUNTIME_BOOTSTRAP_EVENT;
        ingress->event = runtime_bootstrap_event_from_opcode(
            frame->protocol_opcode);
        if (ingress->event == (ucn_v6_bootstrap_event_t)0) {
            return UCN_V6_ERR_MALFORMED;
        }
        memset(&fragment, 0, sizeof(fragment));
        result = ucn_v6_bootstrap_fragment_decode(
            frame->payload, frame->payload_length, &fragment);
        if (result != UCN_V6_OK) return result;
        ingress->key.ingress_link_id = runtime->active_rx.key.link_id;
        ingress->key.ingress_link_generation =
            runtime->active_rx.key.link_generation;
        ingress->key.local_peer_discriminator =
            runtime->active_rx.local_peer_discriminator;
        ingress->key.identity_digest = fragment.identity_digest;
        ingress->key.transaction_id = fragment.transaction_id;
        memset(&pending, 0, sizeof(pending));
        result = ucn_v6_bootstrap_copy_pending(
            runtime->config.bootstrap, fragment.flow, &ingress->key,
            &pending);
        if (result != UCN_V6_OK) return result;
        reassembly = find_bootstrap_reassembly(
            runtime, frame->protocol_opcode, &ingress->key,
            fragment.fragment_index == 0U);
        if (reassembly == NULL) return UCN_V6_ERR_NO_SPACE;
        result = ucn_v6_bootstrap_reassembly_accept(
            reassembly, frame->protocol_opcode, &ingress->key,
            pending.deadline_us, now_us, &fragment, &complete);
        if (result != UCN_V6_OK) return result;
        if (!complete) {
            *consumed = true;
            increment_saturated(&runtime->stats.bootstrap_frames_consumed);
            return UCN_V6_OK;
        }
        result = ucn_v6_bootstrap_reassembly_borrow(
            reassembly, &logical, &logical_length);
        if (result != UCN_V6_OK) return result;
        result = ucn_v6_bootstrap_logical_decode(
            frame->protocol_opcode, fragment.phase, logical,
            logical_length,
            &ingress->value.authenticated_event.transcript,
            &ingress->value.authenticated_event.evidence);
        if (result != UCN_V6_OK) return result;
        ingress->pending = pending;
        result = call_bootstrap_policy(runtime, now_us);
        if (result != UCN_V6_OK) return result;
        result = apply_bootstrap_event_action(
            runtime, frame, &ingress->key, &pending, now_us);
        if (result == UCN_V6_OK) {
            result = ucn_v6_bootstrap_reassembly_reset(reassembly);
        }
    }
    if (result == UCN_V6_OK) {
        *consumed = true;
        increment_saturated(&runtime->stats.bootstrap_frames_consumed);
    }
    return result;
}

static ucn_v6_result_t phase_security(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_frame_t preview;
    ucn_v6_principal_t ingress_peer;
    bool targets_local = false;
    bool bootstrap_consumed = false;
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    if (runtime->pending_source ==
        UCN_V6_RUNTIME_INVALIDATION_ADAPTER) {
        return invalidation_phase(runtime,
                                  UCN_V6_RUNTIME_INVALIDATION_ADAPTER,
                                  result);
    }
    if (runtime->pending_source == UCN_V6_RUNTIME_INVALIDATION_SECURITY) {
        return invalidation_phase(runtime,
                                  UCN_V6_RUNTIME_INVALIDATION_SECURITY,
                                  result);
    }
    call_result = invalidation_phase(
        runtime, UCN_V6_RUNTIME_INVALIDATION_SECURITY, result);
    if (call_result != UCN_V6_OK || result->has_invalidation) {
        return call_result;
    }
    if (!runtime->ingress_active ||
        runtime->rx_phase != UCN_V6_RUNTIME_RX_RAW) {
        return UCN_V6_OK;
    }
    memset(&preview, 0, sizeof(preview));
    call_result = ucn_v6_wire_decode(runtime->rx_frame,
                                     runtime->active_rx.frame_length,
                                     &preview);
    if (call_result == UCN_V6_OK &&
        preview.frame_type == UCN_V6_FRAME_BOOTSTRAP) {
        call_result = handle_bootstrap_ingress(
            runtime, &preview, now_us, &bootstrap_consumed);
        if (call_result == UCN_V6_OK && bootstrap_consumed) {
            runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
            runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        }
    }
    if (call_result == UCN_V6_OK && !bootstrap_consumed) {
        call_result = ucn_v6_security_frame_targets_local(
            runtime->config.security, &preview, &targets_local);
    }
    if (call_result == UCN_V6_OK && bootstrap_consumed) {
        /* Already owned by the pre-session Bootstrap path. */
    } else if (call_result == UCN_V6_OK && targets_local) {
        call_result = ucn_v6_security_open_ingress_frame(
            runtime->config.security, now_us,
            runtime->active_rx.key.link_id,
            runtime->active_rx.key.link_generation, runtime->rx_frame,
            runtime->active_rx.frame_length, runtime->rx_plaintext,
            sizeof(runtime->rx_plaintext), &runtime->opened_rx);
        if (call_result == UCN_V6_OK) {
            runtime->rx_phase = UCN_V6_RUNTIME_RX_OPENED;
        }
    } else if (call_result == UCN_V6_OK) {
        memset(&ingress_peer, 0, sizeof(ingress_peer));
        call_result = ucn_v6_security_resolve_ingress_peer(
            runtime->config.security, now_us,
            runtime->active_rx.key.link_id,
            runtime->active_rx.key.link_generation, &preview,
            &ingress_peer);
        if (call_result == UCN_V6_OK) {
            call_result = ucn_v6_security_open_relay_ingress(
                runtime->config.security, now_us,
                runtime->active_rx.key.link_id,
                runtime->active_rx.key.link_generation, &ingress_peer,
                runtime->rx_frame, runtime->active_rx.frame_length,
                &runtime->opened_rx);
        }
        if (call_result == UCN_V6_OK) {
            runtime->rx_phase = UCN_V6_RUNTIME_RX_RELAY;
        }
    }
    if (call_result != UCN_V6_OK && packet_local_failure(call_result)) {
        /* Untrusted input and bounded admission failures are packet-local.
         * They are retired without ever reaching product code. */
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_DROP;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
    } else if (call_result != UCN_V6_OK) {
        runtime->stats.faulted = true;
        return call_result;
    }
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t phase_route_authority(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_runtime_tx_slot_t *slot;
    ucn_v6_runtime_tx_slot_t *replaced_slot;
    ucn_v6_qos_enqueue_result_t enqueue = {0};
    uint64_t buffer_token = 0U;
    size_t encoded_length = 0U;
    ucn_v6_result_t call_result;

    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    if (!runtime->ingress_active ||
        runtime->rx_phase != UCN_V6_RUNTIME_RX_RELAY) {
        return UCN_V6_OK;
    }
    slot = find_free_tx_slot(runtime);
    if (slot == NULL ||
        (runtime->opened_rx.frame.delivery_guarantee ==
             UCN_V6_DELIVERY_LATEST &&
         free_release_slots(runtime) == 0U)) {
        return retry_at_next_tick(now_us, result);
    }
    call_result = ucn_v6_wire_encoded_size(
        &runtime->opened_rx.frame, &encoded_length);
    if (call_result == UCN_V6_OK &&
        (encoded_length == 0U ||
         encoded_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES ||
         encoded_length > UINT16_MAX ||
         runtime->opened_rx.frame.payload_length >
             sizeof(slot->payload))) {
        call_result = UCN_V6_ERR_NO_SPACE;
    }
    if (call_result == UCN_V6_OK) {
        call_result = allocate_internal_buffer_token(runtime, &buffer_token);
    }
    if (call_result == UCN_V6_OK) {
        memset(&enqueue, 0, sizeof(enqueue));
        call_result = ucn_v6_qos_enqueue(
            runtime->config.qos, now_us, &runtime->opened_rx, buffer_token,
            (uint16_t)encoded_length, 0U, &enqueue);
    }
    if (call_result == UCN_V6_OK && !enqueue.accepted) {
        call_result = UCN_V6_ERR_STATE;
    }
    if (call_result == UCN_V6_OK && enqueue.replaced_latest) {
        replaced_slot = find_tx_slot(runtime, enqueue.replaced_buffer_token);
        if (replaced_slot == NULL ||
            (replaced_slot->release_to_app &&
             !queue_release(runtime, enqueue.replaced_buffer_token,
                            UCN_V6_ERR_EXHAUSTED, NULL))) {
            runtime->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        memset(replaced_slot, 0, sizeof(*replaced_slot));
    }
    if (call_result == UCN_V6_OK) {
        memset(slot, 0, sizeof(*slot));
        slot->occupied = true;
        slot->release_to_app = false;
        slot->relay = true;
        slot->buffer_token = buffer_token;
        slot->semantic.relay_opened = runtime->opened_rx;
        slot->payload_length = runtime->opened_rx.frame.payload_length;
        if (slot->payload_length != 0U) {
            memcpy(slot->payload, runtime->opened_rx.frame.payload,
                   slot->payload_length);
        }
        slot->semantic.relay_opened.frame.payload = slot->payload;
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        increment_saturated(&runtime->stats.rx_relayed);
    } else if (packet_local_failure(call_result)) {
        /* Authentication already consumed the previous-Hop replay sequence.
         * A forwarding admission failure therefore drops this packet once;
         * it must never re-open or duplicate-forward the same raw record. */
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_DROP;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
    } else {
        runtime->stats.faulted = true;
        return call_result;
    }
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t phase_endpoint(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_runtime_ingress_disposition_t disposition =
        (ucn_v6_runtime_ingress_disposition_t)0;
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    if (!runtime->ingress_active) {
        return UCN_V6_OK;
    }
    if (runtime->rx_phase == UCN_V6_RUNTIME_RX_TRANSFER_DELIVERY) {
        ucn_v6_transfer_completed_view_t transfer;
        if (!runtime->transfer_delivery.pending) {
            runtime->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        memset(&transfer, 0, sizeof(transfer));
        call_result = ucn_v6_transfer_borrow_completed(
            runtime->config.transfer,
            &runtime->transfer_delivery.origin,
            runtime->transfer_delivery.operation_id,
            runtime->transfer_delivery.message_id, &transfer);
        if (call_result != UCN_V6_OK) {
            runtime->stats.faulted = true;
            return call_result;
        }
        if (runtime->config.app.handle_reassembled_transfer == NULL) {
            disposition = UCN_V6_RUNTIME_INGRESS_DROP;
            call_result = UCN_V6_OK;
        } else {
            runtime->callback_active = true;
            call_result =
                runtime->config.app.handle_reassembled_transfer(
                    runtime->config.app.context, runtime, now_us,
                    &transfer, &disposition);
            runtime->callback_active = false;
        }
        if (call_result != UCN_V6_OK ||
            (disposition != UCN_V6_RUNTIME_INGRESS_CONSUMED &&
             disposition != UCN_V6_RUNTIME_INGRESS_DROP &&
             disposition != UCN_V6_RUNTIME_INGRESS_RETRY)) {
            runtime->stats.faulted = true;
            return call_result == UCN_V6_OK ?
                       UCN_V6_ERR_STATE : call_result;
        }
        if (disposition == UCN_V6_RUNTIME_INGRESS_RETRY) {
            increment_saturated(&runtime->stats.rx_retried);
            return retry_at_next_tick(now_us, result);
        }
        call_result = ucn_v6_transfer_retire_completed(
            runtime->config.transfer, now_us,
            &runtime->transfer_delivery.origin,
            runtime->transfer_delivery.operation_id,
            runtime->transfer_delivery.message_id);
        if (call_result != UCN_V6_OK) {
            runtime->stats.faulted = true;
            return call_result;
        }
        memset(&runtime->transfer_delivery, 0,
               sizeof(runtime->transfer_delivery));
        runtime->rx_disposition = disposition;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        if (disposition == UCN_V6_RUNTIME_INGRESS_CONSUMED) {
            increment_saturated(&runtime->stats.transfer_messages_delivered);
        }
        result->work_done = 1U;
        result->has_more = true;
        return UCN_V6_OK;
    }
    if (runtime->rx_phase == UCN_V6_RUNTIME_RX_TRANSFER_RESULT) {
        if (runtime->config.app.handle_transfer_result == NULL) {
            disposition = UCN_V6_RUNTIME_INGRESS_DROP;
            call_result = UCN_V6_OK;
        } else {
            runtime->callback_active = true;
            call_result = runtime->config.app.handle_transfer_result(
                runtime->config.app.context, runtime, now_us,
                &runtime->transfer_result, &disposition);
            runtime->callback_active = false;
        }
        if (call_result != UCN_V6_OK ||
            (disposition != UCN_V6_RUNTIME_INGRESS_CONSUMED &&
             disposition != UCN_V6_RUNTIME_INGRESS_DROP &&
             disposition != UCN_V6_RUNTIME_INGRESS_RETRY)) {
            runtime->stats.faulted = true;
            return call_result == UCN_V6_OK ?
                       UCN_V6_ERR_STATE : call_result;
        }
        if (disposition == UCN_V6_RUNTIME_INGRESS_RETRY) {
            increment_saturated(&runtime->stats.rx_retried);
            return retry_at_next_tick(now_us, result);
        }
        memset(&runtime->transfer_result, 0,
               sizeof(runtime->transfer_result));
        runtime->rx_disposition = disposition;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        result->work_done = 1U;
        result->has_more = true;
        return UCN_V6_OK;
    }
    if (runtime->rx_phase != UCN_V6_RUNTIME_RX_OPENED) {
        return UCN_V6_OK;
    }
    /* Core CONTROL/TRANSFER messages are never delegated to a generic
     * product callback. Earlier/later protocol phases own them. This keeps a
     * missing protocol hook fail-closed instead of silently expanding the
     * application trust boundary.
     * Core CONTROL/TRANSFER 消息绝不委托给通用产品回调；它们由前后专用
     * 协议阶段持有。即便某协议 Hook 尚未闭合，也必须失败关闭，不能静默
     * 扩大应用层可信边界。 */
    if (runtime->opened_rx.frame.frame_type != UCN_V6_FRAME_DATA &&
        runtime->opened_rx.frame.frame_type != UCN_V6_FRAME_DIAGNOSTIC) {
        return UCN_V6_OK;
    }
    runtime->callback_active = true;
    call_result = runtime->config.app.handle_authenticated_ingress(
        runtime->config.app.context, runtime, now_us, &runtime->opened_rx,
        &runtime->active_rx, &disposition);
    runtime->callback_active = false;
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
    runtime->rx_disposition = disposition;
    runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t retire_completed_ingress(
    ucn_v6_runtime_owner_t *runtime,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_result_t retire_result;
    if (!runtime->ingress_active ||
        runtime->rx_phase != UCN_V6_RUNTIME_RX_COMPLETE) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    retire_result = ucn_v6_adapter_retire_rx(
        runtime->config.adapter, &runtime->active_rx.key);
    if (retire_result != UCN_V6_OK) {
        runtime->stats.faulted = true;
        return retire_result;
    }
    if (runtime->rx_disposition == UCN_V6_RUNTIME_INGRESS_CONSUMED) {
        increment_saturated(&runtime->stats.rx_consumed);
    } else {
        increment_saturated(&runtime->stats.rx_dropped);
    }
    runtime->ingress_active = false;
    runtime->rx_phase = UCN_V6_RUNTIME_RX_IDLE;
    runtime->rx_disposition =
        (ucn_v6_runtime_ingress_disposition_t)0;
    memset(&runtime->active_rx, 0, sizeof(runtime->active_rx));
    memset(&runtime->opened_rx, 0, sizeof(runtime->opened_rx));
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t phase_capability(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_capability_summary_t summary;
    ucn_v6_capability_record_t record = {0};
    ucn_v6_capability_query_t query;
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    ucn_v6_hello_disposition_t disposition =
        (ucn_v6_hello_disposition_t)0;
    ucn_v6_result_t call_result;
    bool response_required = false;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    call_result = invalidation_phase(
        runtime, UCN_V6_RUNTIME_INVALIDATION_CAPABILITY, result);
    if (call_result != UCN_V6_OK || result->has_invalidation) {
        return call_result;
    }
    if (!runtime->ingress_active ||
        runtime->rx_phase != UCN_V6_RUNTIME_RX_OPENED ||
        runtime->opened_rx.frame.frame_type != UCN_V6_FRAME_CONTROL) {
        return UCN_V6_OK;
    }
    switch (runtime->opened_rx.frame.protocol_opcode) {
    case UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO:
        call_result = ucn_v6_capability_ingest_group_hello_hint(
            runtime->config.capability, now_us, &runtime->opened_rx);
        break;
    case UCN_V6_PROTOCOL_OPCODE_PEER_HELLO:
        memset(&summary, 0, sizeof(summary));
        call_result = ucn_v6_capability_summary_decode(
            runtime->opened_rx.frame.payload,
            runtime->opened_rx.frame.payload_length, &summary);
        if (call_result == UCN_V6_OK) {
            call_result = ucn_v6_capability_ingest_peer_hello(
                runtime->config.capability, now_us, &runtime->opened_rx,
                &summary, &disposition);
        }
        if (call_result == UCN_V6_OK &&
            disposition != UCN_V6_HELLO_MATCHED &&
            disposition != UCN_V6_HELLO_QUERY_REQUIRED) {
            call_result = UCN_V6_ERR_STATE;
        }
        if (call_result == UCN_V6_OK &&
            disposition == UCN_V6_HELLO_QUERY_REQUIRED) {
            response_required = true;
            memset(&query, 0, sizeof(query));
            query.requested_generation = summary.capability_generation;
            memcpy(query.known_digest, summary.digest,
                   sizeof(query.known_digest));
            call_result = ucn_v6_capability_query_encode(&query, payload);
            if (call_result == UCN_V6_OK) {
                call_result = enqueue_peer_discovery_reply(
                    runtime, now_us, &runtime->opened_rx,
                    UCN_V6_PROTOCOL_OPCODE_CAPABILITY_QUERY,
                    UCN_V6_TRAFFIC_Q1, payload,
                    UCN_V6_CAPABILITY_QUERY_BYTES);
            }
            if (call_result == UCN_V6_OK) {
                increment_saturated(&runtime->stats.capability_queries_sent);
            }
        }
        break;
    case UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE:
        memset(&record, 0, sizeof(record));
        call_result = ucn_v6_capability_record_decode(
            runtime->opened_rx.frame.payload,
            runtime->opened_rx.frame.payload_length, &record);
        if (call_result == UCN_V6_OK) {
            call_result = ucn_v6_capability_ingest_advertise(
                runtime->config.capability, now_us, &runtime->opened_rx,
                &record);
        }
        break;
    case UCN_V6_PROTOCOL_OPCODE_CAPABILITY_QUERY:
        /* QUERY is a Core protocol message. Decode it here so malformed
         * input cannot reach product code. The bounded reply producer is
         * connected by the capability TX work item below.
         * QUERY 属于 Core 协议消息；先在此解码，禁止畸形输入进入产品层。
         * 有界响应由后续 Capability TX work item 产生。 */
        memset(&query, 0, sizeof(query));
        call_result = ucn_v6_capability_query_decode(
            runtime->opened_rx.frame.payload,
            runtime->opened_rx.frame.payload_length, &query);
        if (call_result == UCN_V6_OK) {
            call_result = ucn_v6_capability_copy_local(
                runtime->config.capability, &record, digest);
        }
        if (call_result == UCN_V6_OK &&
            query.requested_generation != 0U &&
            (query.requested_generation != record.capability_generation ||
             memcmp(query.known_digest, digest, sizeof(digest)) != 0)) {
            call_result = UCN_V6_ERR_REPLAY;
        }
        if (call_result == UCN_V6_OK) {
            call_result = ucn_v6_capability_record_encode(&record, payload);
        }
        if (call_result == UCN_V6_OK) {
            response_required = true;
            call_result = enqueue_peer_discovery_reply(
                runtime, now_us, &runtime->opened_rx,
                UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
                UCN_V6_TRAFFIC_Q2, payload,
                UCN_V6_CAPABILITY_RECORD_BYTES);
        }
        if (call_result == UCN_V6_OK) {
            increment_saturated(
                &runtime->stats.capability_advertisements_sent);
        }
        break;
    default:
        return UCN_V6_OK;
    }
    if (call_result == UCN_V6_ERR_STATE ||
        (response_required && call_result == UCN_V6_ERR_NO_SPACE)) {
        increment_saturated(&runtime->stats.rx_retried);
        return retry_at_next_tick(now_us, result);
    }
    if (call_result == UCN_V6_OK) {
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        increment_saturated(&runtime->stats.capability_frames_consumed);
    } else if (packet_local_failure(call_result)) {
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_DROP;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        runtime->stats.last_protocol_error = call_result;
        increment_saturated(&runtime->stats.protocol_frames_rejected);
    } else {
        runtime->stats.faulted = true;
        return call_result;
    }
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_session_key_t transfer_origin_from_opened(
    const ucn_v6_security_open_result_t *opened)
{
    ucn_v6_session_key_t origin;
    memset(&origin, 0, sizeof(origin));
    origin.principal = opened->authenticated_principal;
    origin.binding.realm_id = opened->frame.realm_id;
    origin.binding.node_address = opened->frame.source_address;
    origin.binding.binding_generation =
        opened->frame.source_binding_generation;
    origin.session_generation = opened->frame.session_generation;
    return origin;
}

static ucn_v6_result_t enqueue_transfer_payload(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_route_path_ref_t *route_ref,
    const ucn_v6_message_descriptor_t *message,
    uint16_t protocol_opcode, const uint8_t *payload,
    size_t payload_length, bool has_hop_budget,
    uint64_t initial_hop_budget_us, uint64_t remaining_hop_budget_us,
    uint8_t local_priority, bool transfer_fragment,
    uint64_t transfer_message_id, uint16_t transfer_fragment_index)
{
    ucn_v6_route_resolution_t resolution;
    ucn_v6_runtime_send_request_t request;
    ucn_v6_runtime_send_result_t send_result;
    ucn_v6_runtime_tx_slot_t *slot;
    ucn_v6_address_class_t address_class;
    uint64_t buffer_token;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || route_ref == NULL || message == NULL ||
        payload == NULL || payload_length == 0U || payload_length > UINT16_MAX ||
        local_priority > 7U || message->operation_id == 0U ||
        (has_hop_budget &&
         (initial_hop_budget_us == 0U || remaining_hop_budget_us == 0U ||
          remaining_hop_budget_us > initial_hop_budget_us))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&resolution, 0, sizeof(resolution));
    result = ucn_v6_route_resolve_ref(
        runtime->config.route, now_us, route_ref, &resolution);
    if (result != UCN_V6_OK) return result;
    address_class = address_class_for_route(route_ref);
    if ((uint32_t)address_class > (uint32_t)UCN_V6_ADDRESS_CLASS_A3 ||
        resolution.path.hop_count == 0U ||
        resolution.path.hop_count > UCN_V6_HOP_COUNT_MAX) {
        return UCN_V6_ERR_STATE;
    }
    result = allocate_internal_buffer_token(runtime, &buffer_token);
    if (result != UCN_V6_OK) return result;
    memset(&request, 0, sizeof(request));
    request.frame.address_class = address_class;
    request.frame.frame_type = UCN_V6_FRAME_TRANSFER;
    request.frame.flags =
        UCN_V6_FLAG_PEER_HOP_CONTEXT | UCN_V6_FLAG_E2E_CONTEXT |
        UCN_V6_FLAG_PROTOCOL_CONTEXT | UCN_V6_FLAG_MESSAGE_CONTEXT;
    if (has_hop_budget) {
        request.frame.flags = (uint8_t)(
            request.frame.flags | UCN_V6_FLAG_HOP_BUDGET_CONTEXT);
        request.frame.hop_budget.initial_budget_us = initial_hop_budget_us;
        request.frame.hop_budget.remaining_budget_us =
            remaining_hop_budget_us;
    }
    request.frame.traffic_class = message->traffic_class;
    request.frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    request.frame.hop_limit = resolution.path.hop_count;
    request.frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    request.frame.realm_id = route_ref->domain.origin_binding.realm_id;
    request.frame.source_address =
        route_ref->domain.origin_binding.node_address;
    request.frame.destination_address =
        route_ref->domain.destination_binding.node_address;
    request.frame.source_binding_generation =
        route_ref->domain.origin_binding.binding_generation;
    request.frame.destination_binding_generation =
        route_ref->domain.destination_binding.binding_generation;
    request.frame.session_generation =
        route_ref->domain.origin_session_generation;
    request.frame.protocol_opcode = protocol_opcode;
    request.frame.message.source_endpoint = message->source_endpoint;
    request.frame.message.destination_endpoint = message->destination_endpoint;
    request.frame.message.interaction_role = message->interaction_role;
    request.frame.message.operation_id = message->operation_id;
    request.frame.payload = payload;
    request.frame.payload_length = (uint16_t)payload_length;
    request.route.domain = route_ref->domain;
    request.route.flow_id = message->operation_id;
    request.route.packet_sequence = transfer_fragment ?
        (uint64_t)transfer_fragment_index + 1U :
        message->operation_id;
    request.route.policy = UCN_V6_ROUTE_POLICY_PINNED;
    request.route.pinned_path_id = route_ref->path_id;
    request.route.pinned_path_generation = route_ref->path_generation;
    request.route.allow_reordering = false;
    request.buffer_token = buffer_token;
    request.local_priority = local_priority;
    request.request_timestamp = false;
    memset(&send_result, 0, sizeof(send_result));
    result = runtime_enqueue_exact_frame(
        runtime, now_us, &request, false, route_ref, &send_result);
    if (result != UCN_V6_OK) return result;
    slot = find_tx_slot(runtime, buffer_token);
    if (slot == NULL) {
        runtime->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    slot->transfer_fragment = transfer_fragment;
    slot->transfer_message_id = transfer_message_id;
    slot->transfer_fragment_index = transfer_fragment_index;
    if (!transfer_fragment) {
        increment_saturated(&runtime->stats.transfer_control_frames_sent);
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t queue_transfer_sack(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_transfer_rx_result_t *receive)
{
    ucn_v6_route_path_ref_t reverse;
    ucn_v6_message_descriptor_t message;
    uint8_t payload[UCN_V6_TRANSFER_SACK_BYTES];
    ucn_v6_result_t result;
    memset(&reverse, 0, sizeof(reverse));
    result = select_reverse_route(runtime, now_us, &runtime->opened_rx,
                                  &reverse);
    if (result != UCN_V6_OK) return result;
    result = ucn_v6_transfer_sack_encode(&receive->sack, payload);
    if (result != UCN_V6_OK) return result;
    memset(&message, 0, sizeof(message));
    message.traffic_class = UCN_V6_TRAFFIC_Q1;
    message.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    message.interaction_role =
        runtime->opened_rx.frame.message.interaction_role;
    message.source_endpoint =
        runtime->opened_rx.frame.message.destination_endpoint;
    message.destination_endpoint =
        runtime->opened_rx.frame.message.source_endpoint;
    message.operation_id = runtime->opened_rx.frame.message.operation_id;
    message.payload_length = UCN_V6_TRANSFER_SACK_BYTES;
    return enqueue_transfer_payload(
        runtime, now_us, &reverse, &message,
        UCN_V6_PROTOCOL_OPCODE_TRANSFER_SACK, payload, sizeof(payload),
        false, 0U, 0U, 0U, false, receive->sack.message_id, 0U);
}

static ucn_v6_result_t retire_runtime_transfer(
    ucn_v6_runtime_owner_t *runtime,
    ucn_v6_runtime_transfer_slot_t *slot,
    ucn_v6_result_t completion)
{
    uint64_t buffer_token = 0U;
    ucn_v6_result_t result;
    if (slot == NULL || !slot->occupied || slot->fragment_queued) {
        return UCN_V6_ERR_STATE;
    }
    result = ucn_v6_transfer_retire_tx(
        runtime->config.transfer, slot->message_id, &buffer_token);
    if (result != UCN_V6_OK || buffer_token != slot->buffer_token ||
        !queue_release(runtime, buffer_token, completion, NULL)) {
        return result != UCN_V6_OK ? result : UCN_V6_ERR_STATE;
    }
    memset(slot, 0, sizeof(*slot));
    increment_saturated(&runtime->stats.transfer_messages_retired);
    return UCN_V6_OK;
}

static ucn_v6_result_t pump_runtime_transfer(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us, bool *worked)
{
    size_t offset;
    if (worked == NULL) return UCN_V6_ERR_ARGUMENT;
    *worked = false;
    for (offset = 0U; offset < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++offset) {
        size_t index =
            ((size_t)runtime->transfer_cursor + offset) %
            UCN_V6_CONFIG_TRANSFER_TX_SLOTS;
        ucn_v6_runtime_transfer_slot_t *slot =
            &runtime->transfer_slots[index];
        ucn_v6_transfer_tx_view_t view;
        ucn_v6_transfer_fragment_t fragment;
        size_t payload_length = 0U;
        ucn_v6_result_t result;
        if (!slot->occupied || slot->fragment_queued) continue;
        memset(&view, 0, sizeof(view));
        result = ucn_v6_transfer_copy_tx(
            runtime->config.transfer, slot->message_id, &view);
        if (result != UCN_V6_OK || view.buffer_token != slot->buffer_token) {
            return UCN_V6_ERR_STATE;
        }
        if (view.phase == UCN_V6_TRANSFER_TX_REASSEMBLED ||
            view.phase == UCN_V6_TRANSFER_TX_FAILED) {
            result = retire_runtime_transfer(
                runtime, slot,
                view.phase == UCN_V6_TRANSFER_TX_REASSEMBLED ?
                    UCN_V6_OK : UCN_V6_ERR_TIMEOUT);
            if (result != UCN_V6_OK) return result;
            runtime->transfer_cursor = (uint8_t)((index + 1U) %
                UCN_V6_CONFIG_TRANSFER_TX_SLOTS);
            *worked = true;
            return UCN_V6_OK;
        }
        memset(&fragment, 0, sizeof(fragment));
        result = ucn_v6_transfer_next_fragment(
            runtime->config.transfer, now_us, slot->message_id, &fragment);
        if (result == UCN_V6_ERR_NOT_FOUND) continue;
        if (result != UCN_V6_OK) {
            memset(&view, 0, sizeof(view));
            if (ucn_v6_transfer_copy_tx(runtime->config.transfer,
                                        slot->message_id, &view) == UCN_V6_OK &&
                view.phase == UCN_V6_TRANSFER_TX_FAILED) {
                result = retire_runtime_transfer(
                    runtime, slot, UCN_V6_ERR_TIMEOUT);
                if (result == UCN_V6_OK) {
                    *worked = true;
                    return UCN_V6_OK;
                }
            }
            return result;
        }
        result = ucn_v6_transfer_fragment_encode(
            &fragment, runtime->tx_payload_work,
            sizeof(runtime->tx_payload_work), &payload_length);
        if (result == UCN_V6_OK) {
            result = enqueue_transfer_payload(
                runtime, now_us, &slot->route_ref, &slot->message,
                UCN_V6_PROTOCOL_OPCODE_TRANSFER_FRAGMENT,
                runtime->tx_payload_work, payload_length,
                slot->has_hop_budget, slot->initial_hop_budget_us,
                slot->remaining_hop_budget_us, slot->local_priority,
                true, slot->message_id, fragment.fragment_index);
        }
        if (result == UCN_V6_OK) {
            slot->fragment_queued = true;
            runtime->transfer_cursor = (uint8_t)((index + 1U) %
                UCN_V6_CONFIG_TRANSFER_TX_SLOTS);
            *worked = true;
            return UCN_V6_OK;
        }
        if (ucn_v6_transfer_record_fragment_submit(
                runtime->config.transfer, now_us, slot->message_id,
                fragment.fragment_index, false) != UCN_V6_OK) {
            return UCN_V6_ERR_STATE;
        }
        if (result == UCN_V6_ERR_NO_SPACE ||
            result == UCN_V6_ERR_EXHAUSTED) {
            return result;
        }
        if (ucn_v6_transfer_fail_tx(runtime->config.transfer,
                                    slot->message_id) != UCN_V6_OK) {
            return UCN_V6_ERR_STATE;
        }
        return retire_runtime_transfer(runtime, slot, result);
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t phase_operation(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_transfer_rx_result_t receive;
    bool worked = false;
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    if (!runtime->ingress_active ||
        runtime->rx_phase != UCN_V6_RUNTIME_RX_OPENED ||
        runtime->opened_rx.frame.frame_type != UCN_V6_FRAME_TRANSFER) {
        call_result = pump_runtime_transfer(runtime, now_us, &worked);
        if (call_result == UCN_V6_ERR_NO_SPACE ||
            call_result == UCN_V6_ERR_EXHAUSTED) {
            return retry_at_next_tick(now_us, result);
        }
        if (call_result != UCN_V6_OK) {
            runtime->stats.faulted = true;
            return call_result;
        }
        if (worked) {
            result->work_done = 1U;
            result->has_more = true;
        }
        return UCN_V6_OK;
    }
    switch (runtime->opened_rx.frame.protocol_opcode) {
    case UCN_V6_PROTOCOL_OPCODE_TRANSFER_FRAGMENT:
        memset(&receive, 0, sizeof(receive));
        call_result = ucn_v6_transfer_receive_fragment(
            runtime->config.transfer, now_us, &runtime->opened_rx,
            &receive);
        if (call_result == UCN_V6_OK) {
            call_result = queue_transfer_sack(runtime, now_us, &receive);
        }
        if (call_result == UCN_V6_OK && receive.complete &&
            !receive.recent_replay) {
            memset(&runtime->transfer_delivery, 0,
                   sizeof(runtime->transfer_delivery));
            runtime->transfer_delivery.pending = true;
            runtime->transfer_delivery.origin =
                transfer_origin_from_opened(&runtime->opened_rx);
            runtime->transfer_delivery.operation_id =
                runtime->opened_rx.frame.message.operation_id;
            runtime->transfer_delivery.message_id = receive.sack.message_id;
            runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
            runtime->rx_phase = UCN_V6_RUNTIME_RX_TRANSFER_DELIVERY;
        }
        break;
    case UCN_V6_PROTOCOL_OPCODE_TRANSFER_SACK:
        call_result = ucn_v6_transfer_apply_sack(
            runtime->config.transfer, now_us, &runtime->opened_rx);
        break;
    case UCN_V6_PROTOCOL_OPCODE_TRANSFER_CREDIT:
        call_result = ucn_v6_transfer_ingest_credit(
            runtime->config.transfer, now_us, &runtime->opened_rx,
            runtime->config.transfer_maximum_credit);
        break;
    case UCN_V6_PROTOCOL_OPCODE_TRANSFER_RESULT:
        memset(&runtime->transfer_result, 0,
               sizeof(runtime->transfer_result));
        call_result = ucn_v6_transfer_result_decode(
            runtime->opened_rx.frame.payload,
            runtime->opened_rx.frame.payload_length,
            &runtime->transfer_result);
        if (call_result == UCN_V6_OK &&
            runtime->transfer_result.operation_id !=
                runtime->opened_rx.frame.message.operation_id) {
            call_result = UCN_V6_ERR_SECURITY;
        }
        if (call_result == UCN_V6_OK) {
            runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
            runtime->rx_phase = UCN_V6_RUNTIME_RX_TRANSFER_RESULT;
        }
        break;
    default:
        call_result = UCN_V6_ERR_MALFORMED;
        break;
    }
    if (call_result == UCN_V6_ERR_NO_SPACE ||
        call_result == UCN_V6_ERR_EXHAUSTED) {
        increment_saturated(&runtime->stats.rx_retried);
        return retry_at_next_tick(now_us, result);
    }
    if (call_result == UCN_V6_OK &&
        runtime->rx_phase == UCN_V6_RUNTIME_RX_OPENED) {
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        increment_saturated(&runtime->stats.transfer_frames_consumed);
    } else if (call_result == UCN_V6_OK) {
        increment_saturated(&runtime->stats.transfer_frames_consumed);
    } else if (packet_local_failure(call_result) ||
               call_result == UCN_V6_ERR_STATE) {
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_DROP;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        runtime->stats.last_protocol_error = call_result;
        increment_saturated(&runtime->stats.protocol_frames_rejected);
    } else {
        runtime->stats.faulted = true;
        return call_result;
    }
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t prepare_selected_tx(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    ucn_v6_runtime_tx_slot_t *slot, uint16_t *link_id,
    uint32_t *link_generation, ucn_v6_traffic_class_t *traffic_class,
    size_t *encoded_length, ucn_v6_session_key_t *next_hop_parent,
    ucn_v6_session_key_t *endpoint_parent)
{
    ucn_v6_route_selection_t route;
    ucn_v6_route_domain_t relay_domain = {0};
    ucn_v6_frame_t frame;
    uint64_t flow_id;
    uint64_t hop_budget_debit = 0U;
    ucn_v6_result_t result;
    if (runtime == NULL || slot == NULL || link_id == NULL ||
        link_generation == NULL || traffic_class == NULL ||
        encoded_length == NULL || next_hop_parent == NULL ||
        endpoint_parent == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&route, 0, sizeof(route));
    memset(&frame, 0, sizeof(frame));
    if (slot->relay) {
        result = ucn_v6_qos_flow_id(&slot->semantic.relay_opened, &flow_id);
        if (result == UCN_V6_OK) {
            memset(&relay_domain, 0, sizeof(relay_domain));
            result = ucn_v6_route_select_forward(
                runtime->config.route, now_us,
                &slot->semantic.relay_opened.frame,
                flow_id, slot->semantic.relay_opened.frame.origin_sequence,
                runtime->config.relay_route_policy, &relay_domain, &route);
        }
        if (result == UCN_V6_OK &&
            (slot->semantic.relay_opened.frame.flags &
             UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U) {
            hop_budget_debit = runtime->config.relay_residence_bound_us +
                               runtime->config.relay_transmit_bound_us;
        }
        if (result == UCN_V6_OK) {
            result = ucn_v6_security_forward_opened(
                runtime->config.security, now_us,
                &route.path.next_hop.principal, hop_budget_debit,
                &slot->semantic.relay_opened, runtime->tx_frame_work,
                sizeof(runtime->tx_frame_work), runtime->tx_encoded_work,
                sizeof(runtime->tx_encoded_work), encoded_length, &frame);
        }
    } else if (slot->security_kind ==
                   UCN_V6_RUNTIME_TX_PEER_DISCOVERY &&
               slot->semantic.local.direct_peer_discovery) {
        frame = slot->semantic.local.frame;
        if (!runtime_session_is_valid(
                &slot->semantic.local.direct_peer_session) ||
            slot->semantic.local.direct_link_id == 0U ||
            slot->semantic.local.direct_link_id == UINT16_MAX ||
            slot->semantic.local.direct_link_generation == 0U ||
            slot->semantic.local.direct_link_generation >
                UCN_V6_SERIAL_ROTATION_THRESHOLD) {
            result = UCN_V6_ERR_STATE;
        } else {
            result = ucn_v6_security_protect_peer_discovery(
                runtime->config.security, now_us,
                &slot->semantic.local.direct_peer_session.principal,
                &frame, runtime->tx_frame_work,
                sizeof(runtime->tx_frame_work), runtime->tx_encoded_work,
                sizeof(runtime->tx_encoded_work), encoded_length);
        }
        if (result == UCN_V6_OK) {
            route.path.next_hop =
                slot->semantic.local.direct_peer_session;
            route.path.capability.local_parent_session =
                slot->semantic.local.direct_peer_session;
            route.path.egress_link_id =
                slot->semantic.local.direct_link_id;
            route.path.egress_link_generation =
                slot->semantic.local.direct_link_generation;
        }
    } else {
        if (slot->security_kind != UCN_V6_RUNTIME_TX_ENDPOINT) {
            result = UCN_V6_ERR_STATE;
        } else if (slot->semantic.local.exact_route_ref) {
            ucn_v6_route_resolution_t resolution;
            memset(&resolution, 0, sizeof(resolution));
            result = ucn_v6_route_resolve_ref(
                runtime->config.route, now_us,
                &slot->semantic.local.route_ref, &resolution);
            if (result == UCN_V6_OK) {
                route.route_generation =
                    slot->semantic.local.route_ref.route_generation;
                route.path = resolution.path;
            }
        } else {
            result = ucn_v6_route_select(
                runtime->config.route, now_us,
                &slot->semantic.local.route_request, &route);
        }
        if (result == UCN_V6_OK) {
            frame = slot->semantic.local.frame;
            frame.flags = (uint8_t)(frame.flags |
                                    UCN_V6_FLAG_ROUTE_CONTEXT |
                                    UCN_V6_FLAG_PATH_CONTEXT);
            frame.route_generation = route.route_generation;
            frame.path.path_id = route.path.path_id;
            frame.path.path_generation = route.path.path_generation;
            result = ucn_v6_security_protect_frame(
                runtime->config.security, now_us,
                &route.path.next_hop.principal,
                &slot->semantic.local.route_request.domain
                     .destination_principal,
                &frame, runtime->tx_payload_work,
                sizeof(runtime->tx_payload_work), runtime->tx_frame_work,
                sizeof(runtime->tx_frame_work), runtime->tx_encoded_work,
                sizeof(runtime->tx_encoded_work), encoded_length);
        }
    }
    if (result != UCN_V6_OK) return result;
    if (!runtime_principal_equal(
            &route.path.next_hop.principal,
            &route.path.capability.local_parent_session.principal) ||
        route.path.egress_link_id == 0U ||
        route.path.egress_link_id == UINT16_MAX ||
        route.path.egress_link_generation == 0U ||
        route.path.egress_link_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        *encoded_length == 0U ||
        *encoded_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES ||
        *encoded_length > UINT16_MAX) {
        return UCN_V6_ERR_STATE;
    }
    *link_id = route.path.egress_link_id;
    *link_generation = route.path.egress_link_generation;
    *traffic_class = frame.traffic_class;
    *next_hop_parent = route.path.next_hop;
    if (!slot->relay && slot->security_kind ==
            UCN_V6_RUNTIME_TX_PEER_DISCOVERY) {
        *endpoint_parent = slot->semantic.local.direct_peer_session;
        return UCN_V6_OK;
    }
    endpoint_parent->principal = slot->relay ?
        relay_domain.destination_principal :
        slot->semantic.local.route_request.domain.destination_principal;
    endpoint_parent->binding = slot->relay ?
        relay_domain.destination_binding :
        slot->semantic.local.route_request.domain.destination_binding;
    endpoint_parent->session_generation = slot->relay ?
        relay_domain.destination_session_generation :
        slot->semantic.local.route_request.domain
            .destination_session_generation;
    if (!runtime_session_is_valid(endpoint_parent)) {
        return UCN_V6_ERR_STATE;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t retire_selected_tx_failure(
    ucn_v6_runtime_owner_t *runtime, ucn_v6_runtime_tx_slot_t *slot,
    uint64_t now_us, ucn_v6_result_t failure)
{
    ucn_v6_runtime_transfer_slot_t *transfer = NULL;
    if (slot->transfer_fragment) {
        transfer = find_transfer_slot(runtime, slot->transfer_message_id);
        if (transfer == NULL ||
            ucn_v6_transfer_record_fragment_submit(
                runtime->config.transfer, now_us,
                slot->transfer_message_id, slot->transfer_fragment_index,
                false) != UCN_V6_OK ||
            ucn_v6_transfer_fail_tx(runtime->config.transfer,
                                    slot->transfer_message_id) != UCN_V6_OK) {
            return UCN_V6_ERR_STATE;
        }
        transfer->fragment_queued = false;
    } else if (slot->release_to_app &&
        !queue_release(runtime, slot->buffer_token, failure, NULL)) {
        return UCN_V6_ERR_STATE;
    }
    if (ucn_v6_qos_complete_selection(
            runtime->config.qos, slot->buffer_token,
            UCN_V6_QOS_SELECTION_DROP_RETIRED) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    memset(slot, 0, sizeof(*slot));
    if (transfer != NULL) {
        return retire_runtime_transfer(runtime, transfer, failure);
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t service_bootstrap_tx(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    ucn_v6_stack_phase_result_t *phase,
    bool *handled)
{
    ucn_v6_runtime_bootstrap_tx_slot_t *slot;
    ucn_v6_driver_event_key_t key;
    ucn_v6_session_key_t empty_parent;
    ucn_v6_result_t result;
    if (runtime == NULL || phase == NULL || handled == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *handled = false;
    slot = find_bootstrap_tx_pending(runtime);
    if (slot == NULL) return UCN_V6_OK;
    *handled = true;
    if (now_us >= slot->deadline_us) {
        memset(slot, 0, sizeof(*slot));
        increment_saturated(&runtime->stats.bootstrap_objects_expired);
        phase->work_done = 1U;
        phase->has_more = find_bootstrap_tx_pending(runtime) != NULL;
        return UCN_V6_OK;
    }
    memset(&key, 0, sizeof(key));
    result = ucn_v6_adapter_enqueue_tx(
        runtime->config.adapter, slot->link_id, slot->link_generation,
        slot->buffer_token, slot->frame, slot->frame_length,
        UCN_V6_TRAFFIC_Q0, false, &key);
    if (result == UCN_V6_ERR_NO_SPACE) {
        return retry_at_next_tick(now_us, phase);
    }
    if (result != UCN_V6_OK) {
        if (packet_local_failure(result)) {
            memset(slot, 0, sizeof(*slot));
            runtime->stats.last_protocol_error = result;
            increment_saturated(&runtime->stats.protocol_frames_rejected);
            phase->work_done = 1U;
            phase->has_more = find_bootstrap_tx_pending(runtime) != NULL;
            return UCN_V6_OK;
        }
        return result;
    }
    memset(&empty_parent, 0, sizeof(empty_parent));
    if (!qos_inflight_add(runtime, slot->buffer_token, false, true, &key,
                          &empty_parent, &empty_parent)) {
        runtime->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    memset(slot, 0, sizeof(*slot));
    phase->work_done = 1U;
    phase->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t phase_qos_tx(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_runtime_tx_slot_t *slot;
    ucn_v6_qos_selection_t selection;
    ucn_v6_driver_event_key_t key;
    uint16_t link_id = 0U;
    uint32_t link_generation = 0U;
    ucn_v6_traffic_class_t traffic_class = UCN_V6_TRAFFIC_Q3;
    size_t encoded_length = 0U;
    ucn_v6_session_key_t next_hop_parent;
    ucn_v6_session_key_t endpoint_parent;
    ucn_v6_transfer_credit_reservation_t credit;
    bool credit_reserved = false;
    bool submitted = false;
    bool bootstrap_handled = false;
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime) || budget == 0U || result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(result, 0, sizeof(*result));
    call_result = retire_completed_ingress(runtime, result);
    if (call_result == UCN_V6_OK) return UCN_V6_OK;
    if (call_result != UCN_V6_ERR_NOT_FOUND) return call_result;
    call_result = ucn_v6_adapter_service_tx(runtime->config.adapter,
                                            &submitted);
    if (call_result != UCN_V6_OK) return call_result;
    if (submitted) {
        result->work_done = 1U;
        result->has_more = true;
        return UCN_V6_OK;
    }
    if (!qos_inflight_has_free(runtime)) {
        return retry_at_next_tick(now_us, result);
    }
    if (runtime->bootstrap_tx_turn &&
        find_bootstrap_tx_pending(runtime) != NULL) {
        runtime->bootstrap_tx_turn = false;
        return service_bootstrap_tx(
            runtime, now_us, result, &bootstrap_handled);
    }
    memset(&selection, 0, sizeof(selection));
    memset(&next_hop_parent, 0, sizeof(next_hop_parent));
    memset(&endpoint_parent, 0, sizeof(endpoint_parent));
    call_result = ucn_v6_qos_select_next(
        runtime->config.qos, now_us, &selection);
    if (call_result == UCN_V6_ERR_NOT_FOUND) {
        if (find_bootstrap_tx_pending(runtime) != NULL) {
            runtime->bootstrap_tx_turn = false;
            return service_bootstrap_tx(
                runtime, now_us, result, &bootstrap_handled);
        }
        return UCN_V6_OK;
    }
    if (call_result != UCN_V6_OK) return call_result;
    slot = find_tx_slot(runtime, selection.buffer_token);
    if (slot == NULL) {
        runtime->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    if ((slot->release_to_app || slot->transfer_fragment) &&
        free_release_slots(runtime) == 0U) {
        ucn_v6_result_t rollback = ucn_v6_qos_complete_selection(
            runtime->config.qos, slot->buffer_token,
            UCN_V6_QOS_SELECTION_RETRY);
        if (rollback != UCN_V6_OK) {
            runtime->stats.faulted = true;
            return rollback;
        }
        return retry_at_next_tick(now_us, result);
    }
    if (selection.action == UCN_V6_QOS_ACTION_DROP_EXPIRED) {
        if (retire_selected_tx_failure(
                runtime, slot, now_us, UCN_V6_ERR_EXHAUSTED) !=
            UCN_V6_OK) {
            runtime->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        result->work_done = 1U;
        result->has_more = true;
        return UCN_V6_OK;
    }
    call_result = prepare_selected_tx(
        runtime, now_us, slot, &link_id, &link_generation, &traffic_class,
        &encoded_length, &next_hop_parent, &endpoint_parent);
    if (call_result != UCN_V6_OK) {
        if (packet_local_failure(call_result)) {
            ucn_v6_result_t retire = retire_selected_tx_failure(
                runtime, slot, now_us, call_result);
            if (retire != UCN_V6_OK) {
                runtime->stats.faulted = true;
                return retire;
            }
            result->work_done = 1U;
            result->has_more = true;
            return UCN_V6_OK;
        }
        runtime->stats.faulted = true;
        return call_result;
    }
    memset(&credit, 0, sizeof(credit));
    if (slot->transfer_fragment) {
        call_result = ucn_v6_transfer_reserve_credit(
            runtime->config.transfer, now_us, &next_hop_parent,
            link_id, link_generation, traffic_class, &credit);
        if (call_result != UCN_V6_OK) {
            ucn_v6_result_t rollback = ucn_v6_qos_complete_selection(
                runtime->config.qos, slot->buffer_token,
                UCN_V6_QOS_SELECTION_RETRY);
            if (rollback != UCN_V6_OK) {
                runtime->stats.faulted = true;
                return rollback;
            }
            if (call_result == UCN_V6_ERR_NO_SPACE) {
                return retry_at_next_tick(now_us, result);
            }
            runtime->stats.faulted = true;
            return call_result;
        }
        credit_reserved = true;
    }
    memset(&key, 0, sizeof(key));
    call_result = ucn_v6_adapter_enqueue_tx(
        runtime->config.adapter, link_id, link_generation,
        slot->buffer_token,
        runtime->tx_encoded_work, encoded_length, traffic_class,
        slot->request_timestamp, &key);
    if (call_result != UCN_V6_OK) {
        if (credit_reserved &&
            ucn_v6_transfer_finish_credit(
                runtime->config.transfer, credit.reservation_id, false) !=
                UCN_V6_OK) {
            runtime->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        ucn_v6_result_t rollback = ucn_v6_qos_complete_selection(
            runtime->config.qos, slot->buffer_token,
            UCN_V6_QOS_SELECTION_RETRY);
        if (rollback != UCN_V6_OK) {
            runtime->stats.faulted = true;
            return rollback;
        }
        if (call_result == UCN_V6_ERR_NO_SPACE ||
            call_result == UCN_V6_ERR_EXHAUSTED ||
            call_result == UCN_V6_ERR_TIMEOUT) {
            return retry_at_next_tick(now_us, result);
        }
        return call_result;
    }
    if (credit_reserved &&
        ucn_v6_transfer_finish_credit(
            runtime->config.transfer, credit.reservation_id, true) !=
            UCN_V6_OK) {
        runtime->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    if (slot->transfer_fragment) {
        if (ucn_v6_transfer_record_fragment_submit(
                runtime->config.transfer, now_us,
                slot->transfer_message_id, slot->transfer_fragment_index,
                true) != UCN_V6_OK) {
            runtime->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        clear_transfer_fragment_marker(runtime, slot);
    }
    call_result = ucn_v6_qos_complete_selection(
        runtime->config.qos, slot->buffer_token,
        UCN_V6_QOS_SELECTION_LINK_SUBMITTED);
    if (call_result != UCN_V6_OK ||
        !qos_inflight_add(runtime, slot->buffer_token,
                          slot->release_to_app, false, &key,
                          &next_hop_parent,
                          &endpoint_parent)) {
        runtime->stats.faulted = true;
        return call_result != UCN_V6_OK ? call_result : UCN_V6_ERR_STATE;
    }
#if UCN_V6_FEATURE_REALTIME_ENABLED
    bind_time_tx_submission(runtime, slot->buffer_token, &key);
#endif
    memset(slot, 0, sizeof(*slot));
    runtime->bootstrap_tx_turn = true;
    result->work_done = 1U;
    result->has_more = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t invalidate_adapter(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    size_t index;
    if (!runtime_is_valid(runtime) ||
        !ucn_v6_stack_invalidation_is_valid(invalidation)) {
        return UCN_V6_ERR_STATE;
    }
    if (invalidation->type != UCN_V6_STACK_INVALIDATE_LINK) {
        return UCN_V6_OK;
    }
    (void)ucn_v6_bootstrap_invalidate_link(
        runtime->config.bootstrap, invalidation->link_id,
        invalidation->link_generation);
    for (index = 0U; index < UCN_V6_RUNTIME_BOOTSTRAP_TX_SLOTS; ++index) {
        ucn_v6_runtime_bootstrap_tx_slot_t *tx =
            &runtime->bootstrap_tx[index];
        if (tx->occupied && tx->link_id == invalidation->link_id &&
            tx->link_generation == invalidation->link_generation) {
            memset(tx, 0, sizeof(*tx));
        }
    }
    for (index = 0U;
         index < UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U; ++index) {
        ucn_v6_runtime_bootstrap_initiation_t *initiation =
            &runtime->bootstrap_initiations[index];
        ucn_v6_bootstrap_reassembly_t *reassembly =
            &runtime->bootstrap_reassembly[index];
        if (initiation->occupied &&
            initiation->link_id == invalidation->link_id &&
            initiation->link_generation == invalidation->link_generation) {
            memset(initiation, 0, sizeof(*initiation));
        }
        if (reassembly->occupied &&
            reassembly->key.ingress_link_id == invalidation->link_id &&
            reassembly->key.ingress_link_generation ==
                invalidation->link_generation) {
            (void)ucn_v6_bootstrap_reassembly_reset(reassembly);
        }
    }
    return UCN_V6_OK;
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

static ucn_v6_result_t retire_reopened_link_tokens(
    ucn_v6_runtime_owner_t *runtime, const uint64_t *tokens, size_t count)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        const ucn_v6_runtime_qos_inflight_t *marker =
            qos_inflight_find_const(runtime, tokens[index]);
        bool release_to_app = marker == NULL || marker->release_to_app;
        if (marker != NULL && !marker->bootstrap) {
            ucn_v6_result_t result = ucn_v6_qos_record_completion(
                runtime->config.qos, tokens[index],
                UCN_V6_QOS_COMPLETION_PHYSICAL_COMPLETED);
            if (result == UCN_V6_OK) {
                result = ucn_v6_qos_retire_completion(
                    runtime->config.qos, tokens[index]);
            }
            if (result != UCN_V6_OK) return result;
        }
        if (marker != NULL) qos_inflight_remove(runtime, tokens[index]);
#if UCN_V6_FEATURE_REALTIME_ENABLED
        {
            size_t time_index;
            for (time_index = 0U;
                 time_index < UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES;
                 ++time_index) {
                ucn_v6_runtime_time_slot_t *time_slot =
                    &runtime->time_slots[time_index];
                if (time_slot->occupied && time_slot->tx_queued &&
                    time_slot->tx_buffer_token == tokens[index]) {
                    memset(time_slot, 0, sizeof(*time_slot));
                    increment_saturated(
                        &runtime->stats.realtime_exchanges_expired);
                }
            }
        }
#endif
        if (release_to_app &&
            !queue_release(runtime, tokens[index], UCN_V6_ERR_ACCESS,
                           NULL)) {
            return UCN_V6_ERR_STATE;
        }
    }
    return UCN_V6_OK;
}

static bool runtime_inflight_matches_invalidation(
    const ucn_v6_runtime_qos_inflight_t *marker,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    if (marker == NULL || marker->buffer_token == 0U) return false;
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_LINK) {
        return marker->adapter_key.link_id == invalidation->link_id &&
               marker->adapter_key.link_generation ==
                   invalidation->link_generation;
    }
    if (invalidation->type != UCN_V6_STACK_INVALIDATE_SESSION) {
        return false;
    }
    return runtime_session_equal(&marker->next_hop_parent,
                                 &invalidation->session) ||
           runtime_session_equal(&marker->endpoint_parent,
                                 &invalidation->session);
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
    if (!queue_retired_tokens(runtime, tokens, count)) {
        return UCN_V6_ERR_STATE;
    }
    for (capacity = 0U; capacity < count; ++capacity) {
        size_t index;
        for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS;
             ++index) {
            if (runtime->transfer_slots[index].occupied &&
                runtime->transfer_slots[index].buffer_token ==
                    tokens[capacity]) {
                memset(&runtime->transfer_slots[index], 0,
                       sizeof(runtime->transfer_slots[index]));
                break;
            }
        }
    }
    return UCN_V6_OK;
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
    size_t index;
    ucn_v6_result_t call_result;
    if (!runtime_is_valid(runtime)) return UCN_V6_ERR_STATE;
    /* Peer-discovery replies deliberately bypass Route because they are
     * bound to the authenticated ingress Link/Session.  Cancel queued
     * replies against that exact parent before the generic QoS fan-out;
     * their local quota identity cannot express the remote parent.
     * Peer-discovery 响应刻意绕过 Route，并绑定认证入站 Link/Session；
     * 通用 QoS 的本机配额身份无法表达该远端父级，因此必须在此精确撤销。 */
    for (index = 0U; index < UCN_V6_RUNTIME_TX_SLOTS; ++index) {
        ucn_v6_runtime_tx_slot_t *slot = &runtime->tx_slots[index];
        bool matches = false;
        if (!slot->occupied || slot->relay ||
            slot->security_kind != UCN_V6_RUNTIME_TX_PEER_DISCOVERY ||
            !slot->semantic.local.direct_peer_discovery) {
            continue;
        }
        if (invalidation->type == UCN_V6_STACK_INVALIDATE_LINK) {
            matches = slot->semantic.local.direct_link_id ==
                          invalidation->link_id &&
                      slot->semantic.local.direct_link_generation ==
                          invalidation->link_generation;
        } else if (invalidation->type == UCN_V6_STACK_INVALIDATE_SESSION) {
            matches = runtime_session_equal(
                &slot->semantic.local.direct_peer_session,
                &invalidation->session);
        }
        if (matches) {
            call_result = ucn_v6_qos_cancel_queued(
                runtime->config.qos, slot->buffer_token);
            if (call_result != UCN_V6_OK) return call_result;
            memset(slot, 0, sizeof(*slot));
        }
    }
    /* Adapter already owns inflight bytes. Request cancellation first and
     * keep both Runtime/QoS markers until the exact Adapter completion is
     * retired. This prevents invalidation from returning a buffer twice. */
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        ucn_v6_runtime_qos_inflight_t *marker =
            &runtime->qos_inflight[index];
        if (runtime_inflight_matches_invalidation(marker, invalidation)) {
            call_result = ucn_v6_adapter_cancel_tx(
                runtime->config.adapter, &marker->adapter_key);
            if (call_result != UCN_V6_OK &&
                call_result != UCN_V6_ERR_REPLAY) {
                return call_result;
            }
        }
    }
    capacity = free_release_slots(runtime);
    if (capacity > sizeof(tokens) / sizeof(tokens[0])) {
        capacity = sizeof(tokens) / sizeof(tokens[0]);
    }
    call_result = ucn_v6_qos_apply_invalidation(
        runtime->config.qos, invalidation, tokens, capacity, &count);
    if (call_result != UCN_V6_OK) return call_result;
    for (index = 0U; index < count; ++index) {
        ucn_v6_runtime_tx_slot_t *slot = find_tx_slot(runtime, tokens[index]);
        if (slot == NULL ||
            (slot->release_to_app &&
             !queue_release(runtime, slot->buffer_token,
                            UCN_V6_ERR_ACCESS, NULL))) {
            return UCN_V6_ERR_STATE;
        }
        memset(slot, 0, sizeof(*slot));
    }
    return UCN_V6_OK;
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
            storage, storage_bytes, config->bootstrap_ops.context,
            config->bootstrap_ops.context != NULL ? 1U : 0U) ||
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
    initialized->bootstrap_tx_turn = true;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    initialized->next_time_handle_cookie = 1U;
#endif
    initialized->next_internal_buffer_token = UINT64_MAX;
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
    next.route_authority = phase_route_authority;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    next.realtime = phase_realtime;
#endif
    next.operation = phase_operation;
    next.endpoint = phase_endpoint;
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

ucn_v6_result_t ucn_v6_runtime_bootstrap_start(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    uint16_t link_id,
    uint32_t link_generation,
    uint32_t local_peer_discriminator,
    ucn_v6_address_class_t address_class,
    uint32_t realm_id,
    const ucn_v6_bootstrap_hello_t *hello)
{
    ucn_v6_runtime_bootstrap_initiation_t *slot;
    ucn_v6_frame_t frame_template;
    ucn_v6_principal_t local_principal;
    ucn_v6_binding_key_t local_binding;
    uint8_t payload[UCN_V6_BOOTSTRAP_HELLO_BYTES];
    uint64_t token = 0U;
    uint64_t deadline;
    size_t index;
    ucn_v6_result_t result;

    if (!runtime_is_valid(runtime) || hello == NULL ||
        runtime->callback_active || runtime->ingress_active ||
        runtime->pending_source != UCN_V6_RUNTIME_INVALIDATION_NONE ||
        link_id == 0U || link_id > UCN_V6_LINK_ID_MAX ||
        link_generation == 0U ||
        link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        local_peer_discriminator == 0U || realm_id == 0U ||
        address_class < UCN_V6_ADDRESS_CLASS_A0 ||
        address_class > UCN_V6_ADDRESS_CLASS_A3 ||
        ucn_v6_memory_ranges_overlap(runtime, sizeof(*runtime), hello,
                                     sizeof(*hello)) ||
        now_us > UINT64_MAX -
                     UCN_V6_CONFIG_RUNTIME_BOOTSTRAP_TX_TIMEOUT_US) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = ucn_v6_bootstrap_hello_encode(hello, payload);
    if (result != UCN_V6_OK) return result;
    deadline = now_us + UCN_V6_CONFIG_RUNTIME_BOOTSTRAP_TX_TIMEOUT_US;
    for (index = 0U;
         index < UCN_V6_CONFIG_BOOTSTRAP_PENDING * 2U; ++index) {
        const ucn_v6_runtime_bootstrap_initiation_t *existing =
            &runtime->bootstrap_initiations[index];
        if (!existing->occupied || existing->link_id != link_id ||
            existing->link_generation != link_generation ||
            existing->local_peer_discriminator !=
                local_peer_discriminator ||
            existing->hello.flow != hello->flow ||
            existing->hello.transaction_id != hello->transaction_id) {
            continue;
        }
        return memcmp(&existing->hello, hello, sizeof(*hello)) == 0 &&
                       existing->realm_id == realm_id &&
                       existing->address_class == address_class ?
                   UCN_V6_ERR_REPLAY : UCN_V6_ERR_STATE;
    }
    slot = find_bootstrap_initiation_free(runtime);
    if (slot == NULL || find_bootstrap_tx_free(runtime) == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(&frame_template, 0, sizeof(frame_template));
    frame_template.address_class = address_class;
    frame_template.realm_id = realm_id;
    if (hello->flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH) {
        memset(&local_principal, 0, sizeof(local_principal));
        memset(&local_binding, 0, sizeof(local_binding));
        result = ucn_v6_security_copy_local_identity(
            runtime->config.security, &local_principal, &local_binding);
        if (result != UCN_V6_OK || local_binding.realm_id != realm_id) {
            return result == UCN_V6_OK ? UCN_V6_ERR_ACCESS : result;
        }
        frame_template.source_address = local_binding.node_address;
        frame_template.source_binding_generation =
            local_binding.binding_generation;
    }
    result = queue_bootstrap_payload(
        runtime, &frame_template, link_id, link_generation,
        UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_HELLO, payload, sizeof(payload),
        deadline, &token);
    if (result != UCN_V6_OK) return result;
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->link_id = link_id;
    slot->link_generation = link_generation;
    slot->local_peer_discriminator = local_peer_discriminator;
    slot->address_class = address_class;
    slot->realm_id = realm_id;
    slot->hello = *hello;
    slot->deadline_us = deadline;
    return UCN_V6_OK;
}

static bool runtime_principal_equal(
    const ucn_v6_principal_t *left,
    const ucn_v6_principal_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool runtime_binding_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right)
{
    return left != NULL && right != NULL &&
           ucn_v6_binding_key_equal(left, right);
}

static bool runtime_session_equal(
    const ucn_v6_session_key_t *left,
    const ucn_v6_session_key_t *right)
{
    return left != NULL && right != NULL &&
           runtime_principal_equal(&left->principal, &right->principal) &&
           runtime_binding_equal(&left->binding, &right->binding) &&
           left->session_generation == right->session_generation;
}

static bool runtime_session_is_valid(
    const ucn_v6_session_key_t *session)
{
    return session != NULL &&
           ucn_v6_principal_is_valid(&session->principal) &&
           ucn_v6_binding_key_is_valid(&session->binding) &&
           session->session_generation != 0U &&
           session->session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool runtime_route_ref_equal(
    const ucn_v6_route_path_ref_t *left,
    const ucn_v6_route_path_ref_t *right)
{
    return left != NULL && right != NULL &&
           runtime_principal_equal(&left->domain.origin_principal,
                                   &right->domain.origin_principal) &&
           runtime_binding_equal(&left->domain.origin_binding,
                                 &right->domain.origin_binding) &&
           left->domain.origin_session_generation ==
               right->domain.origin_session_generation &&
           runtime_principal_equal(&left->domain.destination_principal,
                                   &right->domain.destination_principal) &&
           runtime_binding_equal(&left->domain.destination_binding,
                                 &right->domain.destination_binding) &&
           left->domain.destination_session_generation ==
               right->domain.destination_session_generation &&
           left->route_generation == right->route_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation;
}

static ucn_v6_address_class_t address_class_for_route(
    const ucn_v6_route_path_ref_t *reference)
{
    ucn_v6_address_class_t address_class;
    uint32_t maximum;
    if (reference == NULL) return (ucn_v6_address_class_t)-1;
    maximum = reference->domain.origin_binding.node_address;
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

static ucn_v6_result_t select_reverse_route(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_route_path_ref_t *reference)
{
    ucn_v6_principal_t local_principal;
    ucn_v6_binding_key_t local_binding;
    ucn_v6_route_select_request_t request;
    ucn_v6_route_selection_t selection;
    uint64_t flow_id;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || opened == NULL || reference == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = ucn_v6_security_copy_local_identity(
        runtime->config.security, &local_principal, &local_binding);
    if (result != UCN_V6_OK) return result;
    if (opened->frame.realm_id != local_binding.realm_id ||
        opened->frame.destination_address != local_binding.node_address ||
        opened->frame.destination_binding_generation !=
            local_binding.binding_generation ||
        !ucn_v6_principal_is_valid(&opened->authenticated_principal) ||
        opened->frame.session_generation == 0U ||
        opened->frame.session_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_SECURITY;
    }
    memset(&request, 0, sizeof(request));
    request.domain.origin_principal = local_principal;
    request.domain.origin_binding = local_binding;
    request.domain.origin_session_generation =
        opened->frame.session_generation;
    request.domain.destination_principal = opened->authenticated_principal;
    request.domain.destination_binding.realm_id = opened->frame.realm_id;
    request.domain.destination_binding.node_address =
        opened->frame.source_address;
    request.domain.destination_binding.binding_generation =
        opened->frame.source_binding_generation;
    request.domain.destination_session_generation =
        opened->frame.session_generation;
    flow_id = opened->frame.message.operation_id != 0U ?
                  opened->frame.message.operation_id :
                  opened->frame.origin_sequence ^
                      ((uint64_t)opened->frame.protocol_opcode << 32U) ^
                      (uint64_t)opened->frame.source_address;
    if (flow_id == 0U) flow_id = UINT64_C(1);
    request.flow_id = flow_id;
    request.packet_sequence = opened->frame.origin_sequence;
    request.policy = runtime->config.relay_route_policy;
    request.allow_reordering = false;
    memset(&selection, 0, sizeof(selection));
    result = ucn_v6_route_select(
        runtime->config.route, now_us, &request, &selection);
    if (result != UCN_V6_OK) return result;
    memset(reference, 0, sizeof(*reference));
    reference->domain = request.domain;
    reference->route_generation = selection.route_generation;
    reference->path_id = selection.path.path_id;
    reference->path_generation = selection.path.path_generation;
    return UCN_V6_OK;
}

static ucn_v6_result_t estimate_protected_frame_size(
    const ucn_v6_frame_t *frame, size_t *encoded_length)
{
    ucn_v6_frame_t sized;
    if (frame == NULL || encoded_length == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    sized = *frame;
    sized.origin_sequence =
        (sized.flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U ? 1U : 0U;
    sized.hop_sequence =
        (sized.flags & UCN_V6_FLAG_PEER_HOP_CONTEXT) != 0U ? 1U : 0U;
    if ((sized.flags & UCN_V6_FLAG_PEER_HOP_CONTEXT) != 0U) {
        sized.peer_hop.suite_id = 1U;
        sized.peer_hop.key_id = 1U;
        sized.peer_hop.key_generation = 1U;
    }
    if ((sized.flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U) {
        sized.e2e.mode = UCN_V6_E2E_AUTH_ONLY;
        sized.e2e.suite_id = 1U;
        sized.e2e.key_id = 1U;
        sized.e2e.key_generation = 1U;
    }
    return ucn_v6_wire_encoded_size(&sized, encoded_length);
}

static ucn_v6_result_t enqueue_peer_discovery_reply(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_security_open_result_t *opened, uint16_t opcode,
    ucn_v6_traffic_class_t traffic_class,
    const uint8_t *payload, size_t payload_length)
{
    ucn_v6_runtime_tx_slot_t *slot;
    ucn_v6_qos_admission_t admission;
    ucn_v6_qos_enqueue_result_t enqueue;
    ucn_v6_principal_t local_principal;
    ucn_v6_binding_key_t local_binding;
    ucn_v6_frame_t frame;
    uint64_t buffer_token;
    uint64_t flow_id;
    size_t encoded_length = 0U;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || opened == NULL || payload == NULL ||
        payload_length == 0U ||
        payload_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES ||
        (opcode != UCN_V6_PROTOCOL_OPCODE_CAPABILITY_QUERY &&
         opcode != UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE) ||
        (traffic_class != UCN_V6_TRAFFIC_Q1 &&
         traffic_class != UCN_V6_TRAFFIC_Q2) ||
        !opened->hop_authenticated || opened->group_discovery_only ||
        !runtime_principal_equal(&opened->authenticated_principal,
                                 &opened->ingress_peer_session.principal) ||
        !runtime_binding_equal(
            &opened->ingress_peer_session.binding,
            &(ucn_v6_binding_key_t){opened->frame.realm_id,
                                   opened->frame.source_address,
                                   opened->frame.source_binding_generation}) ||
        opened->ingress_link_instance_id != runtime->active_rx.key.link_id ||
        opened->ingress_link_instance_generation !=
            runtime->active_rx.key.link_generation) {
        return UCN_V6_ERR_SECURITY;
    }
    result = ucn_v6_security_copy_local_identity(
        runtime->config.security, &local_principal, &local_binding);
    if (result != UCN_V6_OK) return result;
    if (local_binding.realm_id != opened->frame.realm_id ||
        local_binding.node_address != opened->frame.destination_address ||
        local_binding.binding_generation !=
            opened->frame.destination_binding_generation) {
        return UCN_V6_ERR_SECURITY;
    }
    slot = find_free_tx_slot(runtime);
    if (slot == NULL) return UCN_V6_ERR_NO_SPACE;
    result = allocate_internal_buffer_token(runtime, &buffer_token);
    if (result != UCN_V6_OK) return result;
    memset(&frame, 0, sizeof(frame));
    frame.address_class = opened->frame.address_class;
    frame.frame_type = UCN_V6_FRAME_CONTROL;
    frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                  UCN_V6_FLAG_PROTOCOL_CONTEXT;
    frame.traffic_class = traffic_class;
    frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    frame.hop_limit = 1U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = local_binding.realm_id;
    frame.source_address = local_binding.node_address;
    frame.destination_address = opened->frame.source_address;
    frame.source_binding_generation = local_binding.binding_generation;
    frame.destination_binding_generation =
        opened->frame.source_binding_generation;
    frame.session_generation = opened->frame.session_generation;
    frame.protocol_opcode = opcode;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;
    result = estimate_protected_frame_size(&frame, &encoded_length);
    if (result != UCN_V6_OK) return result;
    if (encoded_length == 0U || encoded_length > UINT16_MAX ||
        encoded_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES) {
        return UCN_V6_ERR_NO_SPACE;
    }
    flow_id = ((uint64_t)opcode << 48U) ^
              ((uint64_t)local_binding.node_address << 16U) ^
              (uint64_t)opened->frame.source_address;
    if (flow_id == 0U) flow_id = 1U;
    memset(&admission, 0, sizeof(admission));
    admission.quota_identity.principal = local_principal;
    admission.quota_identity.binding = local_binding;
    admission.quota_identity.session_generation =
        opened->frame.session_generation;
    admission.flow_id = flow_id;
    admission.traffic_class = traffic_class;
    admission.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    admission.authenticated = true;
    memset(&enqueue, 0, sizeof(enqueue));
    result = ucn_v6_qos_enqueue_admitted(
        runtime->config.qos, now_us, &admission, buffer_token,
        (uint16_t)encoded_length, 0U, &enqueue);
    if (result != UCN_V6_OK) return result;
    if (!enqueue.accepted || enqueue.replaced_latest) {
        runtime->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->security_kind = UCN_V6_RUNTIME_TX_PEER_DISCOVERY;
    slot->buffer_token = buffer_token;
    slot->semantic.local.frame = frame;
    slot->semantic.local.direct_peer_discovery = true;
    slot->semantic.local.direct_peer_session = opened->ingress_peer_session;
    slot->semantic.local.direct_link_id = opened->ingress_link_instance_id;
    slot->semantic.local.direct_link_generation =
        opened->ingress_link_instance_generation;
    slot->payload_length = (uint16_t)payload_length;
    memcpy(slot->payload, payload, payload_length);
    slot->semantic.local.frame.payload = slot->payload;
    return UCN_V6_OK;
}

static ucn_v6_result_t runtime_enqueue_frame_kind(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    const ucn_v6_runtime_send_request_t *request,
    bool release_to_app,
    ucn_v6_runtime_tx_security_kind_t security_kind,
    ucn_v6_runtime_send_result_t *result_out)
{
    ucn_v6_runtime_tx_slot_t *slot;
    ucn_v6_runtime_tx_slot_t *replaced_slot;
    ucn_v6_route_selection_t selection;
    ucn_v6_runtime_send_result_t result;
    ucn_v6_qos_admission_t scheduling;
    ucn_v6_principal_t local_principal;
    ucn_v6_binding_key_t local_binding;
    ucn_v6_qos_enqueue_result_t enqueue;
    ucn_v6_frame_t scheduled_frame;
    size_t encoded_length = 0U;
    ucn_v6_result_t call_result;

    if (!runtime_is_valid(runtime) || request == NULL || result_out == NULL ||
        request->buffer_token == 0U ||
        (security_kind != UCN_V6_RUNTIME_TX_ENDPOINT &&
         security_kind != UCN_V6_RUNTIME_TX_PEER_DISCOVERY) ||
        (release_to_app && request->buffer_token > INT64_MAX) ||
        (!release_to_app && request->buffer_token <= INT64_MAX) ||
        request->local_priority > 7U ||
        runtime->pending_source != UCN_V6_RUNTIME_INVALIDATION_NONE ||
        ucn_v6_memory_ranges_overlap(runtime, sizeof(*runtime), request,
                                     sizeof(*request)) ||
        ucn_v6_memory_ranges_overlap(runtime, sizeof(*runtime), result_out,
                                     sizeof(*result_out)) ||
        ucn_v6_memory_ranges_overlap(request, sizeof(*request), result_out,
                                     sizeof(*result_out)) ||
        (request->frame.payload_length != 0U &&
         request->frame.payload == NULL) ||
        (request->frame.payload_length != 0U &&
         ucn_v6_memory_ranges_overlap(
             request->frame.payload, request->frame.payload_length,
             result_out, sizeof(*result_out))) ||
        request->frame.payload_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES ||
        request->route.domain.origin_binding.realm_id !=
            request->route.domain.destination_binding.realm_id ||
        request->frame.realm_id !=
            request->route.domain.origin_binding.realm_id ||
        request->frame.source_address !=
            request->route.domain.origin_binding.node_address ||
        request->frame.source_binding_generation !=
            request->route.domain.origin_binding.binding_generation ||
        request->frame.destination_address !=
            request->route.domain.destination_binding.node_address ||
        request->frame.destination_binding_generation !=
            request->route.domain.destination_binding.binding_generation ||
        request->frame.session_generation !=
            request->route.domain.origin_session_generation ||
        request->route.domain.origin_session_generation !=
            request->route.domain.destination_session_generation ||
        !ucn_v6_principal_is_valid(
            &request->route.domain.origin_principal) ||
        !ucn_v6_principal_is_valid(
            &request->route.domain.destination_principal) ||
        !ucn_v6_binding_key_is_valid(
            &request->route.domain.origin_binding) ||
        !ucn_v6_binding_key_is_valid(
            &request->route.domain.destination_binding) ||
        find_tx_slot(runtime, request->buffer_token) != NULL ||
        find_release_token(runtime, request->buffer_token) != NULL ||
        (request->frame.delivery_guarantee == UCN_V6_DELIVERY_LATEST &&
         free_release_slots(runtime) == 0U)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_free_tx_slot(runtime);
    if (slot == NULL) return UCN_V6_ERR_NO_SPACE;
    memset(&selection, 0, sizeof(selection));
    call_result = ucn_v6_route_select(
        runtime->config.route, now_us, &request->route, &selection);
    if (call_result != UCN_V6_OK) return call_result;
    if (!runtime_principal_equal(
            &selection.path.next_hop.principal,
            &selection.path.capability.local_parent_session.principal) ||
        selection.path.egress_link_id == 0U ||
        selection.path.egress_link_id == UINT16_MAX ||
        selection.path.egress_link_generation == 0U ||
        selection.path.egress_link_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_STATE;
    }
    scheduled_frame = request->frame;
    if (security_kind == UCN_V6_RUNTIME_TX_ENDPOINT) {
        scheduled_frame.flags = (uint8_t)(scheduled_frame.flags |
                                          UCN_V6_FLAG_ROUTE_CONTEXT |
                                          UCN_V6_FLAG_PATH_CONTEXT);
        scheduled_frame.route_generation = selection.route_generation;
        scheduled_frame.path.path_id = selection.path.path_id;
        scheduled_frame.path.path_generation =
            selection.path.path_generation;
    } else if (selection.path.hop_count != 1U ||
               !runtime_principal_equal(
                   &selection.path.next_hop.principal,
                   &request->route.domain.destination_principal)) {
        return UCN_V6_ERR_ACCESS;
    }
    call_result = estimate_protected_frame_size(
        &scheduled_frame, &encoded_length);
    if (call_result != UCN_V6_OK) return call_result;
    if (encoded_length == 0U ||
        encoded_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES ||
        encoded_length > UINT16_MAX) {
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(&scheduling, 0, sizeof(scheduling));
    memset(&local_principal, 0, sizeof(local_principal));
    memset(&local_binding, 0, sizeof(local_binding));
    call_result = ucn_v6_security_copy_local_identity(
        runtime->config.security, &local_principal, &local_binding);
    if (call_result != UCN_V6_OK) return call_result;
    if (!runtime_principal_equal(
            &local_principal, &request->route.domain.origin_principal) ||
        memcmp(&local_binding, &request->route.domain.origin_binding,
               sizeof(local_binding)) != 0 ||
        request->route.flow_id == 0U) {
        return UCN_V6_ERR_ACCESS;
    }
    scheduling.quota_identity.principal = local_principal;
    scheduling.quota_identity.binding = local_binding;
    scheduling.quota_identity.session_generation =
        request->route.domain.origin_session_generation;
    scheduling.flow_id = request->route.flow_id;
    scheduling.traffic_class = scheduled_frame.traffic_class;
    scheduling.delivery_guarantee =
        scheduled_frame.delivery_guarantee;
    scheduling.has_hop_budget =
        (scheduled_frame.flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U;
    scheduling.initial_budget_us =
        scheduled_frame.hop_budget.initial_budget_us;
    scheduling.remaining_budget_us =
        scheduled_frame.hop_budget.remaining_budget_us;
    scheduling.authenticated = true;
    memset(&enqueue, 0, sizeof(enqueue));
    call_result = ucn_v6_qos_enqueue_admitted(
        runtime->config.qos, now_us, &scheduling, request->buffer_token,
        (uint16_t)encoded_length, request->local_priority, &enqueue);
    if (call_result != UCN_V6_OK) return call_result;
    if (!enqueue.accepted) {
        runtime->stats.faulted = true;
        return UCN_V6_ERR_STATE;
    }
    if (enqueue.replaced_latest) {
        replaced_slot = find_tx_slot(runtime, enqueue.replaced_buffer_token);
        if (replaced_slot == NULL ||
            (replaced_slot->release_to_app &&
             !queue_release(runtime, enqueue.replaced_buffer_token,
                            UCN_V6_ERR_EXHAUSTED, NULL))) {
            runtime->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        memset(replaced_slot, 0, sizeof(*replaced_slot));
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->release_to_app = release_to_app;
    slot->relay = false;
    slot->security_kind = security_kind;
    slot->buffer_token = request->buffer_token;
    slot->semantic.local.frame = request->frame;
    slot->semantic.local.route_request = request->route;
    slot->request_timestamp = request->request_timestamp;
    slot->payload_length = request->frame.payload_length;
    if (slot->payload_length != 0U) {
        memcpy(slot->payload, request->frame.payload, slot->payload_length);
    }
    slot->semantic.local.frame.payload = slot->payload;
    memset(&result, 0, sizeof(result));
    result.admission_route_ref.domain = request->route.domain;
    result.admission_route_ref.route_generation = selection.route_generation;
    result.admission_route_ref.path_id = selection.path.path_id;
    result.admission_route_ref.path_generation =
        selection.path.path_generation;
    result.estimated_encoded_length = (uint16_t)encoded_length;
    *result_out = result;
    return UCN_V6_OK;
}

static ucn_v6_result_t runtime_enqueue_frame(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    const ucn_v6_runtime_send_request_t *request,
    bool release_to_app,
    ucn_v6_runtime_send_result_t *result_out)
{
    return runtime_enqueue_frame_kind(
        runtime, now_us, request, release_to_app,
        UCN_V6_RUNTIME_TX_ENDPOINT, result_out);
}

static ucn_v6_result_t runtime_enqueue_exact_frame(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_runtime_send_request_t *request, bool release_to_app,
    const ucn_v6_route_path_ref_t *reference,
    ucn_v6_runtime_send_result_t *result_out)
{
    ucn_v6_runtime_tx_slot_t *slot;
    ucn_v6_runtime_send_result_t result;
    ucn_v6_result_t call_result;
    if (reference == NULL || result_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&result, 0, sizeof(result));
    call_result = runtime_enqueue_frame(
        runtime, now_us, request, release_to_app, &result);
    if (call_result != UCN_V6_OK) return call_result;
    if (!runtime_route_ref_equal(&result.admission_route_ref, reference) ||
        (slot = find_tx_slot(runtime, request->buffer_token)) == NULL) {
        slot = find_tx_slot(runtime, request->buffer_token);
        if (slot != NULL &&
            ucn_v6_qos_cancel_queued(runtime->config.qos,
                                     request->buffer_token) == UCN_V6_OK) {
            memset(slot, 0, sizeof(*slot));
        } else {
            runtime->stats.faulted = true;
        }
        return UCN_V6_ERR_STATE;
    }
    slot->semantic.local.exact_route_ref = true;
    slot->semantic.local.route_ref = *reference;
    *result_out = result;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_runtime_send_frame(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    const ucn_v6_runtime_send_request_t *request,
    ucn_v6_runtime_send_result_t *result_out)
{
    return runtime_enqueue_frame(
        runtime, now_us, request, true, result_out);
}

ucn_v6_result_t ucn_v6_runtime_transfer_send(
    ucn_v6_runtime_owner_t *runtime,
    uint64_t now_us,
    const ucn_v6_runtime_transfer_send_request_t *request)
{
    ucn_v6_runtime_transfer_slot_t *slot;
    size_t index;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || request == NULL ||
        request->local_priority > 7U ||
        request->transfer.buffer_token == 0U ||
        request->transfer.buffer_token > INT64_MAX ||
        request->transfer.payload == NULL ||
        request->transfer.payload_length == 0U ||
        runtime->pending_source != UCN_V6_RUNTIME_INVALIDATION_NONE ||
        (request->has_hop_budget &&
         (request->initial_hop_budget_us == 0U ||
          request->remaining_hop_budget_us == 0U ||
          request->remaining_hop_budget_us >
              request->initial_hop_budget_us)) ||
        (!request->has_hop_budget &&
         (request->initial_hop_budget_us != 0U ||
          request->remaining_hop_budget_us != 0U)) ||
        ucn_v6_memory_ranges_overlap(runtime, sizeof(*runtime), request,
                                     sizeof(*request)) ||
        ucn_v6_memory_ranges_overlap(
            runtime, sizeof(*runtime), request->transfer.payload,
            request->transfer.payload_length) ||
        find_transfer_slot(runtime, request->transfer.message_id) != NULL ||
        find_tx_slot(runtime, request->transfer.buffer_token) != NULL ||
        find_release_token(runtime, request->transfer.buffer_token) != NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        if (runtime->transfer_slots[index].occupied &&
            runtime->transfer_slots[index].buffer_token ==
                request->transfer.buffer_token) {
            return UCN_V6_ERR_REPLAY;
        }
    }
    slot = find_free_transfer_slot(runtime);
    if (slot == NULL) return UCN_V6_ERR_NO_SPACE;
    result = ucn_v6_transfer_send_begin(
        runtime->config.transfer, now_us, &request->transfer);
    if (result != UCN_V6_OK) return result;
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->message_id = request->transfer.message_id;
    slot->buffer_token = request->transfer.buffer_token;
    slot->route_ref = request->transfer.route_ref;
    slot->message = request->transfer.message;
    slot->message_class = request->transfer.message_class;
    slot->has_hop_budget = request->has_hop_budget;
    slot->initial_hop_budget_us = request->initial_hop_budget_us;
    slot->remaining_hop_budget_us = request->remaining_hop_budget_us;
    slot->local_priority = request->local_priority;
    increment_saturated(&runtime->stats.transfer_messages_started);
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
        retire_reopened_link_tokens(runtime, tokens, retired_count) !=
            UCN_V6_OK) {
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

static ucn_v6_result_t send_time_control(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_route_path_ref_t *reference,
    uint16_t opcode, const uint8_t *payload, size_t payload_length,
    uint64_t buffer_token, bool request_timestamp, uint64_t now_us)
{
    ucn_v6_route_resolution_t resolution;
    ucn_v6_frame_t frame;
    ucn_v6_runtime_send_request_t request;
    ucn_v6_runtime_send_result_t enqueue_result;
    ucn_v6_address_class_t address_class;
    ucn_v6_result_t result;
    if (reference == NULL || payload == NULL ||
        payload_length == 0U ||
        payload_length > UCN_V6_CONFIG_ADAPTER_FRAME_BYTES ||
        buffer_token == 0U) {
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
    memset(&request, 0, sizeof(request));
    request.frame = frame;
    request.route.domain = reference->domain;
    request.route.flow_id = ((uint64_t)opcode << 48U) ^
                            ((uint64_t)frame.source_address << 16U) ^
                            frame.destination_address;
    if (request.route.flow_id == 0U) request.route.flow_id = 1U;
    request.route.packet_sequence = (uint64_t)opcode + 1U;
    request.route.policy = UCN_V6_ROUTE_POLICY_PINNED;
    request.route.pinned_path_id = reference->path_id;
    request.route.pinned_path_generation = reference->path_generation;
    request.buffer_token = buffer_token;
    request.request_timestamp = request_timestamp;
    memset(&enqueue_result, 0, sizeof(enqueue_result));
    result = runtime_enqueue_frame(
        runtime, now_us, &request, buffer_token <= INT64_MAX,
        &enqueue_result);
    if (result != UCN_V6_OK) return result;
    if (enqueue_result.admission_route_ref.route_generation !=
            reference->route_generation ||
        enqueue_result.admission_route_ref.path_id != reference->path_id ||
        enqueue_result.admission_route_ref.path_generation !=
            reference->path_generation) {
        ucn_v6_runtime_tx_slot_t *slot = find_tx_slot(runtime, buffer_token);
        if (ucn_v6_qos_cancel_queued(runtime->config.qos, buffer_token) !=
                UCN_V6_OK ||
            slot == NULL) {
            runtime->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        memset(slot, 0, sizeof(*slot));
        return UCN_V6_ERR_REPLAY;
    }
    {
        ucn_v6_runtime_tx_slot_t *slot = find_tx_slot(runtime, buffer_token);
        if (slot == NULL || slot->relay) {
            runtime->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        slot->semantic.local.exact_route_ref = true;
        slot->semantic.local.route_ref = *reference;
    }
    return UCN_V6_OK;
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
        payload, sizeof(payload), buffer_token, true, now_us);
    if (result != UCN_V6_OK) {
        memset(slot, 0, sizeof(*slot));
        return result;
    }
    slot->tx_buffer_token = buffer_token;
    slot->tx_queued = true;
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
            if (candidate->tx_queued && !candidate->tx_bound) {
                ucn_v6_runtime_tx_slot_t *queued =
                    find_tx_slot(runtime, candidate->tx_buffer_token);
                if (queued == NULL ||
                    ucn_v6_qos_cancel_queued(
                        runtime->config.qos,
                        candidate->tx_buffer_token) != UCN_V6_OK) {
                    return UCN_V6_ERR_STATE;
                }
                memset(queued, 0, sizeof(*queued));
            } else if (candidate->tx_bound &&
                       !candidate->local_tx_complete &&
                       ucn_v6_adapter_cancel_tx(runtime->config.adapter,
                                                &candidate->tx_key) !=
                           UCN_V6_OK) {
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
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || handle == NULL || buffer_token == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_time_handle(runtime, handle, UCN_V6_RUNTIME_TIME_MEMBER);
    if (slot == NULL) return UCN_V6_ERR_NOT_FOUND;
    if (now_us >= slot->deadline_us || slot->tx_queued) {
        return slot->tx_queued ? UCN_V6_ERR_REPLAY : UCN_V6_ERR_TIMEOUT;
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
        payload, sizeof(payload), buffer_token, true, now_us);
    if (result != UCN_V6_OK) return result;
    slot->tx_buffer_token = buffer_token;
    slot->tx_queued = true;
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
        payload, sizeof(payload), buffer_token, false, now_us);
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

static ucn_v6_result_t select_reverse_time_route(
    ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_route_path_ref_t *reference)
{
    return select_reverse_route(runtime, now_us, opened, reference);
}

static bool existing_member_time_handle(
    ucn_v6_runtime_owner_t *runtime,
    const ucn_v6_time_sync_announce_t *announce,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_driver_rx_view_t *rx,
    ucn_v6_runtime_time_handle_t *handle)
{
    ucn_v6_runtime_time_slot_t *slot = find_time_slot(
        runtime, UCN_V6_RUNTIME_TIME_MEMBER, announce->clock_domain_id,
        announce->domain_generation, announce->sync_sequence);
    ucn_v6_binding_key_t source = frame_source_binding(&opened->frame);
    ucn_v6_binding_key_t destination =
        frame_destination_binding(&opened->frame);
    size_t index;
    if (slot == NULL || slot->tx_queued ||
        !principal_equal(&slot->remote_principal,
                         &opened->authenticated_principal) ||
        !binding_equal(&slot->remote_binding, &source) ||
        !binding_equal(&slot->local_binding, &destination) ||
        slot->local_rx.link_id != rx->key.link_id ||
        slot->local_rx.link_generation != rx->key.link_generation ||
        slot->local_rx.event_token != rx->key.event_token) {
        return false;
    }
    index = (size_t)(slot - runtime->time_slots);
    handle->opaque[0] = slot->handle_cookie;
    handle->opaque[1] = UCN_V6_RUNTIME_CANARY ^ slot->handle_cookie ^
                        (uint64_t)(index + 1U) ^
                        runtime->config.runtime_instance_generation;
    return true;
}

static ucn_v6_result_t phase_realtime(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *phase_result)
{
    ucn_v6_runtime_owner_t *runtime = (ucn_v6_runtime_owner_t *)context;
    ucn_v6_runtime_time_handle_t handle;
    ucn_v6_route_path_ref_t reverse;
    ucn_v6_time_sync_announce_t announce;
    uint64_t buffer_token = 0U;
    ucn_v6_result_t result;
    if (!runtime_is_valid(runtime) || budget == 0U || phase_result == NULL) {
        return UCN_V6_ERR_STATE;
    }
    memset(phase_result, 0, sizeof(*phase_result));
    if (!runtime->ingress_active ||
        runtime->rx_phase != UCN_V6_RUNTIME_RX_OPENED ||
        runtime->opened_rx.frame.frame_type != UCN_V6_FRAME_CONTROL) {
        return UCN_V6_OK;
    }
    switch (runtime->opened_rx.frame.protocol_opcode) {
    case UCN_V6_PROTOCOL_OPCODE_TIME_SYNC:
        memset(&handle, 0, sizeof(handle));
        memset(&announce, 0, sizeof(announce));
        result = ucn_v6_time_sync_announce_decode(
            runtime->opened_rx.frame.payload,
            runtime->opened_rx.frame.payload_length, &announce);
        if (result == UCN_V6_OK &&
            !existing_member_time_handle(runtime, &announce,
                                         &runtime->opened_rx,
                                         &runtime->active_rx, &handle)) {
            memset(&reverse, 0, sizeof(reverse));
            result = select_reverse_time_route(
                runtime, now_us, &runtime->opened_rx, &reverse);
            if (result == UCN_V6_OK) {
                result = ucn_v6_runtime_time_observe_sync(
                    runtime, &runtime->opened_rx, &runtime->active_rx,
                    &reverse, now_us, &handle);
            }
        }
        if (result == UCN_V6_OK) {
            result = allocate_internal_buffer_token(runtime, &buffer_token);
        }
        if (result == UCN_V6_OK) {
            result = ucn_v6_runtime_time_send_delay_request(
                runtime, &handle, buffer_token, now_us);
        }
        break;
    case UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_REQUEST:
        result = allocate_internal_buffer_token(runtime, &buffer_token);
        if (result == UCN_V6_OK) {
            result = ucn_v6_runtime_time_respond_delay_request(
                runtime, &runtime->opened_rx, &runtime->active_rx,
                buffer_token, now_us);
        }
        break;
    case UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE:
        result = ucn_v6_runtime_time_complete(
            runtime, &runtime->opened_rx, &runtime->active_rx, now_us);
        break;
    default:
        return UCN_V6_OK;
    }
    if (result == UCN_V6_ERR_NO_SPACE) {
        increment_saturated(&runtime->stats.rx_retried);
        return retry_at_next_tick(now_us, phase_result);
    }
    if (result == UCN_V6_OK) {
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
    } else if (packet_local_failure(result) || result == UCN_V6_ERR_STATE) {
        runtime->rx_disposition = UCN_V6_RUNTIME_INGRESS_DROP;
        runtime->rx_phase = UCN_V6_RUNTIME_RX_COMPLETE;
        runtime->stats.last_protocol_error = result;
        increment_saturated(&runtime->stats.protocol_frames_rejected);
    } else {
        runtime->stats.faulted = true;
        return result;
    }
    phase_result->work_done = 1U;
    phase_result->has_more = true;
    return UCN_V6_OK;
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
