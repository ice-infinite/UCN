#include "ucn/v6/ucn_v6_bootstrap.h"

#include <string.h>

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

static bool flow_is_valid(ucn_v6_bootstrap_flow_t flow)
{
    return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ||
           flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH;
}

static bool key_is_valid(const ucn_v6_bootstrap_key_t *key)
{
    return key != NULL && key->ingress_link_generation != 0U &&
           key->local_peer_discriminator != 0U &&
           ucn_v6_principal_is_valid(&key->identity_digest) &&
           key->transaction_id != 0U;
}

static bool key_equal(
    const ucn_v6_bootstrap_key_t *left,
    const ucn_v6_bootstrap_key_t *right)
{
    return left->ingress_link_generation == right->ingress_link_generation &&
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
           bytes_nonzero(transcript->binding_lease_id,
                         sizeof(transcript->binding_lease_id)) &&
           transcript->binding_lease_duration_us != 0U &&
           transcript->authority_lease_sequence != 0U &&
           transcript->authority_lease_sequence <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           transcript->selected_hop_suite != 0U &&
           transcript->selected_hop_key_context != 0U &&
           transcript->selected_e2e_suite != 0U &&
           transcript->selected_e2e_key_context != 0U &&
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
           memcmp(left->binding_lease_id, right->binding_lease_id,
                  sizeof(left->binding_lease_id)) == 0 &&
           left->binding_lease_duration_us ==
               right->binding_lease_duration_us &&
           left->authority_lease_sequence ==
               right->authority_lease_sequence &&
           left->selected_hop_suite == right->selected_hop_suite &&
           left->selected_hop_key_context ==
               right->selected_hop_key_context &&
           left->selected_e2e_suite == right->selected_e2e_suite &&
           left->selected_e2e_key_context ==
               right->selected_e2e_key_context &&
           memcmp(left->prior_messages_hash, right->prior_messages_hash,
                  sizeof(left->prior_messages_hash)) == 0;
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
    uint32_t link_generation)
{
    size_t index;
    ucn_v6_bootstrap_link_budget_t *empty = NULL;

    for (index = 0U; index < UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS; ++index) {
        if (owner->budgets[index].occupied &&
            owner->budgets[index].ingress_link_generation == link_generation) {
            return &owner->budgets[index];
        }
        if (!owner->budgets[index].occupied && empty == NULL) {
            empty = &owner->budgets[index];
        }
    }
    return empty;
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
            if (pending->key.ingress_link_generation !=
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

ucn_v6_result_t ucn_v6_bootstrap_owner_init(
    ucn_v6_bootstrap_owner_t *owner,
    const ucn_v6_bootstrap_config_t *config)
{
    if (owner == NULL || config == NULL || config->max_pending == 0U ||
        config->max_pending > UCN_V6_BOOTSTRAP_MAX_PENDING ||
        config->max_pending_per_link == 0U ||
        config->max_pending_per_link > config->max_pending ||
        config->token_burst == 0U || config->tokens_per_second == 0U ||
        config->pending_timeout_us == 0U) {
        return UCN_V6_ERR_CONFIG;
    }
    memset(owner, 0, sizeof(*owner));
    owner->config = *config;
    owner->initialized = true;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_admit_initial_hello(
    ucn_v6_bootstrap_owner_t *owner,
    uint32_t ingress_link_generation,
    uint64_t now_us,
    size_t request_bytes,
    size_t response_bytes)
{
    ucn_v6_bootstrap_link_budget_t *budget;
    uint64_t elapsed_seconds;
    uint64_t missing_tokens;
    uint64_t seconds_to_full;

    if (owner == NULL || !owner->initialized ||
        ingress_link_generation == 0U || request_bytes == 0U ||
        response_bytes > request_bytes) {
        return UCN_V6_ERR_ARGUMENT;
    }
    budget = find_budget(owner, ingress_link_generation);
    if (budget == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (!budget->occupied) {
        budget->occupied = true;
        budget->ingress_link_generation = ingress_link_generation;
        budget->tokens = owner->config.token_burst;
        budget->last_refill_us = now_us;
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
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_open_after_cookie(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    bool cookie_verified,
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

    if (owner == NULL || !owner->initialized || !flow_is_valid(flow) ||
        !key_is_valid(key) || !transcript_is_valid(transcript) ||
        !cookie_verified ||
        !principal_equal(&key->identity_digest,
                         &transcript->joining_device_identity_digest) ||
        key->transaction_id != transcript->transaction_id) {
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
        return transcript_equal(&duplicate->transcript, transcript) ?
                   UCN_V6_OK : UCN_V6_ERR_REPLAY;
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
    bool proof_verified,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t *pending;
    ucn_v6_bootstrap_phase_t next_phase;

    if (owner == NULL || !owner->initialized || !flow_is_valid(flow) ||
        !key_is_valid(key) || !transcript_is_valid(transcript)) {
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
        pending->phase = UCN_V6_BOOTSTRAP_ABORTED;
        return UCN_V6_OK;
    }
    if (!proof_verified) {
        return UCN_V6_ERR_SECURITY;
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
        return UCN_V6_OK;
    } else {
        return UCN_V6_ERR_STATE;
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

    if (owner == NULL || !owner->initialized || !flow_is_valid(flow) ||
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

size_t ucn_v6_bootstrap_expire(
    ucn_v6_bootstrap_owner_t *owner,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t *arrays[2];
    size_t array_index;
    size_t slot_index;
    size_t expired = 0U;

    if (owner == NULL || !owner->initialized) {
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
    return expired;
}
