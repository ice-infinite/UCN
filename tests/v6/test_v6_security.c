#include "ucn/v6/ucn_v6_security.h"

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

static ucn_v6_bootstrap_transcript_t make_transcript(
    const ucn_v6_principal_t *device,
    const ucn_v6_principal_t *authority)
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
    value.device_nonce = 11U;
    value.authority_nonce = 12U;
    value.transaction_id = 13U;
    value.lease_freshness_challenge_nonce = 14U;
    value.realm_id = 1U;
    value.proposed_address = 7U;
    value.address_binding_generation = 3U;
    value.authority_address = 8U;
    value.authority_binding_generation = 4U;
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
    value.selected_link_instance_generation = 6U;
    fill_bytes(value.prior_messages_hash, sizeof(value.prior_messages_hash),
               0x70U);
    return value;
}

static ucn_v6_join_commit_t make_join(
    const ucn_v6_bootstrap_transcript_t *transcript,
    bool device_side)
{
    static const uint8_t device_proof[] = { UCN_V6_PROOF_JOINING_DEVICE };
    static const uint8_t authority_proof[] = { UCN_V6_PROOF_ADDRESS_AUTHORITY };
    ucn_v6_join_commit_t commit;
    ucn_v6_binding_key_t device_binding = { 1U, 7U, 3U };
    ucn_v6_binding_key_t authority_binding = { 1U, 8U, 4U };
    memset(&commit, 0, sizeof(commit));
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
    commit.session_generation = 1U;
    commit.link_instance_generation = 6U;
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
    CHECK(frame.packet_sequence == 1U);
    CHECK(memcmp(cipher, payload, sizeof(payload)) != 0);
    memset(&opened, 0, sizeof(opened));
    CHECK(ucn_v6_security_open_frame(
              authority_manager, 200U, 6U, &device, encoded, encoded_length,
              plaintext, sizeof(plaintext), &opened) == UCN_V6_OK);
    CHECK(opened.endpoint_authorized && !opened.group_discovery_only);
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
        control.packet_sequence = 0U;
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
        CHECK(decoded.packet_sequence == 1U && decoded.group.group_id == 41U);
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
    frame.packet_sequence = 1U;
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
    frame.packet_sequence = 2U;
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
    frame.packet_sequence = 3U;
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

int main(void)
{
    CHECK(test_join_acl_aead_replay() == 0);
    CHECK(test_group_fixed_slots_and_replay() == 0);
    CHECK(test_witness_rollback_fails_closed() == 0);
    puts("ucn v6 security tests passed");
    return 0;
}
