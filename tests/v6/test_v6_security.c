#include "ucn/v6/ucn_v6_config.h"
#if UCN_V6_FEATURE_ADAPTER_ENABLED
#include "ucn/v6/ucn_v6_adapter.h"
#endif
#include "ucn/v6/ucn_v6_qos.h"
#if UCN_V6_FEATURE_ADAPTER_ENABLED && UCN_V6_FEATURE_REALTIME_ENABLED
#include "ucn/v6/ucn_v6_runtime.h"
#endif
#include "ucn/v6/ucn_v6_security.h"
#include "ucn/v6/ucn_v6_transfer.h"

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
    bool present;
    bool reject_submit;
    bool corrupt_reload;
    bool perturb_padding;
    bool witness_present;
    ucn_v6_durable_generation_witness_t witness;
    bool reenter_on_reserve;
    ucn_v6_security_manager_t *reenter_manager;
    ucn_v6_principal_t reenter_peer;
    ucn_v6_result_t reenter_result;
    unsigned loads;
    unsigned submits;
    ucn_v6_security_snapshot_t snapshot;
} fake_store_t;

static void poison_gap(
    void *object,
    size_t left_offset,
    size_t left_size,
    size_t right_offset)
{
    size_t index;
    uint8_t *bytes = (uint8_t *)object;
    for (index = left_offset + left_size; index < right_offset; ++index) {
        bytes[index] = UINT8_C(0xA5);
    }
}

static void perturb_snapshot_padding(
    ucn_v6_security_snapshot_t *snapshot)
{
    size_t index;
    size_t group_index;
    size_t key_index;

    poison_gap(snapshot,
               offsetof(ucn_v6_security_snapshot_t, local_binding_valid),
               sizeof(snapshot->local_binding_valid),
               offsetof(ucn_v6_security_snapshot_t, local_binding));
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        ucn_v6_security_session_record_t *session =
            &snapshot->sessions[index];
        poison_gap(session,
                   offsetof(ucn_v6_security_session_record_t,
                            link_instance_id),
                   sizeof(session->link_instance_id),
                   offsetof(ucn_v6_security_session_record_t,
                            link_instance_generation));
    }
    for (index = 0U; index < UCN_V6_CONFIG_ACL_ENTRIES; ++index) {
        ucn_v6_acl_entry_t *entry = &snapshot->acl_entries[index];
        poison_gap(entry, offsetof(ucn_v6_acl_entry_t, revoked),
                   sizeof(entry->revoked),
                   offsetof(ucn_v6_acl_entry_t, key));
    }
    for (group_index = 0U;
         group_index < UCN_V6_CONFIG_STATIC_GROUP_SLOTS; ++group_index) {
        for (key_index = 0U;
             key_index < UCN_V6_CONFIG_GROUP_KEY_SLOTS; ++key_index) {
            ucn_v6_group_key_slot_t *key =
                &snapshot->group_keys[group_index][key_index];
            poison_gap(key, offsetof(ucn_v6_group_key_slot_t, requires_rekey),
                       sizeof(key->requires_rekey),
                       offsetof(ucn_v6_group_key_slot_t, group_id));
        }
    }
    for (index = 0U;
         index < UCN_V6_CONFIG_GROUP_REPLAY_SOURCES; ++index) {
        ucn_v6_group_replay_source_t *source =
            &snapshot->group_replay_sources[index];
        poison_gap(source,
                   offsetof(ucn_v6_group_replay_source_t, occupied),
                   sizeof(source->occupied),
                   offsetof(ucn_v6_group_replay_source_t, group_id));
    }
}

static ucn_v6_result_t load_witness(
    void *context,
    ucn_v6_durable_generation_witness_t *witness)
{
    fake_store_t *store = (fake_store_t *)context;
    if (witness == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!store->witness_present) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *witness = store->witness;
    return UCN_V6_OK;
}

static ucn_v6_result_t reserve_witness(
    void *context,
    const ucn_v6_durable_generation_witness_t *witness)
{
    fake_store_t *store = (fake_store_t *)context;
    if (store->reenter_on_reserve) {
        store->reenter_result = ucn_v6_security_require_reauth(
            store->reenter_manager, &store->reenter_peer);
    }
    if (witness == NULL || witness->witness_generation == 0U ||
        (store->witness_present && witness->witness_generation <=
             store->witness.witness_generation)) {
        return UCN_V6_ERR_REPLAY;
    }
    store->witness = *witness;
    store->witness_present = true;
    return UCN_V6_OK;
}

typedef struct fake_crypto {
    unsigned proofs;
    unsigned tag_checks;
    unsigned aead_opens;
    bool reenter_on_proof;
    bool invalid_result_on_proof;
    bool leave_gate_on_proof;
    ucn_v6_security_manager_t *reenter_manager;
    ucn_v6_callback_gate_t *reenter_gate;
    ucn_v6_principal_t reenter_peer;
    ucn_v6_result_t reenter_result;
    ucn_v6_result_t forced_leave_result;
} fake_crypto_t;

#if UCN_V6_FEATURE_ADAPTER_ENABLED
typedef struct pipeline_runtime {
    bool locked;
    unsigned notifications;
} pipeline_runtime_t;

typedef struct pipeline_driver {
    uint8_t frame[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    size_t frame_length;
    ucn_v6_driver_event_key_t key;
    unsigned submit_calls;
} pipeline_driver_t;

static ucn_v6_adapter_owner_storage_t pipeline_adapter_storage;
#endif
static ucn_v6_qos_owner_storage_t pipeline_qos_storage;
static ucn_v6_transfer_owner_storage_t pipeline_transfer_storage;
static ucn_v6_capability_owner_storage_t pipeline_capability_storage;
static ucn_v6_route_owner_storage_t pipeline_route_storage;

#if UCN_V6_FEATURE_ADAPTER_ENABLED && UCN_V6_FEATURE_REALTIME_ENABLED
typedef struct runtime_time_driver {
    ucn_v6_adapter_owner_t *adapter;
    uint8_t frame[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    size_t frame_length;
    ucn_v6_driver_event_key_t key;
    bool request_timestamp;
    unsigned submit_calls;
} runtime_time_driver_t;

typedef struct runtime_time_store {
    bool valid;
    ucn_v6_realtime_domain_record_t record;
} runtime_time_store_t;

typedef struct runtime_time_app {
    ucn_v6_security_manager_t *security;
    ucn_v6_principal_t peer;
    ucn_v6_route_path_ref_t reverse_ref;
    ucn_v6_runtime_time_handle_t member_handle;
    uint64_t next_buffer_token;
    ucn_v6_result_t callback_result;
    bool auto_send_delay_request;
    unsigned sync_frames;
    unsigned delay_requests;
    unsigned delay_responses;
    unsigned releases;
} runtime_time_app_t;

static ucn_v6_adapter_owner_storage_t runtime_time_adapter_storage[2];
static ucn_v6_runtime_owner_storage_t runtime_time_runtime_storage[2];
static ucn_v6_capability_owner_storage_t runtime_time_capability_storage[2];
static ucn_v6_route_owner_storage_t runtime_time_route_storage[2];
static ucn_v6_realtime_owner_storage_t runtime_time_realtime_storage;
#endif

static ucn_v6_capability_record_t pipeline_capability_record(
    uint32_t generation, uint32_t link_generation)
{
    ucn_v6_capability_record_t value;
    memset(&value, 0, sizeof(value));
    value.capability_generation = generation;
    value.link.link_instance_generation = link_generation;
    value.link.carrier_mtu = 512U;
    value.link.link_frame_mtu = 256U;
    value.link.processing_frame_mtu = 220U;
    value.link.carrier_header_bytes = 2U;
    value.link.carrier_padding_bytes = 1U;
    value.link.carrier_crc_bytes = 2U;
    value.link.carrier_tag_bytes = 8U;
    value.link.carrier_max_fragments = 32U;
    value.link.link_flags = UCN_V6_LINK_ORDERED | UCN_V6_LINK_RELIABLE |
                            UCN_V6_LINK_UNICAST | UCN_V6_LINK_SECURITY;
    value.link.nominal_rate_bps = UINT32_C(3000000);
    value.link.hardware_priority_count = 4U;
    value.link.timestamp_capability_bits = UCN_V6_TIMESTAMP_RX_HARDWARE |
                                           UCN_V6_TIMESTAMP_TX_HARDWARE;
    value.link.timestamp_uncertainty_us = 4U;
    value.peer.feature_bits = UCN_V6_COMPILED_FEATURE_BITS |
                              UCN_V6_FEATURE_TRANSFER;
    value.peer.hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    value.peer.e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    value.peer.max_message_class =
        (ucn_v6_message_class_t)UCN_V6_CONFIG_TRANSFER_MAX_CLASS;
    value.peer.max_rx_window = UCN_V6_CONFIG_TRANSFER_WINDOW;
    value.peer.max_concurrent_transfers = UCN_V6_CONFIG_TRANSFER_RX_SLOTS;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    value.peer.realtime_mode_bits = UCN_V6_REALTIME_MODE_SYNCED |
                                    UCN_V6_REALTIME_MODE_DEADLINE;
    value.peer.clock_domain_id = 1U;
    value.peer.clock_domain_generation = 1U;
#endif
    return value;
}

#if UCN_V6_FEATURE_ADAPTER_ENABLED
static ucn_v6_result_t pipeline_lock_task(void *context)
{
    pipeline_runtime_t *runtime = (pipeline_runtime_t *)context;
    if (runtime->locked) return UCN_V6_ERR_STATE;
    runtime->locked = true;
    return UCN_V6_OK;
}

static bool pipeline_try_lock_from_isr(void *context)
{
    return pipeline_lock_task(context) == UCN_V6_OK;
}

static void pipeline_unlock(void *context)
{
    ((pipeline_runtime_t *)context)->locked = false;
}

static ucn_v6_result_t pipeline_post(
    void *context, ucn_v6_owner_event_t event, bool from_isr)
{
    pipeline_runtime_t *runtime = (pipeline_runtime_t *)context;
    (void)event;
    (void)from_isr;
    ++runtime->notifications;
    return UCN_V6_OK;
}

static ucn_v6_result_t pipeline_submit(
    void *context,
    const ucn_v6_driver_event_key_t *key,
    const uint8_t *frame,
    size_t frame_length,
    uint8_t hardware_priority,
    bool request_timestamp)
{
    pipeline_driver_t *driver = (pipeline_driver_t *)context;
    (void)hardware_priority;
    (void)request_timestamp;
    if (frame_length > sizeof(driver->frame)) return UCN_V6_ERR_NO_SPACE;
    memcpy(driver->frame, frame, frame_length);
    driver->frame_length = frame_length;
    driver->key = *key;
    ++driver->submit_calls;
    return UCN_V6_OK;
}

static ucn_v6_result_t pipeline_cancel(
    void *context, const ucn_v6_driver_event_key_t *key)
{
    (void)context;
    (void)key;
    return UCN_V6_OK;
}

static ucn_v6_result_t pipeline_quiesce(void *context)
{
    (void)context;
    return UCN_V6_OK;
}
#endif

#if UCN_V6_FEATURE_ADAPTER_ENABLED && UCN_V6_FEATURE_REALTIME_ENABLED
static ucn_v6_result_t runtime_time_submit(
    void *context, const ucn_v6_driver_event_key_t *key,
    const uint8_t *frame, size_t frame_length,
    uint8_t hardware_priority, bool request_timestamp)
{
    runtime_time_driver_t *driver = (runtime_time_driver_t *)context;
    (void)hardware_priority;
    if (driver == NULL || key == NULL || frame == NULL || frame_length == 0U ||
        frame_length > sizeof(driver->frame)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memcpy(driver->frame, frame, frame_length);
    driver->frame_length = frame_length;
    driver->key = *key;
    driver->request_timestamp = request_timestamp;
    ++driver->submit_calls;
    return UCN_V6_OK;
}

static ucn_v6_result_t runtime_time_load_domain(
    void *context, uint16_t clock_domain_id,
    ucn_v6_realtime_domain_record_t *record)
{
    runtime_time_store_t *store = (runtime_time_store_t *)context;
    if (store == NULL || record == NULL) return UCN_V6_ERR_ARGUMENT;
    if (!store->valid ||
        store->record.config.clock_domain_id != clock_domain_id) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *record = store->record;
    return UCN_V6_OK;
}

static ucn_v6_result_t runtime_time_reserve_domain(
    void *context, const ucn_v6_realtime_domain_record_t *record)
{
    runtime_time_store_t *store = (runtime_time_store_t *)context;
    if (store == NULL || record == NULL) return UCN_V6_ERR_ARGUMENT;
    store->record = *record;
    store->valid = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t runtime_time_release(
    void *context, uint64_t buffer_token, ucn_v6_result_t result,
    const ucn_v6_driver_timestamp_t *timestamp)
{
    runtime_time_app_t *app = (runtime_time_app_t *)context;
    (void)buffer_token;
    (void)result;
    (void)timestamp;
    if (app == NULL) return UCN_V6_ERR_ARGUMENT;
    ++app->releases;
    return UCN_V6_OK;
}

static ucn_v6_result_t runtime_time_ingress(
    void *context, ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const uint8_t *encoded_frame, size_t encoded_length,
    const ucn_v6_driver_rx_view_t *rx,
    ucn_v6_runtime_ingress_disposition_t *disposition)
{
    runtime_time_app_t *app = (runtime_time_app_t *)context;
    ucn_v6_security_open_result_t opened;
    uint8_t plaintext[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    ucn_v6_result_t result;
    if (app == NULL || runtime == NULL || encoded_frame == NULL ||
        rx == NULL || disposition == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&opened, 0, sizeof(opened));
    result = ucn_v6_security_open_frame(
        app->security, now_us, rx->key.link_id, rx->key.link_generation,
        &app->peer, encoded_frame, encoded_length, plaintext,
        sizeof(plaintext), &opened);
    if (result != UCN_V6_OK) {
        fprintf(stderr, "runtime time security open failed: %d\n",
                (int)result);
        app->callback_result = result;
        return result;
    }
    switch (opened.frame.protocol_opcode) {
    case UCN_V6_PROTOCOL_OPCODE_TIME_SYNC:
        ++app->sync_frames;
        result = ucn_v6_runtime_time_observe_sync(
            runtime, &opened, rx, &app->reverse_ref, now_us,
            &app->member_handle);
        if (result == UCN_V6_OK && app->auto_send_delay_request) {
            result = ucn_v6_runtime_time_send_delay_request(
                runtime, &app->member_handle, app->next_buffer_token++,
                now_us);
        }
        break;
    case UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_REQUEST:
        ++app->delay_requests;
        result = ucn_v6_runtime_time_respond_delay_request(
            runtime, &opened, rx, app->next_buffer_token++, now_us);
        break;
    case UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE:
        ++app->delay_responses;
        result = ucn_v6_runtime_time_complete(runtime, &opened, rx, now_us);
        break;
    default:
        result = UCN_V6_ERR_STATE;
        break;
    }
    app->callback_result = result;
    if (result != UCN_V6_OK) {
        fprintf(stderr, "runtime time opcode %u failed: %d\n",
                (unsigned)opened.frame.protocol_opcode, (int)result);
        return result;
    }
    *disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
    return UCN_V6_OK;
}
#endif

static ucn_v6_principal_t make_principal(uint8_t seed)
{
    ucn_v6_principal_t value;
    size_t index;
    for (index = 0U; index < sizeof(value.bytes); ++index) {
        value.bytes[index] = (uint8_t)(seed + index);
    }
    return value;
}

static void fill_bytes(uint8_t *bytes, size_t length, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        bytes[index] = (uint8_t)(seed + index);
    }
}

static ucn_v6_result_t store_load(void *context,
                                  ucn_v6_security_snapshot_t *snapshot)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->loads;
    if (!store->present) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *snapshot = store->snapshot;
    if (store->perturb_padding) {
        perturb_snapshot_padding(snapshot);
    }
    if (store->corrupt_reload) {
        snapshot->magic ^= UINT32_C(1);
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t store_submit(
    void *context,
    const ucn_v6_security_snapshot_t *snapshot)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->submits;
    if (store->reject_submit) {
        return UCN_V6_ERR_STATE;
    }
    store->snapshot = *snapshot;
    store->present = true;
    return UCN_V6_OK;
}

static void fake_tag(
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t tag[UCN_V6_SECURITY_TAG_BYTES])
{
    uint32_t state = UINT32_C(0x9E3779B9) ^ selector->suite_id ^
                     ((uint32_t)selector->key_id << 8U) ^
                     selector->key_generation;
    size_t index;
    for (index = 0U; index < aad_length; ++index) {
        state = (state << 5U) ^ (state >> 2U) ^ aad[index];
    }
    for (index = 0U; index < payload_length; ++index) {
        state = (state << 5U) ^ (state >> 2U) ^ payload[index];
    }
    for (index = 0U; index < UCN_V6_SECURITY_TAG_BYTES; ++index) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        tag[index] = (uint8_t)(state >> 24U);
    }
}

static ucn_v6_result_t verify_proof(
    void *context,
    ucn_v6_security_proof_role_t role,
    const ucn_v6_principal_t *principal,
    const uint8_t *canonical,
    size_t canonical_length,
    const uint8_t *proof,
    size_t proof_length)
{
    fake_crypto_t *crypto = (fake_crypto_t *)context;
    ++crypto->proofs;
    if (crypto->reenter_on_proof) {
        crypto->reenter_result = ucn_v6_security_require_reauth(
            crypto->reenter_manager, &crypto->reenter_peer);
    }
    if (crypto->leave_gate_on_proof) {
        crypto->forced_leave_result = ucn_v6_callback_gate_leave(
            crypto->reenter_gate, crypto->reenter_manager);
    }
    if (crypto->invalid_result_on_proof) {
        return (ucn_v6_result_t)1;
    }
    return ucn_v6_principal_is_valid(principal) && canonical != NULL &&
           canonical_length != 0U && proof != NULL && proof_length == 1U &&
           proof[0] == (uint8_t)role ? UCN_V6_OK : UCN_V6_ERR_SECURITY;
}

static ucn_v6_result_t verify_tag(
    void *context,
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *payload,
    size_t payload_length,
    const uint8_t tag[UCN_V6_SECURITY_TAG_BYTES])
{
    fake_crypto_t *crypto = (fake_crypto_t *)context;
    uint8_t expected[UCN_V6_SECURITY_TAG_BYTES];
    ++crypto->tag_checks;
    fake_tag(selector, aad, aad_length, payload, payload_length, expected);
    return memcmp(expected, tag, sizeof(expected)) == 0 ?
               UCN_V6_OK : UCN_V6_ERR_SECURITY;
}

static ucn_v6_result_t compute_tag(
    void *context,
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t tag[UCN_V6_SECURITY_TAG_BYTES])
{
    (void)context;
    fake_tag(selector, aad, aad_length, payload, payload_length, tag);
    return UCN_V6_OK;
}

static ucn_v6_result_t seal_aead(
    void *context,
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *plaintext,
    size_t plaintext_length,
    uint8_t *ciphertext,
    uint8_t tag[UCN_V6_SECURITY_TAG_BYTES])
{
    size_t index;
    (void)context;
    for (index = 0U; index < plaintext_length; ++index) {
        ciphertext[index] = (uint8_t)(plaintext[index] ^ UINT8_C(0xA5));
    }
    fake_tag(selector, aad, aad_length, ciphertext, plaintext_length, tag);
    return UCN_V6_OK;
}

static ucn_v6_result_t open_aead(
    void *context,
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *ciphertext,
    size_t ciphertext_length,
    const uint8_t tag[UCN_V6_SECURITY_TAG_BYTES],
    uint8_t *plaintext)
{
    fake_crypto_t *crypto = (fake_crypto_t *)context;
    size_t index;
    if (verify_tag(context, selector, aad, aad_length, ciphertext,
                   ciphertext_length, tag) != UCN_V6_OK) {
        return UCN_V6_ERR_SECURITY;
    }
    ++crypto->aead_opens;
    for (index = 0U; index < ciphertext_length; ++index) {
        plaintext[index] = (uint8_t)(ciphertext[index] ^ UINT8_C(0xA5));
    }
    return UCN_V6_OK;
}

static void no_lock(void *context)
{
    (void)context;
}

static ucn_v6_security_crypto_ops_t crypto_ops(fake_crypto_t *crypto)
{
    ucn_v6_security_crypto_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = crypto;
    ops.verify_proof = verify_proof;
    ops.verify_tag = verify_tag;
    ops.compute_tag = compute_tag;
    ops.seal_aead = seal_aead;
    ops.open_aead = open_aead;
    return ops;
}

static ucn_v6_security_store_ops_t store_ops(fake_store_t *store)
{
    ucn_v6_security_store_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = store;
    ops.load_witness = load_witness;
    ops.reserve_witness = reserve_witness;
    ops.load = store_load;
    ops.submit = store_submit;
    return ops;
}

static ucn_v6_bootstrap_transcript_t make_transcript_pair(
    const ucn_v6_principal_t *device,
    uint32_t device_address,
    uint32_t device_binding_generation,
    const ucn_v6_principal_t *authority,
    uint32_t authority_address,
    uint32_t authority_binding_generation,
    uint64_t transaction_id,
    uint32_t link_instance_generation)
{
    ucn_v6_bootstrap_transcript_t value;
    memset(&value, 0, sizeof(value));
    value.protocol_version = UCN_V6_PROTOCOL_VERSION;
    value.bootstrap_header_contract = 1U;
    value.flow = UCN_V6_BOOTSTRAP_FLOW_JOIN;
    value.joining_device_principal = *device;
    value.joining_device_identity_digest = make_principal(0x30U);
    value.authority_principal = *authority;
    /* Give every test Authority a stable Realm-wide epoch derived from its
     * fixed address.  This keeps multi-peer fixtures honest: changing the
     * Authority principal is an Authority transfer, never a same-generation
     * alias. */
    value.authority_generation = authority_address;
    value.device_nonce = transaction_id + 1U;
    value.authority_nonce = transaction_id + 2U;
    value.transaction_id = transaction_id;
    value.lease_freshness_challenge_nonce = transaction_id + 3U;
    value.realm_id = 1U;
    value.proposed_address = device_address;
    value.address_binding_generation = device_binding_generation;
    value.authority_address = authority_address;
    value.authority_binding_generation = authority_binding_generation;
    value.selected_link_instance_id = 1U;
    fill_bytes(value.binding_lease_id, sizeof(value.binding_lease_id), 0x50U);
    value.binding_lease_duration_us = 50000U;
    value.authority_lease_sequence = authority_address;
    value.authority_lease_duration_us = 100000U;
    value.freshness_max_remaining_lease_us = 50000U;
    fill_bytes(value.durable_fence_token,
               sizeof(value.durable_fence_token),
               (uint8_t)(0x80U + authority_address));
    fill_bytes(value.allocation_high_water_digest,
               sizeof(value.allocation_high_water_digest), 0x90U);
    fill_bytes(value.quorum_config_digest,
               sizeof(value.quorum_config_digest), 0xA0U);
    fill_bytes(value.signer_set_digest,
               sizeof(value.signer_set_digest), 0xB0U);
    fill_bytes(value.threshold_proof_digest,
               sizeof(value.threshold_proof_digest), 0xC0U);
    fill_bytes(value.freshness_proof_transcript_hash,
               sizeof(value.freshness_proof_transcript_hash), 0xD0U);
    value.authority_signer_count = 3U;
    value.authority_quorum_threshold = 2U;
    value.binding_mode = UCN_V6_ADDRESS_LEASED;
    value.selected_hop_suite = UCN_V6_SUITE_HMAC_SHA256_128;
    value.selected_hop_key_id = 2U;
    value.selected_hop_key_generation = 3U;
    value.selected_e2e_mode = UCN_V6_E2E_AEAD;
    value.selected_e2e_suite = UCN_V6_SUITE_AES_GCM_128;
    value.selected_e2e_key_id = 4U;
    value.selected_e2e_key_generation = 5U;
    value.selected_session_generation = 1U;
    value.selected_link_instance_generation = link_instance_generation;
    fill_bytes(value.prior_messages_hash, sizeof(value.prior_messages_hash),
               0x70U);
    return value;
}

static ucn_v6_bootstrap_transcript_t make_transcript(
    const ucn_v6_principal_t *device,
    const ucn_v6_principal_t *authority)
{
    return make_transcript_pair(device, 7U, 3U, authority, 8U, 4U,
                                13U, 6U);
}

static ucn_v6_join_commit_t make_join(
    const ucn_v6_bootstrap_transcript_t *transcript,
    bool device_side)
{
    static const uint8_t device_proof[] = { UCN_V6_PROOF_JOINING_DEVICE };
    static const uint8_t authority_proof[] = { UCN_V6_PROOF_ADDRESS_AUTHORITY };
    ucn_v6_join_commit_t commit;
    ucn_v6_binding_key_t device_binding;
    ucn_v6_binding_key_t authority_binding;
    memset(&commit, 0, sizeof(commit));
    device_binding.realm_id = transcript->realm_id;
    device_binding.node_address = transcript->proposed_address;
    device_binding.binding_generation =
        transcript->address_binding_generation;
    authority_binding.realm_id = transcript->realm_id;
    authority_binding.node_address = transcript->authority_address;
    authority_binding.binding_generation =
        transcript->authority_binding_generation;
    commit.transcript = *transcript;
    commit.authority_epoch.realm_id = transcript->realm_id;
    commit.authority_epoch.authority_principal =
        transcript->authority_principal;
    commit.authority_epoch.authority_generation =
        transcript->authority_generation;
    memcpy(commit.authority_epoch.durable_fence_token,
           transcript->durable_fence_token,
           sizeof(commit.authority_epoch.durable_fence_token));
    memcpy(commit.authority_epoch.allocation_high_water_digest,
           transcript->allocation_high_water_digest,
           sizeof(commit.authority_epoch.allocation_high_water_digest));
    commit.authority_epoch.lease_sequence =
        transcript->authority_lease_sequence;
    commit.authority_epoch.lease_duration_us =
        transcript->authority_lease_duration_us;
    memcpy(commit.authority_epoch.quorum_config_digest,
           transcript->quorum_config_digest,
           sizeof(commit.authority_epoch.quorum_config_digest));
    memcpy(commit.authority_epoch.signer_set_digest,
           transcript->signer_set_digest,
           sizeof(commit.authority_epoch.signer_set_digest));
    memcpy(commit.authority_epoch.threshold_proof_digest,
           transcript->threshold_proof_digest,
           sizeof(commit.authority_epoch.threshold_proof_digest));
    commit.authority_epoch.signer_count = transcript->authority_signer_count;
    commit.authority_epoch.quorum_threshold =
        transcript->authority_quorum_threshold;
    commit.authority_freshness.verifier_device_principal =
        transcript->joining_device_principal;
    commit.authority_freshness.challenge_nonce =
        transcript->lease_freshness_challenge_nonce;
    commit.authority_freshness.transaction_id = transcript->transaction_id;
    commit.authority_freshness.authority_lease_sequence =
        transcript->authority_lease_sequence;
    commit.authority_freshness.max_remaining_lease_us =
        transcript->freshness_max_remaining_lease_us;
    memcpy(commit.authority_freshness.binding_lease_id,
           transcript->binding_lease_id,
           sizeof(commit.authority_freshness.binding_lease_id));
    commit.authority_freshness.binding_generation =
        transcript->address_binding_generation;
    memcpy(commit.authority_freshness.proof_transcript_hash,
           transcript->freshness_proof_transcript_hash,
           sizeof(commit.authority_freshness.proof_transcript_hash));
    commit.joining_binding_certificate.device_principal =
        transcript->joining_device_principal;
    commit.joining_binding_certificate.authority_principal =
        transcript->authority_principal;
    commit.joining_binding_certificate.binding = device_binding;
    commit.joining_binding_certificate.authority_generation =
        transcript->authority_generation;
    memcpy(commit.joining_binding_certificate.lease_id,
           transcript->binding_lease_id, 16U);
    commit.joining_binding_certificate.lease_duration_us =
        transcript->binding_lease_duration_us;
    commit.joining_binding_certificate.authority_lease_sequence =
        transcript->authority_lease_sequence;
    commit.joining_binding_certificate.mode =
        (ucn_v6_address_mode_t)transcript->binding_mode;
    commit.local_binding = device_side ? device_binding : authority_binding;
    commit.peer_binding = device_side ? authority_binding : device_binding;
    commit.session_generation = transcript->selected_session_generation;
    commit.link_instance_id = transcript->selected_link_instance_id;
    commit.link_instance_generation =
        transcript->selected_link_instance_generation;
    commit.hop_selector.suite_id = transcript->selected_hop_suite;
    commit.hop_selector.key_id = transcript->selected_hop_key_id;
    commit.hop_selector.key_generation =
        transcript->selected_hop_key_generation;
    commit.e2e_selector.suite_id = transcript->selected_e2e_suite;
    commit.e2e_selector.key_id = transcript->selected_e2e_key_id;
    commit.e2e_selector.key_generation =
        transcript->selected_e2e_key_generation;
    commit.authority_lease_policy.local_timer_resolution_us = 1U;
    commit.authority_lease_policy.timer_read_uncertainty_known = true;
    commit.authority_lease_policy.local_policy_max_lease_us =
        transcript->freshness_max_remaining_lease_us;
    commit.device_proof = device_proof;
    commit.device_proof_length = sizeof(device_proof);
    commit.authority_proof = authority_proof;
    commit.authority_proof_length = sizeof(authority_proof);
    return commit;
}

static ucn_v6_bootstrap_key_t make_bootstrap_key(
    const ucn_v6_bootstrap_transcript_t *transcript,
    uint32_t discriminator)
{
    ucn_v6_bootstrap_key_t key;
    memset(&key, 0, sizeof(key));
    key.ingress_link_id = 1U;
    key.ingress_link_generation =
        transcript->selected_link_instance_generation;
    key.local_peer_discriminator = discriminator;
    key.identity_digest = transcript->joining_device_identity_digest;
    key.transaction_id = transcript->transaction_id;
    return key;
}

static ucn_v6_callback_gate_t bootstrap_gate = {0};
static bool bootstrap_gate_ready;

static ucn_v6_bootstrap_evidence_t bootstrap_evidence(
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding)
{
    ucn_v6_bootstrap_evidence_t evidence;
    memset(&evidence, 0, sizeof(evidence));
    evidence.length = 16U;
    evidence.bytes[0] = (uint8_t)event;
    evidence.bytes[1] = (uint8_t)flow;
    evidence.bytes[2] = (uint8_t)(key->ingress_link_id >> 8U);
    evidence.bytes[3] = (uint8_t)key->ingress_link_id;
    evidence.bytes[4] = (uint8_t)(key->transaction_id >> 56U);
    evidence.bytes[5] = (uint8_t)(key->transaction_id >> 48U);
    evidence.bytes[6] = (uint8_t)(key->transaction_id >> 40U);
    evidence.bytes[7] = (uint8_t)(key->transaction_id >> 32U);
    evidence.bytes[8] = (uint8_t)(key->transaction_id >> 24U);
    evidence.bytes[9] = (uint8_t)(key->transaction_id >> 16U);
    evidence.bytes[10] = (uint8_t)(key->transaction_id >> 8U);
    evidence.bytes[11] = (uint8_t)key->transaction_id;
    evidence.bytes[12] = (uint8_t)(transcript->authority_generation >> 8U);
    evidence.bytes[13] = (uint8_t)transcript->authority_generation;
    evidence.bytes[14] = existing_binding == NULL ? 0U :
        (uint8_t)(existing_binding->binding_generation >> 8U);
    evidence.bytes[15] = existing_binding == NULL ? 0U :
        (uint8_t)existing_binding->binding_generation;
    return evidence;
}

static ucn_v6_result_t authorize_bootstrap_event(
    void *context,
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    uint64_t now_us,
    const ucn_v6_bootstrap_evidence_t *evidence)
{
    ucn_v6_bootstrap_evidence_t expected = bootstrap_evidence(
        event, flow, key, transcript, existing_binding);
    (void)context;
    (void)now_us;
    return evidence != NULL &&
                   memcmp(evidence, &expected, sizeof(expected)) == 0 ?
               UCN_V6_OK : UCN_V6_ERR_SECURITY;
}

static ucn_v6_result_t bootstrap_owner_init_for_test(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_bootstrap_config_t *config,
    ucn_v6_bootstrap_owner_t **owner)
{
    ucn_v6_bootstrap_verifier_ops_t verifier;
    memset(&verifier, 0, sizeof(verifier));
    verifier.authorize_event = authorize_bootstrap_event;
    if (!bootstrap_gate_ready) {
        ucn_v6_result_t result = ucn_v6_callback_gate_init(
            &bootstrap_gate, NULL, no_lock, no_lock);
        if (result != UCN_V6_OK) {
            return result;
        }
        bootstrap_gate_ready = true;
    }
    return ucn_v6_bootstrap_owner_init_in_place(
        storage, storage_bytes, manifest, config, &verifier,
        &bootstrap_gate, owner);
}

static ucn_v6_result_t bootstrap_open_for_test(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    bool verified,
    uint64_t now_us)
{
    ucn_v6_bootstrap_evidence_t evidence = bootstrap_evidence(
        UCN_V6_BOOTSTRAP_EVENT_COOKIE, flow, key, transcript,
        existing_binding);
    if (!verified) {
        evidence.bytes[0] ^= UINT8_C(0x80);
    }
    return ucn_v6_bootstrap_open_after_cookie(
        owner, flow, key, transcript, existing_binding, &evidence, now_us);
}

static ucn_v6_result_t bootstrap_advance_for_test(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_event_t event,
    bool verified,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t pending;
    const ucn_v6_binding_key_t *existing_binding = NULL;
    ucn_v6_bootstrap_evidence_t evidence;
    memset(&pending, 0, sizeof(pending));
    if (flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH &&
        ucn_v6_bootstrap_copy_pending(owner, flow, key, &pending) ==
            UCN_V6_OK) {
        existing_binding = &pending.existing_binding;
    }
    evidence = bootstrap_evidence(event, flow, key, transcript,
                                  existing_binding);
    if (!verified) {
        evidence.bytes[0] ^= UINT8_C(0x80);
    }
    return ucn_v6_bootstrap_advance(
        owner, flow, key, transcript, event, &evidence, now_us);
}

#define ucn_v6_bootstrap_owner_init_in_place bootstrap_owner_init_for_test
#define ucn_v6_bootstrap_open_after_cookie bootstrap_open_for_test
#define ucn_v6_bootstrap_advance bootstrap_advance_for_test

static bool security_session_event_matches(
    const ucn_v6_stack_invalidation_t *event,
    uint16_t link_id,
    uint32_t link_generation,
    const ucn_v6_principal_t *peer,
    const ucn_v6_binding_key_t *peer_binding,
    uint32_t session_generation)
{
    return event->type == UCN_V6_STACK_INVALIDATE_SESSION &&
           event->link_id == link_id &&
           event->link_generation == link_generation &&
           memcmp(event->session.principal.bytes, peer->bytes,
                  sizeof(peer->bytes)) == 0 &&
           ucn_v6_binding_key_equal(&event->session.binding, peer_binding) &&
           event->session.session_generation == session_generation &&
           event->capability_generation == 0U && event->path_id == 0U &&
           event->path_generation == 0U;
}

static ucn_v6_stack_invalidation_t link_invalidation(
    uint16_t link_id,
    uint32_t link_generation)
{
    ucn_v6_stack_invalidation_t value;
    memset(&value, 0, sizeof(value));
    value.type = UCN_V6_STACK_INVALIDATE_LINK;
    value.link_id = link_id;
    value.link_generation = link_generation;
    return value;
}

static int complete_bootstrap(
    ucn_v6_bootstrap_owner_t *owner,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_bootstrap_key_t *key,
    uint64_t now_us)
{
    ucn_v6_binding_key_t existing;
    const ucn_v6_binding_key_t *existing_ptr = NULL;
    if (transcript->flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH) {
        existing.realm_id = transcript->realm_id;
        existing.node_address = transcript->proposed_address;
        existing.binding_generation = transcript->address_binding_generation;
        existing_ptr = &existing;
    }
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, transcript->flow, key, transcript, existing_ptr, true,
              now_us) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_advance(
              owner, transcript->flow, key, transcript,
              UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF, true, now_us + 1U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_advance(
              owner, transcript->flow, key, transcript,
              UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF, true, now_us + 2U) ==
          UCN_V6_OK);
    if (transcript->flow == UCN_V6_BOOTSTRAP_FLOW_JOIN) {
        CHECK(ucn_v6_bootstrap_advance(
                  owner, transcript->flow, key, transcript,
                  UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER, true, now_us + 3U) ==
              UCN_V6_OK);
        CHECK(ucn_v6_bootstrap_advance(
                  owner, transcript->flow, key, transcript,
                  UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT, true, now_us + 4U) ==
              UCN_V6_OK);
    }
    CHECK(ucn_v6_bootstrap_advance(
              owner, transcript->flow, key, transcript,
              UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE, true, now_us + 5U) ==
          UCN_V6_OK);
    return 0;
}

static int install_pair_session(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *device,
    uint32_t device_address,
    uint32_t device_binding_generation,
    const ucn_v6_principal_t *authority,
    uint32_t authority_address,
    uint32_t authority_binding_generation,
    bool local_is_device,
    uint64_t transaction_id,
    uint32_t discriminator)
{
    const ucn_v6_bootstrap_config_t config = {
        2U, 1U, 2U, 1U, UINT64_C(10000)
    };
    ucn_v6_bootstrap_owner_storage_t bootstrap_storage;
    ucn_v6_bootstrap_owner_t *bootstrap = NULL;
    ucn_v6_bootstrap_transcript_t value = make_transcript_pair(
        device, device_address, device_binding_generation,
        authority, authority_address, authority_binding_generation,
        transaction_id, 6U);
    ucn_v6_bootstrap_key_t key = make_bootstrap_key(&value, discriminator);
    ucn_v6_join_commit_t join = make_join(&value, local_is_device);

    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              bootstrap_storage.bytes, sizeof(bootstrap_storage),
              ucn_v6_compiled_manifest(), &config, &bootstrap) == UCN_V6_OK);
    CHECK(complete_bootstrap(bootstrap, &value, &key, 10U) == 0);
    CHECK(ucn_v6_security_commit_join(
              manager, bootstrap, &key, 20U, &join) == UCN_V6_OK);
    return 0;
}

static int reauth_pair_session_exact(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *device,
    uint32_t device_address,
    uint32_t device_binding_generation,
    const ucn_v6_principal_t *authority,
    uint32_t authority_address,
    uint32_t authority_binding_generation,
    bool local_is_device,
    uint64_t transaction_id,
    uint32_t discriminator,
    uint32_t session_generation,
    uint32_t link_instance_generation,
    uint32_t hop_key_generation)
{
    const ucn_v6_bootstrap_config_t config = {
        2U, 1U, 2U, 1U, UINT64_C(10000)
    };
    ucn_v6_bootstrap_owner_storage_t bootstrap_storage;
    ucn_v6_bootstrap_owner_t *bootstrap = NULL;
    ucn_v6_bootstrap_transcript_t value = make_transcript_pair(
        device, device_address, device_binding_generation,
        authority, authority_address, authority_binding_generation,
        transaction_id, link_instance_generation);
    ucn_v6_bootstrap_key_t key;
    ucn_v6_join_commit_t join;
    value.flow = UCN_V6_BOOTSTRAP_FLOW_REAUTH;
    value.selected_session_generation = session_generation;
    value.selected_hop_key_generation = hop_key_generation;
    key = make_bootstrap_key(&value, discriminator);
    join = make_join(&value, local_is_device);

    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              bootstrap_storage.bytes, sizeof(bootstrap_storage),
              ucn_v6_compiled_manifest(), &config, &bootstrap) == UCN_V6_OK);
    CHECK(complete_bootstrap(bootstrap, &value, &key, 30U) == 0);
    CHECK(ucn_v6_security_commit_join(
              manager, bootstrap, &key, 40U, &join) == UCN_V6_OK);
    return 0;
}

static int reauth_pair_session(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *device,
    uint32_t device_address,
    uint32_t device_binding_generation,
    const ucn_v6_principal_t *authority,
    uint32_t authority_address,
    uint32_t authority_binding_generation,
    bool local_is_device,
    uint64_t transaction_id,
    uint32_t discriminator)
{
    return reauth_pair_session_exact(
        manager, device, device_address, device_binding_generation,
        authority, authority_address, authority_binding_generation,
        local_is_device, transaction_id, discriminator, 2U, 7U, 4U);
}

static ucn_v6_acl_entry_t make_acl(
    const ucn_v6_principal_t *source_principal,
    uint32_t source,
    uint32_t source_generation,
    uint32_t destination,
    uint32_t destination_generation,
    ucn_v6_security_direction_t direction)
{
    ucn_v6_acl_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.occupied = true;
    entry.key.device_principal = *source_principal;
    entry.key.source_binding.realm_id = 1U;
    entry.key.source_binding.node_address = source;
    entry.key.source_binding.binding_generation = source_generation;
    entry.key.destination_binding.realm_id = 1U;
    entry.key.destination_binding.node_address = destination;
    entry.key.destination_binding.binding_generation = destination_generation;
    entry.key.session_generation = 1U;
    entry.key.source_endpoint = 10U;
    entry.key.destination_endpoint = 20U;
    entry.key.frame_type = UCN_V6_FRAME_DATA;
    entry.key.traffic_class = UCN_V6_TRAFFIC_Q2;
    entry.key.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    entry.key.interaction_role = UCN_V6_INTERACTION_REQUEST;
    entry.key.operation_id_policy = UCN_V6_OPERATION_ID_ANY_NONZERO;
    entry.key.direction = direction;
    return entry;
}

static ucn_v6_acl_entry_t make_control_acl(
    const ucn_v6_principal_t *source_principal,
    uint16_t opcode,
    ucn_v6_security_direction_t direction)
{
    ucn_v6_acl_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.occupied = true;
    entry.key.device_principal = *source_principal;
    entry.key.source_binding.realm_id = 1U;
    entry.key.source_binding.node_address = 7U;
    entry.key.source_binding.binding_generation = 3U;
    entry.key.destination_binding.realm_id = 1U;
    entry.key.destination_binding.node_address = 8U;
    entry.key.destination_binding.binding_generation = 4U;
    entry.key.session_generation = 1U;
    entry.key.frame_type = UCN_V6_FRAME_CONTROL;
    entry.key.protocol_opcode = opcode;
    entry.key.traffic_class = UCN_V6_TRAFFIC_Q0;
    entry.key.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    entry.key.interaction_role = UCN_V6_INTERACTION_ONE_WAY;
    entry.key.operation_id_policy = UCN_V6_OPERATION_ID_NONE;
    entry.key.direction = direction;
    return entry;
}

#if UCN_V6_FEATURE_ADAPTER_ENABLED && UCN_V6_FEATURE_REALTIME_ENABLED
static ucn_v6_acl_entry_t make_runtime_time_acl(
    const ucn_v6_principal_t *source_principal,
    const ucn_v6_binding_key_t *source_binding,
    const ucn_v6_binding_key_t *destination_binding,
    uint16_t opcode, ucn_v6_security_direction_t direction)
{
    ucn_v6_acl_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.occupied = true;
    entry.key.device_principal = *source_principal;
    entry.key.source_binding = *source_binding;
    entry.key.destination_binding = *destination_binding;
    entry.key.session_generation = 1U;
    entry.key.frame_type = UCN_V6_FRAME_CONTROL;
    entry.key.protocol_opcode = opcode;
    entry.key.traffic_class = UCN_V6_TRAFFIC_Q0;
    entry.key.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    entry.key.interaction_role = UCN_V6_INTERACTION_ONE_WAY;
    entry.key.operation_id_policy = UCN_V6_OPERATION_ID_NONE;
    entry.key.direction = direction;
    return entry;
}

static int install_runtime_time_route(
    ucn_v6_capability_owner_t *capability,
    ucn_v6_route_owner_t *route,
    const ucn_v6_principal_t *local_principal,
    const ucn_v6_binding_key_t *local_binding,
    const ucn_v6_principal_t *peer_principal,
    const ucn_v6_binding_key_t *peer_binding,
    uint16_t path_id, uint64_t now_us,
    ucn_v6_route_path_ref_t *reference)
{
    ucn_v6_capability_record_t record =
        pipeline_capability_record(1U, 6U);
    ucn_v6_security_open_result_t opened;
    ucn_v6_cached_peer_capability_t cached;
    ucn_v6_path_capability_t path;
    ucn_v6_route_domain_t domain;
    ucn_v6_route_path_t route_path;
    ucn_v6_route_activation_t activation;
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint64_t transaction_id = UINT64_C(0x70000000) + path_id;

    CHECK(ucn_v6_capability_record_encode(&record, payload) == UCN_V6_OK);
    memset(&opened, 0, sizeof(opened));
    opened.authenticated_principal = *peer_principal;
    opened.ingress_peer_session.principal = *peer_principal;
    opened.ingress_peer_session.binding = *peer_binding;
    opened.ingress_peer_session.session_generation = 1U;
    opened.ingress_link_instance_id = 1U;
    opened.ingress_link_instance_generation = 6U;
    opened.hop_authenticated = true;
    opened.frame.address_class = UCN_V6_ADDRESS_CLASS_A0;
    opened.frame.frame_type = UCN_V6_FRAME_CONTROL;
    opened.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_PROTOCOL_CONTEXT;
    opened.frame.traffic_class = UCN_V6_TRAFFIC_Q2;
    opened.frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    opened.frame.hop_limit = 1U;
    opened.frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    opened.frame.realm_id = peer_binding->realm_id;
    opened.frame.source_address = peer_binding->node_address;
    opened.frame.destination_address = local_binding->node_address;
    opened.frame.source_binding_generation =
        peer_binding->binding_generation;
    opened.frame.destination_binding_generation =
        local_binding->binding_generation;
    opened.frame.session_generation = 1U;
    opened.frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE;
    opened.frame.payload = payload;
    opened.frame.payload_length = sizeof(payload);
    CHECK(ucn_v6_capability_ingest_advertise(
              capability, now_us, &opened, &record) == UCN_V6_OK);
    CHECK(ucn_v6_capability_copy_peer(
              capability, now_us, peer_principal, peer_binding, 1U, 6U,
              &cached) == UCN_V6_OK);

    memset(&path, 0, sizeof(path));
    path.valid = true;
    path.immutable_for_realtime = true;
    path.destination_principal = *peer_principal;
    path.destination_binding = *peer_binding;
    path.destination_session_generation = 1U;
    path.destination_capability_generation =
        cached.record.capability_generation;
    memcpy(path.destination_capability_digest, cached.digest,
           sizeof(path.destination_capability_digest));
    path.destination_realtime_mode_bits =
        cached.record.peer.realtime_mode_bits;
    path.destination_clock_domain_id =
        cached.record.peer.clock_domain_id;
    path.destination_clock_domain_generation =
        cached.record.peer.clock_domain_generation;
    path.local_parent_session.principal = *peer_principal;
    path.local_parent_session.binding = *peer_binding;
    path.local_parent_session.session_generation = 1U;
    path.local_parent_link_id = 1U;
    path.local_parent_link_generation = 6U;
    path.local_parent_capability_generation =
        cached.record.capability_generation;
    memcpy(path.local_parent_capability_digest, cached.digest,
           sizeof(path.local_parent_capability_digest));
    path.route_generation = 1U;
    path.path_id = path_id;
    path.path_generation = 1U;
    path.hop_count = 1U;
    path.path_frame_mtu = cached.record.link.processing_frame_mtu;
    path.payload_budget = 160U;
    path.fragment_data_budget = 128U;
    path.feature_bits = cached.record.peer.feature_bits;
    path.hop_suite_bits = cached.record.peer.hop_suite_bits;
    path.e2e_suite_bits = cached.record.peer.e2e_suite_bits;
    path.max_message_class = cached.record.peer.max_message_class;
    path.max_window = cached.record.peer.max_rx_window;
    path.max_concurrency = cached.record.peer.max_concurrent_transfers;
    path.timestamp_capability_bits =
        cached.record.link.timestamp_capability_bits;
    path.timestamp_uncertainty_us =
        cached.record.link.timestamp_uncertainty_us;
    path.deadline_us = now_us + UINT64_C(100000);
    CHECK(ucn_v6_capability_install_path(capability, now_us, &path) ==
          UCN_V6_OK);

    memset(&domain, 0, sizeof(domain));
    domain.origin_principal = *local_principal;
    domain.origin_binding = *local_binding;
    domain.origin_session_generation = 1U;
    domain.destination_principal = *peer_principal;
    domain.destination_binding = *peer_binding;
    domain.destination_session_generation = 1U;
    memset(&route_path, 0, sizeof(route_path));
    route_path.path_id = path_id;
    route_path.path_generation = 1U;
    route_path.next_hop = path.local_parent_session;
    route_path.egress_link_id = 1U;
    route_path.egress_link_generation = 6U;
    route_path.next_hop_capability_generation =
        path.local_parent_capability_generation;
    route_path.hop_count = 1U;
    route_path.priority = 1U;
    route_path.weight = 1U;
    route_path.available = true;
    route_path.capability = path;
    CHECK(ucn_v6_route_candidate_begin(
              route, now_us + 1U, transaction_id, &domain, 1U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_add_path(
              route, now_us + 2U, transaction_id, &domain, &route_path) ==
          UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_probe(
              route, now_us + 3U, transaction_id, &domain, path_id, 1U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              route, now_us + 4U, transaction_id, &domain, &activation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_activation_send(
              route, now_us + 4U, transaction_id, &domain, true) ==
          UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_commit_ack(
              route, now_us + 5U, &activation) == UCN_V6_OK);
    memset(reference, 0, sizeof(*reference));
    reference->domain = domain;
    reference->route_generation = 1U;
    reference->path_id = path_id;
    reference->path_generation = 1U;
    return 0;
}
#endif

static ucn_v6_frame_t make_data_frame(const uint8_t *payload, size_t length)
{
    ucn_v6_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.address_class = UCN_V6_ADDRESS_CLASS_A0;
    frame.frame_type = UCN_V6_FRAME_DATA;
    frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                  UCN_V6_FLAG_E2E_CONTEXT |
                  UCN_V6_FLAG_MESSAGE_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q2;
    frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    frame.hop_limit = 4U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = 1U;
    frame.source_address = 7U;
    frame.destination_address = 8U;
    frame.source_binding_generation = 3U;
    frame.destination_binding_generation = 4U;
    frame.session_generation = 1U;
    frame.message.source_endpoint = 10U;
    frame.message.destination_endpoint = 20U;
    frame.message.interaction_role = UCN_V6_INTERACTION_REQUEST;
    frame.message.operation_id = 99U;
    frame.payload = payload;
    frame.payload_length = (uint16_t)length;
    return frame;
}

#if UCN_V6_FEATURE_ADAPTER_ENABLED && UCN_V6_FEATURE_REALTIME_ENABLED
static int runtime_time_service_timestamped_tx(
    const ucn_v6_stack_hooks_t *hooks, runtime_time_driver_t *driver,
    uint64_t now_us, uint64_t timestamp_us)
{
    ucn_v6_stack_phase_result_t phase;
    ucn_v6_driver_timestamp_t timestamp;
    bool submitted = false;
    CHECK(hooks->qos_tx(hooks->context, now_us, 1U, &phase) == UCN_V6_OK);
    CHECK(driver->submit_calls != 0U && driver->request_timestamp);
    memset(&timestamp, 0, sizeof(timestamp));
    timestamp.timestamp_us = timestamp_us;
    timestamp.uncertainty_us = 2U;
    timestamp.valid = true;
    timestamp.hardware = true;
    CHECK(ucn_v6_adapter_publish_tx_completion(
              driver->adapter, &driver->key, UCN_V6_OK, &timestamp, false) ==
          UCN_V6_OK);
    CHECK(hooks->tx_completion(hooks->context, now_us + 1U, 1U, &phase) ==
          UCN_V6_OK);
    CHECK(hooks->tx_completion(hooks->context, now_us + 2U, 1U, &phase) ==
          UCN_V6_OK);
    (void)submitted;
    return 0;
}

static int runtime_time_service_untimestamped_tx(
    const ucn_v6_stack_hooks_t *hooks, runtime_time_driver_t *driver,
    uint64_t now_us)
{
    ucn_v6_stack_phase_result_t phase;
    CHECK(hooks->qos_tx(hooks->context, now_us, 1U, &phase) == UCN_V6_OK);
    CHECK(driver->submit_calls != 0U && !driver->request_timestamp);
    return 0;
}

static int runtime_time_deliver(
    ucn_v6_adapter_owner_t *adapter,
    const ucn_v6_stack_hooks_t *hooks,
    const runtime_time_driver_t *source,
    uint64_t now_us, uint64_t timestamp_us)
{
    ucn_v6_stack_phase_result_t phase;
    ucn_v6_driver_timestamp_t timestamp;
    ucn_v6_driver_event_key_t key;
    ucn_v6_result_t result;
    memset(&timestamp, 0, sizeof(timestamp));
    timestamp.timestamp_us = timestamp_us;
    timestamp.uncertainty_us = 2U;
    timestamp.valid = true;
    timestamp.hardware = true;
    CHECK(ucn_v6_adapter_publish_rx(
              adapter, 1U, 6U, source->frame, source->frame_length,
              &timestamp, false, &key) == UCN_V6_OK);
    result = hooks->rx_ingress(hooks->context, now_us, 1U, &phase);
    if (result != UCN_V6_OK) {
        fprintf(stderr, "runtime time RX failed: %d at %llu\n", (int)result,
                (unsigned long long)now_us);
    }
    CHECK(result == UCN_V6_OK);
    CHECK(phase.work_done == 1U);
    return 0;
}

static int test_runtime_owned_four_event_exchange(void)
{
    static const uint8_t admin_proof[] = { UCN_V6_PROOF_REALM_ADMIN };
    ucn_v6_principal_t a = make_principal(0x11U);
    ucn_v6_principal_t b = make_principal(0x31U);
    ucn_v6_principal_t admin = make_principal(0xA1U);
    ucn_v6_binding_key_t a_binding = { 1U, 10U, 1U };
    ucn_v6_binding_key_t b_binding = { 1U, 20U, 2U };
    fake_store_t security_stores[2];
    fake_crypto_t cryptos[2];
    ucn_v6_security_store_ops_t security_store_ops[2];
    ucn_v6_security_crypto_ops_t security_crypto_ops[2];
    ucn_v6_callback_gate_t security_gates[2];
    ucn_v6_security_manager_storage_t security_storage[2];
    ucn_v6_security_manager_t *security[2] = { NULL, NULL };
    ucn_v6_capability_record_t local_capability;
    ucn_v6_capability_owner_t *capability[2] = { NULL, NULL };
    ucn_v6_route_owner_t *route[2] = { NULL, NULL };
    ucn_v6_route_path_ref_t a_to_b;
    ucn_v6_route_path_ref_t b_to_a;
    pipeline_runtime_t adapter_locks[2];
    runtime_time_driver_t drivers[2];
    ucn_v6_driver_runtime_ops_t adapter_ops[2];
    ucn_v6_driver_link_config_t link;
    ucn_v6_adapter_owner_t *adapter[2] = { NULL, NULL };
    runtime_time_store_t time_store;
    ucn_v6_realtime_generation_store_ops_t time_store_ops;
    ucn_v6_callback_gate_t time_gate = UCN_V6_CALLBACK_GATE_INITIALIZER;
    ucn_v6_realtime_owner_t *realtime = NULL;
    ucn_v6_time_domain_config_t domain;
    runtime_time_app_t apps[2];
    ucn_v6_runtime_config_t runtime_config;
    ucn_v6_runtime_owner_t *runtime[2] = { NULL, NULL };
    ucn_v6_stack_hooks_t hooks[2];
    ucn_v6_acl_entry_t acl;
    ucn_v6_time_sync_announce_t announce;
    ucn_v6_runtime_time_handle_t master_handle;
    ucn_v6_runtime_time_handle_t forged_handle = {
        { UINT64_C(4), UINT64_C(5) }
    };
    ucn_v6_realtime_clock_view_t clock_view;
    ucn_v6_runtime_view_t runtime_view;
    uint8_t dummy[2] = {0U, 0U};
    unsigned before_submits;
    size_t index;

    memset(security_stores, 0, sizeof(security_stores));
    memset(cryptos, 0, sizeof(cryptos));
    memset(security_gates, 0, sizeof(security_gates));
    memset(security_storage, 0, sizeof(security_storage));
    for (index = 0U; index < 2U; ++index) {
        security_store_ops[index] = store_ops(&security_stores[index]);
        security_crypto_ops[index] = crypto_ops(&cryptos[index]);
        CHECK(ucn_v6_callback_gate_init(
                  &security_gates[index], NULL, no_lock, no_lock) ==
              UCN_V6_OK);
    }
    CHECK(ucn_v6_security_init_in_place(
              security_storage[0].bytes, sizeof(security_storage[0]),
              ucn_v6_compiled_manifest(), 1U, &a, &security_store_ops[0],
              &security_crypto_ops[0], &security_gates[0], &security[0]) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              security_storage[1].bytes, sizeof(security_storage[1]),
              ucn_v6_compiled_manifest(), 1U, &b, &security_store_ops[1],
              &security_crypto_ops[1], &security_gates[1], &security[1]) ==
          UCN_V6_OK);
    CHECK(install_pair_session(
              security[0], &a, 10U, 1U, &b, 20U, 2U, true, 501U, 51U) ==
          0);
    CHECK(install_pair_session(
              security[1], &a, 10U, 1U, &b, 20U, 2U, false, 501U, 51U) ==
          0);

    acl = make_runtime_time_acl(
        &a, &a_binding, &b_binding, UCN_V6_PROTOCOL_OPCODE_TIME_SYNC,
        UCN_V6_SECURITY_OUTBOUND);
    CHECK(ucn_v6_security_set_acl(
              security[0], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    acl.key.direction = UCN_V6_SECURITY_INBOUND;
    CHECK(ucn_v6_security_set_acl(
              security[1], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    acl = make_runtime_time_acl(
        &b, &b_binding, &a_binding,
        UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_REQUEST,
        UCN_V6_SECURITY_OUTBOUND);
    CHECK(ucn_v6_security_set_acl(
              security[1], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    acl.key.direction = UCN_V6_SECURITY_INBOUND;
    CHECK(ucn_v6_security_set_acl(
              security[0], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    acl = make_runtime_time_acl(
        &a, &a_binding, &b_binding,
        UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE,
        UCN_V6_SECURITY_OUTBOUND);
    CHECK(ucn_v6_security_set_acl(
              security[0], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    acl.key.direction = UCN_V6_SECURITY_INBOUND;
    CHECK(ucn_v6_security_set_acl(
              security[1], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);

    local_capability = pipeline_capability_record(1U, 6U);
    for (index = 0U; index < 2U; ++index) {
        memset(&runtime_time_capability_storage[index], 0,
               sizeof(runtime_time_capability_storage[index]));
        memset(&runtime_time_route_storage[index], 0,
               sizeof(runtime_time_route_storage[index]));
        CHECK(ucn_v6_capability_owner_init_in_place(
                  runtime_time_capability_storage[index].bytes,
                  sizeof(runtime_time_capability_storage[index]),
                  ucn_v6_compiled_manifest(), &local_capability,
                  UINT64_C(100000), UINT64_C(100000),
                  &capability[index]) == UCN_V6_OK);
        CHECK(ucn_v6_route_owner_init_in_place(
                  runtime_time_route_storage[index].bytes,
                  sizeof(runtime_time_route_storage[index]),
                  ucn_v6_compiled_manifest(), capability[index],
                  UINT64_C(10000), UINT64_C(100), 3U,
                  UINT64_C(1000), UINT64_C(1000), &route[index]) ==
              UCN_V6_OK);
    }
    CHECK(install_runtime_time_route(
              capability[0], route[0], &a, &a_binding, &b, &b_binding,
              2U, 50U, &a_to_b) == 0);
    CHECK(install_runtime_time_route(
              capability[1], route[1], &b, &b_binding, &a, &a_binding,
              3U, 50U, &b_to_a) == 0);

    memset(&time_store, 0, sizeof(time_store));
    memset(&time_store_ops, 0, sizeof(time_store_ops));
    time_store_ops.context = &time_store;
    time_store_ops.load_domain_record = runtime_time_load_domain;
    time_store_ops.reserve_domain_record = runtime_time_reserve_domain;
    CHECK(ucn_v6_callback_gate_init(
              &time_gate, NULL, no_lock, no_lock) == UCN_V6_OK);
    memset(&runtime_time_realtime_storage, 0,
           sizeof(runtime_time_realtime_storage));
    CHECK(ucn_v6_realtime_owner_init_in_place(
              runtime_time_realtime_storage.bytes,
              sizeof(runtime_time_realtime_storage),
              ucn_v6_compiled_manifest(), route[1], &time_store_ops,
              &time_gate, &realtime) == UCN_V6_OK);
    memset(&domain, 0, sizeof(domain));
    domain.clock_domain_id = 1U;
    domain.domain_generation = 1U;
    domain.master_principal = a;
    domain.master_binding = a_binding;
    domain.master_session_generation = 1U;
    domain.lock_sample_count = 1U;
    domain.sync_timeout_us = UINT64_C(1000);
    domain.max_holdover_us = UINT64_C(2000);
    domain.max_offset_jump_us = 1000U;
    domain.oscillator_uncertainty_ppb = 1000U;
    domain.timer_resolution_bound_us = 1U;
    domain.filter_residual_bound_us = 1U;
    domain.arithmetic_rounding_bound_us = 1U;
    domain.sample_capture_bound_us = 1U;
    CHECK(ucn_v6_realtime_bind_domain(
              realtime, &domain, &b_to_a, 60U) == UCN_V6_OK);

    memset(adapter_locks, 0, sizeof(adapter_locks));
    memset(drivers, 0, sizeof(drivers));
    memset(adapter_ops, 0, sizeof(adapter_ops));
    for (index = 0U; index < 2U; ++index) {
        adapter_ops[index].context = &adapter_locks[index];
        adapter_ops[index].lock_task = pipeline_lock_task;
        adapter_ops[index].try_lock_from_isr = pipeline_try_lock_from_isr;
        adapter_ops[index].unlock_task = pipeline_unlock;
        adapter_ops[index].unlock_from_isr = pipeline_unlock;
        adapter_ops[index].post_owner_event = pipeline_post;
        memset(&runtime_time_adapter_storage[index], 0,
               sizeof(runtime_time_adapter_storage[index]));
        CHECK(ucn_v6_adapter_init_in_place(
                  runtime_time_adapter_storage[index].bytes,
                  sizeof(runtime_time_adapter_storage[index]),
                  ucn_v6_compiled_manifest(), &adapter_ops[index],
                  &adapter[index]) == UCN_V6_OK);
        drivers[index].adapter = adapter[index];
        memset(&link, 0, sizeof(link));
        link.link_id = 1U;
        link.initial_generation = 6U;
        link.bearer = UCN_V6_BEARER_CUSTOM;
        link.nominal_bitrate_bps = UINT32_C(3000000);
        link.carrier_mtu = UCN_V6_CONFIG_ADAPTER_FRAME_BYTES;
        link.link_frame_mtu = UCN_V6_CONFIG_ADAPTER_FRAME_BYTES;
        link.hardware_priority_count = 4U;
        link.rx_slot_quota = 2U;
        link.tx_slot_quota = 2U;
        link.rx_timestamp_hardware = true;
        link.tx_timestamp_hardware = true;
        link.ops.struct_size = sizeof(link.ops);
        link.ops.api_version = UCN_V6_ADAPTER_API_VERSION;
        link.ops.context = &drivers[index];
        link.ops.submit = runtime_time_submit;
        link.ops.cancel = pipeline_cancel;
        link.ops.quiesce = pipeline_quiesce;
        CHECK(ucn_v6_adapter_register_link(adapter[index], &link) ==
              UCN_V6_OK);
        CHECK(ucn_v6_adapter_set_link_readiness(
                  adapter[index], 1U, 6U, UCN_V6_LINK_READY) == UCN_V6_OK);
    }

    memset(apps, 0, sizeof(apps));
    apps[0].security = security[0];
    apps[0].peer = b;
    apps[0].reverse_ref = a_to_b;
    apps[0].next_buffer_token = UINT64_C(1000);
    apps[0].auto_send_delay_request = true;
    apps[1].security = security[1];
    apps[1].peer = a;
    apps[1].reverse_ref = b_to_a;
    apps[1].next_buffer_token = UINT64_C(2000);
    apps[1].auto_send_delay_request = true;
    for (index = 0U; index < 2U; ++index) {
        memset(&runtime_config, 0, sizeof(runtime_config));
        runtime_config.runtime_instance_generation = index + 1U;
        runtime_config.adapter = adapter[index];
        runtime_config.bootstrap =
            (ucn_v6_bootstrap_owner_t *)&dummy[index];
        runtime_config.security = security[index];
        runtime_config.capability = capability[index];
        runtime_config.route = route[index];
        runtime_config.metric = (ucn_v6_metric_owner_t *)&dummy[index];
        runtime_config.qos = (ucn_v6_qos_owner_t *)&dummy[index];
        runtime_config.transfer = (ucn_v6_transfer_owner_t *)&dummy[index];
        runtime_config.realtime = index == 1U ? realtime :
            (ucn_v6_realtime_owner_t *)&dummy[index];
#if UCN_V6_FEATURE_CLUSTER_ENABLED
        runtime_config.cluster = (ucn_v6_cluster_owner_t *)&dummy[index];
#endif
        runtime_config.app.context = &apps[index];
        runtime_config.app.handle_ingress = runtime_time_ingress;
        runtime_config.app.release_buffer = runtime_time_release;
        memset(&runtime_time_runtime_storage[index], 0,
               sizeof(runtime_time_runtime_storage[index]));
        CHECK(ucn_v6_runtime_init_in_place(
                  runtime_time_runtime_storage[index].bytes,
                  sizeof(runtime_time_runtime_storage[index]),
                  ucn_v6_compiled_manifest(), &runtime_config,
                  &runtime[index]) == UCN_V6_OK);
        CHECK(ucn_v6_runtime_make_stack_hooks(runtime[index], &hooks[index]) ==
              UCN_V6_OK);
    }

    before_submits = drivers[1].submit_calls;
    CHECK(ucn_v6_runtime_time_send_delay_request(
              runtime[1], &forged_handle, UINT64_C(999), 99U) ==
          UCN_V6_ERR_NOT_FOUND);
    CHECK(drivers[1].submit_calls == before_submits);

    memset(&announce, 0, sizeof(announce));
    announce.clock_domain_id = 1U;
    announce.domain_generation = 1U;
    announce.sync_sequence = 1U;
    CHECK(ucn_v6_runtime_time_start_sync(
              runtime[0], &a_to_b, &announce, UINT64_C(101), 100U,
              &master_handle) == UCN_V6_OK);
    CHECK(ucn_v6_runtime_time_send_delay_request(
              runtime[1], &master_handle, UINT64_C(998), 100U) ==
          UCN_V6_ERR_NOT_FOUND);
    CHECK(runtime_time_service_timestamped_tx(
              &hooks[0], &drivers[0], 105U, 110U) == 0);
    CHECK(runtime_time_deliver(
              adapter[1], &hooks[1], &drivers[0], 121U, 120U) == 0);
    CHECK(apps[1].callback_result == UCN_V6_OK &&
          apps[1].sync_frames == 1U);
    CHECK(runtime_time_service_timestamped_tx(
              &hooks[1], &drivers[1], 125U, 130U) == 0);
    CHECK(runtime_time_deliver(
              adapter[0], &hooks[0], &drivers[1], 141U, 140U) == 0);
    CHECK(apps[0].callback_result == UCN_V6_OK &&
          apps[0].delay_requests == 1U);
    CHECK(runtime_time_service_untimestamped_tx(
              &hooks[0], &drivers[0], 145U) == 0);
    CHECK(runtime_time_deliver(
              adapter[1], &hooks[1], &drivers[0], 151U, 150U) == 0);
    CHECK(apps[1].callback_result == UCN_V6_OK &&
          apps[1].delay_responses == 1U);
    CHECK(ucn_v6_realtime_get_clock(realtime, 1U, 160U, &clock_view) ==
          UCN_V6_OK);
    CHECK(clock_view.available && clock_view.domain_generation == 1U &&
          clock_view.domain_time_us == 160U);
    CHECK(ucn_v6_runtime_copy_view(runtime[1], &runtime_view) == UCN_V6_OK);
    CHECK(runtime_view.realtime_exchanges_started == 1U &&
          runtime_view.realtime_tx_timestamps_captured == 1U &&
          runtime_view.realtime_exchanges_completed == 1U);

#if UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGES > 1U
    /* Profiles with a second concurrent exchange slot can observe another
     * authenticated Sync while the Master intentionally retains the first
     * response for exact duplicate replay. Only the Runtime-issued handle is
     * usable, and the half-open deadline rejects it without Adapter effects.
     * A one-slot Nano profile already completed the full T1/T2/T3/T4 path
     * above; its sole Master slot remains occupied until replay expiry. */
    apps[1].auto_send_delay_request = false;
    announce.sync_sequence = 2U;
    CHECK(ucn_v6_runtime_time_start_sync(
              runtime[0], &a_to_b, &announce, UINT64_C(102), 200U,
              &master_handle) == UCN_V6_OK);
    CHECK(runtime_time_service_timestamped_tx(
              &hooks[0], &drivers[0], 205U, 210U) == 0);
    CHECK(runtime_time_deliver(
              adapter[1], &hooks[1], &drivers[0], 221U, 220U) == 0);
    CHECK(apps[1].sync_frames == 2U);
    before_submits = drivers[1].submit_calls;
    CHECK(ucn_v6_runtime_time_send_delay_request(
              runtime[0], &apps[1].member_handle, UINT64_C(997), 221U) ==
          UCN_V6_ERR_NOT_FOUND);
    CHECK(ucn_v6_runtime_time_send_delay_request(
              runtime[1], &apps[1].member_handle, UINT64_C(996),
              221U + UCN_V6_CONFIG_RUNTIME_TIME_EXCHANGE_TIMEOUT_US) ==
          UCN_V6_ERR_TIMEOUT);
    CHECK(drivers[1].submit_calls == before_submits);
#endif
    return 0;
}
#endif

static int test_join_acl_aead_replay(void)
{
    static const uint8_t admin_proof[] = { UCN_V6_PROOF_REALM_ADMIN };
    const uint8_t payload[] = { 1U, 2U, 3U, 4U, 5U };
    uint8_t cipher[sizeof(payload)];
    uint8_t frame_work[256U];
    uint8_t encoded[256U];
    uint8_t plaintext[sizeof(payload)];
    uint8_t tampered[256U];
    size_t encoded_length = 0U;
    fake_store_t device_store;
    fake_store_t authority_store;
    fake_crypto_t device_crypto;
    fake_crypto_t authority_crypto;
    ucn_v6_callback_gate_t device_gate;
    ucn_v6_callback_gate_t authority_gate;
    ucn_v6_bootstrap_owner_storage_t device_bootstrap_storage;
    ucn_v6_bootstrap_owner_storage_t authority_bootstrap_storage;
    ucn_v6_bootstrap_owner_t *device_bootstrap = NULL;
    ucn_v6_bootstrap_owner_t *authority_bootstrap = NULL;
    const ucn_v6_bootstrap_config_t bootstrap_config = {
        2U, 2U, 4U, 4U, 1000U
    };
    ucn_v6_security_manager_storage_t device_storage;
    ucn_v6_security_manager_storage_t authority_storage;
    ucn_v6_security_manager_t *device_manager = NULL;
    ucn_v6_security_manager_t *authority_manager = NULL;
    ucn_v6_principal_t device = make_principal(0x10U);
    ucn_v6_principal_t authority = make_principal(0x20U);
    ucn_v6_principal_t admin = make_principal(0xA0U);
    ucn_v6_bootstrap_transcript_t transcript =
        make_transcript(&device, &authority);
    ucn_v6_join_commit_t device_join = make_join(&transcript, true);
    ucn_v6_join_commit_t authority_join = make_join(&transcript, false);
    ucn_v6_bootstrap_key_t bootstrap_key =
        make_bootstrap_key(&transcript, 1U);
    ucn_v6_security_store_ops_t device_store_api;
    ucn_v6_security_store_ops_t authority_store_api;
    ucn_v6_security_crypto_ops_t device_crypto_api;
    ucn_v6_security_crypto_ops_t authority_crypto_api;
    ucn_v6_acl_entry_t outbound;
    ucn_v6_acl_entry_t inbound;
    ucn_v6_frame_t frame;
    ucn_v6_security_open_result_t opened;
    ucn_v6_security_open_result_t sentinel;
    ucn_v6_security_view_t view;
    ucn_v6_stack_invalidation_t invalidation;
    uint8_t durable_receipt[512U];
    uint8_t durable_receipt_again[512U];
    size_t durable_receipt_length = 0U;
    size_t durable_receipt_again_length = 0U;
    uint64_t durable_receipt_generation = 0U;
    uint64_t durable_receipt_again_generation = 0U;
    static const uint8_t durable_receipt_proof[] = {
        UCN_V6_PROOF_SESSION_DURABLE_RECEIPT
    };

    memset(&device_store, 0, sizeof(device_store));
    memset(&authority_store, 0, sizeof(authority_store));
    memset(&device_crypto, 0, sizeof(device_crypto));
    memset(&authority_crypto, 0, sizeof(authority_crypto));
    memset(&device_gate, 0, sizeof(device_gate));
    memset(&authority_gate, 0, sizeof(authority_gate));
    device_store_api = store_ops(&device_store);
    authority_store_api = store_ops(&authority_store);
    device_crypto_api = crypto_ops(&device_crypto);
    authority_crypto_api = crypto_ops(&authority_crypto);
    CHECK(ucn_v6_callback_gate_init(&device_gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_callback_gate_init(&authority_gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              device_bootstrap_storage.bytes, sizeof(device_bootstrap_storage),
              ucn_v6_compiled_manifest(), &bootstrap_config,
              &device_bootstrap) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              authority_bootstrap_storage.bytes,
              sizeof(authority_bootstrap_storage), ucn_v6_compiled_manifest(),
              &bootstrap_config, &authority_bootstrap) == UCN_V6_OK);
    CHECK(complete_bootstrap(device_bootstrap, &transcript, &bootstrap_key,
                             10U) == 0);
    CHECK(complete_bootstrap(authority_bootstrap, &transcript, &bootstrap_key,
                             10U) == 0);
    CHECK(ucn_v6_security_init_in_place(
              device_storage.bytes, sizeof(device_storage),
              ucn_v6_compiled_manifest(), 1U, &device, &device_store_api,
              &device_crypto_api, &device_gate, &device_manager) == UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              authority_storage.bytes, sizeof(authority_storage),
              ucn_v6_compiled_manifest(), 1U, &authority,
              &authority_store_api, &authority_crypto_api, &authority_gate,
              &authority_manager) == UCN_V6_OK);
    {
        ucn_v6_bootstrap_owner_storage_t incomplete_storage;
        ucn_v6_bootstrap_owner_t *incomplete_owner = NULL;
        ucn_v6_bootstrap_transcript_t incomplete_transcript = transcript;
        ucn_v6_bootstrap_key_t incomplete_key;
        ucn_v6_join_commit_t incomplete_join;
        incomplete_transcript.transaction_id = 77U;
        incomplete_key = make_bootstrap_key(&incomplete_transcript, 9U);
        incomplete_join = make_join(&incomplete_transcript, true);
        CHECK(ucn_v6_bootstrap_owner_init_in_place(
                  incomplete_storage.bytes, sizeof(incomplete_storage),
                  ucn_v6_compiled_manifest(), &bootstrap_config,
                  &incomplete_owner) == UCN_V6_OK);
        CHECK(ucn_v6_bootstrap_open_after_cookie(
                  incomplete_owner, UCN_V6_BOOTSTRAP_FLOW_JOIN,
                  &incomplete_key, &incomplete_transcript, NULL, true, 20U) ==
              UCN_V6_OK);
        CHECK(ucn_v6_security_commit_join(
                  device_manager, incomplete_owner, &incomplete_key, 30U,
                  &incomplete_join) == UCN_V6_ERR_STATE);
        CHECK(device_store.submits == 1U);
    }
    {
        ucn_v6_bootstrap_pending_t pending = {0};
        CHECK(ucn_v6_bootstrap_copy_pending(
                  device_bootstrap, UCN_V6_BOOTSTRAP_FLOW_JOIN,
                  &bootstrap_key, &pending) == UCN_V6_OK);
        CHECK(pending.challenge_started_local_us == 10U);
    }

    /* A durable peer receipt may supplement proof, but it must never replace
     * the receiver's own live Bootstrap transaction because that local Owner
     * is the only source of the lease start time. */
    CHECK(ucn_v6_security_commit_join(
              authority_manager, authority_bootstrap, &bootstrap_key, 100U,
              &authority_join) == UCN_V6_OK);
    memset(durable_receipt, 0xA5, sizeof(durable_receipt));
    CHECK(ucn_v6_security_export_join_durable_receipt(
              authority_manager, &device, durable_receipt,
              sizeof(durable_receipt), &durable_receipt_length,
              &durable_receipt_generation) == UCN_V6_OK);
    CHECK(durable_receipt_length == UCN_V6_SECURITY_JOIN_RECEIPT_BYTES &&
          durable_receipt[0] == 0xD1U &&
          durable_receipt_generation ==
              authority_store.snapshot.snapshot_generation);
    memset(durable_receipt_again, 0x5A, sizeof(durable_receipt_again));
    CHECK(ucn_v6_security_export_join_durable_receipt(
              authority_manager, &device, durable_receipt_again,
              sizeof(durable_receipt_again),
              &durable_receipt_again_length,
              &durable_receipt_again_generation) == UCN_V6_OK);
    CHECK(durable_receipt_again_length == durable_receipt_length &&
          durable_receipt_again_generation == durable_receipt_generation &&
          memcmp(durable_receipt_again, durable_receipt,
                 durable_receipt_length) == 0);
    {
        ucn_v6_principal_t stranger = make_principal(0xE0U);
        uint8_t before[sizeof(durable_receipt_again)];
        size_t length_before = durable_receipt_again_length;
        uint64_t generation_before = durable_receipt_again_generation;
        memcpy(before, durable_receipt_again, sizeof(before));
        CHECK(ucn_v6_security_export_join_durable_receipt(
                  authority_manager, &stranger, durable_receipt_again,
                  sizeof(durable_receipt_again),
                  &durable_receipt_again_length,
                  &durable_receipt_again_generation) == UCN_V6_ERR_NOT_FOUND);
        CHECK(memcmp(before, durable_receipt_again, sizeof(before)) == 0 &&
              durable_receipt_again_length == length_before &&
              durable_receipt_again_generation == generation_before);
    }
    device_join.peer_durable_receipt_proof = durable_receipt_proof;
    device_join.peer_durable_receipt_proof_length =
        sizeof(durable_receipt_proof);
    device_join.peer_durable_receipt_generation =
        durable_receipt_generation;
    CHECK(ucn_v6_security_commit_join(
              device_manager, NULL, NULL, 100U, &device_join) ==
          UCN_V6_ERR_SECURITY);
    CHECK(device_store.submits == 1U && authority_store.submits == 2U);
    CHECK(ucn_v6_security_commit_join(
              device_manager, device_bootstrap, &bootstrap_key, 101U,
              &device_join) == UCN_V6_OK);
    CHECK(device_store.submits == 2U);
    --device_join.authority_freshness.max_remaining_lease_us;
    CHECK(ucn_v6_security_commit_join(
              device_manager, device_bootstrap, &bootstrap_key, 102U,
              &device_join) == UCN_V6_ERR_SECURITY);
    ++device_join.authority_freshness.max_remaining_lease_us;
    CHECK(device_store.submits == 2U);

    outbound = make_acl(&device, 7U, 3U, 8U, 4U,
                        UCN_V6_SECURITY_OUTBOUND);
    inbound = make_acl(&device, 7U, 3U, 8U, 4U,
                       UCN_V6_SECURITY_INBOUND);
    CHECK(ucn_v6_security_set_acl(device_manager, &outbound, &admin,
                                  admin_proof, sizeof(admin_proof)) == UCN_V6_OK);
    CHECK(ucn_v6_security_set_acl(authority_manager, &inbound, &admin,
                                  admin_proof, sizeof(admin_proof)) == UCN_V6_OK);

    frame = make_data_frame(payload, sizeof(payload));
    {
        typedef union frame_length_alias {
            ucn_v6_frame_t frame;
            size_t length;
        } frame_length_alias_t;
        frame_length_alias_t alias;
        frame_length_alias_t alias_before;
        uint8_t alias_payload_work[32U];
        uint8_t alias_frame_work[256U];
        uint8_t alias_output[256U];
        uint8_t payload_before[sizeof(alias_payload_work)];
        uint8_t work_before[sizeof(alias_frame_work)];
        uint8_t output_before[sizeof(alias_output)];
        unsigned submits_before = device_store.submits;

        memset(&alias, 0, sizeof(alias));
        alias.frame = frame;
        memcpy(&alias_before, &alias, sizeof(alias_before));
        memset(alias_payload_work, 0x31, sizeof(alias_payload_work));
        memset(alias_frame_work, 0x42, sizeof(alias_frame_work));
        memset(alias_output, 0x53, sizeof(alias_output));
        memcpy(payload_before, alias_payload_work, sizeof(payload_before));
        memcpy(work_before, alias_frame_work, sizeof(work_before));
        memcpy(output_before, alias_output, sizeof(output_before));
        CHECK(ucn_v6_security_protect_frame(
                  device_manager, 149U, &authority, &authority,
                  &alias.frame, alias_payload_work,
                  sizeof(alias_payload_work), alias_frame_work,
                  sizeof(alias_frame_work), alias_output,
                  sizeof(alias_output), &alias.length) ==
              UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(&alias, &alias_before, sizeof(alias)) == 0);
        CHECK(memcmp(alias_payload_work, payload_before,
                     sizeof(payload_before)) == 0);
        CHECK(memcmp(alias_frame_work, work_before, sizeof(work_before)) == 0);
        CHECK(memcmp(alias_output, output_before, sizeof(output_before)) == 0);
        CHECK(device_store.submits == submits_before);
    }
    {
        uint8_t alias[32U];
        uint8_t alias_before[sizeof(alias)];
        ucn_v6_frame_t alias_frame;
        ucn_v6_frame_t frame_before;
        size_t length_before = 91U;
        unsigned submits_before = device_store.submits;
        memset(alias, 0x5AU, sizeof(alias));
        memcpy(alias_before, alias, sizeof(alias));
        alias_frame = make_data_frame(alias, sizeof(payload));
        frame_before = alias_frame;
        CHECK(ucn_v6_security_protect_frame(
                  device_manager, 149U, &authority, &authority, &alias_frame,
                  alias + 1U, sizeof(alias) - 1U, frame_work,
                  sizeof(frame_work), encoded, sizeof(encoded),
                  &length_before) == UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(alias, alias_before, sizeof(alias)) == 0 &&
              memcmp(&alias_frame, &frame_before, sizeof(alias_frame)) == 0 &&
              length_before == 91U && device_store.submits == submits_before);
    }
    {
        ucn_v6_frame_t frame_before = frame;
        uint8_t output_before[sizeof(encoded)];
        size_t length_before = 77U;
        memset(encoded, 0xA6, sizeof(encoded));
        memcpy(output_before, encoded, sizeof(encoded));
        CHECK(ucn_v6_security_protect_frame(
                  device_manager, 149U, &authority, &authority, &frame,
                  cipher, sizeof(cipher), frame_work, sizeof(frame_work),
                  encoded, 1U, &length_before) == UCN_V6_ERR_NO_SPACE);
        CHECK(memcmp(&frame, &frame_before, sizeof(frame)) == 0);
        CHECK(memcmp(encoded, output_before, sizeof(encoded)) == 0);
        CHECK(length_before == 77U);
    }
    CHECK(ucn_v6_security_protect_frame(
              device_manager, 150U, &authority, &authority, &frame,
              cipher, sizeof(cipher), frame_work, sizeof(frame_work),
              encoded, sizeof(encoded), &encoded_length) == UCN_V6_OK);
    CHECK(frame.origin_sequence == 1U && frame.hop_sequence == 1U);
    CHECK(memcmp(cipher, payload, sizeof(payload)) != 0);
    memset(&opened, 0, sizeof(opened));
    sentinel = opened;
    CHECK(ucn_v6_security_open_frame(
              authority_manager, 200U, 1U, 6U, &device, encoded, encoded_length,
              encoded + 1U, encoded_length - 1U, &opened) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(&opened, &sentinel, sizeof(opened)) == 0);
    CHECK(ucn_v6_security_open_frame(
              authority_manager, 200U, 1U, 6U, &device, encoded, encoded_length,
              plaintext, sizeof(plaintext), &opened) == UCN_V6_OK);
    CHECK(opened.hop_authenticated && opened.endpoint_authorized &&
          !opened.group_discovery_only);
    CHECK(memcmp(plaintext, payload, sizeof(payload)) == 0);
    memset(&sentinel, 0xA5, sizeof(sentinel));
    opened = sentinel;
    CHECK(ucn_v6_security_open_frame(
              authority_manager, 201U, 1U, 6U, &device, encoded, encoded_length,
              plaintext, sizeof(plaintext), &opened) == UCN_V6_ERR_REPLAY);
    CHECK(memcmp(&opened, &sentinel, sizeof(opened)) == 0);

    memcpy(tampered, encoded, encoded_length);
    tampered[encoded_length - 5U] ^= 1U;
    /* Recompute only CRC: the Link Tag must still reject the mutation. */
    {
        uint32_t crc = ucn_v6_crc32c(tampered, encoded_length - 4U);
        tampered[encoded_length - 4U] = (uint8_t)(crc >> 24U);
        tampered[encoded_length - 3U] = (uint8_t)(crc >> 16U);
        tampered[encoded_length - 2U] = (uint8_t)(crc >> 8U);
        tampered[encoded_length - 1U] = (uint8_t)crc;
    }
    CHECK(ucn_v6_security_open_frame(
              authority_manager, 202U, 1U, 6U, &device, tampered, encoded_length,
              plaintext, sizeof(plaintext), &opened) == UCN_V6_ERR_SECURITY);
    {
        ucn_v6_acl_entry_t control_out = make_control_acl(
            &device, 10U, UCN_V6_SECURITY_OUTBOUND);
        ucn_v6_acl_entry_t control_in = make_control_acl(
            &device, 10U, UCN_V6_SECURITY_INBOUND);
        ucn_v6_acl_entry_t other_out = make_control_acl(
            &device, 11U, UCN_V6_SECURITY_OUTBOUND);
        ucn_v6_frame_t control;
        uint8_t one_byte_work[1U];
        CHECK(ucn_v6_security_set_acl(
                  device_manager, &control_out, &admin, admin_proof,
                  sizeof(admin_proof)) == UCN_V6_OK);
        CHECK(ucn_v6_security_set_acl(
                  authority_manager, &control_in, &admin, admin_proof,
                  sizeof(admin_proof)) == UCN_V6_OK);
        CHECK(ucn_v6_security_set_acl(
                  device_manager, &other_out, &admin, admin_proof,
                  sizeof(admin_proof)) == UCN_V6_OK);
        memset(&control, 0, sizeof(control));
        control.address_class = UCN_V6_ADDRESS_CLASS_A0;
        control.frame_type = UCN_V6_FRAME_CONTROL;
        control.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                        UCN_V6_FLAG_E2E_CONTEXT |
                        UCN_V6_FLAG_PROTOCOL_CONTEXT;
        control.traffic_class = UCN_V6_TRAFFIC_Q0;
        control.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
        control.hop_limit = 2U;
        control.header_contract = UCN_V6_HEADER_CONTRACT_1;
        control.realm_id = 1U;
        control.source_address = 7U;
        control.destination_address = 8U;
        control.source_binding_generation = 3U;
        control.destination_binding_generation = 4U;
        control.session_generation = 1U;
        control.protocol_opcode = 10U;
        CHECK(ucn_v6_security_protect_frame(
                  device_manager, 250U, &authority, &authority, &control,
                  one_byte_work, sizeof(one_byte_work), frame_work,
                  sizeof(frame_work), encoded, sizeof(encoded),
                  &encoded_length) == UCN_V6_OK);
        CHECK(ucn_v6_security_open_frame(
                  authority_manager, 251U, 1U, 6U, &device, encoded,
                  encoded_length, one_byte_work, sizeof(one_byte_work),
                  &opened) == UCN_V6_OK);
        control.protocol_opcode = 11U;
        control.origin_sequence = 0U;
        control.hop_sequence = 0U;
        CHECK(ucn_v6_security_protect_frame(
                  device_manager, 252U, &authority, &authority, &control,
                  one_byte_work, sizeof(one_byte_work), frame_work,
                  sizeof(frame_work), encoded, sizeof(encoded),
                  &encoded_length) == UCN_V6_OK);
        memset(&sentinel, 0x5A, sizeof(sentinel));
        opened = sentinel;
        CHECK(ucn_v6_security_open_frame(
                  authority_manager, 253U, 1U, 6U, &device, encoded,
                  encoded_length, one_byte_work, sizeof(one_byte_work),
                  &opened) == UCN_V6_ERR_ACCESS);
        CHECK(memcmp(&opened, &sentinel, sizeof(opened)) == 0);
    }
    {
        uint8_t hello_payload[24U];
        ucn_v6_frame_t hello;
        ucn_v6_frame_t before;
        uint8_t output_before[sizeof(encoded)];
        size_t length_before = 91U;
        fill_bytes(hello_payload, sizeof(hello_payload), 0xC0U);
        memset(&hello, 0, sizeof(hello));
        hello.address_class = UCN_V6_ADDRESS_CLASS_A0;
        hello.frame_type = UCN_V6_FRAME_CONTROL;
        hello.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                      UCN_V6_FLAG_PROTOCOL_CONTEXT;
        hello.traffic_class = UCN_V6_TRAFFIC_Q1;
        hello.delivery_guarantee = UCN_V6_DELIVERY_LATEST;
        hello.hop_limit = 1U;
        hello.header_contract = UCN_V6_HEADER_CONTRACT_1;
        hello.realm_id = 1U;
        hello.source_address = 7U;
        hello.destination_address = 8U;
        hello.source_binding_generation = 3U;
        hello.destination_binding_generation = 4U;
        hello.session_generation = 1U;
        hello.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_PEER_HELLO;
        hello.payload = hello_payload;
        hello.payload_length = sizeof(hello_payload);
        before = hello;
        memset(encoded, 0x93, sizeof(encoded));
        memcpy(output_before, encoded, sizeof(encoded));
        CHECK(ucn_v6_security_protect_peer_discovery(
                  device_manager, 260U, &authority, &hello, frame_work,
                  sizeof(frame_work), encoded, 1U, &length_before) ==
              UCN_V6_ERR_NO_SPACE);
        CHECK(memcmp(&hello, &before, sizeof(hello)) == 0);
        CHECK(memcmp(encoded, output_before, sizeof(encoded)) == 0);
        CHECK(length_before == 91U);
        {
            typedef union frame_length_alias {
                ucn_v6_frame_t frame;
                size_t length;
            } frame_length_alias_t;
            frame_length_alias_t alias;
            frame_length_alias_t alias_before;
            uint8_t local_work[256U];
            uint8_t local_output[256U];
            uint8_t work_before[sizeof(local_work)];
            uint8_t output_before_alias[sizeof(local_output)];
            unsigned submits_before = device_store.submits;

            memset(&alias, 0, sizeof(alias));
            alias.frame = hello;
            memcpy(&alias_before, &alias, sizeof(alias_before));
            memset(local_work, 0x64, sizeof(local_work));
            memset(local_output, 0x75, sizeof(local_output));
            memcpy(work_before, local_work, sizeof(work_before));
            memcpy(output_before_alias, local_output,
                   sizeof(output_before_alias));
            CHECK(ucn_v6_security_protect_peer_discovery(
                      device_manager, 260U, &authority, &alias.frame,
                      local_work, sizeof(local_work), local_output,
                      sizeof(local_output), &alias.length) ==
                  UCN_V6_ERR_ARGUMENT);
            CHECK(memcmp(&alias, &alias_before, sizeof(alias)) == 0);
            CHECK(memcmp(local_work, work_before, sizeof(work_before)) == 0);
            CHECK(memcmp(local_output, output_before_alias,
                         sizeof(output_before_alias)) == 0);
            CHECK(device_store.submits == submits_before);
        }
        CHECK(ucn_v6_security_protect_peer_discovery(
                  device_manager, 260U, &authority, &hello, frame_work,
                  sizeof(frame_work), encoded, sizeof(encoded),
                  &encoded_length) == UCN_V6_OK);
        CHECK(hello.origin_sequence == 0U && hello.hop_sequence == 4U);
        CHECK(ucn_v6_security_open_frame(
                  authority_manager, 261U, 1U, 6U, &device, encoded,
                  encoded_length, NULL, 0U, &opened) == UCN_V6_OK);
        CHECK(opened.hop_authenticated && !opened.endpoint_authorized &&
              !opened.group_discovery_only);
        CHECK(memcmp(opened.authenticated_principal.bytes, device.bytes,
                     sizeof(device.bytes)) == 0);
        CHECK(opened.frame.payload_length == sizeof(hello_payload));
        CHECK(memcmp(opened.frame.payload, hello_payload,
                     sizeof(hello_payload)) == 0);
    }

    {
        ucn_v6_binding_key_t authority_binding = { 1U, 8U, 4U };
        memset(&invalidation, 0xA5, sizeof(invalidation));
        {
            ucn_v6_stack_invalidation_t before = invalidation;
            CHECK(ucn_v6_security_invalidation_peek(
                      device_manager, &invalidation) ==
                  UCN_V6_ERR_NOT_FOUND);
            CHECK(memcmp(&invalidation, &before, sizeof(before)) == 0);
        }
        CHECK(ucn_v6_security_require_reauth(
                  device_manager, &authority) == UCN_V6_OK);
        CHECK(ucn_v6_security_require_reauth(
                  device_manager, &authority) == UCN_V6_OK);
        memset(durable_receipt_again, 0x6CU,
               sizeof(durable_receipt_again));
        durable_receipt_again_length = 77U;
        durable_receipt_again_generation = 88U;
        {
            uint8_t before[sizeof(durable_receipt_again)];
            memcpy(before, durable_receipt_again, sizeof(before));
            CHECK(ucn_v6_security_export_join_durable_receipt(
                      device_manager, &authority, durable_receipt_again,
                      sizeof(durable_receipt_again),
                      &durable_receipt_again_length,
                      &durable_receipt_again_generation) == UCN_V6_ERR_STATE);
            CHECK(memcmp(before, durable_receipt_again, sizeof(before)) == 0 &&
                  durable_receipt_again_length == 77U &&
                  durable_receipt_again_generation == 88U);
        }
        CHECK(ucn_v6_security_invalidation_peek(
                  device_manager, &invalidation) == UCN_V6_OK);
        CHECK(security_session_event_matches(
                  &invalidation, 1U, 6U, &authority,
                  &authority_binding, 1U));
        {
            ucn_v6_stack_invalidation_t wrong = invalidation;
            ++wrong.session.session_generation;
            CHECK(ucn_v6_security_invalidation_ack(
                      device_manager, &wrong) == UCN_V6_ERR_STATE);
            CHECK(ucn_v6_security_invalidation_peek(
                      device_manager, &wrong) == UCN_V6_OK);
            CHECK(wrong.session.session_generation == 1U);
        }
        CHECK(ucn_v6_security_invalidation_ack(
                  device_manager, &invalidation) == UCN_V6_OK);
    }

    /* A reboot never treats a persisted local monotonic deadline as live. */
    device_manager = NULL;
    memset(&device_storage, 0, sizeof(device_storage));
    CHECK(ucn_v6_security_init_in_place(
              device_storage.bytes, sizeof(device_storage),
              ucn_v6_compiled_manifest(), 1U, &device, &device_store_api,
              &device_crypto_api, &device_gate, &device_manager) == UCN_V6_OK);
    CHECK(ucn_v6_security_copy_view(device_manager, &view) == UCN_V6_OK);
    CHECK(view.admitted_sessions == 0U);
    transcript.flow = UCN_V6_BOOTSTRAP_FLOW_REAUTH;
    transcript.device_nonce = 101U;
    transcript.authority_nonce = 102U;
    transcript.transaction_id = 103U;
    transcript.lease_freshness_challenge_nonce = 104U;
    transcript.selected_session_generation = 2U;
    transcript.selected_link_instance_generation = 7U;
    transcript.selected_hop_key_generation = 4U;
    fill_bytes(transcript.prior_messages_hash,
               sizeof(transcript.prior_messages_hash), 0xB0U);
    device_join = make_join(&transcript, true);
    device_join.session_generation = 2U;
    device_join.link_instance_generation = 7U;
    bootstrap_key = make_bootstrap_key(&transcript, 2U);
    device_bootstrap = NULL;
    memset(&device_bootstrap_storage, 0, sizeof(device_bootstrap_storage));
    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              device_bootstrap_storage.bytes, sizeof(device_bootstrap_storage),
              ucn_v6_compiled_manifest(), &bootstrap_config,
              &device_bootstrap) == UCN_V6_OK);
    CHECK(complete_bootstrap(device_bootstrap, &transcript, &bootstrap_key,
                             280U) == 0);
    {
        ucn_v6_result_t reauth_result = ucn_v6_security_commit_join(
            device_manager, device_bootstrap, &bootstrap_key, 300U,
            &device_join);
        if (reauth_result != UCN_V6_OK) {
            fprintf(stderr, "reauth result=%d\n", reauth_result);
        }
        CHECK(reauth_result == UCN_V6_OK);
    }
    CHECK(ucn_v6_security_copy_view(device_manager, &view) == UCN_V6_OK);
    CHECK(view.admitted_sessions == 1U && view.pending_invalidations == 1U);
    {
        ucn_v6_binding_key_t authority_binding = { 1U, 8U, 4U };
        CHECK(ucn_v6_security_invalidation_peek(
                  device_manager, &invalidation) == UCN_V6_OK);
        CHECK(security_session_event_matches(
                  &invalidation, 1U, 6U, &authority,
                  &authority_binding, 1U));
        CHECK(ucn_v6_security_invalidation_ack(
                  device_manager, &invalidation) == UCN_V6_OK);
    }
    {
        ucn_v6_group_policy_slot_t group;
        ucn_v6_group_key_slot_t key;
        ucn_v6_frame_t hello;
        ucn_v6_frame_t decoded;
        uint8_t hello_work[160U];
        uint8_t hello_encoded[160U];
        size_t hello_length = 0U;
        memset(&group, 0, sizeof(group));
        group.state = UCN_V6_GROUP_SLOT_ACTIVE;
        group.group_id = 41U;
        group.group_generation = 1U;
        group.owner_principal = admin;
        CHECK(ucn_v6_security_set_group_policy(
                  device_manager, 0U, &group, &admin, admin_proof,
                  sizeof(admin_proof)) == UCN_V6_OK);
        memset(&key, 0, sizeof(key));
        key.state = UCN_V6_GROUP_KEY_ACTIVE;
        key.group_id = 41U;
        key.group_generation = 1U;
        key.key_id = 11U;
        key.suite_id = UCN_V6_SUITE_HMAC_SHA256_128;
        key.current_generation = 1U;
        CHECK(ucn_v6_security_set_group_key(
                  device_manager, 350U, 0U, 0U, &key, &admin, admin_proof,
                  sizeof(admin_proof)) == UCN_V6_OK);
        memset(&hello, 0, sizeof(hello));
        hello.address_class = UCN_V6_ADDRESS_CLASS_A0;
        hello.frame_type = UCN_V6_FRAME_CONTROL;
        hello.flags = UCN_V6_FLAG_GROUP_CONTEXT |
                      UCN_V6_FLAG_PROTOCOL_CONTEXT;
        hello.traffic_class = UCN_V6_TRAFFIC_Q1;
        hello.delivery_guarantee = UCN_V6_DELIVERY_LATEST;
        hello.hop_limit = 1U;
        hello.header_contract = UCN_V6_HEADER_CONTRACT_1;
        hello.realm_id = 1U;
        hello.source_address = 7U;
        hello.destination_address = UINT8_MAX;
        hello.source_binding_generation = 3U;
        hello.session_generation = 2U;
        hello.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO;
        {
            ucn_v6_frame_t hello_before = hello;
            uint8_t output_before[sizeof(hello_encoded)];
            size_t length_before = 88U;
            memset(hello_encoded, 0x6A, sizeof(hello_encoded));
            memcpy(output_before, hello_encoded, sizeof(hello_encoded));
            CHECK(ucn_v6_security_protect_group_hello(
                      device_manager, 0U, 0U, &hello, hello_work,
                      sizeof(hello_work), hello_encoded, 1U,
                      &length_before) == UCN_V6_ERR_NO_SPACE);
            CHECK(memcmp(&hello, &hello_before, sizeof(hello)) == 0);
            CHECK(memcmp(hello_encoded, output_before,
                         sizeof(hello_encoded)) == 0);
            CHECK(length_before == 88U);
        }
        {
            typedef union frame_length_alias {
                ucn_v6_frame_t frame;
                size_t length;
            } frame_length_alias_t;
            frame_length_alias_t alias;
            frame_length_alias_t alias_before;
            uint8_t local_work[160U];
            uint8_t local_output[160U];
            uint8_t work_before[sizeof(local_work)];
            uint8_t output_before[sizeof(local_output)];
            unsigned submits_before = device_store.submits;

            memset(&alias, 0, sizeof(alias));
            alias.frame = hello;
            memcpy(&alias_before, &alias, sizeof(alias_before));
            memset(local_work, 0x86, sizeof(local_work));
            memset(local_output, 0x97, sizeof(local_output));
            memcpy(work_before, local_work, sizeof(work_before));
            memcpy(output_before, local_output, sizeof(output_before));
            CHECK(ucn_v6_security_protect_group_hello(
                      device_manager, 0U, 0U, &alias.frame, local_work,
                      sizeof(local_work), local_output, sizeof(local_output),
                      &alias.length) == UCN_V6_ERR_ARGUMENT);
            CHECK(memcmp(&alias, &alias_before, sizeof(alias)) == 0);
            CHECK(memcmp(local_work, work_before, sizeof(work_before)) == 0);
            CHECK(memcmp(local_output, output_before,
                         sizeof(output_before)) == 0);
            CHECK(device_store.submits == submits_before);
        }
        CHECK(ucn_v6_security_protect_group_hello(
                  device_manager, 0U, 0U, &hello, hello_work,
                  sizeof(hello_work), hello_encoded, sizeof(hello_encoded),
                  &hello_length) == UCN_V6_OK);
        CHECK(ucn_v6_wire_decode(hello_encoded, hello_length, &decoded) ==
              UCN_V6_OK);
        CHECK(decoded.origin_sequence == 1U && decoded.hop_sequence == 0U &&
              decoded.group.group_id == 41U);
    }
    {
        ucn_v6_key_selector_t next_hop = { 1U, 2U, 5U };
        ucn_v6_key_selector_t next_e2e = { 2U, 4U, 6U };
        CHECK(ucn_v6_security_rotate_session_keys(
                  device_manager, 400U, &authority, &next_hop, &next_e2e,
                  1000U, &admin, admin_proof, sizeof(admin_proof)) ==
              UCN_V6_OK);
        CHECK(ucn_v6_security_revoke_session(
                  device_manager, &authority, &admin, admin_proof,
                  sizeof(admin_proof)) == UCN_V6_OK);
        {
            ucn_v6_binding_key_t authority_binding = { 1U, 8U, 4U };
            CHECK(ucn_v6_security_invalidation_peek(
                      device_manager, &invalidation) == UCN_V6_OK);
            CHECK(security_session_event_matches(
                      &invalidation, 1U, 7U, &authority,
                      &authority_binding, 2U));
            CHECK(ucn_v6_security_invalidation_ack(
                      device_manager, &invalidation) == UCN_V6_OK);
        }
        CHECK(ucn_v6_security_copy_view(device_manager, &view) == UCN_V6_OK);
        CHECK(view.admitted_sessions == 0U);
        CHECK(ucn_v6_security_commit_join(
                  device_manager, device_bootstrap, &bootstrap_key, 500U,
                  &device_join) == UCN_V6_ERR_ACCESS);
    }
    return 0;
}

static int test_independent_sequence_domains_and_verified_relay(void)
{
    static const uint8_t admin_proof[] = { UCN_V6_PROOF_REALM_ADMIN };
    const uint8_t payload_c[] = { 0x31U, 0x32U, 0x33U };
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    const uint8_t payload_d[] = { 0x41U, 0x42U, 0x43U };
#endif
    ucn_v6_principal_t a = make_principal(0x10U);
    ucn_v6_principal_t b = make_principal(0x30U);
    ucn_v6_principal_t c = make_principal(0x50U);
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    ucn_v6_principal_t d = make_principal(0x70U);
#endif
    ucn_v6_principal_t admin = make_principal(0xA0U);
    fake_store_t stores[3];
    fake_crypto_t cryptos[3];
    ucn_v6_security_store_ops_t store_ops_array[3];
    ucn_v6_security_crypto_ops_t crypto_ops_array[3];
    ucn_v6_callback_gate_t gates[3];
    ucn_v6_security_manager_storage_t storage[3];
    ucn_v6_security_manager_t *managers[3] = { NULL, NULL, NULL };
    ucn_v6_acl_entry_t acl;
    ucn_v6_transfer_fragment_t fragment;
    ucn_v6_frame_t frame_c;
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    ucn_v6_frame_t frame_d;
#endif
    ucn_v6_frame_t relayed;
    ucn_v6_security_open_result_t relay_ingress;
    ucn_v6_security_open_result_t opened;
    ucn_v6_qos_owner_t *qos = NULL;
    ucn_v6_qos_policy_t qos_policy;
    ucn_v6_qos_enqueue_result_t enqueue_result;
    ucn_v6_qos_selection_t qos_selection;
    ucn_v6_hop_budget_context_t next_budget;
#if UCN_V6_FEATURE_ADAPTER_ENABLED
    ucn_v6_adapter_owner_t *adapter = NULL;
    pipeline_runtime_t runtime;
    pipeline_driver_t driver;
    ucn_v6_driver_runtime_ops_t runtime_ops;
    ucn_v6_driver_link_config_t link_config;
    ucn_v6_driver_event_key_t adapter_key;
    ucn_v6_driver_tx_completion_t completion;
    ucn_v6_driver_timestamp_t timestamp;
    uint64_t retired_token = 0U;
    bool submitted = false;
#endif
    ucn_v6_transfer_owner_t *transfer_rx = NULL;
    ucn_v6_transfer_rx_result_t transfer_result;
    ucn_v6_transfer_completed_t completed;
    ucn_v6_session_key_t origin;
    ucn_v6_capability_owner_t *capability_owner = NULL;
    ucn_v6_capability_record_t local_capability;
    ucn_v6_capability_record_t destination_capability;
    ucn_v6_security_open_result_t capability_opened;
    ucn_v6_frame_t capability_frame;
    ucn_v6_frame_t budget_contract;
    ucn_v6_path_budget_request_t budget_request;
    ucn_v6_path_capability_t derived_path;
    ucn_v6_capability_peer_ref_t destination_ref;
    ucn_v6_route_owner_t *route_owner = NULL;
    ucn_v6_route_domain_t route_domain;
    ucn_v6_route_path_t route_path;
    ucn_v6_route_activation_t activation;
    ucn_v6_route_select_request_t select_request;
    ucn_v6_route_selection_t route_selection;
    uint8_t capability_payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint8_t capability_work[256U];
    uint8_t capability_encoded[256U];
    size_t capability_encoded_length = 0U;
    uint8_t capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint8_t transfer_payload[64U];
    size_t transfer_payload_length = 0U;
    uint8_t cipher_c[sizeof(transfer_payload)];
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    uint8_t cipher_d[sizeof(payload_d)];
#endif
    uint8_t work_a[256U];
    uint8_t work_b[256U];
    uint8_t encoded_c[256U] = {0};
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    uint8_t encoded_d[256U] = {0};
#endif
    uint8_t relayed_encoded[256U];
    uint8_t plaintext[sizeof(transfer_payload)];
    uint8_t completed_payload[sizeof(payload_c)];
    size_t encoded_c_length = 0U;
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    size_t encoded_d_length = 0U;
#endif
    size_t relayed_length = 0U;
    const uint8_t *delivered_frame = NULL;
    size_t delivered_length = 0U;
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    unsigned b_submits_before;
#endif
    unsigned c_submits_before;
    size_t index;

    memset(stores, 0, sizeof(stores));
    memset(cryptos, 0, sizeof(cryptos));
    memset(gates, 0, sizeof(gates));
    for (index = 0U; index < 3U; ++index) {
        store_ops_array[index] = store_ops(&stores[index]);
        crypto_ops_array[index] = crypto_ops(&cryptos[index]);
        CHECK(ucn_v6_callback_gate_init(
                  &gates[index], NULL, no_lock, no_lock) == UCN_V6_OK);
    }
    CHECK(ucn_v6_security_init_in_place(
              storage[0].bytes, sizeof(storage[0]),
              ucn_v6_compiled_manifest(), 1U, &a, &store_ops_array[0],
              &crypto_ops_array[0], &gates[0], &managers[0]) == UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage[1].bytes, sizeof(storage[1]),
              ucn_v6_compiled_manifest(), 1U, &b, &store_ops_array[1],
              &crypto_ops_array[1], &gates[1], &managers[1]) == UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage[2].bytes, sizeof(storage[2]),
              ucn_v6_compiled_manifest(), 1U, &c, &store_ops_array[2],
              &crypto_ops_array[2], &gates[2], &managers[2]) == UCN_V6_OK);

    CHECK(install_pair_session(managers[0], &a, 10U, 1U, &b, 20U, 2U,
                               true, 101U, 1U) == 0);
    CHECK(install_pair_session(managers[1], &a, 10U, 1U, &b, 20U, 2U,
                               false, 101U, 1U) == 0);
    CHECK(install_pair_session(managers[0], &a, 10U, 1U, &c, 30U, 3U,
                               true, 102U, 2U) == 0);
    CHECK(install_pair_session(managers[2], &a, 10U, 1U, &c, 30U, 3U,
                               false, 102U, 2U) == 0);
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    CHECK(install_pair_session(managers[0], &a, 10U, 1U, &d, 40U, 4U,
                               true, 103U, 3U) == 0);
#endif
    CHECK(install_pair_session(managers[1], &b, 20U, 2U, &c, 30U, 3U,
                               true, 104U, 4U) == 0);
    CHECK(install_pair_session(managers[2], &b, 20U, 2U, &c, 30U, 3U,
                               false, 104U, 4U) == 0);

    acl = make_acl(&a, 10U, 1U, 30U, 3U, UCN_V6_SECURITY_OUTBOUND);
    acl.key.frame_type = UCN_V6_FRAME_TRANSFER;
    acl.key.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TRANSFER_FRAGMENT;
    CHECK(ucn_v6_security_set_acl(
              managers[0], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    acl = make_acl(&a, 10U, 1U, 40U, 4U, UCN_V6_SECURITY_OUTBOUND);
    CHECK(ucn_v6_security_set_acl(
              managers[0], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
#endif
    acl = make_acl(&a, 10U, 1U, 30U, 3U, UCN_V6_SECURITY_INBOUND);
    acl.key.frame_type = UCN_V6_FRAME_TRANSFER;
    acl.key.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TRANSFER_FRAGMENT;
    CHECK(ucn_v6_security_set_acl(
              managers[2], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);

    memset(&fragment, 0, sizeof(fragment));
    fragment.message_class = UCN_V6_MESSAGE_T32;
    fragment.message_id = 201U;
    fragment.total_length = sizeof(payload_c);
    fragment.fragment_count = 1U;
    fragment.fragment_data_budget = sizeof(payload_c);
    fragment.data_length = sizeof(payload_c);
    fragment.message_crc32c = ucn_v6_crc32c(payload_c, sizeof(payload_c));
    fragment.data = payload_c;
    CHECK(ucn_v6_transfer_fragment_encode(
              &fragment, transfer_payload, sizeof(transfer_payload),
              &transfer_payload_length) == UCN_V6_OK);
    frame_c = make_data_frame(transfer_payload, transfer_payload_length);
    frame_c.frame_type = UCN_V6_FRAME_TRANSFER;
    frame_c.flags |= UCN_V6_FLAG_PROTOCOL_CONTEXT |
                     UCN_V6_FLAG_ROUTE_CONTEXT |
                     UCN_V6_FLAG_PATH_CONTEXT |
                     UCN_V6_FLAG_HOP_BUDGET_CONTEXT;
    frame_c.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TRANSFER_FRAGMENT;
    frame_c.route_generation = 1U;
    frame_c.path.path_id = 1U;
    frame_c.path.path_generation = 1U;
    frame_c.hop_budget.initial_budget_us = 1000U;
    frame_c.hop_budget.remaining_budget_us = 900U;
    frame_c.source_address = 10U;
    frame_c.source_binding_generation = 1U;
    frame_c.destination_address = 30U;
    frame_c.destination_binding_generation = 3U;
    frame_c.message.operation_id = 201U;

    /* Build the selected path from a real authenticated Capability frame,
     * then freeze and activate that exact path before sending business data. */
    local_capability = pipeline_capability_record(1U, 6U);
    destination_capability = pipeline_capability_record(1U, 6U);
    CHECK(ucn_v6_capability_record_encode(
              &destination_capability, capability_payload) == UCN_V6_OK);
    memset(&capability_frame, 0, sizeof(capability_frame));
    capability_frame.address_class = UCN_V6_ADDRESS_CLASS_A0;
    capability_frame.frame_type = UCN_V6_FRAME_CONTROL;
    capability_frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                             UCN_V6_FLAG_PROTOCOL_CONTEXT;
    capability_frame.traffic_class = UCN_V6_TRAFFIC_Q2;
    capability_frame.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    capability_frame.hop_limit = 1U;
    capability_frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    capability_frame.realm_id = 1U;
    capability_frame.source_address = 30U;
    capability_frame.destination_address = 10U;
    capability_frame.source_binding_generation = 3U;
    capability_frame.destination_binding_generation = 1U;
    capability_frame.session_generation = 1U;
    capability_frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE;
    capability_frame.payload = capability_payload;
    capability_frame.payload_length = sizeof(capability_payload);
    CHECK(ucn_v6_security_protect_peer_discovery(
              managers[2], 90U, &a, &capability_frame, capability_work,
              sizeof(capability_work), capability_encoded,
              sizeof(capability_encoded), &capability_encoded_length) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_open_frame(
              managers[0], 91U, 1U, 6U, &c, capability_encoded,
              capability_encoded_length, NULL, 0U, &capability_opened) ==
          UCN_V6_OK);
    memset(&pipeline_capability_storage, 0,
           sizeof(pipeline_capability_storage));
    CHECK(ucn_v6_capability_owner_init_in_place(
              pipeline_capability_storage.bytes,
              sizeof(pipeline_capability_storage),
              ucn_v6_compiled_manifest(), &local_capability,
              10000U, 10000U, &capability_owner) == UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_advertise(
              capability_owner, 91U, &capability_opened,
              &destination_capability) == UCN_V6_OK);
    /* The Route next hop has its own live Capability parent. */
    capability_frame.source_address = 20U;
    capability_frame.source_binding_generation = 2U;
    CHECK(ucn_v6_security_protect_peer_discovery(
              managers[1], 91U, &a, &capability_frame, capability_work,
              sizeof(capability_work), capability_encoded,
              sizeof(capability_encoded), &capability_encoded_length) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_open_frame(
              managers[0], 92U, 1U, 6U, &b, capability_encoded,
              capability_encoded_length, NULL, 0U, &capability_opened) ==
          UCN_V6_OK);
    CHECK(ucn_v6_capability_ingest_advertise(
              capability_owner, 92U, &capability_opened,
              &destination_capability) == UCN_V6_OK);
    CHECK(ucn_v6_capability_digest(
              &destination_capability, capability_digest) == UCN_V6_OK);
    budget_contract = frame_c;
    budget_contract.origin_sequence = 1U;
    budget_contract.hop_sequence = 1U;
    budget_contract.peer_hop.suite_id = UCN_V6_SUITE_HMAC_SHA256_128;
    budget_contract.peer_hop.key_id = 2U;
    budget_contract.peer_hop.key_generation = 3U;
    budget_contract.e2e.mode = UCN_V6_E2E_AEAD;
    budget_contract.e2e.suite_id = UCN_V6_SUITE_AES_GCM_128;
    budget_contract.e2e.key_id = 4U;
    budget_contract.e2e.key_generation = 5U;
    budget_contract.payload = NULL;
    budget_contract.payload_length = 0U;
    memset(&budget_request, 0, sizeof(budget_request));
    budget_request.destination_principal = c;
    budget_request.destination_binding.realm_id = 1U;
    budget_request.destination_binding.node_address = 30U;
    budget_request.destination_binding.binding_generation = 3U;
    budget_request.destination_session_generation = 1U;
    budget_request.destination_capability_generation = 1U;
    memcpy(budget_request.destination_capability_digest, capability_digest,
           sizeof(capability_digest));
    budget_request.destination_realtime_mode_bits =
        destination_capability.peer.realtime_mode_bits;
    budget_request.destination_clock_domain_id =
        destination_capability.peer.clock_domain_id;
    budget_request.destination_clock_domain_generation =
        destination_capability.peer.clock_domain_generation;
    budget_request.route_generation = 1U;
    budget_request.path_id = 1U;
    budget_request.path_generation = 1U;
    budget_request.deadline_us = 10000U;
    budget_request.fixed_path = true;
    budget_request.path_policy_frame_mtu = 220U;
    budget_request.required_feature_bits = UCN_V6_FEATURE_TRANSFER;
    budget_request.required_hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    budget_request.required_e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    budget_request.policy_max_message_class = UCN_V6_MESSAGE_T8K;
    budget_request.policy_max_window = UCN_V6_CONFIG_TRANSFER_WINDOW;
    budget_request.policy_max_concurrency = UCN_V6_CONFIG_TRANSFER_RX_SLOTS;
    budget_request.frame_contract = &budget_contract;
    budget_request.fragment_header_bytes =
        UCN_V6_TRANSFER_FRAGMENT_HEADER_BYTES;
    {
        ucn_v6_path_budget_accumulator_t accumulator;
        ucn_v6_path_budget_accumulator_t downstream_accumulator;
        ucn_v6_path_budget_request_t downstream_request = budget_request;
        ucn_v6_path_capability_t downstream_path;
        ucn_v6_capability_peer_ref_t next_hop_ref;
        memset(&destination_ref, 0, sizeof(destination_ref));
        destination_ref.principal = c;
        destination_ref.binding = budget_request.destination_binding;
        destination_ref.session_generation = 1U;
        destination_ref.ingress_link_id = 1U;
        destination_ref.ingress_link_generation = 6U;
        downstream_request.route_generation = 2U;
        downstream_request.path_id = 2U;
        CHECK(ucn_v6_capability_path_reduce_begin(
                  &downstream_request, &downstream_accumulator) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_hop(
                  capability_owner, 92U, &downstream_accumulator,
                  &destination_ref) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_finalize(
                  &downstream_accumulator, &downstream_path) == UCN_V6_OK);
        memset(&next_hop_ref, 0, sizeof(next_hop_ref));
        next_hop_ref.principal = b;
        next_hop_ref.binding.realm_id = 1U;
        next_hop_ref.binding.node_address = 20U;
        next_hop_ref.binding.binding_generation = 2U;
        next_hop_ref.session_generation = 1U;
        next_hop_ref.ingress_link_id = 1U;
        next_hop_ref.ingress_link_generation = 6U;
        CHECK(ucn_v6_capability_path_reduce_begin(
                  &budget_request, &accumulator) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_downstream(
                  &accumulator, &downstream_path, 10000U) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_hop(
                  capability_owner, 92U, &accumulator,
                  &next_hop_ref) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_finalize(
                  &accumulator, &derived_path) == UCN_V6_OK);
    }
    CHECK(ucn_v6_capability_install_path(
              capability_owner, 92U, &derived_path) == UCN_V6_OK);
    memset(&pipeline_route_storage, 0, sizeof(pipeline_route_storage));
    CHECK(ucn_v6_route_owner_init_in_place(
              pipeline_route_storage.bytes, sizeof(pipeline_route_storage),
              ucn_v6_compiled_manifest(), capability_owner,
              1000U, 100U, 3U, 200U, 500U, &route_owner) == UCN_V6_OK);
    memset(&route_domain, 0, sizeof(route_domain));
    route_domain.origin_principal = a;
    route_domain.origin_binding.realm_id = 1U;
    route_domain.origin_binding.node_address = 10U;
    route_domain.origin_binding.binding_generation = 1U;
    route_domain.origin_session_generation = 1U;
    route_domain.destination_principal = c;
    route_domain.destination_binding = budget_request.destination_binding;
    route_domain.destination_session_generation =
        budget_request.destination_session_generation;
    CHECK(ucn_v6_route_candidate_begin(
              route_owner, 92U, 301U, &route_domain, 1U) == UCN_V6_OK);
    memset(&route_path, 0, sizeof(route_path));
    route_path.path_id = 1U;
    route_path.path_generation = 1U;
    route_path.next_hop.principal = b;
    route_path.next_hop.binding.realm_id = 1U;
    route_path.next_hop.binding.node_address = 20U;
    route_path.next_hop.binding.binding_generation = 2U;
    route_path.next_hop.session_generation = 1U;
    route_path.next_hop_capability_generation = 1U;
    route_path.egress_link_id = 1U;
    route_path.egress_link_generation = 6U;
    route_path.hop_count = 2U;
    route_path.priority = 1U;
    route_path.weight = 1U;
    route_path.available = true;
    route_path.capability = derived_path;
    CHECK(ucn_v6_route_candidate_add_path(
              route_owner, 93U, 301U, &route_domain, &route_path) ==
          UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_probe(
              route_owner, 94U, 301U, &route_domain, 1U, 1U) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              route_owner, 95U, 301U, &route_domain,
              &activation) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_activation_send(
              route_owner, 95U, 301U, &route_domain, true) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_commit_ack(
              route_owner, 96U, &activation) == UCN_V6_OK);
    memset(&select_request, 0, sizeof(select_request));
    select_request.domain = route_domain;
    select_request.flow_id = 201U;
    select_request.packet_sequence = 1U;
    select_request.policy = UCN_V6_ROUTE_POLICY_ACTIVE_STANDBY;
    CHECK(ucn_v6_route_select(
              route_owner, 97U, &select_request, &route_selection) ==
          UCN_V6_OK);
    CHECK(route_selection.path.egress_link_id == 1U &&
          memcmp(route_selection.path.next_hop.principal.bytes, b.bytes,
                 sizeof(b.bytes)) == 0);
    CHECK(ucn_v6_security_protect_frame(
              managers[0], 100U, &b, &c, &frame_c, cipher_c,
              sizeof(cipher_c), work_a, sizeof(work_a), encoded_c,
              sizeof(encoded_c), &encoded_c_length) == UCN_V6_OK);
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    frame_d = make_data_frame(payload_d, sizeof(payload_d));
    frame_d.source_address = 10U;
    frame_d.source_binding_generation = 1U;
    frame_d.destination_address = 40U;
    frame_d.destination_binding_generation = 4U;
    frame_d.message.operation_id = 202U;
    CHECK(ucn_v6_security_protect_frame(
              managers[0], 101U, &b, &d, &frame_d, cipher_d,
              sizeof(cipher_d), work_a, sizeof(work_a), encoded_d,
              sizeof(encoded_d), &encoded_d_length) == UCN_V6_OK);
    CHECK(frame_c.origin_sequence == 1U && frame_d.origin_sequence == 1U);
    CHECK(frame_c.hop_sequence == 1U && frame_d.hop_sequence == 2U);
#else
    CHECK(frame_c.origin_sequence == 1U && frame_c.hop_sequence == 1U);
#endif

    {
        uint8_t encoded_before[sizeof(encoded_c)];
        uint8_t output_before[sizeof(relayed_encoded)];
        ucn_v6_security_open_result_t ingress_before;
        ucn_v6_frame_t frame_before;
        size_t length_before = 73U;

        memcpy(encoded_before, encoded_c, sizeof(encoded_before));
        memset(relayed_encoded, 0x96, sizeof(relayed_encoded));
        memcpy(output_before, relayed_encoded, sizeof(output_before));
        memset(&relay_ingress, 0x5B, sizeof(relay_ingress));
        ingress_before = relay_ingress;
        memset(&relayed, 0xC4, sizeof(relayed));
        frame_before = relayed;
        CHECK(ucn_v6_security_relay_frame(
                  managers[1], 108U, 1U, 6U, &a, &c, 5U,
                  encoded_c, encoded_c_length, encoded_c, sizeof(encoded_c),
                  relayed_encoded, sizeof(relayed_encoded), &length_before,
                  &relay_ingress, &relayed) == UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(encoded_c, encoded_before, sizeof(encoded_before)) == 0);
        CHECK(memcmp(relayed_encoded, output_before, sizeof(output_before)) ==
              0);
        CHECK(memcmp(&relay_ingress, &ingress_before,
                     sizeof(ingress_before)) == 0);
        CHECK(memcmp(&relayed, &frame_before, sizeof(frame_before)) == 0);
        CHECK(length_before == 73U);
    }

    {
        typedef union ingress_length_alias {
            ucn_v6_security_open_result_t ingress;
            size_t length;
        } ingress_length_alias_t;
        ingress_length_alias_t alias;
        ingress_length_alias_t alias_before;
        ucn_v6_frame_t frame_before;
        uint8_t output_before[sizeof(relayed_encoded)];

        memset(&alias, 0x4DU, sizeof(alias));
        memcpy(&alias_before, &alias, sizeof(alias_before));
        memset(&relayed, 0xC8, sizeof(relayed));
        frame_before = relayed;
        memset(relayed_encoded, 0x97, sizeof(relayed_encoded));
        memcpy(output_before, relayed_encoded, sizeof(output_before));
        CHECK(ucn_v6_security_relay_frame(
                  managers[1], 108U, 1U, 6U, &a, &c, 5U,
                  encoded_c, encoded_c_length, work_b, sizeof(work_b),
                  relayed_encoded, sizeof(relayed_encoded), &alias.length,
                  &alias.ingress, &relayed) == UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(&alias, &alias_before, sizeof(alias)) == 0);
        CHECK(memcmp(&relayed, &frame_before, sizeof(frame_before)) == 0);
        CHECK(memcmp(relayed_encoded, output_before, sizeof(output_before)) ==
              0);
    }

    {
        uint8_t tampered[sizeof(encoded_c)];
        uint8_t rejected_output[sizeof(relayed_encoded)];
        uint8_t output_before[sizeof(rejected_output)];
        ucn_v6_security_open_result_t rejected_ingress;
        ucn_v6_security_open_result_t ingress_before;
        ucn_v6_frame_t rejected_frame;
        ucn_v6_frame_t frame_before;
        size_t rejected_length = 77U;
        size_t tag_offset;
        uint32_t crc;

        memcpy(tampered, encoded_c, encoded_c_length);
        tag_offset = encoded_c_length - 4U -
                     (2U * UCN_V6_SECURITY_TAG_BYTES);
        tampered[tag_offset] ^= 0x01U;
        crc = ucn_v6_crc32c(tampered, encoded_c_length - 4U);
        tampered[encoded_c_length - 4U] = (uint8_t)(crc >> 24U);
        tampered[encoded_c_length - 3U] = (uint8_t)(crc >> 16U);
        tampered[encoded_c_length - 2U] = (uint8_t)(crc >> 8U);
        tampered[encoded_c_length - 1U] = (uint8_t)crc;
        memset(rejected_output, 0xA5, sizeof(rejected_output));
        memcpy(output_before, rejected_output, sizeof(output_before));
        memset(&rejected_ingress, 0x5A, sizeof(rejected_ingress));
        ingress_before = rejected_ingress;
        memset(&rejected_frame, 0xC3, sizeof(rejected_frame));
        frame_before = rejected_frame;
        CHECK(ucn_v6_security_relay_frame(
                  managers[1], 109U, 1U, 6U, &a, &c, 5U,
                  tampered, encoded_c_length, work_b, sizeof(work_b),
                  rejected_output, sizeof(rejected_output), &rejected_length,
                  &rejected_ingress, &rejected_frame) == UCN_V6_ERR_SECURITY);
        CHECK(memcmp(rejected_output, output_before, sizeof(output_before)) ==
              0);
        CHECK(memcmp(&rejected_ingress, &ingress_before,
                     sizeof(ingress_before)) == 0);
        CHECK(memcmp(&rejected_frame, &frame_before, sizeof(frame_before)) ==
              0);
        CHECK(rejected_length == 77U);
    }

    CHECK(ucn_v6_security_relay_frame(
              managers[1], 110U, 1U, 6U, &a, &c, 5U,
              encoded_c, encoded_c_length, work_b, sizeof(work_b),
              relayed_encoded, sizeof(relayed_encoded), &relayed_length,
              &relay_ingress, &relayed) == UCN_V6_OK);
    CHECK(relay_ingress.hop_authenticated &&
          !relay_ingress.endpoint_authorized &&
          relay_ingress.frame.origin_sequence == frame_c.origin_sequence);
    CHECK(ucn_v6_qos_forward_budget(
              &relay_ingress, 1000U, 2U, 3U, &next_budget) == UCN_V6_OK);
    CHECK(next_budget.remaining_budget_us ==
          relayed.hop_budget.remaining_budget_us);

    memset(&pipeline_qos_storage, 0, sizeof(pipeline_qos_storage));
    ucn_v6_qos_default_policy(&qos_policy);
    CHECK(ucn_v6_qos_owner_init_in_place(
              pipeline_qos_storage.bytes, sizeof(pipeline_qos_storage),
              ucn_v6_compiled_manifest(), &qos_policy, &qos) == UCN_V6_OK);
    CHECK(ucn_v6_qos_enqueue(
              qos, 110U, &relay_ingress, 900U,
              (uint16_t)relayed_length, 1U, &enqueue_result) == UCN_V6_OK);
    CHECK(enqueue_result.accepted);
    CHECK(ucn_v6_qos_select_next(qos, 110U, &qos_selection) == UCN_V6_OK);
    CHECK(qos_selection.action == UCN_V6_QOS_ACTION_SEND &&
          qos_selection.buffer_token == 900U);
    delivered_frame = relayed_encoded;
    delivered_length = relayed_length;

#if UCN_V6_FEATURE_ADAPTER_ENABLED
    memset(&runtime, 0, sizeof(runtime));
    memset(&driver, 0, sizeof(driver));
    memset(&runtime_ops, 0, sizeof(runtime_ops));
    runtime_ops.context = &runtime;
    runtime_ops.lock_task = pipeline_lock_task;
    runtime_ops.try_lock_from_isr = pipeline_try_lock_from_isr;
    runtime_ops.unlock_task = pipeline_unlock;
    runtime_ops.unlock_from_isr = pipeline_unlock;
    runtime_ops.post_owner_event = pipeline_post;
    memset(&pipeline_adapter_storage, 0, sizeof(pipeline_adapter_storage));
    CHECK(ucn_v6_adapter_init_in_place(
              pipeline_adapter_storage.bytes, sizeof(pipeline_adapter_storage),
              ucn_v6_compiled_manifest(), &runtime_ops, &adapter) == UCN_V6_OK);
    memset(&link_config, 0, sizeof(link_config));
    link_config.link_id = 1U;
    link_config.initial_generation = 1U;
    link_config.bearer = UCN_V6_BEARER_CUSTOM;
    link_config.nominal_bitrate_bps = UINT32_C(3000000);
    link_config.carrier_mtu = UCN_V6_CONFIG_ADAPTER_FRAME_BYTES;
    link_config.link_frame_mtu = UCN_V6_CONFIG_ADAPTER_FRAME_BYTES;
    link_config.hardware_priority_count = 4U;
    link_config.rx_slot_quota = 1U;
    link_config.tx_slot_quota = 1U;
    link_config.ops.struct_size = sizeof(link_config.ops);
    link_config.ops.api_version = UCN_V6_ADAPTER_API_VERSION;
    link_config.ops.context = &driver;
    link_config.ops.submit = pipeline_submit;
    link_config.ops.cancel = pipeline_cancel;
    link_config.ops.quiesce = pipeline_quiesce;
    CHECK(ucn_v6_adapter_register_link(adapter, &link_config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 1U, 1U, UCN_V6_LINK_READY) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_enqueue_tx(
              adapter, 1U, qos_selection.buffer_token, relayed_encoded,
              relayed_length, qos_selection.traffic_class, false,
              &adapter_key) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_service_tx(adapter, &submitted) == UCN_V6_OK &&
          submitted && driver.submit_calls == 1U);
    CHECK(memcmp(driver.frame, relayed_encoded, relayed_length) == 0);
    CHECK(ucn_v6_qos_complete_selection(
              qos, 900U, UCN_V6_QOS_SELECTION_LINK_SUBMITTED) == UCN_V6_OK);
    memset(&timestamp, 0, sizeof(timestamp));
    CHECK(ucn_v6_adapter_publish_tx_completion(
              adapter, &driver.key, UCN_V6_OK, &timestamp, false) ==
          UCN_V6_OK);
    CHECK(ucn_v6_adapter_peek_tx_completion(adapter, &completion) ==
          UCN_V6_OK);
    CHECK(ucn_v6_adapter_retire_tx_completion(
              adapter, &completion.key, &retired_token) == UCN_V6_OK &&
          retired_token == 900U);
    CHECK(ucn_v6_qos_record_completion(
              qos, 900U, UCN_V6_QOS_COMPLETION_PHYSICAL_COMPLETED) ==
          UCN_V6_OK);
    delivered_frame = driver.frame;
    delivered_length = driver.frame_length;
#else
    CHECK(ucn_v6_qos_complete_selection(
              qos, 900U, UCN_V6_QOS_SELECTION_LINK_SUBMITTED) == UCN_V6_OK);
    CHECK(ucn_v6_qos_record_completion(
              qos, 900U, UCN_V6_QOS_COMPLETION_PHYSICAL_COMPLETED) ==
          UCN_V6_OK);
#endif
    /* Relay may persist a batched outbound hop-sequence reservation. The
     * next accepted ingress frame itself must not persist replay state. */
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    b_submits_before = stores[1].submits;
    CHECK(ucn_v6_security_open_frame(
              managers[1], 111U, 1U, 6U, &a, encoded_d, encoded_d_length,
              NULL, 0U, &opened) == UCN_V6_OK);
    CHECK(stores[1].submits == b_submits_before);
    CHECK(!opened.endpoint_authorized && opened.hop_authenticated);
#endif
    CHECK(relayed.source_address == frame_c.source_address &&
          relayed.origin_sequence == frame_c.origin_sequence &&
          relayed.hop_sequence == 1U &&
          relayed.hop_limit + 1U == frame_c.hop_limit &&
          memcmp(relayed.e2e_tag, frame_c.e2e_tag,
                 sizeof(relayed.e2e_tag)) == 0 &&
          memcmp(relayed.payload, frame_c.payload,
                 frame_c.payload_length) == 0);

    c_submits_before = stores[2].submits;
    CHECK(ucn_v6_security_open_frame(
              managers[2], 120U, 1U, 6U, &b, delivered_frame, delivered_length,
              plaintext, sizeof(plaintext), &opened) == UCN_V6_OK);
    CHECK(stores[2].submits == c_submits_before);
    CHECK(opened.endpoint_authorized &&
          memcmp(opened.authenticated_principal.bytes, a.bytes,
                 sizeof(a.bytes)) == 0 &&
          memcmp(plaintext, transfer_payload, transfer_payload_length) == 0);
    memset(&pipeline_transfer_storage, 0, sizeof(pipeline_transfer_storage));
    CHECK(ucn_v6_transfer_owner_init_in_place(
              pipeline_transfer_storage.bytes,
              sizeof(pipeline_transfer_storage),
              ucn_v6_compiled_manifest(), NULL,
              100U, 4U, 1000U, 2000U,
              &transfer_rx) == UCN_V6_OK);
    CHECK(ucn_v6_transfer_receive_fragment(
              transfer_rx, 120U, &opened, &transfer_result) == UCN_V6_OK);
    CHECK(transfer_result.accepted && transfer_result.complete);
    memset(&origin, 0, sizeof(origin));
    origin.principal = a;
    origin.binding.realm_id = 1U;
    origin.binding.node_address = 10U;
    origin.binding.binding_generation = 1U;
    origin.session_generation = 1U;
    CHECK(ucn_v6_transfer_copy_completed(
              transfer_rx, &origin, 201U, 201U, completed_payload,
              sizeof(completed_payload), &completed) == UCN_V6_OK);
    CHECK(completed.payload_length == sizeof(payload_c) &&
          memcmp(completed_payload, payload_c, sizeof(payload_c)) == 0);
    CHECK(ucn_v6_qos_record_completion(
              qos, 900U, UCN_V6_QOS_COMPLETION_REMOTE_ACKED) == UCN_V6_OK);
    CHECK(ucn_v6_qos_record_completion(
              qos, 900U, UCN_V6_QOS_COMPLETION_APPLICATION_RESULT) ==
          UCN_V6_OK);
    CHECK(ucn_v6_qos_retire_completion(qos, 900U) == UCN_V6_OK);
    return 0;
}

static int test_witness_rollback_fails_closed(void)
{
    static const uint8_t admin_proof[] = { UCN_V6_PROOF_REALM_ADMIN };
    fake_store_t store;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_security_view_t view;
    ucn_v6_principal_t local = make_principal(0x20U);
    ucn_v6_principal_t admin = make_principal(0xA0U);
    ucn_v6_group_policy_slot_t group;

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    memset(&gate, 0, sizeof(gate));
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) == UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    memset(&group, 0, sizeof(group));
    group.state = UCN_V6_GROUP_SLOT_ACTIVE;
    group.group_id = 31U;
    group.group_generation = 1U;
    group.owner_principal = admin;
    store.reject_submit = true;
    CHECK(ucn_v6_security_set_group_policy(
              manager, 0U, &group, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_ERR_STATE);
    CHECK(store.witness_present && store.present &&
          store.witness.committed_generation == 1U &&
          store.witness.pending_generation == 2U &&
          store.snapshot.snapshot_generation == 1U);
    memset(&storage, 0, sizeof(storage));
    manager = NULL;
    store.reject_submit = false;
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.snapshot_generation == 1U && view.active_groups == 0U &&
          store.witness.committed_generation == 1U &&
          store.witness.pending_generation == 0U);
    return 0;
}

static int test_first_init_pending_witness_recovers_exact_generation(void)
{
    fake_store_t store;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x21U);
    ucn_v6_security_view_t view;

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    memset(&storage, 0, sizeof(storage));
    store.witness_present = true;
    store.witness.magic = UCN_V6_DURABLE_WITNESS_MAGIC;
    store.witness.schema = UCN_V6_DURABLE_WITNESS_SCHEMA;
    store.witness.flags = UCN_V6_DURABLE_WITNESS_COMMISSIONED;
    store.witness.domain = (uint8_t)UCN_V6_DURABLE_WITNESS_SECURITY;
    store.witness.witness_generation = 1U;
    store.witness.pending_generation = 1U;
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);

    store.reject_submit = true;
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) ==
          UCN_V6_ERR_STATE);
    CHECK(manager == NULL && !store.present && store.submits == 1U &&
          store.witness.committed_generation == 0U &&
          store.witness.pending_generation == 1U &&
          store.witness.witness_generation == 1U);

    store.reject_submit = false;
    memset(&storage, 0, sizeof(storage));
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    CHECK(store.submits == 2U && store.present &&
          store.snapshot.snapshot_generation == 1U &&
          store.witness.committed_generation == 1U &&
          store.witness.pending_generation == 0U &&
          store.witness.witness_generation == 2U);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.snapshot_generation == 1U && view.admitted_sessions == 0U);

    /* A later restart consumes the committed pair without another write. */
    memset(&storage, 0, sizeof(storage));
    manager = NULL;
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    CHECK(store.submits == 2U && store.witness.witness_generation == 2U);
    return 0;
}

static int test_snapshot_padding_is_not_persistent_semantics(void)
{
    fake_store_t store;
    fake_store_t corrupted;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_store_ops_t corrupted_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x26U);

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    memset(&storage, 0, sizeof(storage));
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);

    /* A Provider may deserialize into an ABI whose padding differs.  Every
     * semantic empty field remains zero, so reload must still succeed. */
    store.perturb_padding = true;
    memset(&storage, 0, sizeof(storage));
    manager = NULL;
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);

    /* A nonzero semantic field in an empty slot is never padding and must be
     * rejected independently of representation bytes. */
    corrupted = store;
    corrupted.perturb_padding = false;
    corrupted.snapshot.sessions[0].admitted = true;
    corrupted_api = store_ops(&corrupted);
    memset(&storage, 0, sizeof(storage));
    manager = NULL;
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &corrupted_api, &crypto_api, &gate, &manager) ==
          UCN_V6_ERR_STATE);
    CHECK(manager == NULL);
    return 0;
}

static int test_authority_floor_transfer_and_old_authority_replay(void)
{
    const ucn_v6_bootstrap_config_t bootstrap_config = {
        2U, 1U, 2U, 1U, UINT64_C(10000)
    };
    fake_store_t store;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_security_manager_storage_t manager_storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_bootstrap_owner_storage_t bootstrap_storage;
    ucn_v6_bootstrap_owner_t *bootstrap = NULL;
    ucn_v6_principal_t device = make_principal(0x18U);
    ucn_v6_principal_t authority1 = make_principal(0x58U);
    ucn_v6_principal_t authority2 = make_principal(0x78U);
    ucn_v6_bootstrap_transcript_t next;
    ucn_v6_bootstrap_transcript_t stale;
    ucn_v6_bootstrap_key_t key;
    ucn_v6_join_commit_t join;
    ucn_v6_security_view_t view;
    unsigned submits_before;

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    memset(&manager_storage, 0, sizeof(manager_storage));
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              manager_storage.bytes, sizeof(manager_storage),
              ucn_v6_compiled_manifest(), 1U, &device, &store_api,
              &crypto_api, &gate, &manager) == UCN_V6_OK);
    CHECK(install_pair_session(manager, &device, 7U, 3U, &authority1,
                               8U, 4U, true, 700U, 1U) == 0);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.authority_floor_valid &&
          view.authority_floor.authority_generation == 8U &&
          memcmp(view.authority_floor.authority_principal.bytes,
                 authority1.bytes, sizeof(authority1.bytes)) == 0);

    /* The current Authority re-endorses the same Binding Generation.  A
     * REAUTH under the higher fenced Authority atomically advances the floor.
     * The already-admitted old Session keeps only its previously established
     * local lease; the transfer cannot extend it. */
    next = make_transcript_pair(&device, 7U, 3U, &authority2, 9U, 1U,
                                701U, 7U);
    next.flow = UCN_V6_BOOTSTRAP_FLOW_REAUTH;
    next.selected_session_generation = 1U;
    next.durable_fence_token[0] ^= UINT8_C(0x5A);
    key = make_bootstrap_key(&next, 2U);
    join = make_join(&next, true);
    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              bootstrap_storage.bytes, sizeof(bootstrap_storage),
              ucn_v6_compiled_manifest(), &bootstrap_config,
              &bootstrap) == UCN_V6_OK);
    CHECK(complete_bootstrap(bootstrap, &next, &key, 30U) == 0);
    CHECK(ucn_v6_security_commit_join(
              manager, bootstrap, &key, 40U, &join) == UCN_V6_OK);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.authority_floor_valid &&
          view.authority_floor.authority_generation == 9U &&
          memcmp(view.authority_floor.authority_principal.bytes,
                 authority2.bytes, sizeof(authority2.bytes)) == 0 &&
          view.admitted_sessions == 2U &&
          view.pending_invalidations == 0U);

    /* The floor is durable.  After restart, a completely self-consistent old
     * Authority transcript/proof is still below the floor and is rejected
     * before it can create a Session. */
    memset(&manager_storage, 0, sizeof(manager_storage));
    manager = NULL;
    CHECK(ucn_v6_security_init_in_place(
              manager_storage.bytes, sizeof(manager_storage),
              ucn_v6_compiled_manifest(), 1U, &device, &store_api,
              &crypto_api, &gate, &manager) == UCN_V6_OK);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.admitted_sessions == 0U && view.authority_floor_valid &&
          view.authority_floor.authority_generation == 9U);
    stale = make_transcript_pair(&device, 7U, 3U, &authority1, 8U, 4U,
                                 702U, 8U);
    stale.flow = UCN_V6_BOOTSTRAP_FLOW_REAUTH;
    stale.selected_session_generation = 2U;
    stale.selected_hop_key_generation = 4U;
    key = make_bootstrap_key(&stale, 3U);
    join = make_join(&stale, true);
    bootstrap = NULL;
    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              bootstrap_storage.bytes, sizeof(bootstrap_storage),
              ucn_v6_compiled_manifest(), &bootstrap_config,
              &bootstrap) == UCN_V6_OK);
    CHECK(complete_bootstrap(bootstrap, &stale, &key, 50U) == 0);
    submits_before = store.submits;
    CHECK(ucn_v6_security_commit_join(
              manager, bootstrap, &key, 60U, &join) == UCN_V6_ERR_REPLAY);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.authority_floor_valid &&
          view.authority_floor.authority_generation == 9U);
    return 0;
}

static int test_durable_session_transcript_and_link_generation_are_strict(void)
{
    fake_store_t store;
    fake_store_t corrupted;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_store_ops_t corrupted_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x24U);
    ucn_v6_principal_t peer = make_principal(0x44U);

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    memset(&storage, 0, sizeof(storage));
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    CHECK(install_pair_session(manager, &local, 7U, 3U, &peer, 8U, 4U,
                               true, 901U, 1U) == 0);

    corrupted = store;
    ++corrupted.snapshot.sessions[0].bootstrap_transcript.device_nonce;
    corrupted_api = store_ops(&corrupted);
    memset(&storage, 0, sizeof(storage));
    manager = NULL;
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &corrupted_api, &crypto_api, &gate, &manager) ==
          UCN_V6_ERR_STATE);
    CHECK(manager == NULL);

    corrupted = store;
    corrupted.snapshot.sessions[0].link_instance_generation =
        UCN_V6_SERIAL_ROTATION_THRESHOLD + 1U;
    corrupted.snapshot.sessions[0]
        .bootstrap_transcript.selected_link_instance_generation =
        UCN_V6_SERIAL_ROTATION_THRESHOLD + 1U;
    corrupted_api = store_ops(&corrupted);
    memset(&storage, 0, sizeof(storage));
    manager = NULL;
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &corrupted_api, &crypto_api, &gate, &manager) ==
          UCN_V6_ERR_STATE);
    CHECK(manager == NULL);
    return 0;
}

static int test_session_invalidation_queue_full_is_fail_closed(void)
{
    fake_store_t store;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate;
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x11U);
    ucn_v6_principal_t peers[UCN_V6_CONFIG_SECURITY_SESSIONS];
    ucn_v6_security_view_t view;
    ucn_v6_stack_invalidation_t event;
    ucn_v6_binding_key_t first_peer_binding = { 1U, 20U, 1U };
    size_t index;

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    memset(&gate, 0, sizeof(gate));
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              1U, &local, &store_api, &crypto_api, &gate, &manager) ==
          UCN_V6_OK);
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        peers[index] = make_principal((uint8_t)(0x40U + index * 2U));
        CHECK(install_pair_session(
                  manager, &local, 7U, 3U, &peers[index],
                  (uint32_t)(20U + index), 1U, true,
                  (uint64_t)(100U + index), (uint32_t)(index + 1U)) == 0);
    }
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        CHECK(ucn_v6_security_require_reauth(manager, &peers[index]) ==
              UCN_V6_OK);
    }
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == UCN_V6_SECURITY_INVALIDATION_DEPTH &&
          view.admitted_sessions == 0U && !view.faulted);

    /* The exact REAUTH reuses its already-pending old-session event.  A
     * later distinct generation cannot be fenced while the queue is full. */
    CHECK(reauth_pair_session(
              manager, &local, 7U, 3U,
              &peers[UCN_V6_CONFIG_SECURITY_SESSIONS - 1U],
              (uint32_t)(20U + UCN_V6_CONFIG_SECURITY_SESSIONS - 1U),
              1U, true, 500U, 100U) == 0);
    CHECK(ucn_v6_security_require_reauth(
              manager,
              &peers[UCN_V6_CONFIG_SECURITY_SESSIONS - 1U]) ==
          UCN_V6_ERR_NO_SPACE);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == UCN_V6_SECURITY_INVALIDATION_DEPTH &&
          view.admitted_sessions == 1U && view.faulted);
    CHECK(ucn_v6_security_invalidation_peek(manager, &event) == UCN_V6_OK);
    CHECK(security_session_event_matches(
              &event, 1U, 6U, &peers[0], &first_peer_binding, 1U));
    return 0;
}

static int test_link_invalidation_is_exact_durable_and_atomic(void)
{
    fake_store_t store;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x20U);
    ucn_v6_principal_t peer_a = make_principal(0x40U);
    ucn_v6_principal_t peer_b = make_principal(0x60U);
    ucn_v6_binding_key_t peer_a_binding = { 1U, 20U, 1U };
    ucn_v6_binding_key_t peer_b_binding = { 1U, 21U, 1U };
    ucn_v6_stack_invalidation_t stale = link_invalidation(1U, 5U);
    ucn_v6_stack_invalidation_t current = link_invalidation(1U, 6U);
    ucn_v6_stack_invalidation_t event;
    ucn_v6_security_view_t view;
    unsigned submits_before;

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    CHECK(install_pair_session(manager, &peer_a, 20U, 1U, &local, 7U, 3U,
                               false, 100U, 1U) == 0);
    CHECK(install_pair_session(manager, &peer_b, 21U, 1U, &local, 7U, 3U,
                               false, 101U, 2U) == 0);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.admitted_sessions == 2U && view.pending_invalidations == 0U);

    submits_before = store.submits;
    CHECK(ucn_v6_security_apply_link_invalidation(manager, &stale) ==
          UCN_V6_OK);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.admitted_sessions == 2U && view.pending_invalidations == 0U);

    CHECK(ucn_v6_security_apply_link_invalidation(manager, &current) ==
          UCN_V6_OK);
    CHECK(store.submits == submits_before + 1U);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.admitted_sessions == 0U && view.pending_invalidations == 2U &&
          !view.faulted);
    CHECK(ucn_v6_security_invalidation_peek(manager, &event) == UCN_V6_OK);
    CHECK(security_session_event_matches(
              &event, 1U, 6U, &peer_a, &peer_a_binding, 1U));
    CHECK(ucn_v6_security_invalidation_ack(manager, &event) == UCN_V6_OK);
    CHECK(ucn_v6_security_invalidation_peek(manager, &event) == UCN_V6_OK);
    CHECK(security_session_event_matches(
              &event, 1U, 6U, &peer_b, &peer_b_binding, 1U));
    CHECK(ucn_v6_security_invalidation_ack(manager, &event) == UCN_V6_OK);

    /* Replaying the same Link generation cannot allocate another durable
     * generation or recreate an already-consumed child event. */
    submits_before = store.submits;
    CHECK(ucn_v6_security_apply_link_invalidation(manager, &current) ==
          UCN_V6_OK);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_security_invalidation_peek(manager, &event) ==
          UCN_V6_ERR_NOT_FOUND);
    return 0;
}

static int test_link_invalidation_capacity_and_store_failure_are_atomic(void)
{
    fake_store_t store;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x20U);
    ucn_v6_principal_t peers[UCN_V6_CONFIG_SECURITY_SESSIONS];
    ucn_v6_stack_invalidation_t link = link_invalidation(1U, 7U);
    ucn_v6_security_view_t view;
    unsigned submits_before;
    size_t index;

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        peers[index] = make_principal((uint8_t)(0x40U + index * 3U));
        CHECK(install_pair_session(
                  manager, &peers[index], (uint32_t)(20U + index), 1U,
                  &local, 7U, 3U, false, (uint64_t)(100U + index),
                  (uint32_t)(index + 1U)) == 0);
        CHECK(ucn_v6_security_require_reauth(manager, &peers[index]) ==
              UCN_V6_OK);
    }
    CHECK(reauth_pair_session(manager, &peers[0], 20U, 1U, &local, 7U, 3U,
                              false, 500U, 100U) == 0);
    CHECK(reauth_pair_session(manager, &peers[1], 21U, 1U, &local, 7U, 3U,
                              false, 501U, 101U) == 0);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == UCN_V6_SECURITY_INVALIDATION_DEPTH &&
          view.admitted_sessions == 2U && !view.faulted);
    submits_before = store.submits;
    CHECK(ucn_v6_security_apply_link_invalidation(manager, &link) ==
          UCN_V6_ERR_NO_SPACE);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == UCN_V6_SECURITY_INVALIDATION_DEPTH &&
          view.admitted_sessions == 2U && !view.faulted);

    /* A synchronous Provider failure cannot publish any child invalidation or
     * install the candidate flags in RAM.  The Manager faults because its
     * durable witness may now be pending and must be recovered on restart. */
    memset(&store, 0, sizeof(store));
    memset(&storage, 0, sizeof(storage));
    memset(&gate, 0, sizeof(gate));
    store_api = store_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    CHECK(install_pair_session(manager, &peers[0], 20U, 1U, &local, 7U, 3U,
                               false, 600U, 1U) == 0);
    link = link_invalidation(1U, 6U);
    store.reject_submit = true;
    submits_before = store.submits;
    CHECK(ucn_v6_security_apply_link_invalidation(manager, &link) ==
          UCN_V6_ERR_STATE);
    CHECK(store.submits == submits_before + 1U);
    CHECK(store.snapshot.sessions[0].admitted &&
          !store.snapshot.sessions[0].requires_reauth);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.admitted_sessions == 1U && view.pending_invalidations == 0U &&
          view.faulted);
    return 0;
}

static int test_link_invalidation_skips_completed_fence_when_capacity_is_one(void)
{
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    fake_store_t store = {0};
    fake_crypto_t crypto = {0};
    ucn_v6_security_store_ops_t store_api = store_ops(&store);
    ucn_v6_security_crypto_ops_t crypto_api = crypto_ops(&crypto);
    ucn_v6_callback_gate_t gate = UCN_V6_CALLBACK_GATE_INITIALIZER;
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x20U);
    ucn_v6_principal_t fenced_peer = make_principal(0x40U);
    ucn_v6_principal_t active_peer = make_principal(0x60U);
    ucn_v6_principal_t filler_peer = make_principal(0x80U);
    ucn_v6_binding_key_t active_binding = { 1U, 21U, 1U };
    ucn_v6_stack_invalidation_t link = link_invalidation(1U, 6U);
    ucn_v6_stack_invalidation_t event;
    ucn_v6_security_view_t view;
    uint32_t generation;

    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    CHECK(install_pair_session(manager, &fenced_peer, 20U, 1U, &local, 7U,
                               3U, false, 100U, 1U) == 0);
    CHECK(install_pair_session(manager, &active_peer, 21U, 1U, &local, 7U,
                               3U, false, 101U, 2U) == 0);
    CHECK(install_pair_session(manager, &filler_peer, 22U, 1U, &local, 7U,
                               3U, false, 102U, 3U) == 0);

    /* Complete and acknowledge the first child's fence. */
    CHECK(ucn_v6_security_require_reauth(manager, &fenced_peer) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_invalidation_peek(manager, &event) == UCN_V6_OK);
    CHECK(ucn_v6_security_invalidation_ack(manager, &event) == UCN_V6_OK);

    /* One filler Session advances through exact-next generations while its
     * older child events remain queued.  This leaves exactly one queue slot,
     * without allocating unbounded test or production state. */
    CHECK(ucn_v6_security_require_reauth(manager, &filler_peer) ==
          UCN_V6_OK);
    for (generation = 2U;
         generation < UCN_V6_SECURITY_INVALIDATION_DEPTH;
         ++generation) {
        CHECK(reauth_pair_session_exact(
                  manager, &filler_peer, 22U, 1U, &local, 7U, 3U, false,
                  UINT64_C(200) + generation, 100U + generation,
                  generation, generation + 5U, generation + 2U) == 0);
        CHECK(ucn_v6_security_require_reauth(manager, &filler_peer) ==
              UCN_V6_OK);
    }
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations ==
              UCN_V6_SECURITY_INVALIDATION_DEPTH - 1U &&
          view.admitted_sessions == 1U && !view.faulted);

    /* The already-fenced sibling must not be counted or re-emitted.  The
     * sole remaining slot belongs to the still-admitted sibling. */
    CHECK(ucn_v6_security_apply_link_invalidation(manager, &link) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.pending_invalidations == UCN_V6_SECURITY_INVALIDATION_DEPTH &&
          view.admitted_sessions == 0U && !view.faulted);
    for (generation = 0U;
         generation < UCN_V6_SECURITY_INVALIDATION_DEPTH - 1U;
         ++generation) {
        CHECK(ucn_v6_security_invalidation_peek(manager, &event) ==
              UCN_V6_OK);
        CHECK(ucn_v6_security_invalidation_ack(manager, &event) ==
              UCN_V6_OK);
    }
    CHECK(ucn_v6_security_invalidation_peek(manager, &event) == UCN_V6_OK);
    CHECK(security_session_event_matches(
              &event, 1U, 6U, &active_peer, &active_binding, 1U));
#endif
    return 0;
}

static int test_group_fixed_slots_and_replay(void)
{
    static const uint8_t admin_proof[] = { UCN_V6_PROOF_REALM_ADMIN };
    fake_store_t store;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x20U);
    ucn_v6_principal_t admin = make_principal(0xA0U);
    ucn_v6_group_policy_slot_t group;
    ucn_v6_group_key_slot_t key;
    ucn_v6_frame_t frame;
    ucn_v6_key_selector_t selector;
    ucn_v6_security_open_result_t opened;
    uint8_t payload[] = { 0x55U };
    uint8_t encoded[160U];
    size_t encoded_length = 0U;

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
    memset(&gate, 0, sizeof(gate));
    store_api = store_ops(&store);
    crypto_api = crypto_ops(&crypto);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) == UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) == UCN_V6_OK);
    memset(&group, 0, sizeof(group));
    group.state = UCN_V6_GROUP_SLOT_ACTIVE;
    group.group_id = 21U;
    group.group_generation = 1U;
    group.owner_principal = admin;
    CHECK(ucn_v6_security_set_group_policy(
              manager, 0U, &group, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    memset(&key, 0, sizeof(key));
    key.state = UCN_V6_GROUP_KEY_ACTIVE;
    key.group_id = 21U;
    key.group_generation = 1U;
    key.key_id = 9U;
    key.suite_id = UCN_V6_SUITE_HMAC_SHA256_128;
    key.current_generation = 1U;
    CHECK(ucn_v6_security_set_group_key(
              manager, 10U, 0U, 0U, &key, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);

    memset(&frame, 0, sizeof(frame));
    frame.address_class = UCN_V6_ADDRESS_CLASS_A0;
    frame.frame_type = UCN_V6_FRAME_CONTROL;
    frame.flags = UCN_V6_FLAG_GROUP_CONTEXT |
                  UCN_V6_FLAG_PROTOCOL_CONTEXT;
    frame.traffic_class = UCN_V6_TRAFFIC_Q1;
    frame.delivery_guarantee = UCN_V6_DELIVERY_LATEST;
    frame.hop_limit = 1U;
    frame.header_contract = UCN_V6_HEADER_CONTRACT_1;
    frame.realm_id = 1U;
    frame.source_address = 7U;
    frame.destination_address = UINT8_MAX;
    frame.source_binding_generation = 3U;
    frame.session_generation = 1U;
    frame.origin_sequence = 1U;
    frame.group.group_id = 21U;
    frame.group.group_generation = 1U;
    frame.group.suite_id = UCN_V6_SUITE_HMAC_SHA256_128;
    frame.group.key_id = 9U;
    frame.group.key_generation = 1U;
    frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO;
    frame.payload = payload;
    frame.payload_length = sizeof(payload);
    CHECK(ucn_v6_wire_encode(&frame, encoded, sizeof(encoded),
                             &encoded_length) == UCN_V6_OK);
    selector.suite_id = UCN_V6_SUITE_HMAC_SHA256_128;
    selector.key_id = 9U;
    selector.key_generation = 1U;
    fake_tag(&selector, encoded, encoded_length - 20U, NULL, 0U,
             frame.link_tag);
    CHECK(ucn_v6_wire_encode(&frame, encoded, sizeof(encoded),
                             &encoded_length) == UCN_V6_OK);
    memset(&opened, 0xA5, sizeof(opened));
    {
        ucn_v6_security_open_result_t before = opened;
        CHECK(ucn_v6_security_open_frame(
                  manager, 20U, 1U, 0U, NULL, encoded, encoded_length,
                  NULL, 0U, &opened) == UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(&opened, &before, sizeof(opened)) == 0);
    }
    CHECK(ucn_v6_security_open_frame(
              manager, 20U, 1U, 1U, NULL, encoded, encoded_length,
              NULL, 0U, &opened) == UCN_V6_OK);
    CHECK(opened.group_discovery_only && !opened.endpoint_authorized &&
          opened.ingress_link_instance_id == 1U &&
          opened.ingress_link_instance_generation == 1U);
    CHECK(ucn_v6_security_open_frame(
              manager, 21U, 1U, 1U, NULL, encoded, encoded_length,
              NULL, 0U, &opened) == UCN_V6_ERR_REPLAY);

    key.current_generation = 2U;
    key.previous_deadline_us = 100U;
    CHECK(ucn_v6_security_set_group_key(
              manager, 30U, 0U, 0U, &key, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    frame.origin_sequence = 2U;
    frame.group.key_generation = 1U;
    memset(frame.link_tag, 0, sizeof(frame.link_tag));
    CHECK(ucn_v6_wire_encode(&frame, encoded, sizeof(encoded),
                             &encoded_length) == UCN_V6_OK);
    selector.key_generation = 1U;
    fake_tag(&selector, encoded, encoded_length - 20U, NULL, 0U,
             frame.link_tag);
    CHECK(ucn_v6_wire_encode(&frame, encoded, sizeof(encoded),
                             &encoded_length) == UCN_V6_OK);
    CHECK(ucn_v6_security_open_frame(
              manager, 99U, 1U, 1U, NULL, encoded, encoded_length,
              NULL, 0U, &opened) == UCN_V6_OK);
    frame.origin_sequence = 3U;
    memset(frame.link_tag, 0, sizeof(frame.link_tag));
    CHECK(ucn_v6_wire_encode(&frame, encoded, sizeof(encoded),
                             &encoded_length) == UCN_V6_OK);
    fake_tag(&selector, encoded, encoded_length - 20U, NULL, 0U,
             frame.link_tag);
    CHECK(ucn_v6_wire_encode(&frame, encoded, sizeof(encoded),
                             &encoded_length) == UCN_V6_OK);
    CHECK(ucn_v6_security_open_frame(
              manager, 100U, 1U, 1U, NULL, encoded, encoded_length,
              NULL, 0U, &opened) == UCN_V6_ERR_SECURITY);
    key.state = UCN_V6_GROUP_KEY_RETIRED;
    CHECK(ucn_v6_security_set_group_key(
              manager, 40U, 0U, 0U, &key, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    key.state = UCN_V6_GROUP_KEY_ACTIVE;
    key.current_generation = 3U;
    CHECK(ucn_v6_security_set_group_key(
              manager, 50U, 0U, 0U, &key, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_ERR_STATE);
    return 0;
}

static int test_five_node_verified_relay_chain(void)
{
    static const uint8_t admin_proof[] = { UCN_V6_PROOF_REALM_ADMIN };
    static const uint8_t payload[] = { 0x51U, 0x52U, 0x53U, 0x54U };
    static fake_store_t stores[5];
    static ucn_v6_security_manager_storage_t storage[5];
    fake_crypto_t cryptos[5];
    ucn_v6_security_store_ops_t store_ops_array[5];
    ucn_v6_security_crypto_ops_t crypto_ops_array[5];
    ucn_v6_callback_gate_t gates[5];
    ucn_v6_security_manager_t *managers[5] = {
        NULL, NULL, NULL, NULL, NULL
    };
    ucn_v6_principal_t principals[5];
    ucn_v6_principal_t admin = make_principal(0xA0U);
    ucn_v6_acl_entry_t acl;
    ucn_v6_frame_t frame;
    ucn_v6_frame_t relayed;
    ucn_v6_security_open_result_t verified;
    ucn_v6_security_open_result_t opened;
    uint8_t payload_work[sizeof(payload)];
    uint8_t frame_work[256U];
    uint8_t encoded[4][256U];
    uint8_t plaintext[sizeof(payload)];
    uint8_t original_e2e_tag[UCN_V6_SECURITY_TAG_BYTES];
    size_t lengths[4] = { 0U, 0U, 0U, 0U };
    size_t index;

    memset(stores, 0, sizeof(stores));
    memset(storage, 0, sizeof(storage));
    memset(cryptos, 0, sizeof(cryptos));
    memset(gates, 0, sizeof(gates));
    for (index = 0U; index < 5U; ++index) {
        principals[index] = make_principal((uint8_t)(0x10U + index * 0x20U));
        store_ops_array[index] = store_ops(&stores[index]);
        crypto_ops_array[index] = crypto_ops(&cryptos[index]);
        CHECK(ucn_v6_callback_gate_init(
                  &gates[index], NULL, no_lock, no_lock) == UCN_V6_OK);
        CHECK(ucn_v6_security_init_in_place(
                  storage[index].bytes, sizeof(storage[index]),
                  ucn_v6_compiled_manifest(), 1U, &principals[index],
                  &store_ops_array[index], &crypto_ops_array[index],
                  &gates[index], &managers[index]) == UCN_V6_OK);
    }

    for (index = 0U; index < 4U; ++index) {
        CHECK(install_pair_session(
                  managers[index], &principals[index],
                  (uint32_t)((index + 1U) * 10U), (uint32_t)(index + 1U),
                  &principals[index + 1U],
                  (uint32_t)((index + 2U) * 10U), (uint32_t)(index + 2U),
                  true, UINT64_C(200) + index * 2U,
                  (uint32_t)(index * 2U + 1U)) == 0);
        CHECK(install_pair_session(
                  managers[index + 1U], &principals[index],
                  (uint32_t)((index + 1U) * 10U), (uint32_t)(index + 1U),
                  &principals[index + 1U],
                  (uint32_t)((index + 2U) * 10U), (uint32_t)(index + 2U),
                  false, UINT64_C(201) + index * 2U,
                  (uint32_t)(index * 2U + 2U)) == 0);
    }
    CHECK(install_pair_session(
              managers[0], &principals[0], 10U, 1U, &principals[4], 50U,
              5U, true, UINT64_C(300), 20U) == 0);
    CHECK(install_pair_session(
              managers[4], &principals[0], 10U, 1U, &principals[4], 50U,
              5U, false, UINT64_C(301), 21U) == 0);

    acl = make_acl(&principals[0], 10U, 1U, 50U, 5U,
                   UCN_V6_SECURITY_OUTBOUND);
    CHECK(ucn_v6_security_set_acl(
              managers[0], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);
    acl.key.direction = UCN_V6_SECURITY_INBOUND;
    CHECK(ucn_v6_security_set_acl(
              managers[4], &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_OK);

    frame = make_data_frame(payload, sizeof(payload));
    frame.source_address = 10U;
    frame.source_binding_generation = 1U;
    frame.destination_address = 50U;
    frame.destination_binding_generation = 5U;
    frame.message.operation_id = UINT64_C(501);
    CHECK(ucn_v6_security_protect_frame(
              managers[0], 100U, &principals[1], &principals[4], &frame,
              payload_work, sizeof(payload_work), frame_work,
              sizeof(frame_work), encoded[0], sizeof(encoded[0]),
              &lengths[0]) == UCN_V6_OK);
    memcpy(original_e2e_tag, frame.e2e_tag, sizeof(original_e2e_tag));

    for (index = 1U; index <= 3U; ++index) {
        CHECK(ucn_v6_security_relay_frame(
                  managers[index], 100U + index, 1U, 6U,
                  &principals[index - 1U], &principals[index + 1U], 0U,
                  encoded[index - 1U], lengths[index - 1U], frame_work,
                  sizeof(frame_work), encoded[index], sizeof(encoded[index]),
                  &lengths[index], &verified, &relayed) == UCN_V6_OK);
        CHECK(verified.hop_authenticated &&
              !verified.endpoint_authorized &&
              relayed.origin_sequence == frame.origin_sequence &&
              relayed.hop_limit == (uint16_t)(4U - index) &&
              memcmp(relayed.e2e_tag, original_e2e_tag,
                     sizeof(original_e2e_tag)) == 0);
    }

    CHECK(ucn_v6_security_open_frame(
              managers[4], 110U, 1U, 6U, &principals[3], encoded[3], lengths[3],
              plaintext, sizeof(plaintext), &opened) == UCN_V6_OK);
    CHECK(opened.endpoint_authorized && opened.hop_authenticated &&
          memcmp(opened.authenticated_principal.bytes, principals[0].bytes,
                 sizeof(principals[0].bytes)) == 0 &&
          opened.frame.origin_sequence == frame.origin_sequence &&
          opened.frame.hop_sequence == 1U &&
          memcmp(plaintext, payload, sizeof(payload)) == 0);
    return 0;
}

static int test_callback_violations_fail_closed(void)
{
    static const uint8_t admin_proof[] = { UCN_V6_PROOF_REALM_ADMIN };
    ucn_v6_security_manager_storage_t storage = {0};
    fake_store_t store = {0};
    fake_crypto_t crypto = {0};
    ucn_v6_callback_gate_t gate = UCN_V6_CALLBACK_GATE_INITIALIZER;
    ucn_v6_security_store_ops_t store_callbacks = store_ops(&store);
    ucn_v6_security_crypto_ops_t crypto_callbacks = crypto_ops(&crypto);
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_security_view_t view;
    ucn_v6_principal_t local = make_principal(0x21U);
    ucn_v6_principal_t peer = make_principal(0x61U);
    ucn_v6_principal_t admin = make_principal(0xA1U);
    ucn_v6_acl_entry_t acl = make_acl(
        &local, 7U, 1U, 8U, 1U, UCN_V6_SECURITY_OUTBOUND);
    unsigned submits_before;

    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              1U, &local, &store_callbacks, &crypto_callbacks, &gate,
              &manager) == UCN_V6_OK);
    submits_before = store.submits;

    crypto.reenter_on_proof = true;
    crypto.reenter_manager = manager;
    crypto.reenter_peer = peer;
    CHECK(ucn_v6_security_set_acl(
              manager, &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_ERR_STATE);
    CHECK(crypto.reenter_result == UCN_V6_ERR_STATE);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.acl_entries == 0U && !view.faulted);
    crypto.reenter_on_proof = false;

    crypto.invalid_result_on_proof = true;
    CHECK(ucn_v6_security_set_acl(
              manager, &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_ERR_STATE);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.acl_entries == 0U && !view.faulted);
    crypto.invalid_result_on_proof = false;

    crypto.leave_gate_on_proof = true;
    crypto.reenter_gate = &gate;
    CHECK(ucn_v6_security_set_acl(
              manager, &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_ERR_STATE);
    CHECK(crypto.forced_leave_result == UCN_V6_OK);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.acl_entries == 0U && !view.faulted);
    crypto.leave_gate_on_proof = false;

    store.reenter_on_reserve = true;
    store.reenter_manager = manager;
    store.reenter_peer = peer;
    CHECK(ucn_v6_security_set_acl(
              manager, &acl, &admin, admin_proof,
              sizeof(admin_proof)) == UCN_V6_ERR_STATE);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_security_copy_view(manager, &view) == UCN_V6_OK);
    CHECK(view.acl_entries == 0U && view.faulted);
    return 0;
}

int main(void)
{
    CHECK(test_join_acl_aead_replay() == 0);
#if UCN_V6_FEATURE_ADAPTER_ENABLED && UCN_V6_FEATURE_REALTIME_ENABLED
    CHECK(test_runtime_owned_four_event_exchange() == 0);
#endif
    CHECK(test_independent_sequence_domains_and_verified_relay() == 0);
    CHECK(test_five_node_verified_relay_chain() == 0);
    CHECK(test_group_fixed_slots_and_replay() == 0);
    CHECK(test_witness_rollback_fails_closed() == 0);
    CHECK(test_first_init_pending_witness_recovers_exact_generation() == 0);
    CHECK(test_snapshot_padding_is_not_persistent_semantics() == 0);
    CHECK(test_authority_floor_transfer_and_old_authority_replay() == 0);
    CHECK(test_durable_session_transcript_and_link_generation_are_strict() == 0);
    CHECK(test_session_invalidation_queue_full_is_fail_closed() == 0);
    CHECK(test_link_invalidation_is_exact_durable_and_atomic() == 0);
    CHECK(test_link_invalidation_capacity_and_store_failure_are_atomic() == 0);
    CHECK(test_link_invalidation_skips_completed_fence_when_capacity_is_one() == 0);
    CHECK(test_callback_violations_fail_closed() == 0);
    puts("ucn v6 security tests passed");
    return 0;
}
