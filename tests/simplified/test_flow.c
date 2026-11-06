#include "internal/ucn_flow.h"

#include "rust/tests/conformance/v6s_routing_flow_v1.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr_)                                                         \
    do {                                                                     \
        if (!(expr_)) {                                                      \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #expr_);                                       \
            return 1;                                                        \
        }                                                                    \
    } while (0)

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

static int any_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return 1;
        }
    }
    return 0;
}

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = context;
    lock->held = 0U;
}

static ucn_i_lock_ops_t lock_ops(test_lock_t *lock)
{
    ucn_i_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = lock_enter;
    ops.leave = lock_leave;
    return ops;
}

static ucn_i_route_binding_t binding(uint32_t address, uint8_t byte)
{
    ucn_i_route_binding_t value;
    memset(&value, 0, sizeof(value));
    value.address = address;
    value.generation = 1U;
    memset(value.principal, byte, sizeof(value.principal));
    return value;
}

static ucn_i_route_link_ref_t link_ref(ucn_i_route_binding_t peer)
{
    ucn_i_route_link_ref_t value;
    memset(&value, 0, sizeof(value));
    value.peer = peer;
    value.link_generation = 8U;
    value.cost = 20U;
    value.link_id = 2U;
    value.frame_mtu = 512U;
    value.capability_bits = 3U;
    return value;
}

static int test_frozen_codecs(void)
{
    const uint8_t setup_golden[] = { UCN_V6S_FLOW_LABEL_SETUP_BYTES };
    const uint8_t c2_golden[] = { UCN_V6S_FLOW_C2_PREFIX_BYTES };
    const uint8_t c3_golden[] = { UCN_V6S_FLOW_C3_PREFIX_BYTES };
    const uint8_t c4_golden[] = { UCN_V6S_FLOW_C4_PREFIX_BYTES };
    uint8_t bytes[UCN_I_FLOW_LABEL_SETUP_BYTES];
    uint8_t sentinel[UCN_I_FLOW_LABEL_SETUP_BYTES];
    size_t output_bytes = 0U;
    ucn_i_flow_label_setup_t setup;
    ucn_i_flow_prefix_t prefix;

    memset(&setup, 0, sizeof(setup));
    setup.candidate_id = UINT32_C(0x1122);
    setup.route_generation = UINT32_C(0x33445566);
    setup.reverse_label = UINT16_C(0x7788);
    setup.forward_label = UINT16_C(0x99AA);
    setup.path_profile_id = UINT16_C(0xBBCC);
    setup.context_digest = UINT32_C(0xDDEEF001);
    CHECK(ucn_i_flow_label_setup_encode(&setup, bytes) == UCN_OK);
    CHECK(memcmp(bytes, setup_golden, sizeof(setup_golden)) == 0);
    memset(&setup, 0, sizeof(setup));
    CHECK(ucn_i_flow_label_setup_decode(setup_golden, &setup) == UCN_OK);
    CHECK(setup.context_digest == UINT32_C(0xDDEEF001));

    CHECK(ucn_i_flow_prefix_decode(c2_golden, sizeof(c2_golden),
                                   &prefix) == UCN_OK);
    CHECK(prefix.header.contract == 2U);
    CHECK(ucn_i_flow_prefix_encode(&prefix, bytes, sizeof(c2_golden),
                                   &output_bytes) == UCN_OK);
    CHECK(output_bytes == sizeof(c2_golden));
    CHECK(memcmp(bytes, c2_golden, sizeof(c2_golden)) == 0);
    CHECK(ucn_i_flow_prefix_decode(c3_golden, sizeof(c3_golden),
                                   &prefix) == UCN_OK);
    CHECK(ucn_i_flow_prefix_encode(&prefix, bytes, sizeof(c3_golden),
                                   &output_bytes) == UCN_OK);
    CHECK(memcmp(bytes, c3_golden, sizeof(c3_golden)) == 0);
    CHECK(ucn_i_flow_prefix_decode(c4_golden, sizeof(c4_golden),
                                   &prefix) == UCN_OK);
    CHECK(ucn_i_flow_prefix_encode(&prefix, bytes, sizeof(c4_golden),
                                   &output_bytes) == UCN_OK);
    CHECK(memcmp(bytes, c4_golden, sizeof(c4_golden)) == 0);

    memset(sentinel, 0xA5, sizeof(sentinel));
    memcpy(bytes, sentinel, sizeof(bytes));
    prefix.header.contract = 3U;
    prefix.header.delivery = 2U;
    CHECK(ucn_i_flow_prefix_encode(&prefix, bytes, sizeof(c3_golden),
                                   &output_bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, sentinel, sizeof(bytes)) == 0);
    return 0;
}

static int test_advanced_flow_control_codecs(void)
{
    static const uint8_t key_golden[UCN_I_FLOW_PATH_KEY_BYTES] = {
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU,
        0x12U, 0x34U, 0x01U, 0x02U, 0x03U, 0x04U, 0x11U, 0x22U,
        0x33U, 0x44U, 0x55U, 0x66U
    };
    uint8_t bytes[UCN_I_FLOW_PATH_PROBE_BYTES];
    uint8_t sentinel[UCN_I_FLOW_PATH_PROBE_BYTES];
    ucn_i_flow_path_key_body_t key;
    ucn_i_flow_path_key_body_t decoded_key;
    ucn_i_flow_path_probe_body_t probe;
    ucn_i_flow_path_probe_body_t decoded_probe;
    ucn_i_flow_probe_ack_t ack;
    ucn_i_flow_probe_ack_t decoded_ack;
    ucn_i_flow_path_outcome_body_t outcome;
    ucn_i_flow_path_outcome_body_t decoded_outcome;
    size_t index;

    memset(&key, 0, sizeof(key));
    for (index = 0U; index < sizeof(key.proposal_digest); ++index) {
        key.proposal_digest[index] = (uint8_t)index;
    }
    key.candidate_id = UINT16_C(0x1234);
    key.route_generation = UINT32_C(0x01020304);
    key.reverse_label = UINT16_C(0x1122);
    key.forward_label = UINT16_C(0x3344);
    key.path_profile_id = UINT16_C(0x5566);
    CHECK(ucn_i_flow_path_key_encode(&key, bytes) == UCN_OK);
    CHECK(memcmp(bytes, key_golden, sizeof(key_golden)) == 0);
    memset(&decoded_key, 0, sizeof(decoded_key));
    CHECK(ucn_i_flow_path_key_decode(key_golden, &decoded_key) == UCN_OK);
    CHECK(memcmp(&decoded_key, &key, sizeof(key)) == 0);

    memset(&probe, 0, sizeof(probe));
    probe.key = key;
    probe.context_id = UINT16_C(0x7788);
    probe.payload_budget = 256U;
    probe.path_frame_mtu = 512U;
    probe.capability_bits = UINT16_C(0xA55A);
    probe.flow_expires_at_us = UINT64_C(0x0102030405060708);
    CHECK(ucn_i_flow_path_probe_encode(&probe, bytes) == UCN_OK);
    CHECK(memcmp(bytes, key_golden, sizeof(key_golden)) == 0);
    CHECK(bytes[28] == 0x77U && bytes[29] == 0x88U);
    CHECK(bytes[30] == 0x01U && bytes[31] == 0x00U);
    CHECK(bytes[32] == 0x02U && bytes[33] == 0x00U);
    CHECK(bytes[34] == 0xA5U && bytes[35] == 0x5AU);
    CHECK(bytes[36] == 0x01U && bytes[43] == 0x08U);
    memset(&decoded_probe, 0, sizeof(decoded_probe));
    CHECK(ucn_i_flow_path_probe_decode(bytes, &decoded_probe) == UCN_OK);
    CHECK(memcmp(&decoded_probe, &probe, sizeof(probe)) == 0);

    memset(&ack, 0, sizeof(ack));
    memcpy(ack.proposal_digest, key.proposal_digest, 16U);
    ack.candidate_id = key.candidate_id;
    ack.route_generation = key.route_generation;
    ack.measured_rtt_us = UINT32_C(0x10203040);
    ack.path_frame_mtu = probe.path_frame_mtu;
    ack.capability_bits = probe.capability_bits;
    CHECK(ucn_i_flow_path_probe_ack_encode(&ack, bytes) == UCN_OK);
    memset(&decoded_ack, 0, sizeof(decoded_ack));
    CHECK(ucn_i_flow_path_probe_ack_decode(bytes, &decoded_ack) == UCN_OK);
    CHECK(memcmp(&decoded_ack, &ack, sizeof(ack)) == 0);

    memset(&outcome, 0, sizeof(outcome));
    outcome.key = key;
    outcome.phase_or_reason = UCN_I_FLOW_ABORT_TIMEOUT;
    CHECK(ucn_i_flow_path_outcome_encode(&outcome, false, bytes) == UCN_OK);
    CHECK(bytes[28] == UCN_I_FLOW_ABORT_TIMEOUT && bytes[29] == 0U);
    memset(&decoded_outcome, 0, sizeof(decoded_outcome));
    CHECK(ucn_i_flow_path_outcome_decode(bytes, false,
                                         &decoded_outcome) == UCN_OK);
    CHECK(memcmp(&decoded_outcome, &outcome, sizeof(outcome)) == 0);
    outcome.phase_or_reason = UCN_I_FLOW_TERMINAL_ACTIVE;
    CHECK(ucn_i_flow_path_outcome_encode(&outcome, true, bytes) == UCN_OK);
    CHECK(ucn_i_flow_path_outcome_decode(bytes, true,
                                         &decoded_outcome) == UCN_OK);

    memset(sentinel, 0xA5, sizeof(sentinel));
    memcpy(bytes, sentinel, sizeof(bytes));
    key.forward_label = key.reverse_label;
    CHECK(ucn_i_flow_path_key_encode(&key, bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, sentinel, sizeof(bytes)) == 0);
    memcpy(bytes, key_golden, sizeof(key_golden));
    memset(bytes, 0, 16U);
    memset(&decoded_key, 0xA5, sizeof(decoded_key));
    CHECK(ucn_i_flow_path_key_decode(bytes, &decoded_key) ==
          UCN_ERR_MALFORMED);
    for (index = 0U; index < sizeof(decoded_key); ++index) {
        CHECK(((const uint8_t *)&decoded_key)[index] == 0xA5U);
    }
    return 0;
}

static void make_fixture(ucn_i_flow_config_t *config,
                         ucn_i_route_soft_view_t *route,
                         ucn_i_flow_capability_facts_t *capability,
                         ucn_i_flow_requirements_t *requirements,
                         ucn_i_flow_current_facts_t *facts)
{
    ucn_i_route_binding_t local = binding(1U, 0x11U);
    ucn_i_route_binding_t peer = binding(2U, 0x22U);
    ucn_i_route_binding_t destination = binding(3U, 0x33U);
    ucn_i_route_link_ref_t link = link_ref(peer);

    memset(config, 0, sizeof(*config));
    config->local = local;
    config->probe_lifetime_us = 1000U;
    config->stage_lifetime_us = 1000U;
    config->commit_lifetime_us = 1000U;
    config->flow_lifetime_us = 5000U;
    config->receipt_lifetime_us = 2000U;
    config->first_transaction_id = 100U;
    config->runtime_instance = 7U;
    config->realm = 9U;
    config->policy_generation = 4U;
    config->first_candidate_id = 10U;
    config->first_route_generation = 20U;
    config->first_flow_generation = 30U;
    config->owner_instance = 12U;
    config->first_context_id = 40U;
    config->first_label = 50U;

    memset(route, 0, sizeof(*route));
    route->domain.origin = local;
    route->domain.destination = destination;
    route->domain.realm = 9U;
    route->domain.origin_session_generation = 5U;
    route->next_hop = link;
    route->route_causal_id = 200U;
    route->expires_at_us = 10000U;
    route->runtime_instance = 7U;
    route->route_generation = 8U;
    route->cost = 40U;
    route->owner_instance = 13U;
    route->path_frame_mtu = 512U;
    route->capability_bits = 3U;
    route->hop_count = 2U;

    memset(capability, 0, sizeof(*capability));
    memset(capability->digest, 0xC5, sizeof(capability->digest));
    capability->expires_at_us = 9000U;
    capability->runtime_instance = 7U;
    capability->session_generation = 6U;
    capability->capability_generation = 7U;
    capability->feature_bits = 3U;
    capability->security_owner_instance = 14U;

    memset(requirements, 0, sizeof(*requirements));
    requirements->expires_at_us = 8000U;
    requirements->required_feature_bits = 1U;
    requirements->policy_generation = 4U;
    requirements->service_id = 11U;
    requirements->minimum_payload_bytes = 32U;
    requirements->path_profile_id = 15U;
    requirements->contract = 4U;
    requirements->traffic_ceiling = 2U;
    requirements->delivery = 0U;
    requirements->interaction = 0U;
    requirements->payload_kind = 0U;
    requirements->origin_security = 1U;
    requirements->hop_profile = 1U;

    memset(facts, 0, sizeof(*facts));
    facts->local = local;
    facts->destination = destination;
    facts->next_hop = link;
    memcpy(facts->capability_digest, capability->digest, 16U);
    facts->now_us = 100U;
    facts->route_causal_id = route->route_causal_id;
    facts->route_deadline_us = route->expires_at_us;
    facts->capability_deadline_us = capability->expires_at_us;
    facts->route_runtime_instance = route->runtime_instance;
    facts->route_generation = route->route_generation;
    facts->origin_session_generation = 5U;
    facts->capability_session_generation = 6U;
    facts->capability_generation = 7U;
    facts->policy_generation = 4U;
    facts->route_owner_instance = route->owner_instance;
    facts->capability_security_owner_instance = 14U;
}

static int test_origin_activation_and_use(void)
{
    ucn_i_flow_owner_t owner;
    test_lock_t lock = {0};
    ucn_i_lock_ops_t ops = lock_ops(&lock);
    ucn_i_flow_config_t config;
    ucn_i_route_soft_view_t route;
    ucn_i_flow_capability_facts_t capability;
    ucn_i_flow_requirements_t requirements;
    ucn_i_flow_current_facts_t facts;
    ucn_i_flow_proposal_t proposal;
    ucn_i_flow_probe_ack_t probe_ack;
    ucn_i_flow_label_setup_t setup;
    ucn_i_flow_activation_key_t key;
    ucn_i_flow_tx_request_t request;
    ucn_i_flow_tx_plan_t plan;
    ucn_i_flow_tx_plan_t sentinel;
    ucn_handle_t candidate;
    ucn_handle_t activation;
    ucn_handle_t flow;
    ucn_i_flow_phase_t phase;
    uint16_t candidates;
    uint16_t activations;
    uint16_t flows;
    uint16_t receipts;
    uint16_t cursor_before;
    uint32_t reservation_generation;

    make_fixture(&config, &route, &capability, &requirements, &facts);
    memset(&owner, 0, sizeof(owner));
    CHECK(ucn_i_flow_owner_init(&owner, &config, &ops) == UCN_OK);
    CHECK(ucn_i_flow_import_candidate(&owner, &route, &capability,
                                      &requirements, 10U,
                                      &candidate) == UCN_OK);
    CHECK(ucn_i_flow_begin_probe(&owner, candidate, 20U,
                                 &proposal) == UCN_OK);
    CHECK(proposal.key.candidate_id == 10U);
    CHECK(proposal.key.route_generation == 20U);
    CHECK(any_nonzero(proposal.key.proposal_digest, 16U));
    memset(&probe_ack, 0, sizeof(probe_ack));
    memcpy(probe_ack.proposal_digest, proposal.key.proposal_digest, 16U);
    probe_ack.candidate_id = proposal.key.candidate_id;
    probe_ack.route_generation = proposal.key.route_generation;
    probe_ack.measured_rtt_us = 30U;
    probe_ack.path_frame_mtu = proposal.source_route.path_frame_mtu;
    probe_ack.capability_bits = proposal.source_route.capability_bits;
    probe_ack.proposal_digest[0] ^= 1U;
    CHECK(ucn_i_flow_accept_probe_ack(&owner, candidate, &probe_ack,
                                      30U) == UCN_ERR_STATE);
    CHECK(ucn_i_flow_phase(&owner, candidate, &phase) == UCN_OK);
    CHECK(phase == UCN_I_FLOW_PROBING);
    probe_ack.proposal_digest[0] ^= 1U;
    CHECK(ucn_i_flow_accept_probe_ack(&owner, candidate, &probe_ack,
                                      30U) == UCN_OK);
    facts.now_us = 40U;
    CHECK(ucn_i_flow_begin_stage(&owner, candidate, &facts,
                                 &activation, &setup) == UCN_OK);
    CHECK(setup.context_digest != 0U);
    key = proposal.key;
    CHECK(ucn_i_flow_accept_stage_ack(&owner, activation, &key,
                                      &facts) == UCN_ERR_STATE);
    CHECK(ucn_i_flow_stage_submit(&owner, activation,
                                  UCN_I_FLOW_NOT_SUBMITTED,
                                  40U) == UCN_OK);
    CHECK(ucn_i_flow_accept_stage_ack(&owner, activation, &key,
                                      &facts) == UCN_ERR_STATE);
    CHECK(ucn_i_flow_stage_submit(&owner, activation,
                                  UCN_I_FLOW_SUBMITTED,
                                  45U) == UCN_OK);
    facts.now_us = 50U;
    CHECK(ucn_i_flow_accept_stage_ack(&owner, activation, &key,
                                      &facts) == UCN_OK);
    CHECK(ucn_i_flow_begin_commit(&owner, activation, &facts,
                                  &key) == UCN_OK);
    CHECK(ucn_i_flow_commit_submit(&owner, activation,
                                   UCN_I_FLOW_NOT_SUBMITTED,
                                   50U) == UCN_OK);
    CHECK(ucn_i_flow_phase(&owner, activation, &phase) == UCN_OK);
    CHECK(phase == UCN_I_FLOW_STAGED);
    CHECK(ucn_i_flow_commit_submit(&owner, activation,
                                   UCN_I_FLOW_SUBMITTED,
                                   55U) == UCN_OK);
    key.route_generation++;
    CHECK(ucn_i_flow_accept_commit_ack(&owner, activation, &key, &facts,
                                       &flow) == UCN_ERR_STATE);
    key.route_generation--;
    facts.now_us = 60U;
    CHECK(ucn_i_flow_accept_commit_ack(&owner, activation, &key, &facts,
                                       &flow) == UCN_OK);
    CHECK(ucn_i_flow_counts(&owner, &candidates, &activations, &flows,
                            &receipts) == UCN_OK);
    CHECK(candidates == 0U);
    CHECK(activations == 1U);
    CHECK(flows == 1U);
    CHECK(receipts == 1U);

    memset(&request, 0, sizeof(request));
    request.service_id = requirements.service_id;
    request.contract = requirements.contract;
    request.traffic_class = 1U;
    request.delivery = requirements.delivery;
    request.interaction = requirements.interaction;
    request.payload_kind = requirements.payload_kind;
    request.origin_security = requirements.origin_security;
    request.payload_bytes = 64U;
    facts.now_us = 70U;
    CHECK(ucn_i_flow_use_preflight(&owner, flow, &facts, &request,
                                   &plan) == UCN_OK);
    CHECK(plan.flow_generation == 30U);
    CHECK(plan.sequence == 1U);
    CHECK(plan.reservation_generation == 1U);
    reservation_generation = plan.reservation_generation;
    memset(&sentinel, 0xA5, sizeof(sentinel));
    plan = sentinel;
    CHECK(ucn_i_flow_use_preflight(&owner, flow, &facts, &request,
                                   &plan) == UCN_ERR_EXHAUSTED);
    CHECK(memcmp(&plan, &sentinel, sizeof(plan)) == 0);
    CHECK(ucn_i_flow_sequence_abort(&owner, flow, 1U,
                                    reservation_generation) == UCN_OK);
    CHECK(ucn_i_flow_use_preflight(&owner, flow, &facts, &request,
                                   &plan) == UCN_OK);
    CHECK(plan.sequence == 1U);
    CHECK(plan.reservation_generation == 2U);
    CHECK(ucn_i_flow_sequence_commit(&owner, flow, 1U,
                                     reservation_generation) ==
          UCN_ERR_STATE);
    CHECK(ucn_i_flow_sequence_commit(&owner, flow, 1U,
                                     plan.reservation_generation) == UCN_OK);
    CHECK(ucn_i_flow_use_preflight(&owner, flow, &facts, &request,
                                   &plan) == UCN_OK);
    CHECK(plan.sequence == 2U);
    CHECK(ucn_i_flow_sequence_abort(&owner, flow, 2U,
                                    plan.reservation_generation) == UCN_OK);

    facts.capability_generation++;
    plan = sentinel;
    CHECK(ucn_i_flow_use_preflight(&owner, flow, &facts, &request,
                                   &plan) == UCN_ERR_ACCESS);
    CHECK(memcmp(&plan, &sentinel, sizeof(plan)) == 0);
    facts.capability_generation--;
    facts.route_generation++;
    plan = sentinel;
    CHECK(ucn_i_flow_use_preflight(&owner, flow, &facts, &request,
                                   &plan) == UCN_ERR_ACCESS);
    CHECK(memcmp(&plan, &sentinel, sizeof(plan)) == 0);
    facts.route_generation--;
    CHECK(ucn_i_flow_fence(&owner, flow) == UCN_OK);
    CHECK(ucn_i_flow_phase(&owner, flow, &phase) == UCN_OK);
    CHECK(phase == UCN_I_FLOW_FENCED);
    cursor_before = owner.maintenance_cursor;
    CHECK(ucn_i_flow_counts(&owner, &owner.maintenance_cursor, &activations,
                            &flows, &receipts) == UCN_ERR_ARGUMENT);
    CHECK(owner.maintenance_cursor == cursor_before);
    return 0;
}

static int test_commit_timeout_is_in_doubt(void)
{
    ucn_i_flow_owner_t owner;
    test_lock_t lock = {0};
    ucn_i_lock_ops_t ops = lock_ops(&lock);
    ucn_i_flow_config_t config;
    ucn_i_route_soft_view_t route;
    ucn_i_flow_capability_facts_t capability;
    ucn_i_flow_requirements_t requirements;
    ucn_i_flow_current_facts_t facts;
    ucn_i_flow_proposal_t proposal;
    ucn_i_flow_probe_ack_t ack;
    ucn_i_flow_label_setup_t setup;
    ucn_i_flow_activation_key_t key;
    ucn_handle_t candidate;
    ucn_handle_t activation;
    ucn_i_flow_phase_t phase;

    make_fixture(&config, &route, &capability, &requirements, &facts);
    config.stage_lifetime_us = 100U;
    config.commit_lifetime_us = 100U;
    memset(&owner, 0, sizeof(owner));
    CHECK(ucn_i_flow_owner_init(&owner, &config, &ops) == UCN_OK);
    CHECK(ucn_i_flow_import_candidate(&owner, &route, &capability,
                                      &requirements, 10U,
                                      &candidate) == UCN_OK);
    CHECK(ucn_i_flow_begin_probe(&owner, candidate, 20U,
                                 &proposal) == UCN_OK);
    memset(&ack, 0, sizeof(ack));
    memcpy(ack.proposal_digest, proposal.key.proposal_digest, 16U);
    ack.candidate_id = proposal.key.candidate_id;
    ack.route_generation = proposal.key.route_generation;
    ack.measured_rtt_us = 1U;
    ack.path_frame_mtu = proposal.source_route.path_frame_mtu;
    ack.capability_bits = proposal.source_route.capability_bits;
    CHECK(ucn_i_flow_accept_probe_ack(&owner, candidate, &ack, 30U) == UCN_OK);
    facts.now_us = 40U;
    CHECK(ucn_i_flow_begin_stage(&owner, candidate, &facts,
                                 &activation, &setup) == UCN_OK);
    CHECK(ucn_i_flow_stage_submit(&owner, activation,
                                  UCN_I_FLOW_SUBMITTED,
                                  40U) == UCN_OK);
    key = proposal.key;
    CHECK(ucn_i_flow_accept_stage_ack(&owner, activation, &key,
                                      &facts) == UCN_OK);
    CHECK(ucn_i_flow_commit_submit(&owner, activation,
                                   UCN_I_FLOW_SUBMITTED,
                                   50U) == UCN_OK);
    CHECK(ucn_i_flow_expire_activation(&owner, activation, 240U,
                                       &phase) == UCN_OK);
    CHECK(phase == UCN_I_FLOW_IN_DOUBT);
    return 0;
}

static int test_late_stage_submit_is_terminal(void)
{
    ucn_i_flow_owner_t owner;
    test_lock_t lock = {0};
    ucn_i_lock_ops_t ops = lock_ops(&lock);
    ucn_i_flow_config_t config;
    ucn_i_route_soft_view_t route;
    ucn_i_flow_capability_facts_t capability;
    ucn_i_flow_requirements_t requirements;
    ucn_i_flow_current_facts_t facts;
    ucn_i_flow_proposal_t proposal;
    ucn_i_flow_probe_ack_t ack;
    ucn_i_flow_label_setup_t setup;
    ucn_handle_t candidate;
    ucn_handle_t activation;
    ucn_i_flow_phase_t phase;

    make_fixture(&config, &route, &capability, &requirements, &facts);
    config.stage_lifetime_us = 100U;
    memset(&owner, 0, sizeof(owner));
    CHECK(ucn_i_flow_owner_init(&owner, &config, &ops) == UCN_OK);
    CHECK(ucn_i_flow_import_candidate(&owner, &route, &capability,
                                      &requirements, 10U,
                                      &candidate) == UCN_OK);
    CHECK(ucn_i_flow_begin_probe(&owner, candidate, 20U,
                                 &proposal) == UCN_OK);
    memset(&ack, 0, sizeof(ack));
    memcpy(ack.proposal_digest, proposal.key.proposal_digest, 16U);
    ack.candidate_id = proposal.key.candidate_id;
    ack.route_generation = proposal.key.route_generation;
    ack.measured_rtt_us = 1U;
    ack.path_frame_mtu = proposal.source_route.path_frame_mtu;
    ack.capability_bits = proposal.source_route.capability_bits;
    CHECK(ucn_i_flow_accept_probe_ack(&owner, candidate, &ack, 30U) == UCN_OK);
    facts.now_us = 40U;
    CHECK(ucn_i_flow_begin_stage(&owner, candidate, &facts,
                                 &activation, &setup) == UCN_OK);
    CHECK(ucn_i_flow_stage_submit(&owner, activation,
                                  UCN_I_FLOW_SUBMITTED,
                                  140U) == UCN_OK);
    CHECK(ucn_i_flow_phase(&owner, activation, &phase) == UCN_OK);
    CHECK(phase == UCN_I_FLOW_IN_DOUBT);
    return 0;
}

int main(void)
{
    CHECK(test_frozen_codecs() == 0);
    CHECK(test_advanced_flow_control_codecs() == 0);
    CHECK(test_origin_activation_and_use() == 0);
    CHECK(test_commit_timeout_is_in_doubt() == 0);
    CHECK(test_late_stage_submit_is_terminal() == 0);
    puts("ucn_v6s_flow_tests: PASS");
    return 0;
}
