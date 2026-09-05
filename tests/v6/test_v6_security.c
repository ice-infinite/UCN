#include "ucn/v6/ucn_v6_config.h"
#if UCN_V6_FEATURE_ADAPTER_ENABLED
#include "ucn/v6/ucn_v6_adapter.h"
#endif
#include "ucn/v6/ucn_v6_qos.h"
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
    bool witness_present;
    uint64_t generation_witness;
    bool reenter_on_reserve;
    ucn_v6_security_manager_t *reenter_manager;
    ucn_v6_principal_t reenter_peer;
    ucn_v6_result_t reenter_result;
    unsigned loads;
    unsigned submits;
    ucn_v6_security_snapshot_t snapshot;
} fake_store_t;

static ucn_v6_result_t load_generation_witness(
    void *context,
    uint64_t *generation)
{
    fake_store_t *store = (fake_store_t *)context;
    if (!store->witness_present) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *generation = store->generation_witness;
    return UCN_V6_OK;
}

static ucn_v6_result_t reserve_generation_witness(
    void *context,
    uint64_t generation)
{
    fake_store_t *store = (fake_store_t *)context;
    if (store->reenter_on_reserve) {
        store->reenter_result = ucn_v6_security_require_reauth(
            store->reenter_manager, &store->reenter_peer);
    }
    if ((store->witness_present && generation <= store->generation_witness) ||
        generation == 0U) {
        return UCN_V6_ERR_REPLAY;
    }
    store->generation_witness = generation;
    store->witness_present = true;
    return UCN_V6_OK;
}

typedef struct fake_crypto {
    unsigned proofs;
    unsigned tag_checks;
    unsigned aead_opens;
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
    ops.load_generation_witness = load_generation_witness;
    ops.reserve_generation_witness = reserve_generation_witness;
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
    value.authority_generation = 1U;
    value.device_nonce = transaction_id + 1U;
    value.authority_nonce = transaction_id + 2U;
    value.transaction_id = transaction_id;
    value.lease_freshness_challenge_nonce = transaction_id + 3U;
    value.realm_id = 1U;
    value.proposed_address = device_address;
    value.address_binding_generation = device_binding_generation;
    value.authority_address = authority_address;
    value.authority_binding_generation = authority_binding_generation;
    fill_bytes(value.binding_lease_id, sizeof(value.binding_lease_id), 0x50U);
    value.binding_lease_duration_us = 50000U;
    value.authority_lease_sequence = 9U;
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
    commit.authority_epoch.authority_principal =
        transcript->authority_principal;
    commit.authority_epoch.authority_generation = 1U;
    fill_bytes(commit.authority_epoch.durable_fence_token, 16U, 0x80U);
    fill_bytes(commit.authority_epoch.allocation_high_water_digest, 16U, 0x90U);
    commit.authority_epoch.lease_sequence = 9U;
    commit.authority_epoch.lease_duration_us = 100000U;
    commit.authority_epoch.quorum_proven = true;
    commit.authority_epoch.durable = true;
    commit.joining_binding_certificate.device_principal =
        transcript->joining_device_principal;
    commit.joining_binding_certificate.authority_principal =
        transcript->authority_principal;
    commit.joining_binding_certificate.binding = device_binding;
    commit.joining_binding_certificate.authority_generation = 1U;
    memcpy(commit.joining_binding_certificate.lease_id,
           transcript->binding_lease_id, 16U);
    commit.joining_binding_certificate.lease_duration_us = 50000U;
    commit.joining_binding_certificate.authority_lease_sequence = 9U;
    commit.joining_binding_certificate.mode = UCN_V6_ADDRESS_LEASED;
    commit.local_binding = device_side ? device_binding : authority_binding;
    commit.peer_binding = device_side ? authority_binding : device_binding;
    commit.session_generation = transcript->selected_session_generation;
    commit.link_instance_generation =
        transcript->selected_link_instance_generation;
    commit.hop_selector.suite_id = UCN_V6_SUITE_HMAC_SHA256_128;
    commit.hop_selector.key_id = 2U;
    commit.hop_selector.key_generation = 3U;
    commit.e2e_selector.suite_id = UCN_V6_SUITE_AES_GCM_128;
    commit.e2e_selector.key_id = 4U;
    commit.e2e_selector.key_generation = 5U;
    commit.authority_local_lease_deadline_us = 90000U;
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

    memset(&device_store, 0, sizeof(device_store));
    memset(&authority_store, 0, sizeof(authority_store));
    memset(&device_crypto, 0, sizeof(device_crypto));
    memset(&authority_crypto, 0, sizeof(authority_crypto));
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
        CHECK(device_store.submits == 0U);
    }
    CHECK(ucn_v6_security_commit_join(
              device_manager, device_bootstrap, &bootstrap_key, 100U,
              &device_join) == UCN_V6_OK);
    CHECK(ucn_v6_security_commit_join(
              authority_manager, authority_bootstrap, &bootstrap_key, 100U,
              &authority_join) == UCN_V6_OK);
    CHECK(device_store.submits == 1U && authority_store.submits == 1U);
    CHECK(ucn_v6_security_commit_join(
              device_manager, device_bootstrap, &bootstrap_key, 101U,
              &device_join) == UCN_V6_OK);
    CHECK(device_store.submits == 1U);
    --device_join.authority_local_lease_deadline_us;
    CHECK(ucn_v6_security_commit_join(
              device_manager, device_bootstrap, &bootstrap_key, 102U,
              &device_join) == UCN_V6_ERR_STATE);
    ++device_join.authority_local_lease_deadline_us;
    CHECK(device_store.submits == 1U);

    device_store.reenter_manager = device_manager;
    device_store.reenter_peer = authority;
    device_store.reenter_on_reserve = true;

    outbound = make_acl(&device, 7U, 3U, 8U, 4U,
                        UCN_V6_SECURITY_OUTBOUND);
    inbound = make_acl(&device, 7U, 3U, 8U, 4U,
                       UCN_V6_SECURITY_INBOUND);
    CHECK(ucn_v6_security_set_acl(device_manager, &outbound, &admin,
                                  admin_proof, sizeof(admin_proof)) == UCN_V6_OK);
    CHECK(device_store.reenter_result == UCN_V6_ERR_STATE);
    device_store.reenter_on_reserve = false;
    CHECK(ucn_v6_security_set_acl(authority_manager, &inbound, &admin,
                                  admin_proof, sizeof(admin_proof)) == UCN_V6_OK);

    frame = make_data_frame(payload, sizeof(payload));
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
    CHECK(ucn_v6_security_open_frame(
              authority_manager, 200U, 6U, &device, encoded, encoded_length,
              plaintext, sizeof(plaintext), &opened) == UCN_V6_OK);
    CHECK(opened.hop_authenticated && opened.endpoint_authorized &&
          !opened.group_discovery_only);
    CHECK(memcmp(plaintext, payload, sizeof(payload)) == 0);
    memset(&sentinel, 0xA5, sizeof(sentinel));
    opened = sentinel;
    CHECK(ucn_v6_security_open_frame(
              authority_manager, 201U, 6U, &device, encoded, encoded_length,
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
              authority_manager, 202U, 6U, &device, tampered, encoded_length,
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
                  authority_manager, 251U, 6U, &device, encoded,
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
                  authority_manager, 253U, 6U, &device, encoded,
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
        CHECK(ucn_v6_security_protect_peer_discovery(
                  device_manager, 260U, &authority, &hello, frame_work,
                  sizeof(frame_work), encoded, sizeof(encoded),
                  &encoded_length) == UCN_V6_OK);
        CHECK(hello.origin_sequence == 0U && hello.hop_sequence == 4U);
        CHECK(ucn_v6_security_open_frame(
                  authority_manager, 261U, 6U, &device, encoded,
                  encoded_length, NULL, 0U, &opened) == UCN_V6_OK);
        CHECK(opened.hop_authenticated && !opened.endpoint_authorized &&
              !opened.group_discovery_only);
        CHECK(memcmp(opened.authenticated_principal.bytes, device.bytes,
                     sizeof(device.bytes)) == 0);
        CHECK(opened.frame.payload_length == sizeof(hello_payload));
        CHECK(memcmp(opened.frame.payload, hello_payload,
                     sizeof(hello_payload)) == 0);
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
    CHECK(ucn_v6_security_commit_join(
              device_manager, device_bootstrap, &bootstrap_key, 300U,
              &device_join) == UCN_V6_OK);
    CHECK(ucn_v6_security_copy_view(device_manager, &view) == UCN_V6_OK);
    CHECK(view.admitted_sessions == 1U);
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
        ucn_v6_key_selector_t next_hop = { 1U, 2U, 4U };
        ucn_v6_key_selector_t next_e2e = { 2U, 4U, 6U };
        CHECK(ucn_v6_security_rotate_session_keys(
                  device_manager, 400U, &authority, &next_hop, &next_e2e,
                  1000U, &admin, admin_proof, sizeof(admin_proof)) ==
              UCN_V6_OK);
        CHECK(ucn_v6_security_revoke_session(
                  device_manager, &authority, &admin, admin_proof,
                  sizeof(admin_proof)) == UCN_V6_OK);
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
    uint8_t encoded_c[256U];
#if UCN_V6_CONFIG_SECURITY_SESSIONS >= 3U
    uint8_t encoded_d[256U];
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
              managers[0], 91U, 6U, &c, capability_encoded,
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
              capability_owner, 91U, 2U, 6U, &capability_opened,
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
    budget_request.session_generation = 1U;
    budget_request.destination_link_instance_generation = 6U;
    memcpy(budget_request.destination_capability_digest, capability_digest,
           sizeof(capability_digest));
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
        CHECK(ucn_v6_capability_path_reduce_begin(
                  &budget_request, &accumulator) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_hop(
                  &accumulator, &destination_capability) == UCN_V6_OK);
        CHECK(ucn_v6_capability_path_reduce_finalize(
                  &accumulator, &derived_path) == UCN_V6_OK);
    }
    CHECK(ucn_v6_capability_install_path(
              capability_owner, 92U, &derived_path) == UCN_V6_OK);
    memset(&pipeline_route_storage, 0, sizeof(pipeline_route_storage));
    CHECK(ucn_v6_route_owner_init_in_place(
              pipeline_route_storage.bytes, sizeof(pipeline_route_storage),
              ucn_v6_compiled_manifest(), 1000U, 100U, 3U, 200U, 500U,
              &route_owner) == UCN_V6_OK);
    memset(&route_domain, 0, sizeof(route_domain));
    route_domain.origin_principal = a;
    route_domain.origin_binding.realm_id = 1U;
    route_domain.origin_binding.node_address = 10U;
    route_domain.origin_binding.binding_generation = 1U;
    route_domain.origin_session_generation = 1U;
    route_domain.destination_principal = c;
    route_domain.destination_binding = budget_request.destination_binding;
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
    route_path.egress_link_id = 1U;
    route_path.egress_link_generation = 6U;
    route_path.hop_count = 2U;
    route_path.priority = 1U;
    route_path.weight = 1U;
    route_path.available = true;
    route_path.capability = derived_path;
    CHECK(ucn_v6_route_candidate_add_path(
              route_owner, capability_owner, 93U, 301U, &route_domain,
              &route_path) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_probe(
              route_owner, 94U, 301U, 1U, 1U) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_prepare_activation(
              route_owner, 95U, 301U, &activation) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_record_activation_send(
              route_owner, 95U, 301U, true) == UCN_V6_OK);
    CHECK(ucn_v6_route_candidate_commit_ack(
              route_owner, capability_owner, 96U, &activation) == UCN_V6_OK);
    memset(&select_request, 0, sizeof(select_request));
    select_request.domain = route_domain;
    select_request.flow_id = 201U;
    select_request.packet_sequence = 1U;
    select_request.policy = UCN_V6_ROUTE_POLICY_ACTIVE_STANDBY;
    CHECK(ucn_v6_route_select(
              route_owner, capability_owner, 97U, &select_request,
              &route_selection) == UCN_V6_OK);
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
                  managers[1], 108U, 6U, &a, &c, 5U,
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
                  managers[1], 109U, 6U, &a, &c, 5U,
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
              managers[1], 110U, 6U, &a, &c, 5U,
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
              managers[1], 111U, 6U, &a, encoded_d, encoded_d_length,
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
              managers[2], 120U, 6U, &b, delivered_frame, delivered_length,
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
              ucn_v6_compiled_manifest(), 100U, 4U, 1000U, 2000U,
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
    ucn_v6_callback_gate_t gate;
    ucn_v6_security_manager_storage_t storage;
    ucn_v6_security_manager_t *manager = NULL;
    ucn_v6_principal_t local = make_principal(0x20U);
    ucn_v6_principal_t admin = make_principal(0xA0U);
    ucn_v6_group_policy_slot_t group;

    memset(&store, 0, sizeof(store));
    memset(&crypto, 0, sizeof(crypto));
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
    CHECK(store.witness_present && !store.present);
    memset(&storage, 0, sizeof(storage));
    manager = NULL;
    store.reject_submit = false;
    CHECK(ucn_v6_security_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(), 1U,
              &local, &store_api, &crypto_api, &gate, &manager) ==
          UCN_V6_ERR_STATE);
    return 0;
}

static int test_group_fixed_slots_and_replay(void)
{
    static const uint8_t admin_proof[] = { UCN_V6_PROOF_REALM_ADMIN };
    fake_store_t store;
    fake_crypto_t crypto;
    ucn_v6_security_store_ops_t store_api;
    ucn_v6_security_crypto_ops_t crypto_api;
    ucn_v6_callback_gate_t gate;
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
    CHECK(ucn_v6_security_open_frame(
              manager, 20U, 0U, NULL, encoded, encoded_length,
              NULL, 0U, &opened) == UCN_V6_OK);
    CHECK(opened.group_discovery_only && !opened.endpoint_authorized);
    CHECK(ucn_v6_security_open_frame(
              manager, 21U, 0U, NULL, encoded, encoded_length,
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
              manager, 99U, 0U, NULL, encoded, encoded_length,
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
              manager, 100U, 0U, NULL, encoded, encoded_length,
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
                  managers[index], 100U + index, 6U,
                  &principals[index - 1U], &principals[index + 1U], 0U,
                  encoded[index - 1U], lengths[index - 1U], frame_work,
                  sizeof(frame_work), encoded[index], sizeof(encoded[index]),
                  &lengths[index], &verified, &relayed) == UCN_V6_OK);
        CHECK(verified.hop_authenticated &&
              !verified.endpoint_authorized &&
              relayed.origin_sequence == frame.origin_sequence &&
              relayed.hop_limit == (uint8_t)(4U - index) &&
              memcmp(relayed.e2e_tag, original_e2e_tag,
                     sizeof(original_e2e_tag)) == 0);
    }

    CHECK(ucn_v6_security_open_frame(
              managers[4], 110U, 6U, &principals[3], encoded[3], lengths[3],
              plaintext, sizeof(plaintext), &opened) == UCN_V6_OK);
    CHECK(opened.endpoint_authorized && opened.hop_authenticated &&
          memcmp(opened.authenticated_principal.bytes, principals[0].bytes,
                 sizeof(principals[0].bytes)) == 0 &&
          opened.frame.origin_sequence == frame.origin_sequence &&
          opened.frame.hop_sequence == 1U &&
          memcmp(plaintext, payload, sizeof(payload)) == 0);
    return 0;
}

int main(void)
{
    CHECK(test_join_acl_aead_replay() == 0);
    CHECK(test_independent_sequence_domains_and_verified_relay() == 0);
    CHECK(test_five_node_verified_relay_chain() == 0);
    CHECK(test_group_fixed_slots_and_replay() == 0);
    CHECK(test_witness_rollback_fails_closed() == 0);
    puts("ucn v6 security tests passed");
    return 0;
}
