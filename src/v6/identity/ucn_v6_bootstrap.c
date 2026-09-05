#include "../internal/ucn_v6_bootstrap_private.h"

#include "ucn/v6/ucn_v6_config.h"

#include <string.h>

typedef char ucn_v6_bootstrap_storage_size_check[
    sizeof(ucn_v6_bootstrap_owner_t) <= UCN_V6_BOOTSTRAP_OWNER_STORAGE_BYTES ?
        1 : -1];

static bool callback_result_is_declared(ucn_v6_result_t result)
{
    return result <= UCN_V6_OK && result >= UCN_V6_ERR_CANCELLED;
}

static bool owner_is_valid(const ucn_v6_bootstrap_owner_t *owner)
{
    return owner != NULL && owner->initialized &&
           owner->magic == UCN_V6_BOOTSTRAP_OWNER_MAGIC &&
           owner->schema == UCN_V6_STORAGE_LAYOUT &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->verifier.authorize_event != NULL &&
           owner->callback_gate != NULL &&
           ucn_v6_callback_gate_violation_count(owner->callback_gate) !=
               UINT64_MAX &&
           owner->canary == UCN_V6_BOOTSTRAP_OWNER_CANARY;
}

static ucn_v6_result_t callback_scope_finish(
    ucn_v6_callback_gate_t *gate,
    const void *owner,
    uint64_t violations_before,
    ucn_v6_result_t result)
{
    bool clean;

    if (gate == NULL || owner == NULL || !gate->initialized ||
        gate->lock == NULL || gate->unlock == NULL ||
        violations_before == UINT64_MAX) {
        return UCN_V6_ERR_STATE;
    }
    gate->lock(gate->context);
    clean = gate->active && gate->active_owner == owner &&
            gate->violation_count == violations_before &&
            gate->violation_count != UINT64_MAX;
    if (gate->active && gate->active_owner == owner) {
        gate->active = false;
        gate->active_owner = NULL;
    }
    gate->unlock(gate->context);
    return clean && callback_result_is_declared(result) ? result :
                                                         UCN_V6_ERR_STATE;
}

static bool callback_reentry_is_blocked(
    const ucn_v6_bootstrap_owner_t *owner)
{
    ucn_v6_result_t result;

    if (!ucn_v6_callback_gate_is_active(owner->callback_gate)) {
        return false;
    }
    /* Record the violation in the shared Gate so an outer verifier call
     * cannot hide a nested API attempt by discarding its return code. */
    result = ucn_v6_callback_gate_try_enter(owner->callback_gate, owner);
    if (result == UCN_V6_OK &&
        ucn_v6_callback_gate_leave(owner->callback_gate, owner) !=
            UCN_V6_OK) {
        return true;
    }
    return true;
}

static bool principal_equal(
    const ucn_v6_principal_t *left,
    const ucn_v6_principal_t *right)
{
    return memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool bytes_zero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool evidence_is_valid(
    const ucn_v6_bootstrap_evidence_t *evidence)
{
    return evidence != NULL && evidence->length != 0U &&
           evidence->length <= UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES &&
           bytes_nonzero(evidence->bytes, evidence->length) &&
           bytes_zero(evidence->bytes + evidence->length,
                      UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES -
                          evidence->length);
}

static bool flow_is_valid(ucn_v6_bootstrap_flow_t flow)
{
    return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ||
           flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH;
}

static bool key_is_valid(const ucn_v6_bootstrap_key_t *key)
{
    return key != NULL && key->ingress_link_id != 0U &&
           key->ingress_link_generation != 0U &&
           key->ingress_link_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           key->local_peer_discriminator != 0U &&
           ucn_v6_principal_is_valid(&key->identity_digest) &&
           key->transaction_id != 0U &&
           key->transaction_id <= UCN_V6_SERIAL64_ROTATION_THRESHOLD;
}

static bool key_equal(
    const ucn_v6_bootstrap_key_t *left,
    const ucn_v6_bootstrap_key_t *right)
{
    return left->ingress_link_id == right->ingress_link_id &&
           left->ingress_link_generation == right->ingress_link_generation &&
           left->local_peer_discriminator == right->local_peer_discriminator &&
           left->transaction_id == right->transaction_id &&
           principal_equal(&left->identity_digest, &right->identity_digest);
}

static bool transcript_is_valid(
    const ucn_v6_bootstrap_transcript_t *transcript)
{
    return transcript != NULL &&
           transcript->protocol_version == UCN_V6_PROTOCOL_VERSION &&
           transcript->bootstrap_header_contract != 0U &&
           flow_is_valid(transcript->flow) &&
           ucn_v6_principal_is_valid(
               &transcript->joining_device_principal) &&
           ucn_v6_principal_is_valid(
               &transcript->joining_device_identity_digest) &&
           ucn_v6_principal_is_valid(&transcript->authority_principal) &&
           transcript->authority_generation != 0U &&
           transcript->authority_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->device_nonce != 0U &&
           transcript->authority_nonce != 0U &&
           transcript->transaction_id != 0U &&
           transcript->transaction_id <= UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           transcript->lease_freshness_challenge_nonce != 0U &&
           transcript->realm_id != 0U && transcript->realm_id != UINT32_MAX &&
           transcript->proposed_address != 0U &&
           transcript->proposed_address != UINT32_MAX &&
           transcript->address_binding_generation != 0U &&
           transcript->address_binding_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->authority_address != 0U &&
           transcript->authority_address != UINT32_MAX &&
           transcript->authority_binding_generation != 0U &&
           transcript->authority_binding_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->selected_link_instance_id != 0U &&
           bytes_nonzero(transcript->binding_lease_id,
                         sizeof(transcript->binding_lease_id)) &&
           transcript->binding_lease_duration_us != 0U &&
           transcript->authority_lease_sequence != 0U &&
           transcript->authority_lease_sequence <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           transcript->authority_lease_duration_us != 0U &&
           transcript->freshness_max_remaining_lease_us != 0U &&
           transcript->freshness_max_remaining_lease_us <=
               transcript->authority_lease_duration_us &&
           bytes_nonzero(transcript->durable_fence_token,
                         sizeof(transcript->durable_fence_token)) &&
           bytes_nonzero(transcript->allocation_high_water_digest,
                         sizeof(transcript->allocation_high_water_digest)) &&
           bytes_nonzero(transcript->quorum_config_digest,
                         sizeof(transcript->quorum_config_digest)) &&
           bytes_nonzero(transcript->signer_set_digest,
                         sizeof(transcript->signer_set_digest)) &&
           bytes_nonzero(transcript->threshold_proof_digest,
                         sizeof(transcript->threshold_proof_digest)) &&
           bytes_nonzero(transcript->freshness_proof_transcript_hash,
                         sizeof(transcript->freshness_proof_transcript_hash)) &&
           transcript->authority_signer_count != 0U &&
           transcript->authority_quorum_threshold != 0U &&
           transcript->authority_quorum_threshold <=
               transcript->authority_signer_count &&
           (transcript->binding_mode == UCN_V6_ADDRESS_STATIC ||
            transcript->binding_mode == UCN_V6_ADDRESS_LEASED ||
            transcript->binding_mode == UCN_V6_ADDRESS_SELF_PROPOSED) &&
           transcript->selected_hop_suite != 0U &&
           transcript->selected_hop_key_id != 0U &&
           transcript->selected_hop_key_generation != 0U &&
           transcript->selected_hop_key_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           (transcript->selected_e2e_mode == UCN_V6_E2E_AUTH_ONLY ||
            transcript->selected_e2e_mode == UCN_V6_E2E_AEAD) &&
           transcript->selected_e2e_suite != 0U &&
           transcript->selected_e2e_key_id != 0U &&
           transcript->selected_e2e_key_generation != 0U &&
           transcript->selected_e2e_key_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->selected_session_generation != 0U &&
           transcript->selected_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->selected_link_instance_generation != 0U &&
           transcript->selected_link_instance_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_nonzero(transcript->prior_messages_hash,
                         sizeof(transcript->prior_messages_hash));
}

static bool transcript_equal(
    const ucn_v6_bootstrap_transcript_t *left,
    const ucn_v6_bootstrap_transcript_t *right)
{
    return left->protocol_version == right->protocol_version &&
           left->bootstrap_header_contract ==
               right->bootstrap_header_contract &&
           left->flow == right->flow &&
           principal_equal(&left->joining_device_principal,
                           &right->joining_device_principal) &&
           principal_equal(&left->joining_device_identity_digest,
                           &right->joining_device_identity_digest) &&
           principal_equal(&left->authority_principal,
                           &right->authority_principal) &&
           left->authority_generation == right->authority_generation &&
           left->device_nonce == right->device_nonce &&
           left->authority_nonce == right->authority_nonce &&
           left->transaction_id == right->transaction_id &&
           left->lease_freshness_challenge_nonce ==
               right->lease_freshness_challenge_nonce &&
           left->realm_id == right->realm_id &&
           left->proposed_address == right->proposed_address &&
           left->address_binding_generation ==
               right->address_binding_generation &&
           left->authority_address == right->authority_address &&
           left->authority_binding_generation ==
               right->authority_binding_generation &&
           left->selected_link_instance_id ==
               right->selected_link_instance_id &&
           memcmp(left->binding_lease_id, right->binding_lease_id,
                  sizeof(left->binding_lease_id)) == 0 &&
           left->binding_lease_duration_us ==
               right->binding_lease_duration_us &&
           left->authority_lease_sequence ==
               right->authority_lease_sequence &&
           left->authority_lease_duration_us ==
               right->authority_lease_duration_us &&
           left->freshness_max_remaining_lease_us ==
               right->freshness_max_remaining_lease_us &&
           memcmp(left->durable_fence_token, right->durable_fence_token,
                  sizeof(left->durable_fence_token)) == 0 &&
           memcmp(left->allocation_high_water_digest,
                  right->allocation_high_water_digest,
                  sizeof(left->allocation_high_water_digest)) == 0 &&
           memcmp(left->quorum_config_digest, right->quorum_config_digest,
                  sizeof(left->quorum_config_digest)) == 0 &&
           memcmp(left->signer_set_digest, right->signer_set_digest,
                  sizeof(left->signer_set_digest)) == 0 &&
           memcmp(left->threshold_proof_digest,
                  right->threshold_proof_digest,
                  sizeof(left->threshold_proof_digest)) == 0 &&
           memcmp(left->freshness_proof_transcript_hash,
                  right->freshness_proof_transcript_hash,
                  sizeof(left->freshness_proof_transcript_hash)) == 0 &&
           left->authority_signer_count == right->authority_signer_count &&
           left->authority_quorum_threshold ==
               right->authority_quorum_threshold &&
           left->binding_mode == right->binding_mode &&
           left->selected_hop_suite == right->selected_hop_suite &&
           left->selected_hop_key_id == right->selected_hop_key_id &&
           left->selected_hop_key_generation ==
               right->selected_hop_key_generation &&
           left->selected_e2e_mode == right->selected_e2e_mode &&
           left->selected_e2e_suite == right->selected_e2e_suite &&
           left->selected_e2e_key_id == right->selected_e2e_key_id &&
           left->selected_e2e_key_generation ==
               right->selected_e2e_key_generation &&
           left->selected_session_generation ==
               right->selected_session_generation &&
           left->selected_link_instance_generation ==
               right->selected_link_instance_generation &&
           memcmp(left->prior_messages_hash, right->prior_messages_hash,
                  sizeof(left->prior_messages_hash)) == 0;
}

static bool optional_binding_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right)
{
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return ucn_v6_binding_key_equal(left, right);
}

static ucn_v6_result_t authorize_event(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    uint64_t now_us,
    const ucn_v6_bootstrap_evidence_t *evidence)
{
    ucn_v6_bootstrap_key_t key_copy;
    ucn_v6_bootstrap_transcript_t transcript_copy;
    ucn_v6_binding_key_t binding_copy;
    ucn_v6_bootstrap_evidence_t evidence_copy;
    const ucn_v6_binding_key_t *binding_argument = NULL;
    uint64_t violations_before;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    if (!evidence_is_valid(evidence)) {
        return UCN_V6_ERR_SECURITY;
    }
    key_copy = *key;
    transcript_copy = *transcript;
    evidence_copy = *evidence;
    if (existing_binding != NULL) {
        binding_copy = *existing_binding;
        binding_argument = &binding_copy;
    }
    violations_before = ucn_v6_callback_gate_violation_count(
        owner->callback_gate);
    if (violations_before == UINT64_MAX ||
        ucn_v6_callback_gate_try_enter(owner->callback_gate, owner) !=
            UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    result = owner->verifier.authorize_event(
        owner->verifier.context, event, flow, &key_copy, &transcript_copy,
        binding_argument, now_us, &evidence_copy);
    leave_result = callback_scope_finish(
        owner->callback_gate, owner, violations_before, result);
    if (leave_result == UCN_V6_ERR_STATE && result == UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    return leave_result == UCN_V6_OK ? UCN_V6_OK : UCN_V6_ERR_SECURITY;
}

static ucn_v6_bootstrap_pending_t *pending_array(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow)
{
    return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ? owner->join_pending :
                                                owner->reauth_pending;
}

static const ucn_v6_bootstrap_pending_t *pending_array_const(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow)
{
    return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ? owner->join_pending :
                                                owner->reauth_pending;
}

static ucn_v6_bootstrap_pending_t *find_pending(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key)
{
    size_t index;
    ucn_v6_bootstrap_pending_t *slots = pending_array(owner, flow);

    for (index = 0U; index < owner->config.max_pending; ++index) {
        if (slots[index].occupied && key_equal(&slots[index].key, key)) {
            return &slots[index];
        }
    }
    return NULL;
}

static ucn_v6_bootstrap_link_budget_t *find_budget(
    ucn_v6_bootstrap_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation)
{
    size_t index;
    ucn_v6_bootstrap_link_budget_t *empty = NULL;

    for (index = 0U; index < UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS; ++index) {
        if (owner->budgets[index].occupied &&
            owner->budgets[index].ingress_link_id == link_id &&
            owner->budgets[index].ingress_link_generation == link_generation) {
            return &owner->budgets[index];
        }
        if (!owner->budgets[index].occupied && empty == NULL) {
            empty = &owner->budgets[index];
        }
    }
    return empty;
}

static bool budget_has_pending(
    const ucn_v6_bootstrap_owner_t *owner,
    const ucn_v6_bootstrap_link_budget_t *budget)
{
    const ucn_v6_bootstrap_pending_t *arrays[2];
    size_t array_index;
    size_t slot_index;

    arrays[0] = owner->join_pending;
    arrays[1] = owner->reauth_pending;
    for (array_index = 0U; array_index < 2U; ++array_index) {
        for (slot_index = 0U; slot_index < owner->config.max_pending;
             ++slot_index) {
            if (arrays[array_index][slot_index].occupied &&
                arrays[array_index][slot_index].key.ingress_link_id ==
                    budget->ingress_link_id &&
                arrays[array_index][slot_index]
                        .key.ingress_link_generation ==
                    budget->ingress_link_generation) {
                return true;
            }
        }
    }
    return false;
}

static void pending_resource_usage(
    const ucn_v6_bootstrap_owner_t *owner,
    const ucn_v6_bootstrap_key_t *key,
    size_t *global_count,
    size_t *link_count,
    size_t *peer_count)
{
    const ucn_v6_bootstrap_pending_t *arrays[2];
    size_t array_index;
    size_t slot_index;

    *global_count = 0U;
    *link_count = 0U;
    *peer_count = 0U;
    arrays[0] = owner->join_pending;
    arrays[1] = owner->reauth_pending;
    for (array_index = 0U; array_index < 2U; ++array_index) {
        for (slot_index = 0U; slot_index < owner->config.max_pending;
             ++slot_index) {
            const ucn_v6_bootstrap_pending_t *pending =
                &arrays[array_index][slot_index];
            if (!pending->occupied) {
                continue;
            }
            ++(*global_count);
            if (pending->key.ingress_link_id != key->ingress_link_id ||
                pending->key.ingress_link_generation !=
                key->ingress_link_generation) {
                continue;
            }
            ++(*link_count);
            if (pending->key.local_peer_discriminator ==
                    key->local_peer_discriminator &&
                principal_equal(&pending->key.identity_digest,
                                &key->identity_digest)) {
                ++(*peer_count);
            }
        }
    }
}

ucn_v6_result_t ucn_v6_bootstrap_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_bootstrap_config_t *config,
    const ucn_v6_bootstrap_verifier_ops_t *verifier,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_bootstrap_owner_t **owner_out)
{
    ucn_v6_bootstrap_owner_t *owner;
    ucn_v6_result_t result;

    if (owner_out == NULL || config == NULL || verifier == NULL ||
        verifier->authorize_event == NULL || callback_gate == NULL ||
        ucn_v6_callback_gate_violation_count(callback_gate) == UINT64_MAX ||
        ucn_v6_callback_gate_is_active(callback_gate) ||
        config->max_pending == 0U ||
        config->max_pending > UCN_V6_BOOTSTRAP_MAX_PENDING ||
        config->max_pending_per_link == 0U ||
        config->max_pending_per_link > config->max_pending ||
        config->token_burst == 0U || config->tokens_per_second == 0U ||
        config->pending_timeout_us == 0U) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(
        (const ucn_v6_feature_manifest_t *)manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(*owner),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    owner = (ucn_v6_bootstrap_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_BOOTSTRAP_OWNER_MAGIC;
    owner->schema = UCN_V6_STORAGE_LAYOUT;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->config = *config;
    owner->verifier = *verifier;
    owner->callback_gate = callback_gate;
    owner->initialized = true;
    owner->canary = UCN_V6_BOOTSTRAP_OWNER_CANARY;
    *owner_out = owner;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_admit_initial_hello(
    ucn_v6_bootstrap_owner_t *owner,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    uint64_t now_us,
    size_t request_bytes,
    size_t response_bytes)
{
    ucn_v6_bootstrap_link_budget_t *budget;
    uint64_t elapsed_seconds;
    uint64_t missing_tokens;
    uint64_t seconds_to_full;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (ingress_link_id == 0U ||
        ingress_link_generation == 0U ||
        ingress_link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        request_bytes == 0U ||
        response_bytes > request_bytes) {
        return UCN_V6_ERR_ARGUMENT;
    }
    budget = find_budget(owner, ingress_link_id, ingress_link_generation);
    if (budget == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (!budget->occupied) {
        budget->occupied = true;
        budget->ingress_link_id = ingress_link_id;
        budget->ingress_link_generation = ingress_link_generation;
        budget->tokens = owner->config.token_burst;
        budget->last_refill_us = now_us;
        budget->last_activity_us = now_us;
    } else {
        if (now_us < budget->last_refill_us) {
            return UCN_V6_ERR_STATE;
        }
        elapsed_seconds = (now_us - budget->last_refill_us) /
                          UINT64_C(1000000);
        if (elapsed_seconds != 0U) {
            missing_tokens = (uint64_t)owner->config.token_burst -
                             (uint64_t)budget->tokens;
            seconds_to_full =
                (missing_tokens + owner->config.tokens_per_second - 1U) /
                owner->config.tokens_per_second;
            if (elapsed_seconds >= seconds_to_full) {
                budget->tokens = owner->config.token_burst;
            } else {
                budget->tokens = (uint8_t)(
                    (uint64_t)budget->tokens +
                    elapsed_seconds * owner->config.tokens_per_second);
            }
            if (elapsed_seconds > UINT64_MAX / UINT64_C(1000000)) {
                budget->last_refill_us = now_us;
            } else {
                budget->last_refill_us +=
                    elapsed_seconds * UINT64_C(1000000);
            }
        }
    }
    if (budget->tokens == 0U) {
        return UCN_V6_ERR_ACCESS;
    }
    --budget->tokens;
    budget->last_activity_us = now_us;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_open_after_cookie(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    const ucn_v6_bootstrap_evidence_t *cookie_evidence,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t *slots;
    ucn_v6_bootstrap_pending_t *duplicate;
    size_t index;
    size_t global_count;
    size_t link_count = 0U;
    size_t peer_count;
    ucn_v6_bootstrap_pending_t *empty = NULL;
    uint64_t deadline;
    ucn_v6_result_t result;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!flow_is_valid(flow) ||
        !key_is_valid(key) || !transcript_is_valid(transcript) ||
        transcript->flow != flow ||
        !principal_equal(&key->identity_digest,
                         &transcript->joining_device_identity_digest) ||
        key->transaction_id != transcript->transaction_id ||
        key->ingress_link_id != transcript->selected_link_instance_id ||
        key->ingress_link_generation !=
            transcript->selected_link_instance_generation) {
        return UCN_V6_ERR_SECURITY;
    }
    if (flow == UCN_V6_BOOTSTRAP_FLOW_JOIN) {
        if (existing_binding != NULL &&
            ucn_v6_binding_key_is_valid(existing_binding)) {
            return UCN_V6_ERR_STATE;
        }
    } else {
        if (!ucn_v6_binding_key_is_valid(existing_binding) ||
            existing_binding->realm_id != transcript->realm_id ||
            existing_binding->node_address != transcript->proposed_address ||
            existing_binding->binding_generation !=
                transcript->address_binding_generation) {
            return UCN_V6_ERR_STATE;
        }
    }

    duplicate = find_pending(owner, flow, key);
    if (duplicate != NULL) {
        if (!transcript_equal(&duplicate->transcript, transcript) ||
            !optional_binding_equal(
                flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH ?
                    &duplicate->existing_binding : NULL,
                existing_binding)) {
            return UCN_V6_ERR_REPLAY;
        }
        return authorize_event(
            owner, UCN_V6_BOOTSTRAP_EVENT_COOKIE, flow, key, transcript,
            existing_binding, now_us, cookie_evidence);
    }
    if (now_us > UINT64_MAX - owner->config.pending_timeout_us) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    deadline = now_us + owner->config.pending_timeout_us;
    if (deadline == 0U) {
        return UCN_V6_ERR_EXHAUSTED;
    }

    pending_resource_usage(owner, key, &global_count, &link_count,
                           &peer_count);
    if (global_count >= owner->config.max_pending ||
        link_count >= owner->config.max_pending_per_link ||
        peer_count != 0U) {
        return UCN_V6_ERR_NO_SPACE;
    }

    slots = pending_array(owner, flow);
    for (index = 0U; index < owner->config.max_pending; ++index) {
        if (!slots[index].occupied && empty == NULL) {
            empty = &slots[index];
        }
    }
    if (empty == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }

    result = authorize_event(owner, UCN_V6_BOOTSTRAP_EVENT_COOKIE, flow, key,
                             transcript, existing_binding, now_us,
                             cookie_evidence);
    if (result != UCN_V6_OK) {
        return result;
    }

    memset(empty, 0, sizeof(*empty));
    empty->occupied = true;
    empty->flow = flow;
    empty->phase = UCN_V6_BOOTSTRAP_COOKIE_VERIFIED;
    empty->key = *key;
    empty->transcript = *transcript;
    if (existing_binding != NULL) {
        empty->existing_binding = *existing_binding;
    }
    empty->deadline_us = deadline;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_advance(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_event_t event,
    const ucn_v6_bootstrap_evidence_t *evidence,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t *pending;
    ucn_v6_bootstrap_phase_t next_phase;
    ucn_v6_result_t result;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!flow_is_valid(flow) ||
        !key_is_valid(key) || !transcript_is_valid(transcript) ||
        transcript->flow != flow) {
        return UCN_V6_ERR_ARGUMENT;
    }
    pending = find_pending(owner, flow, key);
    if (pending == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (!transcript_equal(&pending->transcript, transcript)) {
        return UCN_V6_ERR_REPLAY;
    }
    if (now_us >= pending->deadline_us) {
        return UCN_V6_ERR_TIMEOUT;
    }
    if (event == UCN_V6_BOOTSTRAP_EVENT_ABORT) {
        if (pending->phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE ||
            pending->phase == UCN_V6_BOOTSTRAP_ABORTED) {
            return UCN_V6_ERR_STATE;
        }
        result = authorize_event(
            owner, event, flow, key, transcript,
            flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH ?
                &pending->existing_binding : NULL,
            now_us, evidence);
        if (result != UCN_V6_OK) {
            return result;
        }
        pending->phase = UCN_V6_BOOTSTRAP_ABORTED;
        return UCN_V6_OK;
    }

    next_phase = pending->phase;
    if (event == UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF &&
        pending->phase == UCN_V6_BOOTSTRAP_COOKIE_VERIFIED) {
        next_phase = UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED;
    } else if (event == UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF &&
               pending->phase == UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED) {
        next_phase = UCN_V6_BOOTSTRAP_DEVICE_VERIFIED;
    } else if (flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
               event == UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER &&
               pending->phase == UCN_V6_BOOTSTRAP_DEVICE_VERIFIED) {
        next_phase = UCN_V6_BOOTSTRAP_ADDRESS_OFFERED;
    } else if (flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
               event == UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT &&
               pending->phase == UCN_V6_BOOTSTRAP_ADDRESS_OFFERED) {
        next_phase = UCN_V6_BOOTSTRAP_DEVICE_COMMITTED;
    } else if (flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
               event == UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE &&
               pending->phase == UCN_V6_BOOTSTRAP_DEVICE_COMMITTED) {
        next_phase = UCN_V6_BOOTSTRAP_FINAL_DURABLE;
    } else if (flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH &&
               event == UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE &&
               pending->phase == UCN_V6_BOOTSTRAP_DEVICE_VERIFIED) {
        next_phase = UCN_V6_BOOTSTRAP_FINAL_DURABLE;
    } else if ((event == UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF &&
                pending->phase == UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED) ||
               (event == UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF &&
                pending->phase == UCN_V6_BOOTSTRAP_DEVICE_VERIFIED) ||
               (event == UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER &&
                pending->phase == UCN_V6_BOOTSTRAP_ADDRESS_OFFERED) ||
               (event == UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT &&
                pending->phase == UCN_V6_BOOTSTRAP_DEVICE_COMMITTED) ||
               (event == UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE &&
                pending->phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE)) {
        next_phase = pending->phase;
    } else {
        return UCN_V6_ERR_STATE;
    }
    result = authorize_event(
        owner, event, flow, key, transcript,
        flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH ?
            &pending->existing_binding : NULL,
        now_us, evidence);
    if (result != UCN_V6_OK) {
        return result;
    }
    pending->phase = next_phase;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_copy_pending(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    ucn_v6_bootstrap_pending_t *pending)
{
    const ucn_v6_bootstrap_pending_t *slots;
    size_t index;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!flow_is_valid(flow) ||
        !key_is_valid(key) || pending == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slots = pending_array_const(owner, flow);
    for (index = 0U; index < owner->config.max_pending; ++index) {
        if (slots[index].occupied && key_equal(&slots[index].key, key)) {
            *pending = slots[index];
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_bootstrap_validate_final(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    uint64_t now_us)
{
    const ucn_v6_bootstrap_pending_t *slots;
    size_t index;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!flow_is_valid(flow) ||
        !key_is_valid(key) || !transcript_is_valid(transcript) ||
        transcript->flow != flow) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slots = pending_array_const(owner, flow);
    for (index = 0U; index < owner->config.max_pending; ++index) {
        if (!slots[index].occupied || !key_equal(&slots[index].key, key)) {
            continue;
        }
        if (!transcript_equal(&slots[index].transcript, transcript)) {
            return UCN_V6_ERR_REPLAY;
        }
        if (now_us >= slots[index].deadline_us) {
            return UCN_V6_ERR_TIMEOUT;
        }
        return slots[index].phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE ?
                   UCN_V6_OK : UCN_V6_ERR_STATE;
    }
    return UCN_V6_ERR_NOT_FOUND;
}

size_t ucn_v6_bootstrap_expire(
    ucn_v6_bootstrap_owner_t *owner,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t *arrays[2];
    size_t array_index;
    size_t slot_index;
    size_t expired = 0U;

    if (!owner_is_valid(owner) || callback_reentry_is_blocked(owner)) {
        return 0U;
    }
    arrays[0] = owner->join_pending;
    arrays[1] = owner->reauth_pending;
    for (array_index = 0U; array_index < 2U; ++array_index) {
        for (slot_index = 0U; slot_index < owner->config.max_pending;
             ++slot_index) {
            if (arrays[array_index][slot_index].occupied &&
                now_us >= arrays[array_index][slot_index].deadline_us) {
                memset(&arrays[array_index][slot_index], 0,
                       sizeof(arrays[array_index][slot_index]));
                ++expired;
            }
        }
    }
    for (slot_index = 0U;
         slot_index < UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS;
         ++slot_index) {
        ucn_v6_bootstrap_link_budget_t *budget =
            &owner->budgets[slot_index];
        uint64_t seconds_to_full =
            ((uint64_t)owner->config.token_burst +
             (uint64_t)owner->config.tokens_per_second - 1U) /
            (uint64_t)owner->config.tokens_per_second;
        uint64_t refill_idle_us =
            seconds_to_full > UINT64_MAX / UINT64_C(1000000) ?
                UINT64_MAX : seconds_to_full * UINT64_C(1000000);
        uint64_t reclaim_idle_us =
            owner->config.pending_timeout_us > refill_idle_us ?
                owner->config.pending_timeout_us : refill_idle_us;
        if (budget->occupied && !budget_has_pending(owner, budget) &&
            now_us >= budget->last_activity_us &&
            now_us - budget->last_activity_us >=
                reclaim_idle_us) {
            memset(budget, 0, sizeof(*budget));
        }
    }
    return expired;
}
