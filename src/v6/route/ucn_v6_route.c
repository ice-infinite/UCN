#include "../internal/ucn_v6_route_private.h"

#include <limits.h>
#include <string.h>

#define UCN_V6_ROUTE_SCHEMA UINT16_C(1)

typedef char ucn_v6_route_owner_storage_must_fit[
    sizeof(struct ucn_v6_route_owner) <= UCN_V6_ROUTE_OWNER_STORAGE_BYTES ?
        1 : -1];

static void saturating_increment(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
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

static bool principal_equal(const ucn_v6_principal_t *left,
                            const ucn_v6_principal_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool session_is_valid(const ucn_v6_session_key_t *session)
{
    return session != NULL &&
           ucn_v6_principal_is_valid(&session->principal) &&
           ucn_v6_binding_key_is_valid(&session->binding) &&
           session->session_generation != 0U &&
           session->session_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool session_equal(const ucn_v6_session_key_t *left,
                          const ucn_v6_session_key_t *right)
{
    return left != NULL && right != NULL &&
           principal_equal(&left->principal, &right->principal) &&
           ucn_v6_binding_key_equal(&left->binding, &right->binding) &&
           left->session_generation == right->session_generation;
}

static bool domain_is_valid(const ucn_v6_route_domain_t *domain)
{
    return domain != NULL &&
           ucn_v6_principal_is_valid(&domain->origin_principal) &&
           ucn_v6_principal_is_valid(&domain->destination_principal) &&
           ucn_v6_binding_key_is_valid(&domain->origin_binding) &&
           ucn_v6_binding_key_is_valid(&domain->destination_binding) &&
           domain->origin_binding.realm_id ==
               domain->destination_binding.realm_id &&
           domain->origin_session_generation != 0U &&
           domain->origin_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool domain_equal(const ucn_v6_route_domain_t *left,
                         const ucn_v6_route_domain_t *right)
{
    return left != NULL && right != NULL &&
           principal_equal(&left->origin_principal,
                           &right->origin_principal) &&
           ucn_v6_binding_key_equal(&left->origin_binding,
                                    &right->origin_binding) &&
           left->origin_session_generation ==
               right->origin_session_generation &&
           principal_equal(&left->destination_principal,
                           &right->destination_principal) &&
           ucn_v6_binding_key_equal(&left->destination_binding,
                                    &right->destination_binding);
}

static bool path_capability_equal(const ucn_v6_path_capability_t *left,
                                  const ucn_v6_path_capability_t *right)
{
    return left != NULL && right != NULL &&
           left->valid == right->valid &&
           principal_equal(&left->destination_principal,
                           &right->destination_principal) &&
           ucn_v6_binding_key_equal(&left->destination_binding,
                                    &right->destination_binding) &&
           left->session_generation == right->session_generation &&
           left->destination_link_instance_generation ==
               right->destination_link_instance_generation &&
           memcmp(left->destination_capability_digest,
                  right->destination_capability_digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           left->route_generation == right->route_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation &&
           left->path_frame_mtu == right->path_frame_mtu &&
           left->payload_budget == right->payload_budget &&
           left->fragment_data_budget == right->fragment_data_budget &&
           left->feature_bits == right->feature_bits &&
           left->hop_suite_bits == right->hop_suite_bits &&
           left->e2e_suite_bits == right->e2e_suite_bits &&
           left->max_message_class == right->max_message_class &&
           left->max_window == right->max_window &&
           left->max_concurrency == right->max_concurrency &&
           left->timestamp_capability_bits ==
               right->timestamp_capability_bits &&
           left->timestamp_uncertainty_us ==
               right->timestamp_uncertainty_us &&
           left->deadline_us == right->deadline_us;
}

static bool route_path_equal(const ucn_v6_route_path_t *left,
                             const ucn_v6_route_path_t *right)
{
    return left != NULL && right != NULL &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation &&
           session_equal(&left->next_hop, &right->next_hop) &&
           left->egress_link_id == right->egress_link_id &&
           left->egress_link_generation == right->egress_link_generation &&
           left->hop_count == right->hop_count &&
           left->priority == right->priority &&
           left->weight == right->weight &&
           left->available == right->available &&
           path_capability_equal(&left->capability, &right->capability);
}

static bool route_path_is_valid(const ucn_v6_route_domain_t *domain,
                                uint32_t route_generation,
                                const ucn_v6_route_path_t *path)
{
    return domain_is_valid(domain) && path != NULL &&
           path->path_id != 0U && path->path_generation != 0U &&
           path->path_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           session_is_valid(&path->next_hop) && path->egress_link_id != 0U &&
           path->egress_link_generation != 0U &&
           path->egress_link_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           path->hop_count != 0U && path->weight != 0U && path->available &&
           path->capability.valid &&
           path->next_hop.binding.realm_id == domain->origin_binding.realm_id &&
           principal_equal(&path->capability.destination_principal,
                           &domain->destination_principal) &&
           ucn_v6_binding_key_equal(&path->capability.destination_binding,
                                    &domain->destination_binding) &&
           path->capability.route_generation == route_generation &&
           path->capability.path_id == path->path_id &&
           path->capability.path_generation == path->path_generation &&
           path->capability.deadline_us != 0U &&
           bytes_nonzero(path->capability.destination_capability_digest,
                         UCN_V6_CAPABILITY_DIGEST_BYTES);
}

static bool proposal_is_valid(const ucn_v6_route_proposal_t *proposal)
{
    size_t left;
    size_t right;
    if (proposal == NULL || !domain_is_valid(&proposal->domain) ||
        proposal->route_generation == 0U ||
        proposal->route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        proposal->path_count == 0U ||
        proposal->path_count > UCN_V6_ROUTE_MAX_PATHS ||
        proposal->preferred_path_index >= proposal->path_count) {
        return false;
    }
    for (left = 0U; left < proposal->path_count; ++left) {
        if (!route_path_is_valid(&proposal->domain,
                                 proposal->route_generation,
                                 &proposal->paths[left])) {
            return false;
        }
        for (right = left + 1U; right < proposal->path_count; ++right) {
            if (proposal->paths[left].path_id ==
                    proposal->paths[right].path_id ||
                (session_equal(&proposal->paths[left].next_hop,
                               &proposal->paths[right].next_hop) &&
                 proposal->paths[left].egress_link_id ==
                     proposal->paths[right].egress_link_id)) {
                return false;
            }
        }
    }
    return true;
}

static bool owner_is_valid(const ucn_v6_route_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_ROUTE_OWNER_MAGIC &&
           owner->schema == UCN_V6_ROUTE_SCHEMA && owner->initialized &&
           !owner->faulted && owner->canary == UCN_V6_ROUTE_OWNER_CANARY &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH;
}

static bool deadline_build(uint64_t now_us,
                           uint64_t duration_us,
                           uint64_t *deadline_us)
{
    if (duration_us == 0U || deadline_us == NULL ||
        UINT64_MAX - now_us < duration_us) {
        return false;
    }
    *deadline_us = now_us + duration_us;
    return *deadline_us != 0U;
}

static ucn_v6_route_set_slot_t *find_set(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        if (owner->sets[index].occupied &&
            domain_equal(&owner->sets[index].current.domain, domain)) {
            return &owner->sets[index];
        }
    }
    return NULL;
}

static const ucn_v6_route_set_slot_t *find_set_const(
    const ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        if (owner->sets[index].occupied &&
            domain_equal(&owner->sets[index].current.domain, domain)) {
            return &owner->sets[index];
        }
    }
    return NULL;
}

static ucn_v6_route_set_slot_t *find_free_set(ucn_v6_route_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        if (!owner->sets[index].occupied) {
            return &owner->sets[index];
        }
    }
    return NULL;
}

static ucn_v6_route_candidate_view_t *find_candidate(
    ucn_v6_route_owner_t *owner,
    uint64_t candidate_transaction_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        if (owner->candidates[index].value.occupied &&
            owner->candidates[index].value.candidate_transaction_id ==
                candidate_transaction_id) {
            return &owner->candidates[index].value;
        }
    }
    return NULL;
}

static const ucn_v6_route_candidate_view_t *find_candidate_const(
    const ucn_v6_route_owner_t *owner,
    uint64_t candidate_transaction_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        if (owner->candidates[index].value.occupied &&
            owner->candidates[index].value.candidate_transaction_id ==
                candidate_transaction_id) {
            return &owner->candidates[index].value;
        }
    }
    return NULL;
}

static ucn_v6_route_candidate_view_t *find_free_candidate(
    ucn_v6_route_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        if (!owner->candidates[index].value.occupied) {
            return &owner->candidates[index].value;
        }
    }
    return NULL;
}

static ucn_v6_route_domain_state_t *find_domain_state(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        if (owner->domains[index].occupied &&
            domain_equal(&owner->domains[index].domain, domain)) {
            return &owner->domains[index];
        }
    }
    return NULL;
}

static ucn_v6_route_domain_state_t *find_free_domain_state(
    ucn_v6_route_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        if (!owner->domains[index].occupied) {
            return &owner->domains[index];
        }
    }
    return NULL;
}

static void digest_byte(uint32_t state[4], uint8_t byte)
{
    size_t index;
    static const uint32_t prime[4] = {
        UINT32_C(16777619), UINT32_C(2246822519),
        UINT32_C(3266489917), UINT32_C(668265263)
    };
    for (index = 0U; index < 4U; ++index) {
        state[index] ^= (uint32_t)(byte + (uint8_t)(index * 0x31U));
        state[index] *= prime[index];
        state[index] ^= state[index] >> 13U;
    }
}

static void digest_u16(uint32_t state[4], uint16_t value)
{
    digest_byte(state, (uint8_t)(value >> 8U));
    digest_byte(state, (uint8_t)value);
}

static void digest_u32(uint32_t state[4], uint32_t value)
{
    digest_u16(state, (uint16_t)(value >> 16U));
    digest_u16(state, (uint16_t)value);
}

static void digest_u64(uint32_t state[4], uint64_t value)
{
    digest_u32(state, (uint32_t)(value >> 32U));
    digest_u32(state, (uint32_t)value);
}

static void digest_bytes(uint32_t state[4], const uint8_t *bytes, size_t count)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        digest_byte(state, bytes[index]);
    }
}

static void digest_binding(uint32_t state[4],
                           const ucn_v6_binding_key_t *binding)
{
    digest_u32(state, binding->realm_id);
    digest_u32(state, binding->node_address);
    digest_u32(state, binding->binding_generation);
}

static void proposal_digest(const ucn_v6_route_proposal_t *proposal,
                            uint8_t output[UCN_V6_ROUTE_PROPOSAL_DIGEST_BYTES])
{
    uint32_t state[4] = {
        UINT32_C(2166136261), UINT32_C(0x9E3779B9),
        UINT32_C(0x85EBCA77), UINT32_C(0xC2B2AE3D)
    };
    size_t path_index;
    size_t word;
    digest_bytes(state, proposal->domain.origin_principal.bytes, 16U);
    digest_binding(state, &proposal->domain.origin_binding);
    digest_u32(state, proposal->domain.origin_session_generation);
    digest_bytes(state, proposal->domain.destination_principal.bytes, 16U);
    digest_binding(state, &proposal->domain.destination_binding);
    digest_u32(state, proposal->route_generation);
    digest_byte(state, proposal->path_count);
    digest_byte(state, proposal->preferred_path_index);
    for (path_index = 0U; path_index < proposal->path_count; ++path_index) {
        const ucn_v6_route_path_t *path = &proposal->paths[path_index];
        const ucn_v6_path_capability_t *capability = &path->capability;
        digest_u16(state, path->path_id);
        digest_u32(state, path->path_generation);
        digest_bytes(state, path->next_hop.principal.bytes, 16U);
        digest_binding(state, &path->next_hop.binding);
        digest_u32(state, path->next_hop.session_generation);
        digest_u16(state, path->egress_link_id);
        digest_u32(state, path->egress_link_generation);
        digest_byte(state, path->hop_count);
        digest_u16(state, path->priority);
        digest_u16(state, path->weight);
        digest_byte(state, path->available ? 1U : 0U);
        digest_u32(state, capability->destination_link_instance_generation);
        digest_u32(state, capability->session_generation);
        digest_bytes(state, capability->destination_capability_digest,
                     UCN_V6_CAPABILITY_DIGEST_BYTES);
        digest_u32(state, capability->path_frame_mtu);
        digest_u32(state, capability->payload_budget);
        digest_u32(state, capability->fragment_data_budget);
        digest_u32(state, capability->feature_bits);
        digest_u32(state, capability->hop_suite_bits);
        digest_u32(state, capability->e2e_suite_bits);
        digest_byte(state, (uint8_t)capability->max_message_class);
        digest_u16(state, capability->max_window);
        digest_u16(state, capability->max_concurrency);
        digest_u16(state, capability->timestamp_capability_bits);
        digest_u32(state, capability->timestamp_uncertainty_us);
        digest_u64(state, capability->deadline_us);
    }
    for (word = 0U; word < 4U; ++word) {
        output[word * 4U] = (uint8_t)(state[word] >> 24U);
        output[word * 4U + 1U] = (uint8_t)(state[word] >> 16U);
        output[word * 4U + 2U] = (uint8_t)(state[word] >> 8U);
        output[word * 4U + 3U] = (uint8_t)state[word];
    }
}

static bool activation_equal_candidate(
    const ucn_v6_route_activation_t *activation,
    const ucn_v6_route_candidate_view_t *candidate)
{
    return activation != NULL && candidate != NULL &&
           activation->candidate_transaction_id ==
               candidate->candidate_transaction_id &&
           domain_equal(&activation->domain, &candidate->proposal.domain) &&
           activation->route_generation ==
               candidate->proposal.route_generation &&
           memcmp(activation->proposal_digest, candidate->proposal_digest,
                  UCN_V6_ROUTE_PROPOSAL_DIGEST_BYTES) == 0;
}

static bool path_capability_is_current(
    const ucn_v6_capability_owner_t *capability_owner,
    uint64_t now_us,
    const ucn_v6_route_path_t *path)
{
    ucn_v6_path_capability_t current;
    if (capability_owner == NULL ||
        ucn_v6_capability_copy_path(
            capability_owner, now_us,
            &path->capability.destination_principal,
            &path->capability.destination_binding,
            path->capability.session_generation,
            path->capability.route_generation,
            path->capability.path_id,
            path->capability.path_generation,
            &current) != UCN_V6_OK) {
        return false;
    }
    return path_capability_equal(&current, &path->capability);
}

static void clear_pins_for_domain(ucn_v6_route_owner_t *owner,
                                  const ucn_v6_route_domain_t *domain)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_FLOW_PINS; ++index) {
        if (owner->pins[index].occupied &&
            domain_equal(&owner->pins[index].domain, domain)) {
            memset(&owner->pins[index], 0, sizeof(owner->pins[index]));
            if (owner->stats.flow_pins != 0U) {
                --owner->stats.flow_pins;
            }
        }
    }
}

ucn_v6_result_t ucn_v6_route_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    uint64_t candidate_timeout_us,
    uint64_t activation_retry_us,
    uint8_t activation_max_attempts,
    uint64_t previous_generation_grace_us,
    uint64_t flow_pin_lease_us,
    ucn_v6_route_owner_t **owner_out)
{
    ucn_v6_route_owner_t *owner;
    if (owner_out == NULL || candidate_timeout_us == 0U ||
        activation_retry_us == 0U || activation_max_attempts == 0U ||
        previous_generation_grace_us == 0U || flow_pin_lease_us == 0U ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_ROUTE_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK) {
        return UCN_V6_ERR_CONFIG;
    }
    owner = (ucn_v6_route_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_ROUTE_OWNER_MAGIC;
    owner->schema = UCN_V6_ROUTE_SCHEMA;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->candidate_timeout_us = candidate_timeout_us;
    owner->activation_retry_us = activation_retry_us;
    owner->activation_max_attempts = activation_max_attempts;
    owner->previous_generation_grace_us = previous_generation_grace_us;
    owner->flow_pin_lease_us = flow_pin_lease_us;
    owner->initialized = true;
    owner->canary = UCN_V6_ROUTE_OWNER_CANARY;
    *owner_out = owner;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_candidate_begin(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    const ucn_v6_route_domain_t *domain,
    uint32_t proposed_route_generation)
{
    ucn_v6_route_candidate_view_t *candidate;
    ucn_v6_route_domain_state_t *domain_state;
    ucn_v6_route_set_slot_t *set;
    uint64_t deadline_us;
    uint32_t expected_generation = 1U;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) ||
        candidate_transaction_id == 0U ||
        candidate_transaction_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        proposed_route_generation == 0U ||
        proposed_route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        !deadline_build(now_us, owner->candidate_timeout_us, &deadline_us)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, candidate_transaction_id);
    if (candidate != NULL) {
        if (now_us >= candidate->deadline_us) {
            return UCN_V6_ERR_TIMEOUT;
        }
        return domain_equal(&candidate->proposal.domain, domain) &&
               candidate->proposal.route_generation ==
                   proposed_route_generation ?
                   UCN_V6_OK : UCN_V6_ERR_REPLAY;
    }
    domain_state = find_domain_state(owner, domain);
    if (domain_state != NULL && candidate_transaction_id <=
                                    domain_state->candidate_transaction_high_water) {
        saturating_increment(&owner->stats.rejected_stale);
        return UCN_V6_ERR_REPLAY;
    }
    set = find_set(owner, domain);
    if (set != NULL &&
        ucn_v6_serial_checked_next(set->current.route_generation,
                                   &expected_generation) != UCN_V6_OK) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    if (proposed_route_generation != expected_generation) {
        saturating_increment(&owner->stats.rejected_stale);
        return UCN_V6_ERR_REPLAY;
    }
    candidate = find_free_candidate(owner);
    if (candidate == NULL ||
        (domain_state == NULL &&
         (domain_state = find_free_domain_state(owner)) == NULL)) {
        saturating_increment(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    if (!domain_state->occupied) {
        memset(domain_state, 0, sizeof(*domain_state));
        domain_state->occupied = true;
        domain_state->domain = *domain;
    }
    domain_state->candidate_transaction_high_water =
        candidate_transaction_id;
    memset(candidate, 0, sizeof(*candidate));
    candidate->occupied = true;
    candidate->candidate_transaction_id = candidate_transaction_id;
    candidate->proposal.domain = *domain;
    candidate->proposal.route_generation = proposed_route_generation;
    candidate->deadline_us = deadline_us;
    ++owner->stats.candidates;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_candidate_add_path(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_capability_owner_t *capability_owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    const ucn_v6_route_domain_t *domain,
    const ucn_v6_route_path_t *path)
{
    ucn_v6_route_candidate_view_t *candidate;
    size_t index;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) || path == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, candidate_transaction_id);
    if (candidate == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (!domain_equal(domain, &candidate->proposal.domain)) {
        return UCN_V6_ERR_REPLAY;
    }
    if (now_us >= candidate->deadline_us) {
        return UCN_V6_ERR_TIMEOUT;
    }
    if (candidate->frozen) {
        return UCN_V6_ERR_STATE;
    }
    if (!route_path_is_valid(domain, candidate->proposal.route_generation,
                             path) ||
        !path_capability_is_current(capability_owner, now_us, path)) {
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < candidate->proposal.path_count; ++index) {
        if (candidate->proposal.paths[index].path_id == path->path_id) {
            return route_path_equal(&candidate->proposal.paths[index], path) ?
                       UCN_V6_OK : UCN_V6_ERR_REPLAY;
        }
    }
    if (candidate->proposal.path_count >= UCN_V6_ROUTE_MAX_PATHS) {
        return UCN_V6_ERR_NO_SPACE;
    }
    candidate->proposal.paths[candidate->proposal.path_count] = *path;
    ++candidate->proposal.path_count;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_candidate_record_probe(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    uint16_t path_id,
    uint32_t path_generation)
{
    ucn_v6_route_candidate_view_t *candidate;
    size_t index;
    if (!owner_is_valid(owner) || path_id == 0U || path_generation == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, candidate_transaction_id);
    if (candidate == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (now_us >= candidate->deadline_us) {
        return UCN_V6_ERR_TIMEOUT;
    }
    for (index = 0U; index < candidate->proposal.path_count; ++index) {
        if (candidate->proposal.paths[index].path_id == path_id &&
            candidate->proposal.paths[index].path_generation ==
                path_generation) {
            if (!candidate->frozen) {
                candidate->proposal.preferred_path_index = 0U;
                if (!proposal_is_valid(&candidate->proposal)) {
                    return UCN_V6_ERR_STATE;
                }
                proposal_digest(&candidate->proposal,
                                candidate->proposal_digest);
                candidate->frozen = true;
            }
            candidate->probed_mask =
                (uint16_t)(candidate->probed_mask |
                           (uint16_t)(UINT16_C(1) << index));
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_REPLAY;
}

ucn_v6_result_t ucn_v6_route_candidate_prepare_activation(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    ucn_v6_route_activation_t *activation)
{
    ucn_v6_route_candidate_view_t *candidate;
    ucn_v6_route_activation_t next;
    uint16_t all_paths;
    if (!owner_is_valid(owner) || activation == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, candidate_transaction_id);
    if (candidate == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (now_us >= candidate->deadline_us) {
        return UCN_V6_ERR_TIMEOUT;
    }
    all_paths = (uint16_t)((UINT16_C(1) <<
                            candidate->proposal.path_count) - 1U);
    if (!candidate->frozen || candidate->probed_mask != all_paths) {
        return UCN_V6_ERR_STATE;
    }
    if (candidate->activation_attempts >= owner->activation_max_attempts) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    if (candidate->activation_sent && now_us < candidate->next_retry_us) {
        return UCN_V6_ERR_STATE;
    }
    memset(&next, 0, sizeof(next));
    next.candidate_transaction_id = candidate->candidate_transaction_id;
    next.domain = candidate->proposal.domain;
    next.route_generation = candidate->proposal.route_generation;
    memcpy(next.proposal_digest, candidate->proposal_digest,
           sizeof(next.proposal_digest));
    *activation = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_candidate_record_activation_send(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    bool submitted)
{
    ucn_v6_route_candidate_view_t *candidate;
    uint16_t all_paths;
    uint64_t next_retry_us;
    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, candidate_transaction_id);
    if (candidate == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (now_us >= candidate->deadline_us) {
        return UCN_V6_ERR_TIMEOUT;
    }
    all_paths = (uint16_t)((UINT16_C(1) <<
                            candidate->proposal.path_count) - 1U);
    if (!candidate->frozen || candidate->probed_mask != all_paths ||
        candidate->activation_attempts >= owner->activation_max_attempts ||
        (candidate->activation_sent && now_us < candidate->next_retry_us)) {
        return UCN_V6_ERR_STATE;
    }
    if (!submitted) {
        return UCN_V6_OK;
    }
    if (!deadline_build(now_us, owner->activation_retry_us,
                        &next_retry_us)) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    if (next_retry_us > candidate->deadline_us) {
        next_retry_us = candidate->deadline_us;
    }
    ++candidate->activation_attempts;
    candidate->activation_sent = true;
    candidate->next_retry_us = next_retry_us;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_candidate_commit_ack(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_capability_owner_t *capability_owner,
    uint64_t now_us,
    const ucn_v6_route_activation_t *ack)
{
    ucn_v6_route_candidate_view_t *candidate;
    ucn_v6_route_set_slot_t *set;
    ucn_v6_route_set_slot_t next;
    uint64_t previous_deadline_us = 0U;
    uint32_t expected_generation = 1U;
    size_t index;
    if (!owner_is_valid(owner) || capability_owner == NULL || ack == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, ack->candidate_transaction_id);
    if (candidate == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (!candidate->activation_sent ||
        !activation_equal_candidate(ack, candidate)) {
        saturating_increment(&owner->stats.rejected_stale);
        return UCN_V6_ERR_REPLAY;
    }
    if (now_us >= candidate->deadline_us ||
        !proposal_is_valid(&candidate->proposal)) {
        return UCN_V6_ERR_TIMEOUT;
    }
    set = find_set(owner, &candidate->proposal.domain);
    if (set != NULL) {
        if (ucn_v6_serial_checked_next(set->current.route_generation,
                                       &expected_generation) != UCN_V6_OK ||
            expected_generation != candidate->proposal.route_generation ||
            !deadline_build(now_us, owner->previous_generation_grace_us,
                            &previous_deadline_us)) {
            return UCN_V6_ERR_REPLAY;
        }
    } else {
        set = find_free_set(owner);
        if (set == NULL) {
            saturating_increment(&owner->stats.rejected_capacity);
            return UCN_V6_ERR_NO_SPACE;
        }
    }
    for (index = 0U; index < candidate->proposal.path_count; ++index) {
        if (!path_capability_is_current(capability_owner, now_us,
                                        &candidate->proposal.paths[index])) {
            return UCN_V6_ERR_STATE;
        }
    }
    memset(&next, 0, sizeof(next));
    next.occupied = true;
    next.current = candidate->proposal;
    if (set->occupied) {
        next.previous_valid = true;
        next.previous = set->current;
        next.previous_deadline_us = previous_deadline_us;
    }
    clear_pins_for_domain(owner, &candidate->proposal.domain);
    if (!set->occupied) {
        ++owner->stats.route_sets;
    }
    *set = next;
    memset(candidate, 0, sizeof(*candidate));
    if (owner->stats.candidates != 0U) {
        --owner->stats.candidates;
    }
    saturating_increment(&owner->stats.activations);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_copy_candidate(
    const ucn_v6_route_owner_t *owner,
    uint64_t candidate_transaction_id,
    ucn_v6_route_candidate_view_t *candidate)
{
    const ucn_v6_route_candidate_view_t *found;
    if (!owner_is_valid(owner) || candidate == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    found = find_candidate_const(owner, candidate_transaction_id);
    if (found == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *candidate = *found;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_copy_set(
    const ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain,
    ucn_v6_route_proposal_t *current)
{
    const ucn_v6_route_set_slot_t *set;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) ||
        current == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    set = find_set_const(owner, domain);
    if (set == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *current = set->current;
    return UCN_V6_OK;
}

static bool path_is_usable(
    const ucn_v6_capability_owner_t *capability_owner,
    const ucn_v6_route_path_t *path,
    uint64_t now_us)
{
    return path != NULL && path->available && path->capability.valid &&
           now_us < path->capability.deadline_us &&
           path_capability_is_current(capability_owner, now_us, path);
}

static int find_path_index(const ucn_v6_route_proposal_t *proposal,
                           const ucn_v6_capability_owner_t *capability_owner,
                           uint16_t path_id,
                           uint32_t path_generation,
                           uint64_t now_us)
{
    size_t index;
    for (index = 0U; index < proposal->path_count; ++index) {
        if (proposal->paths[index].path_id == path_id &&
            proposal->paths[index].path_generation == path_generation &&
            path_is_usable(capability_owner, &proposal->paths[index],
                           now_us)) {
            return (int)index;
        }
    }
    return -1;
}

static uint64_t flow_hash(uint64_t flow_id, uint64_t packet_sequence)
{
    uint64_t value = flow_id ^ (packet_sequence + UINT64_C(0x9E3779B97F4A7C15));
    value ^= value >> 30U;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31U);
}

static int select_primary(const ucn_v6_route_proposal_t *proposal,
                          const ucn_v6_capability_owner_t *capability_owner,
                          uint64_t now_us)
{
    size_t index;
    int best = -1;
    uint16_t best_priority = UINT16_MAX;
    if (proposal->preferred_path_index < proposal->path_count &&
        path_is_usable(capability_owner,
                       &proposal->paths[proposal->preferred_path_index],
                       now_us)) {
        return (int)proposal->preferred_path_index;
    }
    for (index = 0U; index < proposal->path_count; ++index) {
        if (path_is_usable(capability_owner, &proposal->paths[index],
                           now_us) &&
            (best < 0 || proposal->paths[index].priority < best_priority)) {
            best = (int)index;
            best_priority = proposal->paths[index].priority;
        }
    }
    return best;
}

static ucn_v6_route_flow_pin_t *find_flow_pin(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain,
    uint32_t route_generation,
    uint64_t flow_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_FLOW_PINS; ++index) {
        if (owner->pins[index].occupied &&
            owner->pins[index].route_generation == route_generation &&
            owner->pins[index].flow_id == flow_id &&
            domain_equal(&owner->pins[index].domain, domain)) {
            return &owner->pins[index];
        }
    }
    return NULL;
}

static ucn_v6_route_flow_pin_t *find_free_flow_pin(
    ucn_v6_route_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_FLOW_PINS; ++index) {
        if (!owner->pins[index].occupied) {
            return &owner->pins[index];
        }
    }
    return NULL;
}

ucn_v6_result_t ucn_v6_route_select(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_capability_owner_t *capability_owner,
    uint64_t now_us,
    const ucn_v6_route_select_request_t *request,
    ucn_v6_route_selection_t *selection)
{
    ucn_v6_route_set_slot_t *set;
    ucn_v6_route_selection_t next;
    int selected = -1;
    size_t index;
    if (!owner_is_valid(owner) || capability_owner == NULL ||
        request == NULL || selection == NULL ||
        !domain_is_valid(&request->domain) || request->flow_id == 0U ||
        request->policy < UCN_V6_ROUTE_POLICY_PINNED ||
        request->policy > UCN_V6_ROUTE_POLICY_WEIGHTED_MULTIPATH) {
        return UCN_V6_ERR_ARGUMENT;
    }
    set = find_set(owner, &request->domain);
    if (set == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    memset(&next, 0, sizeof(next));
    if (request->policy == UCN_V6_ROUTE_POLICY_PINNED) {
        selected = find_path_index(&set->current, capability_owner,
                                   request->pinned_path_id,
                                   request->pinned_path_generation, now_us);
    } else if (request->policy == UCN_V6_ROUTE_POLICY_ACTIVE_STANDBY) {
        selected = select_primary(&set->current, capability_owner, now_us);
    } else if (request->policy == UCN_V6_ROUTE_POLICY_PER_FLOW_HASH) {
        ucn_v6_route_flow_pin_t *pin = find_flow_pin(
            owner, &request->domain, set->current.route_generation,
            request->flow_id);
        if (pin != NULL && now_us >= pin->deadline_us) {
            memset(pin, 0, sizeof(*pin));
            if (owner->stats.flow_pins != 0U) {
                --owner->stats.flow_pins;
            }
            pin = NULL;
        }
        if (pin != NULL) {
            selected = find_path_index(&set->current, capability_owner,
                                       pin->path_id,
                                       pin->path_generation, now_us);
            if (selected >= 0) {
                next.reused_flow_pin = true;
            } else {
                memset(pin, 0, sizeof(*pin));
                if (owner->stats.flow_pins != 0U) {
                    --owner->stats.flow_pins;
                }
                pin = NULL;
            }
        }
        if (selected < 0) {
            size_t usable_count = 0U;
            size_t ordinal;
            uint64_t deadline_us;
            for (index = 0U; index < set->current.path_count; ++index) {
                if (path_is_usable(capability_owner,
                                   &set->current.paths[index], now_us)) {
                    ++usable_count;
                }
            }
            if (usable_count != 0U) {
                ordinal = (size_t)(flow_hash(request->flow_id, 0U) %
                                   usable_count);
                for (index = 0U; index < set->current.path_count; ++index) {
                    if (path_is_usable(capability_owner,
                                       &set->current.paths[index], now_us)) {
                        if (ordinal == 0U) {
                            selected = (int)index;
                            break;
                        }
                        --ordinal;
                    }
                }
            }
            if (selected >= 0) {
                if (pin == NULL) {
                    pin = find_free_flow_pin(owner);
                }
                if (pin == NULL ||
                    !deadline_build(now_us, owner->flow_pin_lease_us,
                                    &deadline_us)) {
                    saturating_increment(&owner->stats.rejected_capacity);
                    return UCN_V6_ERR_NO_SPACE;
                }
                memset(pin, 0, sizeof(*pin));
                pin->occupied = true;
                pin->domain = request->domain;
                pin->route_generation = set->current.route_generation;
                pin->flow_id = request->flow_id;
                pin->path_id = set->current.paths[selected].path_id;
                pin->path_generation =
                    set->current.paths[selected].path_generation;
                pin->deadline_us = deadline_us;
                ++owner->stats.flow_pins;
            }
        }
    } else {
        uint32_t total_weight = 0U;
        uint32_t pick;
        if (!request->allow_reordering) {
            return UCN_V6_ERR_ACCESS;
        }
        for (index = 0U; index < set->current.path_count; ++index) {
            if (path_is_usable(capability_owner,
                               &set->current.paths[index], now_us)) {
                if (UINT32_MAX - total_weight <
                    set->current.paths[index].weight) {
                    return UCN_V6_ERR_EXHAUSTED;
                }
                total_weight += set->current.paths[index].weight;
            }
        }
        if (total_weight != 0U) {
            pick = (uint32_t)(flow_hash(request->flow_id,
                                       request->packet_sequence) %
                              total_weight);
            for (index = 0U; index < set->current.path_count; ++index) {
                if (path_is_usable(capability_owner,
                                   &set->current.paths[index], now_us)) {
                    if (pick < set->current.paths[index].weight) {
                        selected = (int)index;
                        break;
                    }
                    pick -= set->current.paths[index].weight;
                }
            }
        }
    }
    if (selected < 0) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    next.route_generation = set->current.route_generation;
    next.path = set->current.paths[selected];
    *selection = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_mark_error(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain,
    uint32_t route_generation,
    uint16_t path_id,
    uint32_t path_generation)
{
    ucn_v6_route_set_slot_t *set;
    size_t index;
    size_t pin_index;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) ||
        route_generation == 0U || path_id == 0U || path_generation == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    set = find_set(owner, domain);
    if (set == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (set->current.route_generation != route_generation) {
        saturating_increment(&owner->stats.rejected_stale);
        return UCN_V6_ERR_REPLAY;
    }
    for (index = 0U; index < set->current.path_count; ++index) {
        if (set->current.paths[index].path_id == path_id &&
            set->current.paths[index].path_generation == path_generation) {
            if (!set->current.paths[index].available) {
                return UCN_V6_OK;
            }
            set->current.paths[index].available = false;
            for (pin_index = 0U;
                 pin_index < UCN_V6_CONFIG_ROUTE_FLOW_PINS;
                 ++pin_index) {
                if (owner->pins[pin_index].occupied &&
                    domain_equal(&owner->pins[pin_index].domain, domain) &&
                    owner->pins[pin_index].route_generation ==
                        route_generation &&
                    owner->pins[pin_index].path_id == path_id &&
                    owner->pins[pin_index].path_generation ==
                        path_generation) {
                    memset(&owner->pins[pin_index], 0,
                           sizeof(owner->pins[pin_index]));
                    if (owner->stats.flow_pins != 0U) {
                        --owner->stats.flow_pins;
                    }
                }
            }
            saturating_increment(&owner->stats.failovers);
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

static bool domain_origin_is_session(
    const ucn_v6_route_domain_t *domain,
    const ucn_v6_session_key_t *session)
{
    return domain != NULL && session != NULL &&
           principal_equal(&domain->origin_principal,
                           &session->principal) &&
           ucn_v6_binding_key_equal(&domain->origin_binding,
                                    &session->binding) &&
           domain->origin_session_generation ==
               session->session_generation;
}

static bool route_path_uses_session(
    const ucn_v6_route_path_t *path,
    const ucn_v6_session_key_t *session)
{
    return path != NULL && session != NULL &&
           (session_equal(&path->next_hop, session) ||
            (principal_equal(&path->capability.destination_principal,
                             &session->principal) &&
             ucn_v6_binding_key_equal(
                 &path->capability.destination_binding,
                 &session->binding) &&
             path->capability.session_generation ==
                 session->session_generation));
}

static bool proposal_uses_session(
    const ucn_v6_route_proposal_t *proposal,
    const ucn_v6_session_key_t *session)
{
    size_t index;
    if (proposal == NULL || session == NULL) {
        return false;
    }
    if (domain_origin_is_session(&proposal->domain, session)) {
        return true;
    }
    for (index = 0U; index < proposal->path_count; ++index) {
        if (route_path_uses_session(&proposal->paths[index], session)) {
            return true;
        }
    }
    return false;
}

ucn_v6_result_t ucn_v6_route_invalidate_session(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_session_key_t *session)
{
    size_t index;
    if (!owner_is_valid(owner) || !session_is_valid(session)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        ucn_v6_route_candidate_view_t *candidate =
            &owner->candidates[index].value;
        if (candidate->occupied &&
            proposal_uses_session(&candidate->proposal, session)) {
            memset(candidate, 0, sizeof(*candidate));
            if (owner->stats.candidates != 0U) {
                --owner->stats.candidates;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        ucn_v6_route_set_slot_t *set = &owner->sets[index];
        if (set->occupied && proposal_uses_session(&set->current, session)) {
            clear_pins_for_domain(owner, &set->current.domain);
            memset(set, 0, sizeof(*set));
            if (owner->stats.route_sets != 0U) {
                --owner->stats.route_sets;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        if (owner->domains[index].occupied &&
            domain_origin_is_session(&owner->domains[index].domain,
                                     session)) {
            memset(&owner->domains[index], 0, sizeof(owner->domains[index]));
        }
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_accept_generation(
    const ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_route_domain_t *domain,
    uint32_t route_generation)
{
    const ucn_v6_route_set_slot_t *set;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) ||
        route_generation == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    set = find_set_const(owner, domain);
    if (set == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (set->current.route_generation == route_generation) {
        return UCN_V6_OK;
    }
    if (set->previous_valid &&
        set->previous.route_generation == route_generation &&
        now_us < set->previous_deadline_us) {
        return UCN_V6_OK;
    }
    return UCN_V6_ERR_REPLAY;
}

ucn_v6_result_t ucn_v6_route_expire(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us)
{
    size_t index;
    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        if (owner->candidates[index].value.occupied &&
            now_us >= owner->candidates[index].value.deadline_us) {
            memset(&owner->candidates[index], 0,
                   sizeof(owner->candidates[index]));
            if (owner->stats.candidates != 0U) {
                --owner->stats.candidates;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        if (owner->sets[index].occupied && owner->sets[index].previous_valid &&
            now_us >= owner->sets[index].previous_deadline_us) {
            memset(&owner->sets[index].previous, 0,
                   sizeof(owner->sets[index].previous));
            owner->sets[index].previous_valid = false;
            owner->sets[index].previous_deadline_us = 0U;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_FLOW_PINS; ++index) {
        if (owner->pins[index].occupied &&
            now_us >= owner->pins[index].deadline_us) {
            memset(&owner->pins[index], 0, sizeof(owner->pins[index]));
            if (owner->stats.flow_pins != 0U) {
                --owner->stats.flow_pins;
            }
        }
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_copy_view(
    const ucn_v6_route_owner_t *owner,
    ucn_v6_route_view_t *view)
{
    ucn_v6_route_view_t next;
    if (!owner_is_valid(owner) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    next = owner->stats;
    next.faulted = owner->faulted;
    *view = next;
    return UCN_V6_OK;
}
