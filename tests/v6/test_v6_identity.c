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

typedef struct fake_authority_verifier {
    ucn_v6_result_t forced_result;
    bool return_invalid_result;
    bool reenter_install;
    ucn_v6_identity_authority_t *authority;
    ucn_v6_result_t reenter_result;
    unsigned calls;
} fake_authority_verifier_t;

static fake_authority_verifier_t authority_verifier_context;

static ucn_v6_identity_authority_verifier_ops_t authority_verifier_ops(void);

typedef struct fake_store {
    ucn_v6_identity_snapshot_t snapshot;
    ucn_v6_durable_generation_witness_t witness;
    unsigned witness_load_calls;
    unsigned witness_reserve_calls;
    unsigned load_calls;
    unsigned submit_calls;
    bool present;
    bool witness_present;
    bool fail_next;
    bool fail_submit_once;
    unsigned fail_witness_reserve_at;
    unsigned fail_witness_after_write_at;
    bool fail_submit_after_write_once;
    bool corrupt_reload;
    bool perturb_padding;
    bool invalid_witness_load_result;
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

static ucn_v6_result_t load_witness(
    void *context,
    ucn_v6_durable_generation_witness_t *witness)
{
    fake_store_t *store = (fake_store_t *)context;
    CHECK(witness != NULL);
    ++store->witness_load_calls;
    if (store->invalid_witness_load_result) {
        return (ucn_v6_result_t)1;
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
    ++store->witness_reserve_calls;
    if (store->reenter_init) {
        ucn_v6_identity_authority_t *reentered = NULL;
        ucn_v6_identity_authority_verifier_ops_t verifier =
            authority_verifier_ops();
        store->reenter_init = false;
        store->reenter_result = ucn_v6_identity_authority_init_in_place(
            store->reenter_storage, store->reenter_storage_bytes,
            ucn_v6_compiled_manifest(), 7U, &verifier,
            store->reenter_ops,
            store->reenter_gate, &reentered);
    }
    if (fake_result(store) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    if (store->fail_witness_reserve_at != 0U &&
        store->witness_reserve_calls == store->fail_witness_reserve_at) {
        return UCN_V6_ERR_STATE;
    }
    if (witness == NULL || witness->witness_generation == 0U ||
        (store->witness_present && witness->witness_generation <=
             store->witness.witness_generation)) {
        return UCN_V6_ERR_REPLAY;
    }
    store->witness = *witness;
    store->witness_present = true;
    if (store->fail_witness_after_write_at != 0U &&
        store->witness_reserve_calls ==
            store->fail_witness_after_write_at) {
        return UCN_V6_ERR_STATE;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t load_snapshot(
    void *context,
    ucn_v6_identity_snapshot_t *snapshot)
{
    fake_store_t *store = (fake_store_t *)context;
    CHECK(snapshot != NULL);
    ++store->load_calls;
    if (!store->present) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *snapshot = store->snapshot;
    if (store->perturb_padding) {
        const size_t padding_start =
            offsetof(ucn_v6_identity_snapshot_t, epoch_valid) +
            sizeof(snapshot->epoch_valid);
        const size_t padding_end =
            offsetof(ucn_v6_identity_snapshot_t, epoch);
        if (padding_end > padding_start) {
            ((uint8_t *)snapshot)[padding_start] ^= UINT8_C(0xA5);
        }
    }
    if (store->corrupt_reload) {
        snapshot->record_generation ^= UINT64_C(1);
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t submit_snapshot(
    void *context,
    const ucn_v6_identity_snapshot_t *snapshot)
{
    fake_store_t *store = (fake_store_t *)context;
    CHECK(snapshot != NULL);
    ++store->submit_calls;
    if (store->fail_submit_once) {
        store->fail_submit_once = false;
        return UCN_V6_ERR_STATE;
    }
    if (fake_result(store) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    store->snapshot = *snapshot;
    store->present = true;
    if (store->fail_submit_after_write_once) {
        store->fail_submit_after_write_once = false;
        return UCN_V6_ERR_STATE;
    }
    return UCN_V6_OK;
}

static ucn_v6_identity_store_ops_t store_ops(fake_store_t *store)
{
    ucn_v6_identity_store_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = store;
    ops.load_witness = load_witness;
    ops.reserve_witness = reserve_witness;
    ops.load = load_snapshot;
    ops.submit = submit_snapshot;
    return ops;
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

static uint64_t authority_canonical_hash(
    const uint8_t *bytes,
    size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static ucn_v6_authority_proof_t authority_proof_from_canonical(
    const uint8_t *canonical,
    size_t canonical_bytes)
{
    ucn_v6_authority_proof_t proof;
    uint64_t hash = authority_canonical_hash(canonical, canonical_bytes);
    size_t index;

    memset(&proof, 0, sizeof(proof));
    proof.length = 16U;
    for (index = 0U; index < 8U; ++index) {
        proof.bytes[index] = (uint8_t)(hash >> (56U - index * 8U));
        proof.bytes[index + 8U] = (uint8_t)~proof.bytes[index];
    }
    return proof;
}

static ucn_v6_result_t verify_authority_epoch_transition(
    void *context,
    const ucn_v6_authority_transition_request_t *request,
    const uint8_t *canonical_transition,
    size_t canonical_transition_bytes,
    const ucn_v6_authority_proof_t *proof)
{
    fake_authority_verifier_t *verifier =
        (fake_authority_verifier_t *)context;
    ucn_v6_authority_proof_t expected;

    ++verifier->calls;
    if (verifier->reenter_install) {
        verifier->reenter_install = false;
        verifier->reenter_result = ucn_v6_identity_authority_install_epoch(
            verifier->authority, &request->proposed_epoch,
            &request->freshness, request->challenge_started_local_us,
            request->challenge_started_local_us,
            &request->lease_policy, proof);
    }
    if (verifier->return_invalid_result) {
        return (ucn_v6_result_t)1;
    }
    if (verifier->forced_result != UCN_V6_OK) {
        return verifier->forced_result;
    }
    if (canonical_transition == NULL ||
        canonical_transition_bytes !=
            UCN_V6_AUTHORITY_TRANSITION_CANONICAL_BYTES ||
        proof == NULL) {
        return UCN_V6_ERR_SECURITY;
    }
    expected = authority_proof_from_canonical(
        canonical_transition, canonical_transition_bytes);
    return proof->length == expected.length &&
                   memcmp(proof->bytes, expected.bytes,
                          UCN_V6_AUTHORITY_PROOF_MAX_BYTES) == 0 ?
               UCN_V6_OK : UCN_V6_ERR_SECURITY;
}

static ucn_v6_identity_authority_verifier_ops_t authority_verifier_ops(void)
{
    ucn_v6_identity_authority_verifier_ops_t verifier;
    memset(&verifier, 0, sizeof(verifier));
    verifier.context = &authority_verifier_context;
    verifier.verify_epoch_transition = verify_authority_epoch_transition;
    return verifier;
}

typedef struct fake_bootstrap_verifier {
    ucn_v6_result_t forced_result;
    bool return_invalid_result;
    bool reenter;
    ucn_v6_bootstrap_owner_t *owner;
    ucn_v6_bootstrap_flow_t flow;
    ucn_v6_bootstrap_key_t key;
    ucn_v6_bootstrap_transcript_t transcript;
    ucn_v6_result_t reenter_result;
} fake_bootstrap_verifier_t;

static fake_bootstrap_verifier_t bootstrap_verifier_context;
static ucn_v6_callback_gate_t bootstrap_verifier_gate = {0};
static bool bootstrap_verifier_gate_ready;

static ucn_v6_bootstrap_evidence_t bootstrap_evidence(
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding)
{
    ucn_v6_bootstrap_evidence_t evidence;
    memset(&evidence, 0, sizeof(evidence));
    evidence.length = 24U;
    evidence.bytes[0] = (uint8_t)event;
    evidence.bytes[1] = (uint8_t)flow;
    evidence.bytes[2] = (uint8_t)(key->ingress_link_id >> 8U);
    evidence.bytes[3] = (uint8_t)key->ingress_link_id;
    evidence.bytes[4] = (uint8_t)(key->ingress_link_generation >> 24U);
    evidence.bytes[5] = (uint8_t)(key->ingress_link_generation >> 16U);
    evidence.bytes[6] = (uint8_t)(key->ingress_link_generation >> 8U);
    evidence.bytes[7] = (uint8_t)key->ingress_link_generation;
    evidence.bytes[8] = (uint8_t)(key->transaction_id >> 56U);
    evidence.bytes[9] = (uint8_t)(key->transaction_id >> 48U);
    evidence.bytes[10] = (uint8_t)(key->transaction_id >> 40U);
    evidence.bytes[11] = (uint8_t)(key->transaction_id >> 32U);
    evidence.bytes[12] = (uint8_t)(key->transaction_id >> 24U);
    evidence.bytes[13] = (uint8_t)(key->transaction_id >> 16U);
    evidence.bytes[14] = (uint8_t)(key->transaction_id >> 8U);
    evidence.bytes[15] = (uint8_t)key->transaction_id;
    evidence.bytes[16] = (uint8_t)(transcript->authority_generation >> 24U);
    evidence.bytes[17] = (uint8_t)(transcript->authority_generation >> 16U);
    evidence.bytes[18] = (uint8_t)(transcript->authority_generation >> 8U);
    evidence.bytes[19] = (uint8_t)transcript->authority_generation;
    evidence.bytes[20] = (uint8_t)(
        transcript->address_binding_generation >> 24U);
    evidence.bytes[21] = (uint8_t)(
        transcript->address_binding_generation >> 16U);
    evidence.bytes[22] = existing_binding == NULL ? 0U :
        (uint8_t)(existing_binding->binding_generation >> 8U);
    evidence.bytes[23] = existing_binding == NULL ? 0U :
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
    fake_bootstrap_verifier_t *verifier =
        (fake_bootstrap_verifier_t *)context;
    ucn_v6_bootstrap_evidence_t expected = bootstrap_evidence(
        event, flow, key, transcript, existing_binding);
    (void)now_us;

    if (verifier->reenter) {
        verifier->reenter = false;
        verifier->reenter_result = ucn_v6_bootstrap_copy_pending(
            verifier->owner, verifier->flow, &verifier->key,
            &(ucn_v6_bootstrap_pending_t){0});
    }
    if (verifier->return_invalid_result) {
        return (ucn_v6_result_t)1;
    }
    if (verifier->forced_result != UCN_V6_OK) {
        return verifier->forced_result;
    }
    return evidence != NULL &&
                   memcmp(evidence, &expected, sizeof(expected)) == 0 ?
               UCN_V6_OK : UCN_V6_ERR_SECURITY;
}

static ucn_v6_bootstrap_verifier_ops_t bootstrap_verifier_ops(void)
{
    ucn_v6_bootstrap_verifier_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = &bootstrap_verifier_context;
    ops.authorize_event = authorize_bootstrap_event;
    return ops;
}

static ucn_v6_result_t ensure_bootstrap_verifier_gate(void)
{
    if (!bootstrap_verifier_gate_ready) {
        ucn_v6_result_t result = ucn_v6_callback_gate_init(
            &bootstrap_verifier_gate, NULL, fake_gate_lock,
            fake_gate_unlock);
        if (result != UCN_V6_OK) {
            return result;
        }
        bootstrap_verifier_gate_ready = true;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t bootstrap_owner_init_for_test(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_bootstrap_config_t *config,
    ucn_v6_bootstrap_owner_t **owner)
{
    ucn_v6_bootstrap_verifier_ops_t verifier = bootstrap_verifier_ops();
    if (ensure_bootstrap_verifier_gate() != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    return ucn_v6_bootstrap_owner_init_in_place(
        storage, storage_bytes, manifest, config, &verifier,
        &bootstrap_verifier_gate, owner);
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

static ucn_v6_authority_epoch_t authority_epoch(uint32_t generation)
{
    ucn_v6_authority_epoch_t epoch;
    memset(&epoch, 0, sizeof(epoch));
    epoch.realm_id = 7U;
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
    fill_bytes(epoch.quorum_config_digest,
               sizeof(epoch.quorum_config_digest),
               (uint8_t)(0x70U + generation));
    fill_bytes(epoch.signer_set_digest, sizeof(epoch.signer_set_digest),
               (uint8_t)(0x80U + generation));
    fill_bytes(epoch.threshold_proof_digest,
               sizeof(epoch.threshold_proof_digest),
               (uint8_t)(0x90U + generation));
    epoch.signer_count = 3U;
    epoch.quorum_threshold = 2U;
    return epoch;
}

static ucn_v6_authority_freshness_t authority_freshness(
    const ucn_v6_authority_epoch_t *epoch,
    uint64_t transaction_id,
    uint64_t challenge_nonce,
    uint64_t max_remaining_us)
{
    ucn_v6_authority_freshness_t value;
    memset(&value, 0, sizeof(value));
    value.verifier_device_principal = epoch->authority_principal;
    value.challenge_nonce = challenge_nonce;
    value.transaction_id = transaction_id;
    value.authority_lease_sequence = epoch->lease_sequence;
    value.max_remaining_lease_us = max_remaining_us;
    fill_bytes(value.proof_transcript_hash,
               sizeof(value.proof_transcript_hash), 0xB0U);
    return value;
}

static bool authority_owner_fields_equal_for_test(
    const ucn_v6_authority_epoch_t *left,
    const ucn_v6_authority_epoch_t *right)
{
    return left->realm_id == right->realm_id &&
           left->authority_generation == right->authority_generation &&
           memcmp(left->authority_principal.bytes,
                  right->authority_principal.bytes,
                  sizeof(left->authority_principal.bytes)) == 0 &&
           memcmp(left->durable_fence_token,
                  right->durable_fence_token,
                  sizeof(left->durable_fence_token)) == 0;
}

static ucn_v6_result_t identity_authority_init_for_test(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    uint32_t realm_id,
    const ucn_v6_identity_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_identity_authority_t **authority)
{
    ucn_v6_identity_authority_verifier_ops_t verifier =
        authority_verifier_ops();
    ucn_v6_result_t result = ucn_v6_identity_authority_init_in_place(
        storage, storage_bytes, manifest, realm_id, &verifier, store,
        callback_gate, authority);

    if (result == UCN_V6_OK && authority != NULL) {
        authority_verifier_context.authority = *authority;
    }
    return result;
}

static ucn_v6_result_t authority_proof_for_install(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    const ucn_v6_authority_freshness_t *freshness,
    uint64_t challenge_started_local_us,
    const ucn_v6_lease_verifier_policy_t *lease_policy,
    ucn_v6_authority_transition_request_t *request_out,
    ucn_v6_authority_proof_t *proof_out)
{
    ucn_v6_identity_authority_view_t view;
    ucn_v6_authority_transition_request_t request;
    uint8_t canonical[UCN_V6_AUTHORITY_TRANSITION_CANONICAL_BYTES];
    uint64_t deadline = 0U;
    size_t canonical_bytes = 0U;

    if (proof_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(proof_out, 0, sizeof(*proof_out));
    proof_out->length = 1U;
    proof_out->bytes[0] = 1U;
    memset(&request, 0, sizeof(request));
    if (authority != NULL && epoch != NULL && freshness != NULL &&
        lease_policy != NULL &&
        ucn_v6_identity_authority_copy_view(authority, &view) == UCN_V6_OK &&
        ucn_v6_lease_deadline_build(
            challenge_started_local_us, freshness->max_remaining_lease_us,
            lease_policy, &deadline) == UCN_V6_OK) {
        request.realm_id = epoch->realm_id;
        request.committed_epoch_valid = view.epoch_valid;
        if (view.epoch_valid) {
            request.committed_epoch = view.epoch;
        }
        request.proposed_epoch = *epoch;
        request.freshness = *freshness;
        request.challenge_started_local_us = challenge_started_local_us;
        request.lease_policy = *lease_policy;
        request.derived_local_deadline_us = deadline;
        if (!view.epoch_valid) {
            request.kind = UCN_V6_AUTHORITY_TRANSITION_INITIAL;
        } else if (view.epoch.lease_sequence == epoch->lease_sequence) {
            request.kind = UCN_V6_AUTHORITY_TRANSITION_FRESHNESS;
        } else if (authority_owner_fields_equal_for_test(&view.epoch,
                                                         epoch)) {
            request.kind = UCN_V6_AUTHORITY_TRANSITION_RENEWAL;
        } else {
            request.kind = UCN_V6_AUTHORITY_TRANSITION_TRANSFER;
        }
        if (ucn_v6_authority_transition_encode_canonical(
                &request, canonical, sizeof(canonical),
                &canonical_bytes) == UCN_V6_OK) {
            *proof_out = authority_proof_from_canonical(canonical,
                                                        canonical_bytes);
            if (request_out != NULL) {
                *request_out = request;
            }
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_ARGUMENT;
}

static ucn_v6_result_t identity_install_epoch_for_test(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    const ucn_v6_authority_freshness_t *freshness,
    uint64_t challenge_started_local_us,
    const ucn_v6_lease_verifier_policy_t *lease_policy)
{
    ucn_v6_authority_proof_t proof;

    (void)authority_proof_for_install(
        authority, epoch, freshness, challenge_started_local_us,
        lease_policy, NULL, &proof);
    return ucn_v6_identity_authority_install_epoch(
        authority, epoch, freshness, challenge_started_local_us,
        challenge_started_local_us,
        lease_policy, &proof);
}

#define ucn_v6_identity_authority_init_in_place \
    identity_authority_init_for_test
#define ucn_v6_identity_authority_install_epoch \
    identity_install_epoch_for_test

static ucn_v6_result_t install_epoch(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    uint64_t challenge_started_us,
    uint64_t transaction_id)
{
    ucn_v6_authority_freshness_t freshness = authority_freshness(
        epoch, transaction_id, transaction_id + 100U, UINT64_C(900000));
    ucn_v6_lease_verifier_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.local_timer_resolution_us = 1U;
    policy.timer_read_uncertainty_known = true;
    policy.local_policy_max_lease_us = UINT64_C(900000);
    return ucn_v6_identity_authority_install_epoch(
        authority, epoch, &freshness, challenge_started_us, &policy);
}

static const ucn_v6_binding_slot_t *snapshot_binding(
    const ucn_v6_identity_snapshot_t *snapshot,
    uint32_t node_address)
{
    size_t index;

    for (index = 0U; index < UCN_V6_MAX_BINDING_SLOTS; ++index) {
        if (snapshot->bindings[index].occupied &&
            snapshot->bindings[index].node_address == node_address) {
            return &snapshot->bindings[index];
        }
    }
    return NULL;
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
    value.selected_link_instance_id = 1U;
    fill_bytes(value.binding_lease_id, sizeof(value.binding_lease_id), 0x70U);
    value.binding_lease_duration_us = UINT64_C(500000);
    value.authority_lease_sequence = UINT64_C(9);
    value.authority_lease_duration_us = UINT64_C(1000000);
    value.freshness_max_remaining_lease_us = UINT64_C(400000);
    fill_bytes(value.durable_fence_token,
               sizeof(value.durable_fence_token), 0xA0U);
    fill_bytes(value.allocation_high_water_digest,
               sizeof(value.allocation_high_water_digest), 0xB0U);
    fill_bytes(value.quorum_config_digest,
               sizeof(value.quorum_config_digest), 0xC0U);
    fill_bytes(value.signer_set_digest,
               sizeof(value.signer_set_digest), 0xD0U);
    fill_bytes(value.threshold_proof_digest,
               sizeof(value.threshold_proof_digest), 0xE0U);
    fill_bytes(value.freshness_proof_transcript_hash,
               sizeof(value.freshness_proof_transcript_hash), 0x31U);
    value.authority_signer_count = 3U;
    value.authority_quorum_threshold = 2U;
    value.binding_mode = UCN_V6_ADDRESS_LEASED;
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
    ucn_v6_bootstrap_transcript_t *value,
    uint32_t link_generation,
    uint32_t discriminator)
{
    ucn_v6_bootstrap_key_t key;
    memset(&key, 0, sizeof(key));
    value->selected_link_instance_generation = link_generation;
    key.ingress_link_id = value->selected_link_instance_id;
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

static int test_callback_gate_ignores_padding_but_not_fields(void)
{
    ucn_v6_callback_gate_t gate;
    ucn_v6_callback_gate_t invalid;
    ucn_v6_callback_gate_t before;

    /* Deliberately leave representation padding nonzero while making every
     * semantic field pristine. A whole-struct memcmp would reject this valid
     * object on ABIs that insert padding.
     * 刻意保留非零 padding，但把所有语义字段复位；若实现比较整个 struct，
     * 在含 padding 的 ABI 上会错误拒绝。 */
    memset(&gate, 0xA5, sizeof(gate));
    gate.context = NULL;
    gate.lock = NULL;
    gate.unlock = NULL;
    gate.active_owner = NULL;
    gate.violation_count = 0U;
    gate.initialized = false;
    gate.active = false;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) ==
          UCN_V6_ERR_ARGUMENT);

    memset(&invalid, 0, sizeof(invalid));
    invalid.active = true;
    before = invalid;
    CHECK(ucn_v6_callback_gate_init(&invalid, NULL, fake_gate_lock,
                                    fake_gate_unlock) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(&invalid, &before, sizeof(invalid)) == 0);
    return 0;
}

static int test_authority_persist_before_publish(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_identity_authority_storage_t authority_storage = {0};
    ucn_v6_identity_authority_t *authority = NULL;
    ucn_v6_identity_authority_view_t authority_view;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_authority_epoch_t epoch;
    ucn_v6_binding_certificate_t certificate;
    ucn_v6_binding_certificate_t certificate_before;
    ucn_v6_principal_t device = principal(0xA0U);
    uint8_t lease_id[16];
    uint32_t group_id = 0U;
    unsigned witness_reserves_before;
    unsigned submits_before;

    memset(&store, 0, sizeof(store));
    memset(&gate, 0, sizeof(gate));
    ops = store_ops(&store);
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
    CHECK(install_epoch(authority, &epoch, 0U, 1U) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_identity_authority_copy_view(
              authority, &authority_view) == UCN_V6_OK);
    CHECK(store.submit_calls == 1U && !authority_view.epoch_valid);
    CHECK(authority_view.record_generation == 1U && authority_view.faulted);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);

    memset(&store, 0, sizeof(store));
    ops.context = &store;
    CHECK(ucn_v6_identity_authority_init_in_place(
              authority_storage.bytes, sizeof(authority_storage),
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &authority) == UCN_V6_OK);
    CHECK(install_epoch(authority, &epoch, 0U, 1U) == UCN_V6_OK);

    /* A Provider return outside the public result enum is an ABI anomaly,
     * never an application-defined success/failure.  It must stop before
     * reserving a witness or submitting a snapshot and must not publish the
     * caller's output.
     * Provider 返回结果若超出公共枚举域，就是 ABI 异常，不能被当成业务
     * 成功/失败。它必须在 Witness 预留和 Snapshot 提交前停止，且不得发布
     * 调用方输出。 */
    fill_bytes(lease_id, sizeof(lease_id), 0xC0U);
    memset(&certificate, 0xA5, sizeof(certificate));
    certificate_before = certificate;
    witness_reserves_before = store.witness_reserve_calls;
    submits_before = store.submit_calls;
    store.invalid_witness_load_result = true;
    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 10U, 9U, &device, UCN_V6_ADDRESS_LEASED,
              lease_id, UINT64_C(500000), &certificate) ==
          UCN_V6_ERR_STATE);
    CHECK(memcmp(&certificate, &certificate_before, sizeof(certificate)) ==
          0);
    CHECK(store.witness_reserve_calls == witness_reserves_before &&
          store.submit_calls == submits_before);
    CHECK(ucn_v6_identity_authority_copy_view(
              authority, &authority_view) == UCN_V6_OK);
    CHECK(authority_view.faulted && authority_view.occupied_bindings == 0U);

    /* Reinitialize after the deliberate fail-closed fault for the remaining
     * persistence-path cases. */
    memset(&store, 0, sizeof(store));
    ops.context = &store;
    CHECK(ucn_v6_identity_authority_init_in_place(
              authority_storage.bytes, sizeof(authority_storage),
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &authority) == UCN_V6_OK);
    CHECK(install_epoch(authority, &epoch, 0U, 1U) == UCN_V6_OK);

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
    CHECK(install_epoch(authority, &epoch, 0U, 1U) == UCN_V6_OK);
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
              authority, 14U, group_id) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_allocate_dynamic_group(
              authority, 15U, &group_id) == UCN_V6_OK);
    CHECK(group_id == 2U);
    return 0;
}

static int test_factory_epoch_requires_first_lease_sequence(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_identity_authority_storage_t storage = {0};
    ucn_v6_identity_authority_storage_t before;
    ucn_v6_identity_authority_t *authority = NULL;
    ucn_v6_authority_epoch_t skipped = authority_epoch(1U);
    unsigned submits_before;
    unsigned witness_reserves_before;

    memset(&store, 0, sizeof(store));
    ops = store_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              7U, &ops, &gate, &authority) == UCN_V6_OK);
    skipped.lease_sequence = 2U;
    before = storage;
    submits_before = store.submit_calls;
    witness_reserves_before = store.witness_reserve_calls;
    CHECK(install_epoch(authority, &skipped, 0U, 1U) ==
          UCN_V6_ERR_STATE);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);
    CHECK(store.submit_calls == submits_before);
    CHECK(store.witness_reserve_calls == witness_reserves_before);

    skipped.lease_sequence = 1U;
    CHECK(install_epoch(authority, &skipped, 0U, 1U) == UCN_V6_OK);
    return 0;
}

static int test_authority_renewal_and_transfer_preserve_bindings(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_identity_authority_storage_t storage = {0};
    ucn_v6_identity_authority_storage_t before;
    ucn_v6_identity_authority_t *authority = NULL;
    ucn_v6_identity_authority_view_t old_view;
    ucn_v6_identity_authority_view_t renewed_view;
    ucn_v6_identity_authority_view_t transferred_view;
    ucn_v6_authority_epoch_t initial = authority_epoch(1U);
    ucn_v6_authority_epoch_t renewal;
    ucn_v6_authority_epoch_t skipped_renewal;
    ucn_v6_authority_epoch_t transfer;
    ucn_v6_binding_certificate_t first_certificate;
    ucn_v6_binding_certificate_t renewed_certificate;
    ucn_v6_binding_certificate_t transferred_certificate;
    ucn_v6_binding_certificate_t reendorsed_certificate;
    ucn_v6_binding_certificate_t rejected_certificate;
    ucn_v6_binding_certificate_t rejected_certificate_before;
    ucn_v6_principal_t first_device = principal(0xA0U);
    ucn_v6_principal_t second_device = principal(0xB0U);
    ucn_v6_principal_t third_device = principal(0xC0U);
    const ucn_v6_binding_slot_t *first_slot;
    uint8_t first_lease_id[16];
    uint8_t second_lease_id[16];
    uint8_t third_lease_id[16];

    memset(&store, 0, sizeof(store));
    ops = store_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              7U, &ops, &gate, &authority) == UCN_V6_OK);
    CHECK(install_epoch(authority, &initial, 0U, 1U) == UCN_V6_OK);

    fill_bytes(first_lease_id, sizeof(first_lease_id), 0x11U);
    fill_bytes(second_lease_id, sizeof(second_lease_id), 0x31U);
    fill_bytes(third_lease_id, sizeof(third_lease_id), 0x51U);
    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 10U, 9U, &first_device, UCN_V6_ADDRESS_LEASED,
              first_lease_id, UINT64_C(500000),
              &first_certificate) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_copy_view(authority, &old_view) ==
          UCN_V6_OK);

    /* Renewal is exact-next in the realm-wide Lease Sequence. Skipping a
     * sequence is rejected atomically even though the owner/Fence is stable.
     * 续租必须精确推进 Realm 级 Lease Sequence；即使 Owner/Fence 未变化，
     * 跳号也必须原子拒绝。 */
    renewal = initial;
    renewal.lease_sequence = 2U;
    renewal.threshold_proof_digest[0] ^= UINT8_C(0x5A);
    skipped_renewal = renewal;
    skipped_renewal.lease_sequence = 3U;
    before = storage;
    CHECK(install_epoch(authority, &skipped_renewal, 100U, 2U) ==
          UCN_V6_ERR_REPLAY);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);

    CHECK(install_epoch(authority, &renewal, 100U, 2U) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_copy_view(authority, &renewed_view) ==
          UCN_V6_OK);
    CHECK(renewed_view.epoch.authority_generation == 1U);
    CHECK(renewed_view.epoch.lease_sequence == 2U);
    CHECK(renewed_view.local_lease_deadline_us >
          old_view.local_lease_deadline_us);
    CHECK(renewed_view.occupied_bindings == 1U);
    first_slot = snapshot_binding(&store.snapshot, 9U);
    CHECK(first_slot != NULL && first_slot->active);
    CHECK(first_slot->certificate.authority_generation == 1U);
    CHECK(first_slot->certificate.authority_lease_sequence == 1U);
    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 110U, 10U, &second_device,
              UCN_V6_ADDRESS_LEASED, second_lease_id,
              UINT64_C(500000), &renewed_certificate) == UCN_V6_OK);
    CHECK(renewed_certificate.authority_generation == 1U);
    CHECK(renewed_certificate.authority_lease_sequence == 2U);

    /* A fenced Authority transfer advances both Authority Generation and the
     * realm-wide Lease Sequence without deleting durable old certificates.
     * 带 Fence 的换主同时精确推进 Authority Generation 与 Realm 级 Lease
     * Sequence，且不得删除旧权威签发的持久证书。 */
    transfer = authority_epoch(2U);
    transfer.lease_sequence = 3U;
    CHECK(install_epoch(authority, &transfer, 200U, 3U) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_copy_view(authority,
                                               &transferred_view) ==
          UCN_V6_OK);
    CHECK(transferred_view.epoch.authority_generation == 2U);
    CHECK(transferred_view.epoch.lease_sequence == 3U);
    CHECK(transferred_view.occupied_bindings == 2U);
    first_slot = snapshot_binding(&store.snapshot, 9U);
    CHECK(first_slot != NULL && first_slot->active);
    CHECK(first_slot->certificate.authority_generation == 1U);
    CHECK(first_slot->certificate.authority_lease_sequence == 1U);

    /* The new Authority lease cannot renew or overwrite an old active Binding
     * implicitly. The exact old Certificate remains, and the address stays
     * occupied until an explicit durable retirement.
     * 新 Authority Lease 不能隐式续期或覆盖旧 active Binding；旧证书保持
     * 精确不变，地址只有经过显式持久退休后才能重新分配。 */
    memset(&rejected_certificate, 0xA5, sizeof(rejected_certificate));
    rejected_certificate_before = rejected_certificate;
    before = storage;
    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 205U, 9U, &third_device,
              UCN_V6_ADDRESS_LEASED, third_lease_id,
              UINT64_C(500000), &rejected_certificate) ==
          UCN_V6_ERR_STATE);
    CHECK(memcmp(&rejected_certificate, &rejected_certificate_before,
                 sizeof(rejected_certificate)) == 0);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);
    first_slot = snapshot_binding(&store.snapshot, 9U);
    CHECK(first_slot != NULL && first_slot->active);
    CHECK(memcmp(&first_slot->certificate, &first_certificate,
                 sizeof(first_certificate)) == 0);

    /* A transfer keeps the Binding identity but requires one explicit,
     * durable current-Authority endorsement before a new Session may use it.
     * 换主保留 Binding 身份；新 Session 使用前必须显式、持久地由当前
     * Authority 重签。 */
    CHECK(ucn_v6_identity_authority_reendorse_binding(
              authority, 207U, &first_certificate.binding, &first_device,
              UCN_V6_ADDRESS_LEASED, third_lease_id, UINT64_C(400000),
              &reendorsed_certificate) == UCN_V6_OK);
    CHECK(ucn_v6_binding_key_equal(&reendorsed_certificate.binding,
                                   &first_certificate.binding));
    CHECK(reendorsed_certificate.binding.binding_generation == 1U);
    CHECK(reendorsed_certificate.authority_generation == 2U);
    CHECK(reendorsed_certificate.authority_lease_sequence == 3U);
    CHECK(memcmp(reendorsed_certificate.authority_principal.bytes,
                 transfer.authority_principal.bytes,
                 sizeof(transfer.authority_principal.bytes)) == 0);
    CHECK(ucn_v6_identity_authority_reendorse_binding(
              authority, 208U, &first_certificate.binding, &first_device,
              UCN_V6_ADDRESS_LEASED, third_lease_id, UINT64_C(400000),
              &reendorsed_certificate) == UCN_V6_OK);
    second_lease_id[0] ^= UINT8_C(0x55);
    CHECK(ucn_v6_identity_authority_reendorse_binding(
              authority, 208U, &first_certificate.binding, &first_device,
              UCN_V6_ADDRESS_LEASED, second_lease_id, UINT64_C(400000),
              &reendorsed_certificate) == UCN_V6_ERR_REPLAY);

    CHECK(ucn_v6_identity_authority_allocate_binding(
              authority, 210U, 11U, &third_device,
              UCN_V6_ADDRESS_LEASED, third_lease_id,
              UINT64_C(500000), &transferred_certificate) == UCN_V6_OK);
    CHECK(transferred_certificate.authority_generation == 2U);
    CHECK(transferred_certificate.authority_lease_sequence == 3U);
    return 0;
}

static int test_same_lease_freshness_cannot_extend_deadline(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_identity_authority_storage_t storage = {0};
    ucn_v6_identity_authority_storage_t before;
    ucn_v6_identity_authority_t *authority = NULL;
    ucn_v6_identity_authority_view_t view;
    ucn_v6_authority_epoch_t epoch = authority_epoch(1U);
    ucn_v6_authority_freshness_t freshness;
    ucn_v6_lease_verifier_policy_t policy;
    unsigned submits_before;

    memset(&store, 0, sizeof(store));
    memset(&policy, 0, sizeof(policy));
    policy.local_timer_resolution_us = 1U;
    policy.timer_read_uncertainty_known = true;
    policy.local_policy_max_lease_us = UINT64_C(900000);
    ops = store_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              7U, &ops, &gate, &authority) == UCN_V6_OK);
    CHECK(install_epoch(authority, &epoch, 0U, 1U) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_copy_view(authority, &view) ==
          UCN_V6_OK);
    CHECK(view.local_lease_deadline_us == UINT64_C(899999));
    submits_before = store.submit_calls;

    /* A later Challenge under the same Lease Sequence must not manufacture a
     * later local deadline, even when its signed remaining value is equal.
     * 同一 Lease Sequence 下，较晚 Challenge 即使携带相同签名剩余时长，
     * 也不能制造更晚的本地截止期。 */
    freshness = authority_freshness(&epoch, 2U, 102U,
                                    UINT64_C(900000));
    before = storage;
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, UINT64_C(1000), &policy) ==
          UCN_V6_ERR_REPLAY);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);
    CHECK(store.submit_calls == submits_before);

    /* Reducing max_remaining by the elapsed amount produces exactly the old
     * deadline and is accepted. Exact proof replay stays idempotent.
     * 按已流逝时间扣减后得到相同截止期可接受；同一证明重放保持幂等。 */
    freshness = authority_freshness(&epoch, 2U, 103U,
                                    UINT64_C(899000));
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, UINT64_C(1000), &policy) ==
          UCN_V6_OK);
    before = storage;
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, UINT64_C(1000), &policy) ==
          UCN_V6_OK);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);
    CHECK(ucn_v6_identity_authority_copy_view(authority, &view) ==
          UCN_V6_OK);
    CHECK(view.local_lease_deadline_us == UINT64_C(899999));
    CHECK(store.submit_calls == submits_before);
    return 0;
}

static int test_opaque_storage_preflight_and_corruption(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_identity_authority_storage_t authority_storage = {0};
    ucn_v6_identity_authority_storage_t authority_before;
    ucn_v6_bootstrap_owner_storage_t bootstrap_storage = {0};
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
    memset(&gate, 0, sizeof(gate));
    ops = store_ops(&store);
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

static int test_authority_restart_and_rollback_witness(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_identity_authority_storage_t first_storage = {0};
    ucn_v6_identity_authority_storage_t restarted_storage = {0};
    ucn_v6_identity_authority_storage_t rejected_storage = {0};
    ucn_v6_identity_authority_storage_t rejected_before;
    ucn_v6_identity_authority_t *first = NULL;
    ucn_v6_identity_authority_t *restarted = NULL;
    ucn_v6_identity_authority_t *rejected =
        (ucn_v6_identity_authority_t *)(uintptr_t)1U;
    ucn_v6_identity_authority_view_t view;
    ucn_v6_authority_epoch_t epoch = authority_epoch(1U);
    ucn_v6_identity_snapshot_t old_snapshot;
    uint32_t group_id = 0U;
    unsigned submits_before;

    memset(&store, 0, sizeof(store));
    memset(&gate, 0, sizeof(gate));
    ops = store_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_init_in_place(
              first_storage.bytes, sizeof(first_storage),
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &first) == UCN_V6_OK);
    store.perturb_padding = true;
    CHECK(install_epoch(first, &epoch, 0U, 1U) == UCN_V6_OK);
    old_snapshot = store.snapshot;
    CHECK(ucn_v6_identity_authority_allocate_dynamic_group(
              first, 10U, &group_id) == UCN_V6_OK);
    CHECK(group_id == 1U);
    submits_before = store.submit_calls;
    CHECK(ucn_v6_identity_authority_retire_dynamic_group(
              first, UINT64_C(1000000), group_id) == UCN_V6_ERR_ACCESS);
    CHECK(store.submit_calls == submits_before);
    CHECK(ucn_v6_identity_authority_retire_dynamic_group(
              first, 11U, group_id) == UCN_V6_OK);

    CHECK(ucn_v6_identity_authority_init_in_place(
              restarted_storage.bytes, sizeof(restarted_storage),
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &restarted) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_copy_view(restarted, &view) == UCN_V6_OK);
    CHECK(view.record_generation == store.witness.committed_generation);
    CHECK(view.epoch_valid);
    CHECK(view.local_lease_deadline_us == 0U);
    CHECK(view.dynamic_group_id_high_water == 1U);
    CHECK(install_epoch(restarted, &epoch, 0U, 2U) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_allocate_dynamic_group(
              restarted, 12U, &group_id) == UCN_V6_OK);
    CHECK(group_id == 2U);

    /* A stale-but-valid slot must never be accepted when the independent
     * witness proves that a newer state was published.
     * 独立 witness 证明已经发布更新状态时，旧但有效的槽绝不能回退加载。 */
    store.snapshot = old_snapshot;
    memset(&rejected_storage, 0xA5, sizeof(rejected_storage));
    rejected_before = rejected_storage;
    CHECK(ucn_v6_identity_authority_init_in_place(
              rejected_storage.bytes, sizeof(rejected_storage),
              ucn_v6_compiled_manifest(), 7U, &ops, &gate,
              &rejected) == UCN_V6_ERR_STATE);
    CHECK(rejected == (ucn_v6_identity_authority_t *)(uintptr_t)1U);
    CHECK(memcmp(&rejected_storage, &rejected_before,
                 sizeof(rejected_storage)) == 0);
    return 0;
}

static int test_authority_torn_write_recovers_pending(void)
{
    fake_store_t store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_identity_authority_storage_t storage = {0};
    ucn_v6_identity_authority_storage_t reboot_storage = {0};
    ucn_v6_identity_authority_t *authority = NULL;
    ucn_v6_identity_authority_t *rebooted = NULL;
    ucn_v6_identity_authority_view_t view;
    ucn_v6_authority_epoch_t epoch = authority_epoch(1U);
    ucn_v6_result_t reboot_result;

    memset(&store, 0, sizeof(store));
    memset(&gate, 0, sizeof(gate));
    ops = store_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_identity_authority_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              7U, &ops, &gate, &authority) == UCN_V6_OK);
    store.fail_submit_once = true;
    CHECK(install_epoch(authority, &epoch, 0U, 1U) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_identity_authority_copy_view(authority, &view) == UCN_V6_OK);
    CHECK(view.faulted && !view.epoch_valid);
    CHECK(store.witness.pending_generation == 2U && store.present);
    reboot_result = ucn_v6_identity_authority_init_in_place(
        reboot_storage.bytes, sizeof(reboot_storage),
        ucn_v6_compiled_manifest(), 7U, &ops, &gate, &rebooted);
    if (reboot_result != UCN_V6_OK) {
        fprintf(stderr, "unexpected torn reboot result=%d\n", reboot_result);
    }
    CHECK(reboot_result == UCN_V6_OK);
    CHECK(rebooted != NULL);
    CHECK(store.witness.pending_generation == 0U);
    CHECK(store.witness.committed_generation == 1U);
    return 0;
}

static int test_first_commissioning_recovers_every_torn_boundary(void)
{
    unsigned mode;

    for (mode = 1U; mode <= 5U; ++mode) {
        fake_store_t store;
        ucn_v6_identity_store_ops_t ops;
        ucn_v6_callback_gate_t gate = {0};
        ucn_v6_identity_authority_storage_t failed_storage = {0};
        ucn_v6_identity_authority_storage_t reboot_storage = {0};
        ucn_v6_identity_authority_storage_t verify_storage = {0};
        ucn_v6_identity_authority_t *failed = NULL;
        ucn_v6_identity_authority_t *rebooted = NULL;
        ucn_v6_identity_authority_t *verified = NULL;
        ucn_v6_identity_authority_view_t view;
        unsigned submits_after_recovery;
        unsigned witness_after_recovery;

        memset(&store, 0, sizeof(store));
        if (mode == 1U) {
            store.fail_witness_after_write_at = 1U;
        } else if (mode == 2U) {
            store.fail_submit_once = true;
        } else if (mode == 3U) {
            store.fail_submit_after_write_once = true;
        } else if (mode == 4U) {
            store.fail_witness_reserve_at = 2U;
        } else {
            store.fail_witness_after_write_at = 2U;
        }
        ops = store_ops(&store);
        CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                        fake_gate_unlock) == UCN_V6_OK);
        CHECK(ucn_v6_identity_authority_init_in_place(
                  failed_storage.bytes, sizeof(failed_storage),
                  ucn_v6_compiled_manifest(), 7U, &ops, &gate,
                  &failed) == UCN_V6_ERR_STATE);
        CHECK(failed == NULL && store.witness_present);
        CHECK(store.witness.pending_generation == 1U ||
              (store.witness.pending_generation == 0U &&
               store.witness.committed_generation == 1U));

        CHECK(ucn_v6_identity_authority_init_in_place(
                  reboot_storage.bytes, sizeof(reboot_storage),
                  ucn_v6_compiled_manifest(), 7U, &ops, &gate,
                  &rebooted) == UCN_V6_OK);
        CHECK(rebooted != NULL && store.present &&
              store.snapshot.record_generation == 1U &&
              store.witness.pending_generation == 0U &&
              store.witness.committed_generation == 1U);
        CHECK(ucn_v6_identity_authority_copy_view(rebooted, &view) ==
              UCN_V6_OK);
        CHECK(view.record_generation == 1U && !view.epoch_valid &&
              !view.faulted);

        submits_after_recovery = store.submit_calls;
        witness_after_recovery = store.witness_reserve_calls;
        CHECK(ucn_v6_identity_authority_init_in_place(
                  verify_storage.bytes, sizeof(verify_storage),
                  ucn_v6_compiled_manifest(), 7U, &ops, &gate,
                  &verified) == UCN_V6_OK);
        CHECK(verified != NULL &&
              store.submit_calls == submits_after_recovery &&
              store.witness_reserve_calls == witness_after_recovery &&
              store.snapshot.record_generation == 1U);
    }
    return 0;
}

static int test_bootstrap_open_binds_exact_selected_link(void)
{
    ucn_v6_bootstrap_owner_storage_t owner_storage = {0};
    uint8_t before[UCN_V6_BOOTSTRAP_OWNER_STORAGE_BYTES];
    ucn_v6_bootstrap_owner_t *owner = NULL;
    ucn_v6_bootstrap_config_t config;
    ucn_v6_bootstrap_transcript_t value = transcript(UINT64_C(901));
    ucn_v6_bootstrap_key_t key = bootstrap_key(&value, 1U, 9U);
    ucn_v6_binding_key_t binding = {
        UINT32_C(0x10203040), 7U, 3U
    };

    memset(&config, 0, sizeof(config));
    config.max_pending = UCN_V6_CONFIG_BOOTSTRAP_PENDING;
    config.max_pending_per_link = 1U;
    config.token_burst = 2U;
    config.tokens_per_second = 1U;
    config.pending_timeout_us = UINT64_C(3000000);
    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              owner_storage.bytes, sizeof(owner_storage),
              ucn_v6_compiled_manifest(), &config, &owner) == UCN_V6_OK);
    memcpy(before, owner_storage.bytes, sizeof(before));
    key.ingress_link_id = UINT16_MAX;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value, NULL,
              true, 10U) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(owner_storage.bytes, before, sizeof(before)) == 0);

    key = bootstrap_key(&value, 1U, 9U);
    value.selected_link_instance_id = UINT16_MAX;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value, NULL,
              true, 10U) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(owner_storage.bytes, before, sizeof(before)) == 0);

    value = transcript(UINT64_C(901));
    key = bootstrap_key(&value, 1U, 9U);
    key.ingress_link_generation = 2U;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value, NULL,
              true, 10U) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(owner_storage.bytes, before, sizeof(before)) == 0);

    key = bootstrap_key(&value, 1U, 9U);
    key.ingress_link_id = 2U;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value, NULL,
              true, 10U) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(owner_storage.bytes, before, sizeof(before)) == 0);

    value = transcript(UINT64_C(902));
    value.flow = UCN_V6_BOOTSTRAP_FLOW_REAUTH;
    key = bootstrap_key(&value, 3U, 10U);
    key.ingress_link_generation = 4U;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &value,
              &binding, true, 11U) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(owner_storage.bytes, before, sizeof(before)) == 0);

    key = bootstrap_key(&value, 3U, 10U);
    value.selected_link_instance_id = 2U;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &value,
              &binding, true, 11U) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(owner_storage.bytes, before, sizeof(before)) == 0);
    return 0;
}

static int test_bootstrap_resource_and_state_contract(void)
{
    ucn_v6_bootstrap_owner_storage_t owner_storage = {0};
    ucn_v6_bootstrap_owner_t *owner = NULL;
    ucn_v6_bootstrap_config_t config = {
        UCN_V6_CONFIG_BOOTSTRAP_PENDING,
        UCN_V6_CONFIG_BOOTSTRAP_PENDING < 2U ?
            UCN_V6_CONFIG_BOOTSTRAP_PENDING : 2U,
        4U, 2U, UINT64_C(3000000)
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
              owner, 1U, 1U, 0U, 32U, 33U) == UCN_V6_ERR_ARGUMENT);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 0U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 1U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 2U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 3U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 4U, 32U, 32U) == UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 2U, 1U, 4U, 32U, 32U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, UINT64_C(1000000), 32U, 32U) == UCN_V6_OK);

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
    value.flow = UCN_V6_BOOTSTRAP_FLOW_REAUTH;
    key = bootstrap_key(&value, 2U, 1U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &value,
              NULL, true, 30U) == UCN_V6_ERR_STATE);
    conflict = value;
    conflict.flow = UCN_V6_BOOTSTRAP_FLOW_JOIN;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &conflict,
              &binding, true, 30U) == UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key,
              &pending) == UCN_V6_ERR_NOT_FOUND);
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
    ucn_v6_bootstrap_owner_storage_t owner_storage = {0};
    ucn_v6_bootstrap_owner_t *owner = NULL;
#if UCN_V6_CONFIG_BOOTSTRAP_PENDING >= 4U
    ucn_v6_bootstrap_config_t config = {
        4U, 2U, 4U, 2U, UINT64_C(3000000)
    };
#else
    ucn_v6_bootstrap_config_t config = {
        UCN_V6_CONFIG_BOOTSTRAP_PENDING, 1U, 4U, 2U,
        UINT64_C(3000000)
    };
#endif
    ucn_v6_bootstrap_transcript_t join_a = transcript(UINT64_C(301));
    ucn_v6_bootstrap_transcript_t reauth_a = transcript(UINT64_C(302));
    ucn_v6_bootstrap_transcript_t reauth_b = transcript(UINT64_C(303));
    ucn_v6_bootstrap_transcript_t join_b = transcript(UINT64_C(304));
#if UCN_V6_CONFIG_BOOTSTRAP_PENDING >= 4U
    ucn_v6_bootstrap_transcript_t join_c = transcript(UINT64_C(305));
    ucn_v6_bootstrap_transcript_t join_d = transcript(UINT64_C(306));
    ucn_v6_bootstrap_transcript_t join_e = transcript(UINT64_C(307));
#endif
    ucn_v6_bootstrap_key_t key;
    ucn_v6_binding_key_t binding = { UINT32_C(0x10203040), 7U, 3U };

    reauth_a.flow = UCN_V6_BOOTSTRAP_FLOW_REAUTH;
    reauth_b.flow = UCN_V6_BOOTSTRAP_FLOW_REAUTH;

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

#if UCN_V6_CONFIG_BOOTSTRAP_PENDING >= 4U
    key = bootstrap_key(&reauth_b, 1U, 2U);
#else
    key = bootstrap_key(&reauth_b, 2U, 2U);
#endif
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_REAUTH, &key, &reauth_b,
              &binding, true, 2U) == UCN_V6_OK);
#if UCN_V6_CONFIG_BOOTSTRAP_PENDING >= 4U
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
#else
    key = bootstrap_key(&join_b, 2U, 3U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &join_b,
              NULL, true, 3U) == UCN_V6_ERR_NO_SPACE);
#endif
    return 0;
}

static int test_bootstrap_exact_link_instance_budget(void)
{
    ucn_v6_bootstrap_owner_storage_t owner_storage = {0};
    ucn_v6_bootstrap_owner_t *owner = NULL;
#if UCN_V6_CONFIG_BOOTSTRAP_PENDING >= 4U
    ucn_v6_bootstrap_config_t config = {
        4U, 2U, 2U, 1U, UINT64_C(3000000)
    };
#else
    ucn_v6_bootstrap_config_t config = {
        UCN_V6_CONFIG_BOOTSTRAP_PENDING, 1U, 2U, 1U,
        UINT64_C(3000000)
    };
#endif
    ucn_v6_bootstrap_transcript_t first = transcript(UINT64_C(401));
    ucn_v6_bootstrap_transcript_t second = transcript(UINT64_C(402));
#if UCN_V6_CONFIG_BOOTSTRAP_PENDING >= 4U
    ucn_v6_bootstrap_transcript_t third = transcript(UINT64_C(403));
#endif
    ucn_v6_bootstrap_key_t key;

    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              owner_storage.bytes, sizeof(owner_storage),
              ucn_v6_compiled_manifest(), &config, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 0U, 16U, 16U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 1U, 16U, 16U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U, 1U, 2U, 16U, 16U) == UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 2U, 1U, 2U, 16U, 16U) == UCN_V6_OK);

    key = bootstrap_key(&first, 1U, 1U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &first,
              NULL, true, 10U) == UCN_V6_OK);
    key = bootstrap_key(&second, 1U, 2U);
#if UCN_V6_CONFIG_BOOTSTRAP_PENDING >= 4U
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &second,
              NULL, true, 11U) == UCN_V6_OK);
    key = bootstrap_key(&third, 1U, 3U);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &third,
              NULL, true, 12U) == UCN_V6_ERR_NO_SPACE);
    key.ingress_link_id = 2U;
    third.selected_link_instance_id = 2U;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &third,
              NULL, true, 12U) == UCN_V6_OK);
#else
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &second,
              NULL, true, 11U) == UCN_V6_ERR_NO_SPACE);
    key.ingress_link_id = 2U;
    second.selected_link_instance_id = 2U;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &second,
              NULL, true, 11U) == UCN_V6_OK);
#endif
    return 0;
}

static int test_bootstrap_budget_generation_churn_reclaims_idle_slots(void)
{
    ucn_v6_bootstrap_owner_storage_t owner_storage = {0};
    ucn_v6_bootstrap_owner_t *owner = NULL;
    ucn_v6_bootstrap_config_t config = {
        2U, 1U, 2U, 1U, UINT64_C(1000)
    };
    size_t index;

    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              owner_storage.bytes, sizeof(owner_storage),
              ucn_v6_compiled_manifest(), &config, &owner) == UCN_V6_OK);
    for (index = 0U; index < UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS; ++index) {
        CHECK(ucn_v6_bootstrap_admit_initial_hello(
                  owner, 1U, (uint32_t)(index + 1U), (uint64_t)index,
                  16U, 16U) == UCN_V6_OK);
    }
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U,
              (uint32_t)(UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS + 1U),
              UINT64_C(20), 16U, 16U) == UCN_V6_ERR_NO_SPACE);

    /* Only the explicit timer owner reclaims idle generations. No hostile
     * request may evict a live rate-limit bucket during admission. */
    CHECK(ucn_v6_bootstrap_expire(
              owner,
              UINT64_C(1000) + UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS) == 0U);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U,
              (uint32_t)(UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS + 1U),
              UINT64_C(1001) + UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS,
              16U, 16U) == UCN_V6_ERR_NO_SPACE);
    CHECK(ucn_v6_bootstrap_expire(
              owner,
              UINT64_C(2000000) + UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS) == 0U);
    CHECK(ucn_v6_bootstrap_admit_initial_hello(
              owner, 1U,
              (uint32_t)(UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS + 1U),
              UINT64_C(2000001) + UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS,
              16U, 16U) == UCN_V6_OK);
    return 0;
}

#undef ucn_v6_bootstrap_owner_init_in_place
#undef ucn_v6_bootstrap_open_after_cookie
#undef ucn_v6_bootstrap_advance

static int test_bootstrap_trusted_verifier_fail_closed(void)
{
    ucn_v6_bootstrap_owner_storage_t storage = {0};
    ucn_v6_bootstrap_owner_t *owner = NULL;
    ucn_v6_bootstrap_config_t config = {
        2U, 1U, 2U, 1U, UINT64_C(1000)
    };
    fake_bootstrap_verifier_t verifier_context;
    ucn_v6_bootstrap_verifier_ops_t verifier;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_bootstrap_transcript_t value = transcript(UINT64_C(900));
    ucn_v6_bootstrap_key_t key = bootstrap_key(&value, 1U, 7U);
    ucn_v6_bootstrap_evidence_t evidence;
    ucn_v6_bootstrap_pending_t pending;
    ucn_v6_bootstrap_pending_t before;

    memset(&verifier_context, 0, sizeof(verifier_context));
    memset(&verifier, 0, sizeof(verifier));
    verifier.context = &verifier_context;
    verifier.authorize_event = authorize_bootstrap_event;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &config, &verifier, &gate, &owner) == UCN_V6_OK);

    evidence = bootstrap_evidence(UCN_V6_BOOTSTRAP_EVENT_COOKIE,
                                  UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
                                  &value, NULL);
    evidence.bytes[0] = UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value, NULL,
              &evidence, 1U) == UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &pending) ==
          UCN_V6_ERR_NOT_FOUND);

    verifier_context.forced_result = UCN_V6_ERR_STATE;
    evidence = bootstrap_evidence(UCN_V6_BOOTSTRAP_EVENT_COOKIE,
                                  UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
                                  &value, NULL);
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value, NULL,
              &evidence, 1U) == UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &pending) ==
          UCN_V6_ERR_NOT_FOUND);
    verifier_context.forced_result = UCN_V6_OK;
    CHECK(ucn_v6_bootstrap_open_after_cookie(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value, NULL,
              &evidence, 1U) == UCN_V6_OK);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &pending) ==
          UCN_V6_OK);
    before = pending;

    evidence = bootstrap_evidence(UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF,
                                  UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
                                  &value, NULL);
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF, &evidence, 2U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &pending) ==
          UCN_V6_OK);
    CHECK(memcmp(&pending, &before, sizeof(pending)) == 0);

    verifier_context.reenter = true;
    verifier_context.owner = owner;
    verifier_context.flow = UCN_V6_BOOTSTRAP_FLOW_JOIN;
    verifier_context.key = key;
    verifier_context.transcript = value;
    evidence = bootstrap_evidence(UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF,
                                  UCN_V6_BOOTSTRAP_FLOW_JOIN, &key,
                                  &value, NULL);
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF, &evidence, 2U) ==
          UCN_V6_ERR_STATE);
    CHECK(verifier_context.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_bootstrap_copy_pending(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &pending) ==
          UCN_V6_OK);
    CHECK(memcmp(&pending, &before, sizeof(pending)) == 0);

    verifier_context.return_invalid_result = true;
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF, &evidence, 2U) ==
          UCN_V6_ERR_SECURITY);
    verifier_context.return_invalid_result = false;
    CHECK(ucn_v6_bootstrap_advance(
              owner, UCN_V6_BOOTSTRAP_FLOW_JOIN, &key, &value,
              UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF, &evidence, 2U) ==
          UCN_V6_OK);
    return 0;
}

#undef ucn_v6_identity_authority_init_in_place
#undef ucn_v6_identity_authority_install_epoch

static int test_authority_trusted_verifier_and_canonical_domain(void)
{
    fake_store_t store;
    fake_store_t fresh_store;
    ucn_v6_identity_store_ops_t ops;
    ucn_v6_identity_store_ops_t fresh_ops;
    ucn_v6_identity_authority_verifier_ops_t verifier =
        authority_verifier_ops();
    ucn_v6_identity_authority_verifier_ops_t invalid_verifier = {0};
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_callback_gate_t fresh_gate = {0};
    ucn_v6_identity_authority_storage_t storage = {0};
    ucn_v6_identity_authority_storage_t fresh_storage = {0};
    ucn_v6_identity_authority_storage_t rejected_storage = {0};
    ucn_v6_identity_authority_storage_t before;
    ucn_v6_identity_authority_t *authority = NULL;
    ucn_v6_identity_authority_t *fresh_authority = NULL;
    ucn_v6_identity_authority_t *rejected = NULL;
    ucn_v6_authority_epoch_t epoch = authority_epoch(1U);
    ucn_v6_authority_epoch_t changed_epoch;
    ucn_v6_authority_freshness_t freshness;
    ucn_v6_lease_verifier_policy_t policy;
    ucn_v6_authority_transition_request_t request;
    ucn_v6_authority_transition_request_t padded_a;
    ucn_v6_authority_transition_request_t padded_b;
    ucn_v6_authority_proof_t proof;
    ucn_v6_authority_proof_t future_proof;
    ucn_v6_authority_proof_t wrong_proof;
    uint8_t canonical[UCN_V6_AUTHORITY_TRANSITION_CANONICAL_BYTES];
    uint8_t canonical_a[UCN_V6_AUTHORITY_TRANSITION_CANONICAL_BYTES];
    uint8_t canonical_b[UCN_V6_AUTHORITY_TRANSITION_CANONICAL_BYTES];
    size_t canonical_bytes = 0U;
    size_t canonical_a_bytes = 0U;
    size_t canonical_b_bytes = 0U;
    unsigned submits_before;

    memset(&authority_verifier_context, 0,
           sizeof(authority_verifier_context));
    memset(&store, 0, sizeof(store));
    memset(&fresh_store, 0, sizeof(fresh_store));
    memset(&policy, 0, sizeof(policy));
    policy.local_timer_resolution_us = 1U;
    policy.timer_read_uncertainty_known = true;
    policy.local_policy_max_lease_us = UINT64_C(900000);
    ops = store_ops(&store);
    fresh_ops = store_ops(&fresh_store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_callback_gate_init(&fresh_gate, NULL, fake_gate_lock,
                                    fake_gate_unlock) == UCN_V6_OK);

    memset(&rejected_storage, 0xA5, sizeof(rejected_storage));
    before = rejected_storage;
    CHECK(ucn_v6_identity_authority_init_in_place(
              rejected_storage.bytes, sizeof(rejected_storage),
              ucn_v6_compiled_manifest(), 7U, &invalid_verifier, &ops,
              &gate, &rejected) == UCN_V6_ERR_CONFIG);
    CHECK(rejected == NULL &&
          memcmp(&rejected_storage, &before, sizeof(before)) == 0 &&
          store.submit_calls == 0U);

    CHECK(ucn_v6_identity_authority_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              7U, &verifier, &ops, &gate, &authority) == UCN_V6_OK);
    authority_verifier_context.authority = authority;
    freshness = authority_freshness(&epoch, 1U, 101U,
                                    UINT64_C(900000));
    CHECK(authority_proof_for_install(
              authority, &epoch, &freshness, 0U, &policy, &request,
              &proof) == UCN_V6_OK);
    CHECK(ucn_v6_authority_transition_encode_canonical(
              &request, canonical, sizeof(canonical),
              &canonical_bytes) == UCN_V6_OK);
    CHECK(canonical_bytes == sizeof(canonical));

    /* Outer-struct and compiler padding never enter the proof domain. */
    memset(&padded_a, 0xA5, sizeof(padded_a));
    memset(&padded_b, 0x5A, sizeof(padded_b));
    padded_a.kind = padded_b.kind = request.kind;
    padded_a.realm_id = padded_b.realm_id = request.realm_id;
    padded_a.committed_epoch_valid = padded_b.committed_epoch_valid =
        request.committed_epoch_valid;
    padded_a.committed_epoch = padded_b.committed_epoch =
        request.committed_epoch;
    padded_a.proposed_epoch = padded_b.proposed_epoch =
        request.proposed_epoch;
    padded_a.freshness = padded_b.freshness = request.freshness;
    padded_a.challenge_started_local_us =
        padded_b.challenge_started_local_us =
            request.challenge_started_local_us;
    padded_a.lease_policy = padded_b.lease_policy = request.lease_policy;
    padded_a.derived_local_deadline_us =
        padded_b.derived_local_deadline_us =
            request.derived_local_deadline_us;
    CHECK(ucn_v6_authority_transition_encode_canonical(
              &padded_a, canonical_a, sizeof(canonical_a),
              &canonical_a_bytes) == UCN_V6_OK);
    CHECK(ucn_v6_authority_transition_encode_canonical(
              &padded_b, canonical_b, sizeof(canonical_b),
              &canonical_b_bytes) == UCN_V6_OK);
    CHECK(canonical_a_bytes == canonical_b_bytes &&
          memcmp(canonical_a, canonical_b, canonical_a_bytes) == 0);

#define CHECK_REQUEST_MUTATION(mutation_)                                      \
    do {                                                                        \
        ucn_v6_result_t mutation_result;                                        \
        padded_a = request;                                                     \
        mutation_;                                                              \
        canonical_a_bytes = 0U;                                                 \
        mutation_result = ucn_v6_authority_transition_encode_canonical(         \
            &padded_a, canonical_a, sizeof(canonical_a),                        \
            &canonical_a_bytes);                                                \
        CHECK(mutation_result != UCN_V6_OK ||                                   \
              canonical_a_bytes != canonical_bytes ||                           \
              memcmp(canonical_a, canonical, canonical_bytes) != 0);            \
    } while (0)

    /* Every semantic field group is either represented in the canonical
     * bytes or makes the claimed transition non-canonical. */
    CHECK_REQUEST_MUTATION(
        padded_a.kind = UCN_V6_AUTHORITY_TRANSITION_FRESHNESS);
    CHECK_REQUEST_MUTATION(++padded_a.realm_id);
    CHECK_REQUEST_MUTATION(padded_a.committed_epoch_valid = true);
    CHECK_REQUEST_MUTATION(++padded_a.committed_epoch.realm_id);
    CHECK_REQUEST_MUTATION(++padded_a.proposed_epoch.realm_id;
                           ++padded_a.realm_id);
    CHECK_REQUEST_MUTATION(
        ++padded_a.proposed_epoch.authority_principal.bytes[0];
        ++padded_a.freshness.verifier_device_principal.bytes[0]);
    CHECK_REQUEST_MUTATION(++padded_a.proposed_epoch.authority_generation);
    CHECK_REQUEST_MUTATION(
        ++padded_a.proposed_epoch.durable_fence_token[0]);
    CHECK_REQUEST_MUTATION(++padded_a.proposed_epoch.lease_sequence;
                           ++padded_a.freshness.authority_lease_sequence);
    CHECK_REQUEST_MUTATION(++padded_a.proposed_epoch.lease_duration_us);
    CHECK_REQUEST_MUTATION(
        ++padded_a.proposed_epoch.allocation_high_water_digest[0]);
    CHECK_REQUEST_MUTATION(
        ++padded_a.proposed_epoch.quorum_config_digest[0]);
    CHECK_REQUEST_MUTATION(
        ++padded_a.proposed_epoch.signer_set_digest[0]);
    CHECK_REQUEST_MUTATION(
        ++padded_a.proposed_epoch.threshold_proof_digest[0]);
    CHECK_REQUEST_MUTATION(++padded_a.proposed_epoch.signer_count);
    CHECK_REQUEST_MUTATION(++padded_a.proposed_epoch.quorum_threshold);
    CHECK_REQUEST_MUTATION(
        ++padded_a.freshness.verifier_device_principal.bytes[0];
        ++padded_a.proposed_epoch.authority_principal.bytes[0]);
    CHECK_REQUEST_MUTATION(++padded_a.freshness.challenge_nonce);
    CHECK_REQUEST_MUTATION(++padded_a.freshness.transaction_id);
    CHECK_REQUEST_MUTATION(++padded_a.freshness.authority_lease_sequence);
    CHECK_REQUEST_MUTATION(--padded_a.freshness.max_remaining_lease_us;
                           --padded_a.derived_local_deadline_us);
    CHECK_REQUEST_MUTATION(++padded_a.freshness.binding_lease_id[0]);
    CHECK_REQUEST_MUTATION(++padded_a.freshness.binding_generation);
    CHECK_REQUEST_MUTATION(
        ++padded_a.freshness.proof_transcript_hash[0]);
    CHECK_REQUEST_MUTATION(++padded_a.challenge_started_local_us;
                           ++padded_a.derived_local_deadline_us);
    CHECK_REQUEST_MUTATION(
        ++padded_a.lease_policy.local_timer_max_slow_ppm);
    CHECK_REQUEST_MUTATION(
        ++padded_a.lease_policy.local_timer_resolution_us);
    CHECK_REQUEST_MUTATION(
        ++padded_a.lease_policy.local_timer_read_uncertainty_us);
    CHECK_REQUEST_MUTATION(
        padded_a.lease_policy.timer_read_uncertainty_known = false);
    CHECK_REQUEST_MUTATION(
        --padded_a.lease_policy.local_policy_max_lease_us;
        --padded_a.derived_local_deadline_us);
    CHECK_REQUEST_MUTATION(++padded_a.derived_local_deadline_us);
#undef CHECK_REQUEST_MUTATION

    CHECK(authority_proof_for_install(
              authority, &epoch, &freshness, 100U, &policy, NULL,
              &future_proof) == UCN_V6_OK);
    before = storage;
    submits_before = store.submit_calls;
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, 100U, 0U, &policy,
              &future_proof) == UCN_V6_ERR_STATE);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0 &&
          store.submit_calls == submits_before);

    wrong_proof = proof;
    wrong_proof.bytes[0] ^= UINT8_C(0x80);
    before = storage;
    submits_before = store.submit_calls;
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, 0U, 0U, &policy,
              &wrong_proof) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0 &&
          store.submit_calls == submits_before);

    wrong_proof = proof;
    wrong_proof.bytes[wrong_proof.length] = UINT8_C(0x7E);
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, 0U, 0U, &policy,
              &wrong_proof) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0 &&
          store.submit_calls == submits_before);

    authority_verifier_context.forced_result = UCN_V6_ERR_STATE;
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, 0U, 0U, &policy,
              &proof) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0 &&
          store.submit_calls == submits_before);
    authority_verifier_context.forced_result = UCN_V6_OK;
    authority_verifier_context.return_invalid_result = true;
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, 0U, 0U, &policy,
              &proof) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0 &&
          store.submit_calls == submits_before);
    authority_verifier_context.return_invalid_result = false;

    authority_verifier_context.reenter_install = true;
    CHECK(ucn_v6_identity_authority_install_epoch(
              authority, &epoch, &freshness, 0U, 0U, &policy,
              &proof) == UCN_V6_ERR_STATE);
    CHECK(authority_verifier_context.reenter_result == UCN_V6_ERR_STATE);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0 &&
          store.submit_calls == submits_before);

    /* A proof for one exact transcript cannot authorize a semantically valid
     * but different threshold/allocation statement. */
    CHECK(ucn_v6_identity_authority_init_in_place(
              fresh_storage.bytes, sizeof(fresh_storage),
              ucn_v6_compiled_manifest(), 7U, &verifier, &fresh_ops,
              &fresh_gate, &fresh_authority) == UCN_V6_OK);
    authority_verifier_context.authority = fresh_authority;
    changed_epoch = epoch;
    changed_epoch.threshold_proof_digest[0] ^= UINT8_C(0x55);
    before = fresh_storage;
    submits_before = fresh_store.submit_calls;
    CHECK(ucn_v6_identity_authority_install_epoch(
              fresh_authority, &changed_epoch, &freshness, 0U, 0U, &policy,
              &proof) == UCN_V6_ERR_SECURITY);
    CHECK(memcmp(&fresh_storage, &before, sizeof(fresh_storage)) == 0 &&
          fresh_store.submit_calls == submits_before);
    return 0;
}

int main(void)
{
    CHECK(test_identity_and_deadline_contract() == 0);
    CHECK(test_callback_gate_ignores_padding_but_not_fields() == 0);
    CHECK(test_authority_persist_before_publish() == 0);
    CHECK(test_factory_epoch_requires_first_lease_sequence() == 0);
    CHECK(test_authority_renewal_and_transfer_preserve_bindings() == 0);
    CHECK(test_same_lease_freshness_cannot_extend_deadline() == 0);
    CHECK(test_opaque_storage_preflight_and_corruption() == 0);
    CHECK(test_authority_restart_and_rollback_witness() == 0);
    CHECK(test_authority_torn_write_recovers_pending() == 0);
    CHECK(test_first_commissioning_recovers_every_torn_boundary() == 0);
    CHECK(test_bootstrap_open_binds_exact_selected_link() == 0);
    CHECK(test_bootstrap_resource_and_state_contract() == 0);
    CHECK(test_bootstrap_cross_flow_capacity_contract() == 0);
    CHECK(test_bootstrap_exact_link_instance_budget() == 0);
    CHECK(test_bootstrap_budget_generation_churn_reclaims_idle_slots() == 0);
    CHECK(test_bootstrap_trusted_verifier_fail_closed() == 0);
    CHECK(test_authority_trusted_verifier_and_canonical_domain() == 0);
    puts("ucn v6 identity/bootstrap tests passed");
    return 0;
}
