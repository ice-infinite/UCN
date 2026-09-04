#include <stdio.h>
#include <string.h>

#include "ucn/v6/ucn_v6_bootstrap.h"
#include "ucn/v6/ucn_v6_config.h"

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,          \
                    __LINE__, #condition);                                      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

typedef struct fake_store {
    unsigned epoch_calls;
    unsigned binding_calls;
    unsigned group_calls;
    bool fail_next;
    bool reenter_init;
    ucn_v6_result_t reenter_result;
    void *reenter_storage;
    size_t reenter_storage_bytes;
    const ucn_v6_identity_store_ops_t *reenter_ops;
    ucn_v6_callback_gate_t *reenter_gate;
} fake_store_t;

static void fake_gate_lock(void *context)
{
    (void)context;
}

static void fake_gate_unlock(void *context)
{
    (void)context;
}

static ucn_v6_result_t fake_result(fake_store_t *store)
{
    if (store->fail_next) {
        store->fail_next = false;
        return UCN_V6_ERR_STATE;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t persist_epoch(
    void *context,
    const ucn_v6_authority_epoch_t *epoch)
{
    fake_store_t *store = (fake_store_t *)context;
    CHECK(epoch != NULL);
    ++store->epoch_calls;
    if (store->reenter_init) {
        ucn_v6_identity_authority_t *reentered = NULL;
        store->reenter_init = false;
        store->reenter_result = ucn_v6_identity_authority_init_in_place(
            store->reenter_storage, store->reenter_storage_bytes,
            ucn_v6_compiled_manifest(), 7U, store->reenter_ops,
            store->reenter_gate, &reentered);
    }
    return fake_result(store);
}

static ucn_v6_result_t persist_binding(
    void *context,
    const ucn_v6_binding_slot_t *slot)
{
    fake_store_t *store = (fake_store_t *)context;
    CHECK(slot != NULL);
    ++store->binding_calls;
    return fake_result(store);
}

static ucn_v6_result_t persist_group(void *context, uint32_t high_water)
{
    fake_store_t *store = (fake_store_t *)context;
    CHECK(high_water != 0U);
    ++store->group_calls;
    return fake_result(store);
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

static void fill_bytes(uint8_t *output, size_t length, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        output[index] = (uint8_t)(seed + index);
    }
}

static ucn_v6_authority_epoch_t authority_epoch(uint32_t generation)
{
    ucn_v6_authority_epoch_t epoch;
    memset(&epoch, 0, sizeof(epoch));
    epoch.authority_principal = principal((uint8_t)(0x20U + generation));
    epoch.authority_generation = generation;
    fill_bytes(epoch.durable_fence_token,
               sizeof(epoch.durable_fence_token),
               (uint8_t)(0x40U + generation));
    epoch.lease_sequence = generation;
    epoch.lease_duration_us = UINT64_C(1000000);
    fill_bytes(epoch.allocation_high_water_digest,
               sizeof(epoch.allocation_high_water_digest),
               (uint8_t)(0x60U + generation));
    epoch.quorum_proven = true;
    epoch.durable = true;
    return epoch;
}

static ucn_v6_bootstrap_transcript_t transcript(uint64_t txid)
{
    ucn_v6_bootstrap_transcript_t value;
    memset(&value, 0, sizeof(value));
    value.protocol_version = UCN_V6_PROTOCOL_VERSION;
    value.bootstrap_header_contract = 1U;
    value.flow = UCN_V6_BOOTSTRAP_FLOW_JOIN;
    value.joining_device_principal = principal(0x10U);
    value.joining_device_identity_digest = principal(0x30U);
    value.authority_principal = principal(0x50U);
    value.authority_generation = 1U;
    value.device_nonce = UINT64_C(11);
    value.authority_nonce = UINT64_C(12);
    value.transaction_id = txid;
    value.lease_freshness_challenge_nonce = UINT64_C(13);
    value.realm_id = UINT32_C(0x10203040);
    value.proposed_address = UINT32_C(7);
    value.address_binding_generation = UINT32_C(3);
    value.authority_address = UINT32_C(8);
    value.authority_binding_generation = UINT32_C(4);
    fill_bytes(value.binding_lease_id, sizeof(value.binding_lease_id), 0x70U);
    value.binding_lease_duration_us = UINT64_C(500000);
    value.authority_lease_sequence = UINT64_C(9);
    value.selected_hop_suite = 1U;
    value.selected_hop_key_id = 2U;
    value.selected_hop_key_generation = UINT32_C(3);
    value.selected_e2e_mode = UCN_V6_E2E_AEAD;
    value.selected_e2e_suite = 3U;
    value.selected_e2e_key_id = 4U;
    value.selected_e2e_key_generation = UINT32_C(5);
    value.selected_session_generation = UINT32_C(1);
    value.selected_link_instance_generation = UINT32_C(6);
    fill_bytes(value.prior_messages_hash,
               sizeof(value.prior_messages_hash), 0x90U);
    return value;
}

static ucn_v6_bootstrap_key_t bootstrap_key(
    const ucn_v6_bootstrap_transcript_t *value,
    uint32_t link_generation,
    uint32_t discriminator)
{
    ucn_v6_bootstrap_key_t key;
    memset(&key, 0, sizeof(key));
    key.ingress_link_generation = link_generation;
    key.local_peer_discriminator = discriminator;
    key.identity_digest = value->joining_device_identity_digest;
    key.transaction_id = value->transaction_id;
    return key;
}

static int test_identity_and_deadline_contract(void)
{
    ucn_v6_principal_t zero;
    ucn_v6_principal_t valid = principal(1U);
    ucn_v6_binding_key_t binding = { 1U, 2U, 3U };
    ucn_v6_lease_verifier_policy_t policy;
    uint32_t next = UINT32_MAX;
    uint64_t deadline = UINT64_MAX;

    memset(&zero, 0, sizeof(zero));
    CHECK(!ucn_v6_principal_is_valid(&zero));
    CHECK(ucn_v6_principal_is_valid(&valid));
    CHECK(ucn_v6_binding_key_is_valid(&binding));
    CHECK(ucn_v6_serial_checked_next(0U, &next) == UCN_V6_OK);
    CHECK(next == 1U);
    CHECK(ucn_v6_serial_checked_next(UCN_V6_SERIAL_ROTATION_THRESHOLD,
                                     &next) == UCN_V6_ERR_EXHAUSTED);

    memset(&policy, 0, sizeof(policy));
    policy.local_timer_resolution_us = UINT64_C(10000);
    policy.timer_read_uncertainty_known = true;
    policy.local_policy_max_lease_us = UINT64_C(100000);
    CHECK(ucn_v6_lease_deadline_build(UINT64_C(1000), UINT64_C(15000),
                                      &policy, &deadline) == UCN_V6_OK);
    CHECK(deadline == UINT64_C(6000));
    CHECK(ucn_v6_lease_deadline_is_live(UINT64_C(5999), deadline));
    CHECK(!ucn_v6_lease_deadline_is_live(deadline, deadline));
    policy.local_timer_resolution_us = 0U;
    CHECK(ucn_v6_lease_deadline_build(0U, UINT64_C(15000), &policy,
                                      &deadline) == UCN_V6_ERR_ARGUMENT);
    return 0;
}

static int test_authority_persist_before_publish(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_identity_authority_storage_t authority_storage;
    ucn_v6_identity_authority_t *authority = NULL;
    ucn_v6_identity_authority_view_t authority_view;
    ucn_v6_callback_gate_t gate;
    ucn_v6_authority_epoch_t epoch;
    ucn_v6_binding_certificate_t certificate;
    ucn_v6_binding_certificate_t certificate_before;
    ucn_v6_principal_t device = principal(0xA0U);
    uint8_t lease_id[16];
    uint32_t group_id = 0U;

    memset(&store, 0, sizeof(store));
    memset(&ops, 0, sizeof(ops));
    ops.context = &store;
    ops.persist_authority_epoch = persist_epoch;
    ops.persist_binding_slot = persist_binding;
    ops.persist_group_high_water = persist_group;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_init_in_place(
              authority_storage.bytes, sizeof(authority_storage),
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &authority) == UCN_V6_OK);
    epoch = authority_epoch(1U);
    store.reenter_init = true;
    store.reenter_storage = authority_storage.bytes;
    store.reenter_storage_bytes = sizeof(authority_storage);
    store.reenter_ops = &ops;
    store.reenter_gate = &gate;
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, UINT64_C(1000000)) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_copy_view(
              authority, &authority_view) == UCN_V6_OK);
    CHECK(store.epoch_calls == 1U && authority_view.epoch_valid);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);

    fill_bytes(lease_id, sizeof(lease_id), 0xC0U);
    memset(&certificate, 0xA5, sizeof(certificate));
    certificate_before = certificate;
    store.fail_next = true;
    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 10U, 9U, &device, UCN_V6_ADDRESS_LEASED,
              lease_id, UINT64_C(500000), &certificate) == UCN_V6_ERR_STATE);
    CHECK(memcmp(&certificate, &certificate_before, sizeof(certificate)) == 0);
    CHECK(ucn_v6_identity_authority_copy_view(
              authority, &authority_view) == UCN_V6_OK);
    CHECK(authority_view.faulted);
    CHECK(authority_view.occupied_bindings == 0U);

    memset(&store, 0, sizeof(store));
    ops.context = &store;
    CHECK(ucn_v6_identity_authority_init_in_place(
              authority_storage.bytes, sizeof(authority_storage),
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &authority) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, UINT64_C(1000000)) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 10U, 9U, &device, UCN_V6_ADDRESS_SELF_PROPOSED,
              lease_id, UINT64_C(500000), &certificate) == UCN_V6_OK);
    CHECK(certificate.binding.realm_id == 7U);
    CHECK(certificate.binding.binding_generation == 1U);
    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 11U, 9U, &device, UCN_V6_ADDRESS_LEASED,
              lease_id, UINT64_C(500000), &certificate) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_identity_authority_retire_binding(
              authority, 12U, 9U, 1U) == UCN_V6_OK);
    lease_id[0] ^= 0x55U;
    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 13U, 9U, &device, UCN_V6_ADDRESS_LEASED,
              lease_id, UINT64_C(500000), &certificate) == UCN_V6_OK);
    CHECK(certificate.binding.binding_generation == 2U);

    CHECK(ucn_v6_identity_authority_allocate_dynamic_group(
              authority, 14U, &group_id) == UCN_V6_OK);
    CHECK(group_id == 1U);
    CHECK(ucn_v6_identity_authority_retire_dynamic_group(
              authority, group_id) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_allocate_dynamic_group(
              authority, 15U, &group_id) == UCN_V6_OK);
    CHECK(group_id == 2U);
    return 0;
}

static int test_opaque_storage_preflight_and_corruption(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_callback_gate_t gate;
    ucn_v6_identity_authority_storage_t authority_storage;
    ucn_v6_identity_authority_storage_t authority_before;
    ucn_v6_bootstrap_owner_storage_t bootstrap_storage;
    ucn_v6_bootstrap_owner_storage_t bootstrap_before;
    ucn_v6_feature_manifest_t bad_manifest = *ucn_v6_compiled_manifest();
    ucn_v6_identity_authority_t *authority = NULL;
    ucn_v6_bootstrap_owner_t *owner = NULL;
    ucn_v6_identity_authority_view_t view;
    ucn_v6_identity_authority_view_t view_before;
    ucn_v6_bootstrap_config_t config = {
        4U, 2U, 4U, 2U, UINT64_C(3000000)
    };

    memset(&store, 0, sizeof(store));
    memset(&ops, 0, sizeof(ops));
    ops.context = &store;
    ops.persist_authority_epoch = persist_epoch;
    ops.persist_binding_slot = persist_binding;
    ops.persist_group_high_water = persist_group;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    bad_manifest.layout_hash ^= UINT64_C(1);
    memset(&authority_storage, 0xA5, sizeof(authority_storage));
    authority_before = authority_storage;
    CHECK(ucn_v6_identity_authority_init_in_place(
              authority_storage.bytes, sizeof(authority_storage),
              &bad_manifest, 7U, &ops, &gate,
              &authority) == UCN_V6_ERR_CONFIG);
    CHECK(authority == NULL);
    CHECK(memcmp(&authority_storage, &authority_before,
                 sizeof(authority_storage)) == 0);
    CHECK(ucn_v6_identity_authority_init_in_place(
              authority_storage.bytes + 1U, sizeof(authority_storage) - 1U,
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &authority) == UCN_V6_ERR_CONFIG);
    CHECK(memcmp(&authority_storage, &authority_before,
                 sizeof(authority_storage)) == 0);
    CHECK(ucn_v6_identity_authority_init_in_place(
              authority_storage.bytes, sizeof(authority_storage),
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &authority) == UCN_V6_OK);
    memset(&view, 0x3C, sizeof(view));
    view_before = view;
    authority_storage.bytes[0] ^= 1U;
    CHECK(ucn_v6_identity_authority_copy_view(
              authority, &view) == UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(&view, &view_before, sizeof(view)) == 0);

    memset(&bootstrap_storage, 0x5A, sizeof(bootstrap_storage));
    bootstrap_before = bootstrap_storage;
    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              bootstrap_storage.bytes, sizeof(bootstrap_storage),
              &bad_manifest, &config, &owner) == UCN_V6_ERR_CONFIG);
    CHECK(owner == NULL);
    CHECK(memcmp(&bootstrap_storage, &bootstrap_before,
                 sizeof(bootstrap_storage)) == 0);
    return 0;
}

static int test_bootstrap_resource_and_state_contract(void)
{
    ucn_v6_bootstrap_owner_storage_t owner_storage;
    ucn_v6_bootstrap_owner_t *owner = NULL;
    ucn_v6_bootstrap_config_t config = {
        4U, 2U, 4U, 2U, UINT64_C(3000000)
    };
    ucn_v6_bootstrap_transcript_t value = transcript(UINT64_C(100));
    ucn_v6_bootstrap_transcript_t conflict;
    ucn_v6_bootstrap_key_t key = bootstrap_key(&value, 1U, 1U);
    ucn_v6_bootstrap_pending_t pending;
    ucn_v6_bootstrap_pending_t before;
    ucn_v6_binding_key_t binding = { UINT32_C(0x10203040), 7U, 3U };

    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              owner_storage.bytes, sizeof(owner_storage),
              ucn_v6_compiled_manifest(), &config, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 0U, 32U, 33U) == UCN_V6_ERR_ARGUMENT);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 0U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 2U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 3U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 4U, 32U, 32U) == UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, UINT64_C(1000000), 32U, 32U) == UCN_V6_OK);

    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              NULL, false, 10U) == UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
              &pending) == UCN_V6_ERR_NOT_FOUND);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              NULL, true, 10U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
              &pending) == UCN_V6_OK);
    CHECK(pending.deadline_us == UINT64_C(3000010));
    before = pending;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              NULL, true, 1000U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
              &pending) == UCN_V6_OK);
    CHECK(memcmp(&pending, &before, sizeof(pending)) == 0);

    conflict = value;
    conflict.proposed_address = 8U;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &conflict,
              NULL, true, before.deadline_us) == UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
              &pending) == UCN_V6_OK);
    CHECK(memcmp(&pending, &before, sizeof(pending)) == 0);

    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF, true,
              20U) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF, false,
              20U) == UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF, true, 20U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF, true, 21U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER, true, 22U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT, true, 23U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE, true, 24U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
              &pending) == UCN_V6_OK);
    CHECK(pending.phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE);
    CHECK(ucn_v6_bootstrap_expire(owner, pending.deadline_us - 1U) == 0U);
    CHECK(ucn_v6_bootstrap_expire(owner, pending.deadline_us) == 1U);

    value = transcript(UINT64_C(200));
    key = bootstrap_key(&value, 2U, 1U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &value,
              NULL, true, 30U) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &value,
              &binding, true, 30U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key,
              &pending) == UCN_V6_OK);
    CHECK(pending.existing_binding.binding_generation == 3U);
    CHECK(ucn_v6_bootstrap_expire(owner, pending.deadline_us - 1U) == 0U);
    CHECK(ucn_v6_bootstrap_expire(owner, pending.deadline_us) == 1U);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key,
              &pending) == UCN_V6_ERR_NOT_FOUND);
    return 0;
}

static int test_bootstrap_cross_flow_capacity_contract(void)
{
    ucn_v6_bootstrap_owner_storage_t owner_storage;
    ucn_v6_bootstrap_owner_t *owner = NULL;
    ucn_v6_bootstrap_config_t config = {
        4U, 2U, 4U, 2U, UINT64_C(3000000)
    };
    ucn_v6_bootstrap_transcript_t join_a = transcript(UINT64_C(301));
    ucn_v6_bootstrap_transcript_t reauth_a = transcript(UINT64_C(302));
    ucn_v6_bootstrap_transcript_t reauth_b = transcript(UINT64_C(303));
    ucn_v6_bootstrap_transcript_t join_b = transcript(UINT64_C(304));
    ucn_v6_bootstrap_transcript_t join_c = transcript(UINT64_C(305));
    ucn_v6_bootstrap_transcript_t join_d = transcript(UINT64_C(306));
    ucn_v6_bootstrap_transcript_t join_e = transcript(UINT64_C(307));
    ucn_v6_bootstrap_key_t key;
    ucn_v6_binding_key_t binding = { UINT32_C(0x10203040), 7U, 3U };

    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              owner_storage.bytes, sizeof(owner_storage),
              ucn_v6_compiled_manifest(), &config, &owner) == UCN_V6_OK);
    key = bootstrap_key(&join_a, 1U, 1U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &join_a,
              NULL, true, 0U) == UCN_V6_OK);

    /* JOIN and REAUTH use separate tables but share the unauthenticated-peer
     * quota. / JOIN 与 REAUTH 分表，但共享未认证 Peer 配额。 */
    key = bootstrap_key(&reauth_a, 1U, 1U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &reauth_a,
              &binding, true, 1U) == UCN_V6_ERR_NO_SPACE);

    key = bootstrap_key(&reauth_b, 1U, 2U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &reauth_b,
              &binding, true, 2U) == UCN_V6_OK);
    key = bootstrap_key(&join_b, 1U, 3U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &join_b,
              NULL, true, 3U) == UCN_V6_ERR_NO_SPACE);

    key = bootstrap_key(&join_c, 2U, 3U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &join_c,
              NULL, true, 4U) == UCN_V6_OK);
    key = bootstrap_key(&join_d, 3U, 4U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &join_d,
              NULL, true, 5U) == UCN_V6_OK);
    key = bootstrap_key(&join_e, 4U, 5U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &join_e,
              NULL, true, 6U) == UCN_V6_ERR_NO_SPACE);
    return 0;
}

int main(void)
{
    CHECK(test_identity_and_deadline_contract() == 0);
    CHECK(test_authority_persist_before_publish() == 0);
    CHECK(test_opaque_storage_preflight_and_corruption() == 0);
    CHECK(test_bootstrap_resource_and_state_contract() == 0);
    CHECK(test_bootstrap_cross_flow_capacity_contract() == 0);
    puts("ucn v6 identity/bootstrap tests passed");
    return 0;
}
