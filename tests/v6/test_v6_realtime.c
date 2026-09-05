#include "ucn/v6/ucn_v6_realtime.h"

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

typedef struct fake_store {
    bool valid;
    ucn_v6_realtime_domain_record_t record;
    unsigned loads;
    unsigned reserves;
    bool fail_reserve;
    bool false_success;
} fake_store_t;

static void gate_lock(void *context)
{
    (void)context;
}

static void gate_unlock(void *context)
{
    (void)context;
}

static ucn_v6_principal_t principal(uint8_t seed)
{
    ucn_v6_principal_t value;
    size_t index;
    for (index = 0U; index < sizeof(value.bytes); ++index) {
        value.bytes[index] = (uint8_t)(seed + index);
    }
    return value;
}

static ucn_v6_result_t load_domain_record(
    void *context,
    const ucn_v6_principal_t *master,
    uint16_t domain,
    ucn_v6_realtime_domain_record_t *record)
{
    fake_store_t *store = (fake_store_t *)context;
    (void)master;
    (void)domain;
    ++store->loads;
    if (!store->valid ||
        memcmp(store->record.config.master_principal.bytes, master->bytes,
               sizeof(master->bytes)) != 0 ||
        store->record.config.clock_domain_id != domain) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *record = store->record;
    return UCN_V6_OK;
}

static ucn_v6_result_t reserve_domain_record(
    void *context, const ucn_v6_realtime_domain_record_t *record)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->reserves;
    if (store->fail_reserve) {
        return UCN_V6_ERR_STATE;
    }
    if (!store->false_success) {
        store->valid = true;
        store->record = *record;
    }
    return UCN_V6_OK;
}

static ucn_v6_cached_peer_capability_t master_capability(
    const ucn_v6_principal_t *master,
    const ucn_v6_binding_key_t *binding,
    uint32_t generation)
{
    ucn_v6_cached_peer_capability_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.principal = *master;
    value.binding = *binding;
    value.session_generation = 3U;
    value.record.peer.feature_bits = UCN_V6_FEATURE_REALTIME;
    value.record.peer.realtime_mode_bits =
        UCN_V6_REALTIME_MODE_SYNCED | UCN_V6_REALTIME_MODE_DEADLINE;
    value.record.peer.clock_domain_id = 7U;
    value.record.peer.clock_domain_generation = generation;
    return value;
}

static ucn_v6_path_capability_t fixed_path(
    const ucn_v6_principal_t *master,
    const ucn_v6_binding_key_t *binding)
{
    ucn_v6_path_capability_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.immutable_for_realtime = true;
    value.destination_principal = *master;
    value.destination_binding = *binding;
    value.session_generation = 3U;
    value.route_generation = 5U;
    value.path_id = 2U;
    value.path_generation = 4U;
    value.feature_bits = UCN_V6_FEATURE_REALTIME;
    value.timestamp_capability_bits =
        UCN_V6_TIMESTAMP_RX_HARDWARE | UCN_V6_TIMESTAMP_TX_HARDWARE;
    value.timestamp_uncertainty_us = 4U;
    return value;
}

static ucn_v6_time_domain_config_t domain_config(
    const ucn_v6_principal_t *master,
    const ucn_v6_binding_key_t *binding,
    uint32_t generation)
{
    ucn_v6_time_domain_config_t value;
    memset(&value, 0, sizeof(value));
    value.clock_domain_id = 7U;
    value.domain_generation = generation;
    value.master_principal = *master;
    value.master_binding = *binding;
    value.master_session_generation = 3U;
    value.route_generation = 5U;
    value.path_id = 2U;
    value.path_generation = 4U;
    value.lock_sample_count = 2U;
    value.sync_timeout_us = 1000U;
    value.max_holdover_us = 2000U;
    value.max_offset_jump_us = 500U;
    value.oscillator_uncertainty_ppb = 1000U;
    return value;
}

static ucn_v6_realtime_uncertainty_t uncertainty(void)
{
    ucn_v6_realtime_uncertainty_t value;
    memset(&value, 0, sizeof(value));
    value.timer_resolution_bound_us = 1U;
    value.link_timestamp_capture_bound_us = 1U;
    value.filter_residual_bound_us = 1U;
    value.arithmetic_rounding_bound_us = 1U;
    value.sample_capture_bound_us = 1U;
    value.path_asymmetry_bound_us = 1U;
    value.known_mask = UCN_V6_REALTIME_KN_ALL;
    return value;
}

static ucn_v6_security_open_result_t opened_sample(
    const ucn_v6_principal_t *master,
    const ucn_v6_binding_key_t *binding)
{
    ucn_v6_security_open_result_t value;
    memset(&value, 0, sizeof(value));
    value.authenticated_principal = *master;
    value.hop_authenticated = true;
    value.endpoint_authorized = true;
    value.frame.flags = UCN_V6_FLAG_E2E_CONTEXT |
                        UCN_V6_FLAG_PROTOCOL_CONTEXT |
                        UCN_V6_FLAG_ROUTE_CONTEXT |
                        UCN_V6_FLAG_PATH_CONTEXT |
                        UCN_V6_FLAG_MESSAGE_CONTEXT;
    value.frame.source_address = binding->node_address;
    value.frame.frame_type = UCN_V6_FRAME_CONTROL;
    value.frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TIME_FOLLOW_UP;
    value.frame.source_binding_generation = binding->binding_generation;
    value.frame.session_generation = 3U;
    value.frame.route_generation = 5U;
    value.frame.path.path_id = 2U;
    value.frame.path.path_generation = 4U;
    return value;
}

static int test_codec_and_uncertainty(void)
{
    ucn_v6_realtime_envelope_t envelope;
    ucn_v6_realtime_envelope_t decoded;
    ucn_v6_realtime_uncertainty_t components = uncertainty();
    uint8_t bytes[UCN_V6_REALTIME_ENVELOPE_BYTES];
    uint8_t before[UCN_V6_REALTIME_ENVELOPE_BYTES];
    uint32_t bound = 0U;
    ucn_v6_time_sync_sample_t sample;
    ucn_v6_time_sync_sample_t sample_decoded;
    uint8_t sample_bytes[UCN_V6_TIME_SYNC_SAMPLE_BYTES];
    memset(&envelope, 0, sizeof(envelope));
    envelope.mode = UCN_V6_REALTIME_DEADLINE;
    envelope.uncertainty_class = 4U;
    envelope.sample_capture_hardware = true;
    envelope.domain_time_valid = true;
    envelope.clock_domain_id = 7U;
    envelope.domain_generation = 2U;
    envelope.capture_time_us = 1234U;
    CHECK(ucn_v6_realtime_envelope_encode(&envelope, bytes) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_envelope_decode(bytes, sizeof(bytes), &decoded) ==
          UCN_V6_OK);
    CHECK(memcmp(&decoded, &envelope, sizeof(envelope)) == 0);
    CHECK(ucn_v6_realtime_uncertainty_aggregate(&components, &bound) ==
          UCN_V6_OK && bound == 6U);
    components.sample_capture_bound_us = 0U;
    CHECK(ucn_v6_realtime_uncertainty_aggregate(&components, &bound) ==
          UCN_V6_ERR_ARGUMENT);
    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(before));
    envelope.domain_generation = 0U;
    CHECK(ucn_v6_realtime_envelope_encode(&envelope, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    envelope.domain_generation = 2U;
    envelope.mode = (ucn_v6_realtime_mode_t)-1;
    CHECK(ucn_v6_realtime_envelope_encode(&envelope, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    memset(&sample, 0, sizeof(sample));
    sample.clock_domain_id = 7U;
    sample.domain_generation = 2U;
    sample.local_sample_us = 123U;
    sample.offset_us = -45;
    sample.uncertainty = uncertainty();
    CHECK(ucn_v6_time_sync_sample_encode(&sample, sample_bytes) == UCN_V6_OK);
    CHECK(ucn_v6_time_sync_sample_decode(sample_bytes, sizeof(sample_bytes),
                                         &sample_decoded) == UCN_V6_OK);
    CHECK(memcmp(&sample, &sample_decoded, sizeof(sample)) == 0);
    sample_bytes[47] = 1U;
    CHECK(ucn_v6_time_sync_sample_decode(sample_bytes, sizeof(sample_bytes),
                                         &sample_decoded) ==
          UCN_V6_ERR_MALFORMED);
    return 0;
}

static int test_fixed_path_domain_and_dual_gate(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate;
    ucn_v6_principal_t master = principal(0x20U);
    ucn_v6_binding_key_t binding = { 1U, 9U, 2U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 2U);
    ucn_v6_path_capability_t path = fixed_path(&master, &binding);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 2U);
    ucn_v6_security_open_result_t opened = opened_sample(&master, &binding);
    ucn_v6_time_sync_sample_t sample;
    ucn_v6_realtime_endpoint_policy_t policy;
    ucn_v6_realtime_send_result_t send;
    ucn_v6_realtime_receive_view_t receive;
    ucn_v6_realtime_clock_view_t clock;
    ucn_v6_realtime_view_t owner_view;
    ucn_v6_realtime_view_t owner_view_before;
    const uint8_t business[] = { 1U, 2U, 3U, 4U };
    const uint8_t *admitted = NULL;
    size_t admitted_length = 0U;
    uint8_t payload[64];
    uint8_t sentinel[64];
    uint8_t sample_payload[UCN_V6_TIME_SYNC_SAMPLE_BYTES];

    memset(&fake, 0, sizeof(fake));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    path.immutable_for_realtime = false;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(fake.loads == 0U && fake.reserves == 0U);
    path.immutable_for_realtime = true;
    fake.fail_reserve = true;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability) ==
          UCN_V6_ERR_STATE);
    CHECK(fake.reserves == 1U);
    fake.fail_reserve = false;
    fake.false_success = true;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability) ==
          UCN_V6_ERR_STATE);
    fake.false_success = false;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability) ==
          UCN_V6_OK);
    CHECK(fake.record.config.domain_generation == 2U &&
          fake.reserves == 3U && fake.loads == 5U);

    memset(&sample, 0, sizeof(sample));
    sample.clock_domain_id = 7U;
    sample.domain_generation = 2U;
    sample.uncertainty = uncertainty();
    sample.local_sample_us = 100U;
    sample.offset_us = 1000;
    CHECK(ucn_v6_time_sync_sample_encode(&sample, sample_payload) == UCN_V6_OK);
    opened.frame.payload = sample_payload;
    opened.frame.payload_length = sizeof(sample_payload);
    CHECK(ucn_v6_realtime_copy_view(owner, &owner_view_before) == UCN_V6_OK);
    opened.frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_PEER_HELLO;
    CHECK(ucn_v6_realtime_ingest_sample(owner, &opened) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(ucn_v6_realtime_copy_view(owner, &owner_view) == UCN_V6_OK);
    CHECK(owner_view.domains == owner_view_before.domains &&
          owner_view.locked_domains == owner_view_before.locked_domains &&
          owner_view.accepted_samples == owner_view_before.accepted_samples &&
          owner_view.rejected_samples == owner_view_before.rejected_samples &&
          owner_view.faulted == owner_view_before.faulted);
    opened.frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TIME_FOLLOW_UP;
    sample_payload[UCN_V6_TIME_SYNC_SAMPLE_BYTES - 1U] = 1U;
    CHECK(ucn_v6_realtime_ingest_sample(owner, &opened) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(ucn_v6_realtime_copy_view(owner, &owner_view) == UCN_V6_OK);
    CHECK(owner_view.domains == owner_view_before.domains &&
          owner_view.locked_domains == owner_view_before.locked_domains &&
          owner_view.accepted_samples == owner_view_before.accepted_samples &&
          owner_view.rejected_samples == owner_view_before.rejected_samples &&
          owner_view.faulted == owner_view_before.faulted);
    CHECK(ucn_v6_time_sync_sample_encode(&sample, sample_payload) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_ingest_sample(owner, &opened) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_get_clock(owner, 7U, 100U, &clock) ==
          UCN_V6_ERR_STATE);
    sample.local_sample_us = 200U;
    CHECK(ucn_v6_time_sync_sample_encode(&sample, sample_payload) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_ingest_sample(owner, &opened) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_get_clock(owner, 7U, 250U, &clock) == UCN_V6_OK);
    CHECK(clock.domain_time_us == 1250U && clock.uncertainty_us == 7U);

    memset(&policy, 0, sizeof(policy));
    policy.destination_endpoint = 20U;
    policy.mode = UCN_V6_REALTIME_DEADLINE;
    policy.requirement = UCN_V6_REALTIME_REQUIRED;
    policy.clock_domain_id = 7U;
    policy.max_age_us = 200U;
    policy.max_uncertainty_us = 64U;
    policy.max_local_holdover_us = 500U;
    policy.require_hardware_capture = true;
    {
        ucn_v6_realtime_endpoint_policy_t invalid = policy;
        invalid.mode = (ucn_v6_realtime_mode_t)-1;
        CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &invalid) ==
              UCN_V6_ERR_ARGUMENT);
        invalid = policy;
        invalid.requirement = (ucn_v6_realtime_requirement_t)-1;
        CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &invalid) ==
              UCN_V6_ERR_ARGUMENT);
    }
    CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &policy) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_prepare_payload(
              owner, 20U, 260U, 4U, true, business, sizeof(business),
              payload, sizeof(payload), &send) == UCN_V6_OK);
    CHECK(send.payload_length == 20U && send.business_offset == 16U);
    opened.frame.message.destination_endpoint = 20U;
    opened.frame.payload = payload;
    opened.frame.payload_length = (uint16_t)send.payload_length;
    CHECK(ucn_v6_realtime_receive_admit(owner, 300U, &opened, &receive) ==
          UCN_V6_OK && receive.accepted);
    CHECK(ucn_v6_realtime_execution_admit(
              owner, 301U, &opened, &receive, &admitted,
              &admitted_length) == UCN_V6_OK);
    CHECK(admitted_length == sizeof(business) &&
          memcmp(admitted, business, sizeof(business)) == 0);

    opened.endpoint_authorized = false;
    CHECK(ucn_v6_realtime_receive_admit(owner, 302U, &opened, &receive) ==
          UCN_V6_OK && !receive.accepted &&
          receive.reason == UCN_V6_REALTIME_REJECT_SECURITY);
    opened.endpoint_authorized = true;
    CHECK(ucn_v6_realtime_receive_admit(owner, 500U, &opened, &receive) ==
          UCN_V6_OK && !receive.accepted &&
          receive.reason == UCN_V6_REALTIME_REJECT_EXPIRED);

    memset(sentinel, 0x5A, sizeof(sentinel));
    memcpy(payload, sentinel, sizeof(payload));
    CHECK(ucn_v6_realtime_prepare_payload(
              owner, 20U, 1200U, 4U, false, business, sizeof(business),
              payload, sizeof(payload), &send) == UCN_V6_ERR_ACCESS);
    CHECK(memcmp(payload, sentinel, sizeof(payload)) == 0);
    CHECK(ucn_v6_realtime_copy_view(owner, &owner_view) == UCN_V6_OK);
    CHECK(owner_view.domains == 1U && owner_view.locked_domains == 1U &&
          owner_view.accepted_samples == 2U &&
          owner_view.rejected_messages >= 2U);
    return 0;
}

static int test_none_has_zero_overhead_and_generation_replay(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate;
    ucn_v6_realtime_endpoint_policy_t policy;
    ucn_v6_realtime_send_result_t result;
    ucn_v6_principal_t master = principal(0x50U);
    ucn_v6_binding_key_t binding = { 1U, 8U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 1U);
    ucn_v6_path_capability_t path = fixed_path(&master, &binding);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 1U);
    const uint8_t business[] = { 9U, 8U, 7U };
    uint8_t output[8];
    memset(&fake, 0, sizeof(fake));
    fake.valid = true;
    fake.record.config = config;
    fake.record.config.domain_generation = 2U;
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability) ==
          UCN_V6_ERR_REPLAY);
    memset(&policy, 0, sizeof(policy));
    policy.destination_endpoint = 30U;
    policy.mode = UCN_V6_REALTIME_NONE;
    policy.requirement = UCN_V6_REALTIME_DISABLED;
    CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &policy) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_prepare_payload(
              owner, 30U, 1U, 0U, false, business, sizeof(business),
              output, sizeof(output), &result) == UCN_V6_OK);
    CHECK(result.payload_length == sizeof(business) &&
          result.business_offset == 0U &&
          memcmp(output, business, sizeof(business)) == 0);
    return 0;
}

static int test_bind_preflight_has_zero_durable_side_effect(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate;
    ucn_v6_principal_t master = principal(0x60U);
    ucn_v6_binding_key_t binding = { 1U, 10U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 1U);
    ucn_v6_path_capability_t path = fixed_path(&master, &binding);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 1U);
    unsigned loads_before;
    unsigned reserves_before;
    uint32_t generation_before;
    size_t index;

    memset(&fake, 0, sizeof(fake));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability) ==
          UCN_V6_OK);

    loads_before = fake.loads;
    reserves_before = fake.reserves;
    generation_before = fake.record.config.domain_generation;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability) ==
          UCN_V6_ERR_REPLAY);
    CHECK(fake.loads == loads_before && fake.reserves == reserves_before &&
          fake.record.config.domain_generation == generation_before);

    for (index = 1U; index < UCN_V6_CONFIG_TIME_DOMAINS; ++index) {
        config.clock_domain_id = (uint16_t)(7U + index);
        config.domain_generation = (uint32_t)(1U + index);
        capability.record.peer.clock_domain_id = config.clock_domain_id;
        capability.record.peer.clock_domain_generation =
            config.domain_generation;
        CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path,
                                           &capability) == UCN_V6_OK);
    }

    config.clock_domain_id = 100U;
    config.domain_generation =
        (uint32_t)(UCN_V6_CONFIG_TIME_DOMAINS + 1U);
    capability.record.peer.clock_domain_id = config.clock_domain_id;
    capability.record.peer.clock_domain_generation = config.domain_generation;
    loads_before = fake.loads;
    reserves_before = fake.reserves;
    generation_before = fake.record.config.domain_generation;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability) ==
          UCN_V6_ERR_NO_SPACE);
    CHECK(fake.loads == loads_before && fake.reserves == reserves_before &&
          fake.record.config.domain_generation == generation_before);
    return 0;
}

static int test_generation_rejects_cross_restart_domain_aba(void)
{
    ucn_v6_realtime_owner_storage_t first_storage;
    ucn_v6_realtime_owner_storage_t restarted_storage;
    ucn_v6_realtime_owner_t *first = NULL;
    ucn_v6_realtime_owner_t *restarted = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate;
    ucn_v6_principal_t master = principal(0x70U);
    ucn_v6_binding_key_t binding = { 1U, 11U, 3U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 5U);
    ucn_v6_path_capability_t path = fixed_path(&master, &binding);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 5U);
    ucn_v6_time_domain_config_t changed;
    ucn_v6_path_capability_t changed_path;

    memset(&fake, 0, sizeof(fake));
    memset(&store, 0, sizeof(store));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              first_storage.bytes, sizeof(first_storage),
              ucn_v6_compiled_manifest(), &store, &gate,
              &first) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(
              first, &config, &path, &capability) == UCN_V6_OK);

    CHECK(ucn_v6_realtime_owner_init_in_place(
              restarted_storage.bytes, sizeof(restarted_storage),
              ucn_v6_compiled_manifest(), &store, &gate,
              &restarted) == UCN_V6_OK);
    changed = config;
    changed.path_generation = 9U;
    changed_path = path;
    changed_path.path_generation = 9U;
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &changed, &changed_path, &capability) ==
          UCN_V6_ERR_REPLAY);
    CHECK(fake.record.config.path_generation == config.path_generation);

    /* Exact same-generation restart recovery is the only idempotent case. */
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &config, &path, &capability) == UCN_V6_OK);
    return 0;
}

int main(void)
{
    CHECK(test_codec_and_uncertainty() == 0);
    CHECK(test_fixed_path_domain_and_dual_gate() == 0);
    CHECK(test_none_has_zero_overhead_and_generation_replay() == 0);
    CHECK(test_bind_preflight_has_zero_durable_side_effect() == 0);
    CHECK(test_generation_rejects_cross_restart_domain_aba() == 0);
    puts("ucn v6 realtime tests passed");
    return 0;
}
