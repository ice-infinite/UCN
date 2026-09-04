#include "ucn/v6/ucn_v6_qos.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static ucn_v6_principal_t principal(uint8_t seed)
{
    ucn_v6_principal_t result;
    size_t index;
    for (index = 0U; index < sizeof(result.bytes); ++index) {
        result.bytes[index] = (uint8_t)(seed + index);
    }
    return result;
}

static ucn_v6_route_domain_t route_domain(uint8_t source_seed,
                                          uint8_t target_seed)
{
    ucn_v6_route_domain_t result;
    memset(&result, 0, sizeof(result));
    result.origin_principal = principal(source_seed);
    result.origin_binding.realm_id = 1U;
    result.origin_binding.node_address = source_seed;
    result.origin_binding.binding_generation = 2U;
    result.origin_session_generation = 3U;
    result.destination_principal = principal(target_seed);
    result.destination_binding.realm_id = 1U;
    result.destination_binding.node_address = target_seed;
    result.destination_binding.binding_generation = 4U;
    return result;
}

static ucn_v6_metric_key_t metric_key(uint16_t path_id)
{
    ucn_v6_metric_key_t result;
    memset(&result, 0, sizeof(result));
    result.domain = route_domain(2U, 7U);
    result.route_generation = 5U;
    result.path_id = path_id;
    result.path_generation = 6U;
    return result;
}

static ucn_v6_metric_sample_t metric_sample(uint64_t measured_at_us)
{
    ucn_v6_metric_sample_t result;
    memset(&result, 0, sizeof(result));
    result.known_mask = UCN_V6_METRIC_ALL_KNOWN;
    result.administrative_cost = 10U;
    result.latency_us = 100U;
    result.jitter_us = 20U;
    result.loss_ppm = 100U;
    result.available_bitrate_bps = 1000000U;
    result.queue_occupancy_permille = 100U;
    result.energy_cost = 3U;
    result.stability_score_permille = 900U;
    result.measured_at_us = measured_at_us;
    result.sample_window_us = 1000U;
    return result;
}

static ucn_v6_security_open_result_t opened_frame(
    uint8_t source_seed,
    ucn_v6_traffic_class_t traffic_class,
    ucn_v6_delivery_guarantee_t delivery,
    uint16_t source_endpoint,
    bool with_budget,
    uint64_t initial_budget,
    uint64_t remaining_budget)
{
    ucn_v6_security_open_result_t result;
    memset(&result, 0, sizeof(result));
    result.authenticated_principal = principal(source_seed);
    result.ingress_peer_session.principal = principal(source_seed);
    result.ingress_peer_session.binding.realm_id = 1U;
    result.ingress_peer_session.binding.node_address = source_seed;
    result.ingress_peer_session.binding.binding_generation = 2U;
    result.ingress_peer_session.session_generation = 4U;
    result.hop_authenticated = true;
    result.frame.address_class = UCN_V6_ADDRESS_CLASS_A0;
    result.frame.frame_type = UCN_V6_FRAME_DATA;
    result.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_MESSAGE_CONTEXT;
    result.frame.traffic_class = traffic_class;
    result.frame.delivery_guarantee = delivery;
    result.frame.realm_id = 1U;
    result.frame.source_address = source_seed;
    result.frame.destination_address = 20U;
    result.frame.source_binding_generation = 2U;
    result.frame.destination_binding_generation = 3U;
    result.frame.session_generation = 4U;
    result.frame.message.source_endpoint = source_endpoint;
    result.frame.message.destination_endpoint = 90U;
    if (with_budget) {
        result.frame.flags |= UCN_V6_FLAG_HOP_BUDGET_CONTEXT;
        result.frame.hop_budget.initial_budget_us = initial_budget;
        result.frame.hop_budget.remaining_budget_us = remaining_budget;
    }
    return result;
}

static ucn_v6_session_key_t session_from(
    const ucn_v6_security_open_result_t *opened)
{
    ucn_v6_session_key_t result;
    memset(&result, 0, sizeof(result));
    return opened->ingress_peer_session;
}

static int test_metric(void)
{
    ucn_v6_metric_owner_storage_t storage;
    ucn_v6_metric_owner_t *owner = NULL;
    ucn_v6_metric_policy_t policy;
    ucn_v6_metric_key_t key = metric_key(1U);
    ucn_v6_metric_sample_t sample = metric_sample(100U);
    ucn_v6_metric_sample_t conflict;
    ucn_v6_metric_cost_t q0;
    ucn_v6_metric_cost_t hop;
    ucn_v6_metric_cost_t total;
    ucn_v6_metric_cost_t before;
    ucn_v6_metric_view_t view;

    memset(&storage, 0, sizeof(storage));
    ucn_v6_metric_default_policy(&policy);
    CHECK(ucn_v6_metric_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &policy, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_metric_ingest(owner, &key, &sample) == UCN_V6_OK);
    CHECK(ucn_v6_metric_ingest(owner, &key, &sample) == UCN_V6_OK);
    conflict = sample;
    ++conflict.latency_us;
    CHECK(ucn_v6_metric_ingest(owner, &key, &conflict) ==
          UCN_V6_ERR_REPLAY);
    conflict.measured_at_us = 99U;
    CHECK(ucn_v6_metric_ingest(owner, &key, &conflict) ==
          UCN_V6_ERR_REPLAY);
    sample.measured_at_us = 200U;
    sample.latency_us = 200U;
    CHECK(ucn_v6_metric_ingest(owner, &key, &sample) == UCN_V6_OK);
    CHECK(ucn_v6_metric_score(owner, 201U, &key, UCN_V6_TRAFFIC_Q0,
                              &q0) == UCN_V6_OK);
    CHECK(q0.algorithm_id == UCN_V6_METRIC_ALGORITHM_DEFAULT &&
          q0.hop_count == 1U && q0.total_cost != 0U);
    hop = q0;
    CHECK(ucn_v6_metric_cost_accumulate(&q0, &hop, &total) == UCN_V6_OK);
    CHECK(total.total_cost == q0.total_cost * 2U && total.hop_count == 2U);
    before = total;
    hop.algorithm_id++;
    CHECK(ucn_v6_metric_cost_accumulate(&q0, &hop, &total) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(&before, &total, sizeof(total)) == 0);
    CHECK(ucn_v6_metric_score(owner, 1000200U, &key,
                              UCN_V6_TRAFFIC_Q0, &q0) ==
          UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_metric_expire(owner, 1000200U) == UCN_V6_OK);
    CHECK(ucn_v6_metric_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.active_paths == 0U && view.samples_ingested == 2U &&
          view.stale_reads == 1U);

    memset(&storage, 0, sizeof(storage));
    owner = NULL;
    policy.class_weights[0].energy = 0U;
    CHECK(ucn_v6_metric_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &policy, &owner) == UCN_V6_OK);
    sample = metric_sample(1U);
    sample.known_mask &= (uint16_t)~UCN_V6_METRIC_ENERGY_KNOWN;
    sample.energy_cost = 0U;
    CHECK(ucn_v6_metric_ingest(owner, &key, &sample) == UCN_V6_OK);
    CHECK(ucn_v6_metric_score(owner, 1U, &key, UCN_V6_TRAFFIC_Q0,
                              &q0) == UCN_V6_OK);
    policy.class_weights[0].energy = 1U;
    return 0;
}

static int test_q1_latest_and_quota(void)
{
    ucn_v6_qos_owner_storage_t storage;
    ucn_v6_qos_owner_t *owner = NULL;
    ucn_v6_qos_policy_t policy;
    ucn_v6_security_open_result_t opened = opened_frame(
        2U, UCN_V6_TRAFFIC_Q1, UCN_V6_DELIVERY_LATEST,
        10U, false, 0U, 0U);
    ucn_v6_qos_enqueue_result_t enqueue;
    ucn_v6_qos_selection_t selection;
    ucn_v6_qos_stats_t stats;

    memset(&storage, 0, sizeof(storage));
    ucn_v6_qos_default_policy(&policy);
    CHECK(ucn_v6_qos_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &policy, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_qos_enqueue(owner, 0U, &opened, 1U, 20U, 2U,
                             &enqueue) == UCN_V6_OK);
    CHECK(enqueue.accepted && !enqueue.replaced_latest);
    CHECK(ucn_v6_qos_enqueue(owner, 1U, &opened, 2U, 21U, 2U,
                             &enqueue) == UCN_V6_OK);
    CHECK(enqueue.replaced_latest && enqueue.replaced_buffer_token == 1U);
    CHECK(ucn_v6_qos_select_next(owner, 2U, &selection) == UCN_V6_OK);
    CHECK(selection.buffer_token == 2U &&
          selection.traffic_class == UCN_V6_TRAFFIC_Q1);
    CHECK(ucn_v6_qos_enqueue(owner, 3U, &opened, 3U, 22U, 2U,
                             &enqueue) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_qos_complete_selection(
              owner, 2U, UCN_V6_QOS_SELECTION_RETRY) == UCN_V6_OK);
    CHECK(ucn_v6_qos_select_next(owner, 4U, &selection) == UCN_V6_OK);
    CHECK(ucn_v6_qos_complete_selection(
              owner, 2U, UCN_V6_QOS_SELECTION_LINK_SUBMITTED) == UCN_V6_OK);
    CHECK(ucn_v6_qos_record_completion(
              owner, 2U, UCN_V6_QOS_COMPLETION_PHYSICAL_COMPLETED) ==
          UCN_V6_OK);
    CHECK(ucn_v6_qos_record_completion(
              owner, 2U, UCN_V6_QOS_COMPLETION_PHYSICAL_COMPLETED) ==
          UCN_V6_OK);
    CHECK(ucn_v6_qos_record_completion(
              owner, 2U, UCN_V6_QOS_COMPLETION_APPLICATION_RESULT) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_qos_record_completion(
              owner, 2U, UCN_V6_QOS_COMPLETION_REMOTE_ACKED) == UCN_V6_OK);
    CHECK(ucn_v6_qos_record_completion(
              owner, 2U, UCN_V6_QOS_COMPLETION_APPLICATION_RESULT) ==
          UCN_V6_OK);
    CHECK(ucn_v6_qos_retire_completion(owner, 2U) == UCN_V6_OK);
    CHECK(ucn_v6_qos_copy_stats(owner, &stats) == UCN_V6_OK);
    CHECK(stats.enqueued[1] == 2U && stats.latest_replaced[1] == 1U &&
          stats.queued[1] == 0U && stats.link_submitted[1] == 1U &&
          stats.physical_completed[1] == 1U &&
          stats.remote_acked[1] == 1U &&
          stats.application_result[1] == 1U && stats.inflight == 0U);

    /* Delivery stays orthogonal to Traffic. Reliable Q1 remains FIFO, while
     * a Latest flow is replaceable even when it uses Q2. */
    opened.frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    CHECK(ucn_v6_qos_enqueue(owner, 10U, &opened, 20U, 10U, 2U,
                             &enqueue) == UCN_V6_OK);
    CHECK(ucn_v6_qos_enqueue(owner, 11U, &opened, 21U, 10U, 2U,
                             &enqueue) == UCN_V6_OK);
    CHECK(!enqueue.replaced_latest);
    opened.frame.traffic_class = UCN_V6_TRAFFIC_Q2;
    opened.frame.delivery_guarantee = UCN_V6_DELIVERY_LATEST;
    CHECK(ucn_v6_qos_enqueue(owner, 12U, &opened, 22U, 10U, 2U,
                             &enqueue) == UCN_V6_OK);
    CHECK(ucn_v6_qos_enqueue(owner, 13U, &opened, 23U, 10U, 2U,
                             &enqueue) == UCN_V6_OK);
    CHECK(enqueue.replaced_latest && enqueue.replaced_buffer_token == 22U);
    return 0;
}

static int test_budget_and_invalidation(void)
{
    ucn_v6_qos_owner_storage_t storage;
    ucn_v6_qos_owner_t *owner = NULL;
    ucn_v6_qos_policy_t policy;
    ucn_v6_security_open_result_t opened = opened_frame(
        3U, UCN_V6_TRAFFIC_Q0, UCN_V6_DELIVERY_RELIABLE,
        11U, true, 1000U, 100U);
    ucn_v6_qos_enqueue_result_t enqueue;
    ucn_v6_qos_selection_t selection;
    ucn_v6_hop_budget_context_t next;
    ucn_v6_hop_budget_context_t before;
    ucn_v6_session_key_t session;
    uint64_t retired[2];
    size_t retired_count = 99U;

    memset(&storage, 0, sizeof(storage));
    ucn_v6_qos_default_policy(&policy);
    CHECK(ucn_v6_qos_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &policy, &owner) == UCN_V6_OK);
    before.initial_budget_us = 77U;
    before.remaining_budget_us = 66U;
    next = before;
    CHECK(ucn_v6_qos_forward_budget(&opened, 1000U, 30U, 20U,
                                    &next) == UCN_V6_OK);
    CHECK(next.initial_budget_us == 1000U && next.remaining_budget_us == 50U);
    next = before;
    CHECK(ucn_v6_qos_forward_budget(&opened, 1000U, 80U, 20U,
                                    &next) == UCN_V6_ERR_TIMEOUT);
    CHECK(memcmp(&next, &before, sizeof(next)) == 0);
    opened.frame.hop_budget.remaining_budget_us = 1001U;
    CHECK(ucn_v6_qos_enqueue(owner, 0U, &opened, 10U, 8U, 0U,
                             &enqueue) == UCN_V6_ERR_ACCESS);
    opened.frame.hop_budget.remaining_budget_us = 100U;
    CHECK(ucn_v6_qos_enqueue(owner, 0U, &opened, 10U, 8U, 0U,
                             &enqueue) == UCN_V6_OK);
    CHECK(ucn_v6_qos_select_next(owner, 100U, &selection) == UCN_V6_OK);
    CHECK(selection.action == UCN_V6_QOS_ACTION_DROP_EXPIRED);
    CHECK(ucn_v6_qos_complete_selection(
              owner, 10U, UCN_V6_QOS_SELECTION_RETRY) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_qos_complete_selection(
              owner, 10U, UCN_V6_QOS_SELECTION_DROP_RETIRED) == UCN_V6_OK);

    opened.frame.flags &= (uint8_t)~UCN_V6_FLAG_HOP_BUDGET_CONTEXT;
    CHECK(ucn_v6_qos_enqueue(owner, 200U, &opened, 11U, 8U, 0U,
                             &enqueue) == UCN_V6_OK);
    session = session_from(&opened);
    CHECK(ucn_v6_qos_invalidate_session(
              owner, &session, retired, 0U, &retired_count) ==
          UCN_V6_ERR_NO_SPACE);
    CHECK(retired_count == 99U);
    CHECK(ucn_v6_qos_invalidate_session(
              owner, &session, retired, 2U, &retired_count) == UCN_V6_OK);
    CHECK(retired_count == 1U && retired[0] == 11U);

    CHECK(ucn_v6_qos_enqueue(owner, 300U, &opened, 12U, 8U, 0U,
                             &enqueue) == UCN_V6_OK);
    CHECK(ucn_v6_qos_select_next(owner, 301U, &selection) == UCN_V6_OK);
    CHECK(ucn_v6_qos_complete_selection(
              owner, 12U, UCN_V6_QOS_SELECTION_LINK_SUBMITTED) ==
          UCN_V6_OK);
    retired_count = 99U;
    CHECK(ucn_v6_qos_invalidate_session(
              owner, &session, retired, 0U, &retired_count) ==
          UCN_V6_ERR_NO_SPACE);
    CHECK(retired_count == 99U);
    CHECK(ucn_v6_qos_invalidate_session(
              owner, &session, retired, 2U, &retired_count) == UCN_V6_OK);
    CHECK(retired_count == 1U && retired[0] == 12U);
    CHECK(ucn_v6_qos_hardware_priority(UCN_V6_TRAFFIC_Q0, 8U) == 7U);
    CHECK(ucn_v6_qos_hardware_priority(UCN_V6_TRAFFIC_Q3, 8U) == 0U);
    return 0;
}

static int test_weighted_fairness(void)
{
    ucn_v6_qos_owner_storage_t storage;
    ucn_v6_qos_owner_t *owner = NULL;
    ucn_v6_qos_policy_t policy;
    ucn_v6_qos_enqueue_result_t enqueue;
    ucn_v6_qos_selection_t selection;
    ucn_v6_security_open_result_t frames[4];
    bool seen[4] = { false, false, false, false };
    size_t index;
    size_t attempts;

    memset(&storage, 0, sizeof(storage));
    ucn_v6_qos_default_policy(&policy);
    policy.q2_quantum_bytes = 64U;
    policy.q3_quantum_bytes = 64U;
    CHECK(ucn_v6_qos_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &policy, &owner) == UCN_V6_OK);
    for (index = 0U; index < 4U; ++index) {
        frames[index] = opened_frame(
            (uint8_t)(10U + index), (ucn_v6_traffic_class_t)index,
            index == 1U ? UCN_V6_DELIVERY_LATEST :
                          UCN_V6_DELIVERY_RELIABLE,
            (uint16_t)(20U + index), false, 0U, 0U);
        CHECK(ucn_v6_qos_enqueue(owner, 0U, &frames[index],
                                 (uint64_t)(100U + index), 64U, 1U,
                                 &enqueue) == UCN_V6_OK);
    }
    for (attempts = 0U; attempts < 16U; ++attempts) {
        CHECK(ucn_v6_qos_select_next(owner, attempts, &selection) ==
              UCN_V6_OK);
        seen[(size_t)selection.traffic_class] = true;
        CHECK(ucn_v6_qos_complete_selection(
                  owner, selection.buffer_token,
                  UCN_V6_QOS_SELECTION_RETRY) == UCN_V6_OK);
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
    return 0;
}

static int test_per_session_flow_quota(void)
{
    ucn_v6_qos_owner_storage_t storage;
    ucn_v6_qos_owner_t *owner = NULL;
    ucn_v6_qos_policy_t policy;
    ucn_v6_security_open_result_t first = opened_frame(
        9U, UCN_V6_TRAFFIC_Q2, UCN_V6_DELIVERY_RELIABLE,
        30U, false, 0U, 0U);
    ucn_v6_security_open_result_t second = first;
    ucn_v6_qos_enqueue_result_t enqueue;
    ucn_v6_qos_stats_t stats;

    memset(&storage, 0, sizeof(storage));
    ucn_v6_qos_default_policy(&policy);
    policy.max_flows_per_session = 1U;
    CHECK(ucn_v6_qos_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &policy, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_qos_enqueue(owner, 0U, &first, 40U, 10U, 1U,
                             &enqueue) == UCN_V6_OK);
    second.frame.message.source_endpoint = 31U;
    CHECK(ucn_v6_qos_enqueue(owner, 1U, &second, 41U, 10U, 1U,
                             &enqueue) == UCN_V6_ERR_NO_SPACE);
    CHECK(ucn_v6_qos_copy_stats(owner, &stats) == UCN_V6_OK);
    CHECK(stats.flow_slots == 1U && stats.queued[2] == 1U &&
          stats.rejected_quota[2] == 1U);
    return 0;
}

int main(void)
{
    CHECK(test_metric() == 0);
    CHECK(test_q1_latest_and_quota() == 0);
    CHECK(test_budget_and_invalidation() == 0);
    CHECK(test_weighted_fairness() == 0);
    CHECK(test_per_session_flow_quota() == 0);
    puts("v6 metric/qos tests passed");
    return 0;
}
