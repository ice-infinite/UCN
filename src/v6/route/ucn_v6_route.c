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
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           domain->destination_session_generation != 0U &&
           domain->destination_session_generation <=
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
                                    &right->destination_binding) &&
           left->destination_session_generation ==
               right->destination_session_generation;
}

/* EN: Identity that is actually visible to a relay on Wire.  Endpoint
 * Principals and the destination Session generation are intentionally absent
 * from the v6 forwarding header, so Route Owner must use Route generation to
 * make this reduced key unambiguous.
 * 中文：中继在 Wire 上真实可见的身份。端点 Principal 与目标 Session 代际
 * 有意不进入 v6 转发头，因此 Route Owner 必须结合 Route 代际保证这个缩减键
 * 唯一。 */
static bool wire_domain_equal(const ucn_v6_route_domain_t *left,
                              const ucn_v6_route_domain_t *right)
{
    return left != NULL && right != NULL &&
           ucn_v6_binding_key_equal(&left->origin_binding,
                                    &right->origin_binding) &&
           left->origin_session_generation ==
               right->origin_session_generation &&
           ucn_v6_binding_key_equal(&left->destination_binding,
                                    &right->destination_binding);
}

static bool proposal_conflicts_with_live_wire_key(
    const ucn_v6_route_owner_t *owner,
    const ucn_v6_route_proposal_t *proposal,
    uint64_t now_us)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        const ucn_v6_route_set_slot_t *slot = &owner->sets[index];
        if (!slot->occupied ||
            domain_equal(&slot->current.domain, &proposal->domain)) {
            continue;
        }
        if ((slot->current.route_generation == proposal->route_generation &&
             wire_domain_equal(&slot->current.domain, &proposal->domain)) ||
            (slot->previous_valid && now_us < slot->previous_deadline_us &&
             slot->previous.route_generation == proposal->route_generation &&
             wire_domain_equal(&slot->previous.domain, &proposal->domain))) {
            return true;
        }
    }
    return false;
}

static bool path_capability_equal(const ucn_v6_path_capability_t *left,
                                  const ucn_v6_path_capability_t *right)
{
    return left != NULL && right != NULL &&
           left->valid == right->valid &&
           left->immutable_for_realtime == right->immutable_for_realtime &&
           principal_equal(&left->destination_principal,
                           &right->destination_principal) &&
           ucn_v6_binding_key_equal(&left->destination_binding,
                                    &right->destination_binding) &&
           left->destination_session_generation ==
               right->destination_session_generation &&
           left->destination_capability_generation ==
               right->destination_capability_generation &&
           memcmp(left->destination_capability_digest,
                  right->destination_capability_digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           left->destination_realtime_mode_bits ==
               right->destination_realtime_mode_bits &&
           left->destination_clock_domain_id ==
               right->destination_clock_domain_id &&
           left->destination_clock_domain_generation ==
               right->destination_clock_domain_generation &&
           session_equal(&left->local_parent_session,
                         &right->local_parent_session) &&
           left->local_parent_link_id == right->local_parent_link_id &&
           left->local_parent_link_generation ==
               right->local_parent_link_generation &&
           left->local_parent_capability_generation ==
               right->local_parent_capability_generation &&
           memcmp(left->local_parent_capability_digest,
                  right->local_parent_capability_digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           left->route_generation == right->route_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation &&
           left->hop_count == right->hop_count &&
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

static bool path_capability_claim_equal(
    const ucn_v6_path_capability_t *left,
    const ucn_v6_path_capability_t *right)
{
    ucn_v6_path_capability_t left_claim;
    ucn_v6_path_capability_t right_claim;
    if (left == NULL || right == NULL) {
        return false;
    }
    left_claim = *left;
    right_claim = *right;
    left_claim.deadline_us = 0U;
    right_claim.deadline_us = 0U;
    return path_capability_equal(&left_claim, &right_claim);
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
           left->next_hop_capability_generation ==
               right->next_hop_capability_generation &&
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
           path->path_id != 0U && path->path_id <= UCN_V6_PATH_ID_MAX &&
           path->path_generation != 0U &&
           path->path_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           session_is_valid(&path->next_hop) && path->egress_link_id != 0U &&
           path->egress_link_id <= UCN_V6_LINK_ID_MAX &&
           path->egress_link_generation != 0U &&
           path->egress_link_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           path->next_hop_capability_generation != 0U &&
           path->next_hop_capability_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           path->hop_count != 0U &&
           path->hop_count <= UCN_V6_HOP_COUNT_MAX &&
           path->weight != 0U && path->available &&
           path->capability.valid &&
           path->next_hop.binding.realm_id == domain->origin_binding.realm_id &&
           principal_equal(&path->capability.destination_principal,
                           &domain->destination_principal) &&
           ucn_v6_binding_key_equal(&path->capability.destination_binding,
                                     &domain->destination_binding) &&
           path->capability.destination_session_generation ==
               domain->destination_session_generation &&
           path->capability.route_generation == route_generation &&
           path->capability.path_id == path->path_id &&
           path->capability.path_generation == path->path_generation &&
           path->capability.hop_count == path->hop_count &&
           session_equal(&path->capability.local_parent_session,
                         &path->next_hop) &&
           path->capability.local_parent_link_id == path->egress_link_id &&
           path->capability.local_parent_link_generation ==
               path->egress_link_generation &&
           path->capability.local_parent_capability_generation ==
               path->next_hop_capability_generation &&
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
           !owner->faulted && owner->capability_owner != NULL &&
           owner->canary == UCN_V6_ROUTE_OWNER_CANARY &&
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
    const ucn_v6_route_domain_t *domain,
    uint64_t candidate_transaction_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        if (owner->candidates[index].value.occupied &&
            owner->candidates[index].value.candidate_transaction_id ==
                candidate_transaction_id &&
            domain_equal(
                &owner->candidates[index].value.proposal.domain, domain)) {
            return &owner->candidates[index].value;
        }
    }
    return NULL;
}

static const ucn_v6_route_candidate_view_t *find_candidate_const(
    const ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain,
    uint64_t candidate_transaction_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        if (owner->candidates[index].value.occupied &&
            owner->candidates[index].value.candidate_transaction_id ==
                candidate_transaction_id &&
            domain_equal(
                &owner->candidates[index].value.proposal.domain, domain)) {
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
    digest_u32(state, proposal->domain.destination_session_generation);
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
        digest_u32(state, path->next_hop_capability_generation);
        digest_u16(state, path->hop_count);
        digest_u16(state, path->priority);
        digest_u16(state, path->weight);
        digest_byte(state, path->available ? 1U : 0U);
        digest_u32(state, capability->destination_capability_generation);
        digest_u32(state, capability->destination_session_generation);
        digest_bytes(state, capability->destination_capability_digest,
                     UCN_V6_CAPABILITY_DIGEST_BYTES);
        digest_u16(state, capability->destination_realtime_mode_bits);
        digest_u16(state, capability->destination_clock_domain_id);
        digest_u32(state, capability->destination_clock_domain_generation);
        digest_bytes(state, capability->local_parent_session.principal.bytes,
                     sizeof(capability->local_parent_session.principal.bytes));
        digest_binding(state, &capability->local_parent_session.binding);
        digest_u32(state,
                   capability->local_parent_session.session_generation);
        digest_u16(state, capability->local_parent_link_id);
        digest_u32(state, capability->local_parent_link_generation);
        digest_u32(state, capability->local_parent_capability_generation);
        digest_bytes(state, capability->local_parent_capability_digest,
                     UCN_V6_CAPABILITY_DIGEST_BYTES);
        digest_u16(state, capability->hop_count);
        digest_byte(state, capability->immutable_for_realtime ? 1U : 0U);
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
    const ucn_v6_route_path_t *path,
    ucn_v6_path_capability_t *current_out)
{
    ucn_v6_path_capability_t current;
    if (capability_owner == NULL ||
        ucn_v6_capability_copy_path(
            capability_owner, now_us,
            &path->capability.destination_principal,
            &path->capability.destination_binding,
            path->capability.destination_session_generation,
            path->capability.route_generation,
            path->capability.path_id,
            path->capability.path_generation,
            &current) != UCN_V6_OK) {
        return false;
    }
    if (!path_capability_claim_equal(&current, &path->capability)) {
        return false;
    }
    if (current_out != NULL) {
        *current_out = current;
    }
    return true;
}

static bool next_hop_capability_is_current(
    const ucn_v6_capability_owner_t *capability_owner,
    uint64_t now_us,
    const ucn_v6_route_path_t *path)
{
    ucn_v6_cached_peer_capability_t current;
    if (capability_owner == NULL || path == NULL ||
        ucn_v6_capability_copy_peer(
            capability_owner, now_us, &path->next_hop.principal,
            &path->next_hop.binding, path->next_hop.session_generation,
            path->egress_link_generation, &current) != UCN_V6_OK) {
        return false;
    }
    return current.ingress_link_id == path->egress_link_id &&
           current.record.capability_generation ==
               path->next_hop_capability_generation;
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
    const ucn_v6_capability_owner_t *capability_owner,
    uint64_t candidate_timeout_us,
    uint64_t activation_retry_us,
    uint8_t activation_max_attempts,
    uint64_t previous_generation_grace_us,
    uint64_t flow_pin_lease_us,
    ucn_v6_route_owner_t **owner_out)
{
    ucn_v6_route_owner_t *owner;
    ucn_v6_capability_view_t capability_view;
    if (owner_out == NULL || capability_owner == NULL ||
        candidate_timeout_us == 0U ||
        activation_retry_us == 0U || activation_max_attempts == 0U ||
        previous_generation_grace_us == 0U || flow_pin_lease_us == 0U ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_capability_copy_view(capability_owner, 0U,
                                    &capability_view) !=
            UCN_V6_OK ||
        capability_view.faulted ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_ROUTE_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK) {
        return UCN_V6_ERR_CONFIG;
    }
    if (ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     owner_out, sizeof(*owner_out)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     manifest, sizeof(*manifest)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     capability_owner, 1U)) {
        return UCN_V6_ERR_CONFIG;
    }
    owner = (ucn_v6_route_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_ROUTE_OWNER_MAGIC;
    owner->schema = UCN_V6_ROUTE_SCHEMA;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->capability_owner = capability_owner;
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
    candidate = find_candidate(owner, domain, candidate_transaction_id);
    if (candidate != NULL) {
        if (now_us >= candidate->deadline_us) {
            return UCN_V6_ERR_TIMEOUT;
        }
        return candidate->proposal.route_generation ==
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
    candidate = find_candidate(owner, domain, candidate_transaction_id);
    if (candidate == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (now_us >= candidate->deadline_us) {
        return UCN_V6_ERR_TIMEOUT;
    }
    if (candidate->frozen) {
        return UCN_V6_ERR_STATE;
    }
    if (!route_path_is_valid(domain, candidate->proposal.route_generation,
                             path) ||
        !next_hop_capability_is_current(owner->capability_owner, now_us,
                                        path) ||
        !path_capability_is_current(owner->capability_owner, now_us, path,
                                    NULL)) {
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
    const ucn_v6_route_domain_t *domain,
    uint16_t path_id,
    uint32_t path_generation)
{
    ucn_v6_route_candidate_view_t *candidate;
    size_t index;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) ||
        path_id == 0U || path_id > UCN_V6_PATH_ID_MAX ||
        path_generation == 0U ||
        path_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, domain, candidate_transaction_id);
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
    const ucn_v6_route_domain_t *domain,
    ucn_v6_route_activation_t *activation)
{
    ucn_v6_route_candidate_view_t *candidate;
    ucn_v6_route_activation_t next;
    uint16_t all_paths;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) ||
        activation == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, domain, candidate_transaction_id);
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
    const ucn_v6_route_domain_t *domain,
    bool submitted)
{
    ucn_v6_route_candidate_view_t *candidate;
    uint16_t all_paths;
    uint64_t next_retry_us;
    if (!owner_is_valid(owner) || !domain_is_valid(domain)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, domain, candidate_transaction_id);
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
    uint64_t now_us,
    const ucn_v6_route_activation_t *ack)
{
    ucn_v6_route_candidate_view_t *candidate;
    ucn_v6_route_set_slot_t *set;
    ucn_v6_route_set_slot_t next;
    uint64_t previous_deadline_us = 0U;
    uint32_t expected_generation = 1U;
    size_t index;
    if (!owner_is_valid(owner) || ack == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!domain_is_valid(&ack->domain)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = find_candidate(owner, &ack->domain,
                               ack->candidate_transaction_id);
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
    if (proposal_conflicts_with_live_wire_key(
            owner, &candidate->proposal, now_us)) {
        saturating_increment(&owner->stats.rejected_stale);
        return UCN_V6_ERR_REPLAY;
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
        if (!next_hop_capability_is_current(
                owner->capability_owner, now_us,
                &candidate->proposal.paths[index]) ||
            !path_capability_is_current(owner->capability_owner, now_us,
                                        &candidate->proposal.paths[index],
                                        NULL)) {
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
    const ucn_v6_route_domain_t *domain,
    ucn_v6_route_candidate_view_t *candidate)
{
    const ucn_v6_route_candidate_view_t *found;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) ||
        candidate == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    found = find_candidate_const(owner, domain, candidate_transaction_id);
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
           next_hop_capability_is_current(capability_owner, now_us, path) &&
           path_capability_is_current(capability_owner, now_us, path, NULL);
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
    uint64_t now_us,
    const ucn_v6_route_select_request_t *request,
    ucn_v6_route_selection_t *selection)
{
    ucn_v6_route_set_slot_t *set;
    ucn_v6_route_selection_t next;
    int selected = -1;
    size_t index;
    if (!owner_is_valid(owner) || request == NULL || selection == NULL ||
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
        selected = find_path_index(&set->current, owner->capability_owner,
                                   request->pinned_path_id,
                                   request->pinned_path_generation, now_us);
    } else if (request->policy == UCN_V6_ROUTE_POLICY_ACTIVE_STANDBY) {
        selected = select_primary(&set->current, owner->capability_owner,
                                  now_us);
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
            selected = find_path_index(&set->current, owner->capability_owner,
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
                if (path_is_usable(owner->capability_owner,
                                   &set->current.paths[index], now_us)) {
                    ++usable_count;
                }
            }
            if (usable_count != 0U) {
                ordinal = (size_t)(flow_hash(request->flow_id, 0U) %
                                   usable_count);
                for (index = 0U; index < set->current.path_count; ++index) {
                    if (path_is_usable(owner->capability_owner,
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
            if (path_is_usable(owner->capability_owner,
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
                if (path_is_usable(owner->capability_owner,
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
    /* A same-generation Path lease refresh changes only the live deadline.
     * Return the Capability Owner's current value rather than the RouteSet's
     * frozen discovery snapshot, while keeping every immutable claim exact. */
    if (!path_capability_is_current(owner->capability_owner, now_us,
                                    &next.path, &next.path.capability)) {
        return UCN_V6_ERR_STATE;
    }
    *selection = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_select_forward(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_frame_t *authenticated_frame,
    uint64_t flow_id,
    uint64_t packet_sequence,
    ucn_v6_route_policy_t policy,
    ucn_v6_route_domain_t *resolved_domain,
    ucn_v6_route_selection_t *selection)
{
    const ucn_v6_route_domain_t *matched = NULL;
    ucn_v6_route_select_request_t request;
    ucn_v6_route_selection_t selected;
    ucn_v6_route_domain_t domain;
    size_t index;
    ucn_v6_result_t result;

    if (!owner_is_valid(owner) || authenticated_frame == NULL ||
        resolved_domain == NULL || selection == NULL || flow_id == 0U ||
        (authenticated_frame->flags & UCN_V6_FLAG_ROUTE_CONTEXT) == 0U ||
        authenticated_frame->route_generation == 0U ||
        authenticated_frame->route_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        authenticated_frame->session_generation == 0U ||
        authenticated_frame->session_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        policy < UCN_V6_ROUTE_POLICY_PINNED ||
        policy > UCN_V6_ROUTE_POLICY_WEIGHTED_MULTIPATH ||
        ucn_v6_memory_ranges_overlap(
            authenticated_frame, sizeof(*authenticated_frame),
            resolved_domain, sizeof(*resolved_domain)) ||
        ucn_v6_memory_ranges_overlap(
            authenticated_frame, sizeof(*authenticated_frame),
            selection, sizeof(*selection)) ||
        ucn_v6_memory_ranges_overlap(
            resolved_domain, sizeof(*resolved_domain),
            selection, sizeof(*selection))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        const ucn_v6_route_domain_t *candidate;
        if (!owner->sets[index].occupied) continue;
        candidate = &owner->sets[index].current.domain;
        if (owner->sets[index].current.route_generation !=
                authenticated_frame->route_generation ||
            candidate->origin_binding.realm_id != authenticated_frame->realm_id ||
            candidate->origin_binding.node_address !=
                authenticated_frame->source_address ||
            candidate->origin_binding.binding_generation !=
                authenticated_frame->source_binding_generation ||
            candidate->destination_binding.realm_id !=
                authenticated_frame->realm_id ||
            candidate->destination_binding.node_address !=
                authenticated_frame->destination_address ||
            candidate->destination_binding.binding_generation !=
                authenticated_frame->destination_binding_generation ||
            candidate->origin_session_generation !=
                authenticated_frame->session_generation) {
            continue;
        }
        if (matched != NULL) {
            owner->faulted = true;
            owner->stats.faulted = true;
            return UCN_V6_ERR_STATE;
        }
        matched = candidate;
    }
    if (matched == NULL) return UCN_V6_ERR_NOT_FOUND;
    domain = *matched;
    result = ucn_v6_route_accept_generation(
        owner, now_us, &domain, authenticated_frame->route_generation);
    if (result != UCN_V6_OK) return result;
    memset(&request, 0, sizeof(request));
    request.domain = domain;
    request.flow_id = flow_id;
    request.packet_sequence = packet_sequence;
    request.policy = policy;
    if (policy == UCN_V6_ROUTE_POLICY_PINNED) {
        if ((authenticated_frame->flags & UCN_V6_FLAG_PATH_CONTEXT) == 0U) {
            return UCN_V6_ERR_ARGUMENT;
        }
        request.pinned_path_id = authenticated_frame->path.path_id;
        request.pinned_path_generation =
            authenticated_frame->path.path_generation;
    }
    memset(&selected, 0, sizeof(selected));
    result = ucn_v6_route_select(owner, now_us, &request, &selected);
    if (result != UCN_V6_OK) return result;
    *resolved_domain = domain;
    *selection = selected;
    return UCN_V6_OK;
}

static ucn_v6_result_t resolve_path(
    const ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_route_domain_t *domain,
    uint32_t route_generation,
    uint16_t path_id,
    uint32_t path_generation,
    ucn_v6_route_path_t *path)
{
    const ucn_v6_route_set_slot_t *set;
    const ucn_v6_route_proposal_t *proposal = NULL;
    ucn_v6_route_path_t resolved;
    int index;
    if (!owner_is_valid(owner) || !domain_is_valid(domain) || path == NULL ||
        route_generation == 0U ||
        route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        path_id == 0U || path_id == UINT16_MAX || path_generation == 0U ||
        path_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    set = find_set_const(owner, domain);
    if (set == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (set->current.route_generation == route_generation) {
        proposal = &set->current;
    } else if (set->previous_valid &&
               set->previous.route_generation == route_generation &&
               now_us < set->previous_deadline_us) {
        proposal = &set->previous;
    } else {
        return UCN_V6_ERR_REPLAY;
    }
    index = find_path_index(proposal, owner->capability_owner, path_id,
                            path_generation, now_us);
    if (index < 0) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    resolved = proposal->paths[index];
    if (!path_capability_is_current(owner->capability_owner, now_us,
                                    &resolved,
                                    &resolved.capability)) {
        return UCN_V6_ERR_STATE;
    }
    *path = resolved;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_resolve_ref(
    const ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_route_path_ref_t *reference,
    ucn_v6_route_resolution_t *resolution)
{
    ucn_v6_route_resolution_t next;
    ucn_v6_result_t result;
    if (reference == NULL || resolution == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&next, 0, sizeof(next));
    result = resolve_path(
        owner, now_us, &reference->domain, reference->route_generation,
        reference->path_id, reference->path_generation, &next.path);
    if (result != UCN_V6_OK) {
        return result;
    }
    if (!principal_equal(&next.path.capability.destination_principal,
                         &reference->domain.destination_principal) ||
        !ucn_v6_binding_key_equal(
            &next.path.capability.destination_binding,
            &reference->domain.destination_binding) ||
        next.path.capability.destination_session_generation !=
            reference->domain.destination_session_generation) {
        return UCN_V6_ERR_STATE;
    }
    next.dependency.type = UCN_V6_STACK_INVALIDATE_PATH;
    next.dependency.link_id = next.path.capability.local_parent_link_id;
    next.dependency.link_generation =
        next.path.capability.local_parent_link_generation;
    next.dependency.session = next.path.capability.local_parent_session;
    next.dependency.capability_generation =
        next.path.capability.local_parent_capability_generation;
    next.dependency.path_id = reference->path_id;
    next.dependency.path_generation = reference->path_generation;
    if (!ucn_v6_stack_invalidation_is_valid(&next.dependency)) {
        return UCN_V6_ERR_STATE;
    }
    *resolution = next;
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
        route_generation == 0U ||
        route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        path_id == 0U || path_id > UCN_V6_PATH_ID_MAX ||
        path_generation == 0U ||
        path_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
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

static bool route_path_matches_invalidation(
    const ucn_v6_route_path_t *path,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    bool next_hop_parent;
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_LINK) {
        return path->egress_link_id == invalidation->link_id &&
               path->egress_link_generation ==
                   invalidation->link_generation;
    }
    next_hop_parent =
        path->egress_link_id == invalidation->link_id &&
        path->egress_link_generation == invalidation->link_generation &&
        session_equal(&path->next_hop, &invalidation->session);
    if (!next_hop_parent) {
        return false;
    }
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_SESSION) {
        return true;
    }
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_CAPABILITY) {
        return path->next_hop_capability_generation ==
               invalidation->capability_generation;
    }
    return path->next_hop_capability_generation ==
               invalidation->capability_generation &&
           path->path_id == invalidation->path_id &&
           path->path_generation == invalidation->path_generation;
}

static bool proposal_matches_invalidation(
    const ucn_v6_route_proposal_t *proposal,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    size_t index;
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_SESSION &&
        domain_origin_is_session(&proposal->domain, &invalidation->session)) {
        return true;
    }
    for (index = 0U; index < proposal->path_count; ++index) {
        if (route_path_matches_invalidation(&proposal->paths[index],
                                            invalidation)) {
            return true;
        }
    }
    return false;
}

ucn_v6_result_t ucn_v6_route_apply_invalidation(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    size_t index;
    if (!owner_is_valid(owner) ||
        !ucn_v6_stack_invalidation_is_valid(invalidation)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_CANDIDATES; ++index) {
        ucn_v6_route_candidate_view_t *candidate =
            &owner->candidates[index].value;
        if (candidate->occupied &&
            proposal_matches_invalidation(&candidate->proposal,
                                          invalidation)) {
            memset(candidate, 0, sizeof(*candidate));
            if (owner->stats.candidates != 0U) {
                --owner->stats.candidates;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        ucn_v6_route_set_slot_t *set = &owner->sets[index];
        bool current_matches;
        bool previous_matches;
        if (!set->occupied) {
            continue;
        }
        current_matches = proposal_matches_invalidation(&set->current,
                                                        invalidation);
        previous_matches = set->previous_valid &&
            proposal_matches_invalidation(&set->previous, invalidation);
        if (current_matches) {
            clear_pins_for_domain(owner, &set->current.domain);
            memset(set, 0, sizeof(*set));
            if (owner->stats.route_sets != 0U) {
                --owner->stats.route_sets;
            }
        } else if (previous_matches) {
            /* A delayed event for the grace-only proposal must not destroy
             * the already-switched current RouteSet.  Previous is an
             * independent immutable snapshot and can be retired alone.
             *
             * 仅匹配 Grace 上一代的迟到事件不得删除已切换的新 Current；
             * Previous 是独立不可变快照，可单独退休。 */
            memset(&set->previous, 0, sizeof(set->previous));
            set->previous_valid = false;
            set->previous_deadline_us = 0U;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ROUTE_SETS; ++index) {
        if (invalidation->type == UCN_V6_STACK_INVALIDATE_SESSION &&
            owner->domains[index].occupied &&
            domain_origin_is_session(&owner->domains[index].domain,
                                     &invalidation->session)) {
            /* Capability/Path are child generations and cannot erase the
             * Session-owned transaction replay floor.  Only retirement of
             * the exact Session parent releases this fixed slot.
             *
             * Capability/Path 属于子代际，不能清除 Session 所有的事务防重放
             * 高水位；只有精确 Session 父域退休才释放该固定槽。 */
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
        route_generation == 0U ||
        route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
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

static void protocol_write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void protocol_write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void protocol_write_u64(uint8_t *output, uint64_t value)
{
    protocol_write_u32(output, (uint32_t)(value >> 32U));
    protocol_write_u32(output + 4U, (uint32_t)value);
}

static uint16_t protocol_read_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t protocol_read_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | input[3];
}

static uint64_t protocol_read_u64(const uint8_t *input)
{
    return ((uint64_t)protocol_read_u32(input) << 32U) |
           protocol_read_u32(input + 4U);
}

static void protocol_write_binding(uint8_t *output,
                                   const ucn_v6_binding_key_t *binding)
{
    protocol_write_u32(output, binding->realm_id);
    protocol_write_u32(output + 4U, binding->node_address);
    protocol_write_u32(output + 8U, binding->binding_generation);
}

static void protocol_read_binding(const uint8_t *input,
                                  ucn_v6_binding_key_t *binding)
{
    binding->realm_id = protocol_read_u32(input);
    binding->node_address = protocol_read_u32(input + 4U);
    binding->binding_generation = protocol_read_u32(input + 8U);
}

static void protocol_write_domain(uint8_t *output,
                                  const ucn_v6_route_domain_t *domain)
{
    memcpy(output, domain->origin_principal.bytes, 16U);
    protocol_write_binding(output + 16U, &domain->origin_binding);
    protocol_write_u32(output + 28U, domain->origin_session_generation);
    memcpy(output + 32U, domain->destination_principal.bytes, 16U);
    protocol_write_binding(output + 48U, &domain->destination_binding);
    protocol_write_u32(output + 60U,
                       domain->destination_session_generation);
}

static void protocol_read_domain(const uint8_t *input,
                                 ucn_v6_route_domain_t *domain)
{
    memcpy(domain->origin_principal.bytes, input, 16U);
    protocol_read_binding(input + 16U, &domain->origin_binding);
    domain->origin_session_generation = protocol_read_u32(input + 28U);
    memcpy(domain->destination_principal.bytes, input + 32U, 16U);
    protocol_read_binding(input + 48U, &domain->destination_binding);
    domain->destination_session_generation = protocol_read_u32(input + 60U);
}

static size_t protocol_message_bytes(ucn_v6_route_protocol_kind_t kind)
{
    switch (kind) {
    case UCN_V6_ROUTE_DISCOVER_REQUEST:
        return 86U;
    case UCN_V6_ROUTE_DISCOVER_RESPONSE:
        return UCN_V6_ROUTE_PROTOCOL_MAX_BYTES;
    case UCN_V6_ROUTE_PATH_PROBE:
    case UCN_V6_ROUTE_PATH_PROBE_ACK:
        return 102U;
    case UCN_V6_ROUTE_PATH_ACTIVATE:
    case UCN_V6_ROUTE_PATH_ACTIVATE_ACK:
        return 96U;
    case UCN_V6_ROUTE_ERROR:
        return 88U;
    default:
        return 0U;
    }
}

static bool protocol_message_is_valid(
    const ucn_v6_route_protocol_message_t *message)
{
    const ucn_v6_route_protocol_kind_t kind =
        message == NULL ? (ucn_v6_route_protocol_kind_t)0 : message->kind;
    if (message == NULL || protocol_message_bytes(kind) == 0U ||
        message->candidate_transaction_id == 0U ||
        message->candidate_transaction_id >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        !domain_is_valid(&message->domain) ||
        message->route_generation == 0U ||
        message->route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return false;
    }
    if (kind == UCN_V6_ROUTE_DISCOVER_REQUEST) {
        return message->body.discover_request.maximum_hops != 0U &&
               message->body.discover_request.maximum_hops <=
                   UCN_V6_HOP_COUNT_MAX &&
               (message->body.discover_request.required_feature_bits &
                ~UCN_V6_CAPABILITY_KNOWN_FEATURES) == 0U;
    }
    if (kind == UCN_V6_ROUTE_DISCOVER_RESPONSE) {
        const ucn_v6_route_discover_response_body_t *response =
            &message->body.discover_response;
        return response->path_id != 0U && response->path_id != UINT16_MAX &&
               response->path_generation != 0U &&
               response->path_generation <=
                   UCN_V6_SERIAL_ROTATION_THRESHOLD &&
               response->hop_count != 0U &&
               response->hop_count <= UCN_V6_HOP_COUNT_MAX &&
               response->weight != 0U &&
               response->destination_capability_generation != 0U &&
               response->destination_capability_generation <=
                   UCN_V6_SERIAL_ROTATION_THRESHOLD &&
               bytes_nonzero(response->destination_capability_digest,
                             UCN_V6_CAPABILITY_DIGEST_BYTES) &&
               (response->destination_realtime_mode_bits &
                (uint16_t)~(UCN_V6_REALTIME_MODE_LOCAL |
                            UCN_V6_REALTIME_MODE_SYNCED |
                            UCN_V6_REALTIME_MODE_DEADLINE)) == 0U &&
               (((response->destination_realtime_mode_bits &
                  (UCN_V6_REALTIME_MODE_SYNCED |
                   UCN_V6_REALTIME_MODE_DEADLINE)) != 0U) ==
                (response->destination_clock_domain_id != 0U &&
                 response->destination_clock_domain_generation != 0U &&
                 response->destination_clock_domain_generation <=
                     UCN_V6_SERIAL_ROTATION_THRESHOLD)) &&
               response->path_frame_mtu != 0U &&
               response->path_frame_mtu <= UCN_V6_WIRE_MAX_FRAME_BYTES &&
               response->payload_budget != 0U &&
               response->payload_budget < response->path_frame_mtu &&
               response->fragment_data_budget != 0U &&
               response->fragment_data_budget <= response->payload_budget &&
               response->feature_bits != 0U &&
               (response->feature_bits &
                ~UCN_V6_CAPABILITY_KNOWN_FEATURES) == 0U &&
               response->hop_suite_bits != 0U &&
               (response->hop_suite_bits &
                ~UCN_V6_CAPABILITY_HOP_SUITE_BITS) == 0U &&
               response->e2e_suite_bits != 0U &&
               (response->e2e_suite_bits &
                ~UCN_V6_CAPABILITY_E2E_SUITE_BITS) == 0U &&
               (uint32_t)response->max_message_class <=
                   (uint32_t)UCN_V6_MESSAGE_T8K &&
               response->max_window != 0U &&
               response->max_concurrency != 0U &&
               (response->timestamp_capability_bits &
                (uint16_t)~(UCN_V6_TIMESTAMP_RX_SOFTWARE |
                            UCN_V6_TIMESTAMP_TX_SOFTWARE |
                            UCN_V6_TIMESTAMP_RX_HARDWARE |
                            UCN_V6_TIMESTAMP_TX_HARDWARE)) == 0U &&
               ((response->timestamp_capability_bits == 0U) ==
                (response->timestamp_uncertainty_us == 0U));
    }
    if (kind == UCN_V6_ROUTE_PATH_PROBE ||
        kind == UCN_V6_ROUTE_PATH_PROBE_ACK) {
        return message->body.probe.path_id != 0U &&
               message->body.probe.path_id != UINT16_MAX &&
               message->body.probe.path_generation != 0U &&
               message->body.probe.path_generation <=
                   UCN_V6_SERIAL_ROTATION_THRESHOLD &&
               bytes_nonzero(message->body.probe.proposal_digest,
                             UCN_V6_ROUTE_PROPOSAL_DIGEST_BYTES);
    }
    if (kind == UCN_V6_ROUTE_PATH_ACTIVATE ||
        kind == UCN_V6_ROUTE_PATH_ACTIVATE_ACK) {
        return bytes_nonzero(message->body.activation.proposal_digest,
                             UCN_V6_ROUTE_PROPOSAL_DIGEST_BYTES);
    }
    return message->body.error.path_id != 0U &&
           message->body.error.path_id != UINT16_MAX &&
           message->body.error.path_generation != 0U &&
           message->body.error.path_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           message->body.error.reason != 0U;
}

uint16_t ucn_v6_route_protocol_opcode(ucn_v6_route_protocol_kind_t kind)
{
    switch (kind) {
    case UCN_V6_ROUTE_DISCOVER_REQUEST:
        return UCN_V6_PROTOCOL_OPCODE_ROUTE_DISCOVER_REQUEST;
    case UCN_V6_ROUTE_DISCOVER_RESPONSE:
        return UCN_V6_PROTOCOL_OPCODE_ROUTE_DISCOVER_RESPONSE;
    case UCN_V6_ROUTE_PATH_PROBE:
        return UCN_V6_PROTOCOL_OPCODE_ROUTE_PATH_PROBE;
    case UCN_V6_ROUTE_PATH_PROBE_ACK:
        return UCN_V6_PROTOCOL_OPCODE_ROUTE_PATH_PROBE_ACK;
    case UCN_V6_ROUTE_PATH_ACTIVATE:
        return UCN_V6_PROTOCOL_OPCODE_ROUTE_PATH_ACTIVATE;
    case UCN_V6_ROUTE_PATH_ACTIVATE_ACK:
        return UCN_V6_PROTOCOL_OPCODE_ROUTE_PATH_ACTIVATE_ACK;
    case UCN_V6_ROUTE_ERROR:
        return UCN_V6_PROTOCOL_OPCODE_ROUTE_ERROR;
    default:
        return 0U;
    }
}

static ucn_v6_route_protocol_kind_t protocol_kind_from_opcode(
    uint16_t opcode)
{
    switch (opcode) {
    case UCN_V6_PROTOCOL_OPCODE_ROUTE_DISCOVER_REQUEST:
        return UCN_V6_ROUTE_DISCOVER_REQUEST;
    case UCN_V6_PROTOCOL_OPCODE_ROUTE_DISCOVER_RESPONSE:
        return UCN_V6_ROUTE_DISCOVER_RESPONSE;
    case UCN_V6_PROTOCOL_OPCODE_ROUTE_PATH_PROBE:
        return UCN_V6_ROUTE_PATH_PROBE;
    case UCN_V6_PROTOCOL_OPCODE_ROUTE_PATH_PROBE_ACK:
        return UCN_V6_ROUTE_PATH_PROBE_ACK;
    case UCN_V6_PROTOCOL_OPCODE_ROUTE_PATH_ACTIVATE:
        return UCN_V6_ROUTE_PATH_ACTIVATE;
    case UCN_V6_PROTOCOL_OPCODE_ROUTE_PATH_ACTIVATE_ACK:
        return UCN_V6_ROUTE_PATH_ACTIVATE_ACK;
    case UCN_V6_PROTOCOL_OPCODE_ROUTE_ERROR:
        return UCN_V6_ROUTE_ERROR;
    default:
        return (ucn_v6_route_protocol_kind_t)0;
    }
}

ucn_v6_result_t ucn_v6_route_protocol_encode(
    const ucn_v6_route_protocol_message_t *message,
    uint8_t *output, size_t output_capacity, size_t *output_length)
{
    uint8_t encoded[UCN_V6_ROUTE_PROTOCOL_MAX_BYTES];
    size_t required;
    if (!protocol_message_is_valid(message) || output == NULL ||
        output_length == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    required = protocol_message_bytes(message->kind);
    if (output_capacity < required) return UCN_V6_ERR_NO_SPACE;
    if (ucn_v6_memory_ranges_overlap(message, sizeof(*message), output,
                                     required) ||
        ucn_v6_memory_ranges_overlap(message, sizeof(*message), output_length,
                                     sizeof(*output_length)) ||
        ucn_v6_memory_ranges_overlap(output, required, output_length,
                                     sizeof(*output_length))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = UCN_V6_ROUTE_PROTOCOL_SCHEMA;
    encoded[1] = (uint8_t)message->kind;
    protocol_write_u64(&encoded[4], message->candidate_transaction_id);
    protocol_write_domain(&encoded[12], &message->domain);
    protocol_write_u32(&encoded[76], message->route_generation);
    if (message->kind == UCN_V6_ROUTE_DISCOVER_REQUEST) {
        protocol_write_u16(&encoded[80],
                           message->body.discover_request.maximum_hops);
        protocol_write_u32(&encoded[82],
                           message->body.discover_request.required_feature_bits);
    } else if (message->kind == UCN_V6_ROUTE_DISCOVER_RESPONSE) {
        const ucn_v6_route_discover_response_body_t *response =
            &message->body.discover_response;
        protocol_write_u16(&encoded[80], response->path_id);
        protocol_write_u32(&encoded[82], response->path_generation);
        protocol_write_u16(&encoded[86], response->hop_count);
        protocol_write_u16(&encoded[88], response->priority);
        protocol_write_u16(&encoded[90], response->weight);
        protocol_write_u32(&encoded[92],
                           response->destination_capability_generation);
        memcpy(&encoded[96], response->destination_capability_digest, 16U);
        protocol_write_u16(&encoded[112],
                           response->destination_realtime_mode_bits);
        protocol_write_u16(&encoded[114],
                           response->destination_clock_domain_id);
        protocol_write_u32(&encoded[116],
                           response->destination_clock_domain_generation);
        protocol_write_u32(&encoded[120], response->path_frame_mtu);
        protocol_write_u32(&encoded[124], response->payload_budget);
        protocol_write_u32(&encoded[128], response->fragment_data_budget);
        protocol_write_u32(&encoded[132], response->feature_bits);
        protocol_write_u32(&encoded[136], response->hop_suite_bits);
        protocol_write_u32(&encoded[140], response->e2e_suite_bits);
        encoded[144] = (uint8_t)response->max_message_class;
        protocol_write_u16(&encoded[145], response->max_window);
        protocol_write_u16(&encoded[147], response->max_concurrency);
        protocol_write_u16(&encoded[149],
                           response->timestamp_capability_bits);
        protocol_write_u32(&encoded[151],
                           response->timestamp_uncertainty_us);
    } else if (message->kind == UCN_V6_ROUTE_PATH_PROBE ||
               message->kind == UCN_V6_ROUTE_PATH_PROBE_ACK) {
        protocol_write_u16(&encoded[80], message->body.probe.path_id);
        protocol_write_u32(&encoded[82],
                           message->body.probe.path_generation);
        memcpy(&encoded[86], message->body.probe.proposal_digest, 16U);
    } else if (message->kind == UCN_V6_ROUTE_PATH_ACTIVATE ||
               message->kind == UCN_V6_ROUTE_PATH_ACTIVATE_ACK) {
        memcpy(&encoded[80], message->body.activation.proposal_digest, 16U);
    } else {
        protocol_write_u16(&encoded[80], message->body.error.path_id);
        protocol_write_u32(&encoded[82],
                           message->body.error.path_generation);
        encoded[86] = message->body.error.reason;
    }
    memcpy(output, encoded, required);
    *output_length = required;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_route_protocol_decode(
    uint16_t protocol_opcode,
    const uint8_t *input, size_t input_length,
    ucn_v6_route_protocol_message_t *message)
{
    ucn_v6_route_protocol_message_t decoded;
    ucn_v6_route_protocol_kind_t kind =
        protocol_kind_from_opcode(protocol_opcode);
    size_t required = protocol_message_bytes(kind);
    if (input == NULL || message == NULL || required == 0U ||
        input_length != required ||
        ucn_v6_memory_ranges_overlap(input, input_length, message,
                                     sizeof(*message))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (input[0] != UCN_V6_ROUTE_PROTOCOL_SCHEMA ||
        input[1] != (uint8_t)kind || input[2] != 0U || input[3] != 0U ||
        (kind == UCN_V6_ROUTE_ERROR && input[87] != 0U)) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.kind = kind;
    decoded.candidate_transaction_id = protocol_read_u64(&input[4]);
    protocol_read_domain(&input[12], &decoded.domain);
    decoded.route_generation = protocol_read_u32(&input[76]);
    if (kind == UCN_V6_ROUTE_DISCOVER_REQUEST) {
        decoded.body.discover_request.maximum_hops =
            protocol_read_u16(&input[80]);
        decoded.body.discover_request.required_feature_bits =
            protocol_read_u32(&input[82]);
    } else if (kind == UCN_V6_ROUTE_DISCOVER_RESPONSE) {
        ucn_v6_route_discover_response_body_t *response =
            &decoded.body.discover_response;
        response->path_id = protocol_read_u16(&input[80]);
        response->path_generation = protocol_read_u32(&input[82]);
        response->hop_count = protocol_read_u16(&input[86]);
        response->priority = protocol_read_u16(&input[88]);
        response->weight = protocol_read_u16(&input[90]);
        response->destination_capability_generation =
            protocol_read_u32(&input[92]);
        memcpy(response->destination_capability_digest, &input[96], 16U);
        response->destination_realtime_mode_bits =
            protocol_read_u16(&input[112]);
        response->destination_clock_domain_id =
            protocol_read_u16(&input[114]);
        response->destination_clock_domain_generation =
            protocol_read_u32(&input[116]);
        response->path_frame_mtu = protocol_read_u32(&input[120]);
        response->payload_budget = protocol_read_u32(&input[124]);
        response->fragment_data_budget = protocol_read_u32(&input[128]);
        response->feature_bits = protocol_read_u32(&input[132]);
        response->hop_suite_bits = protocol_read_u32(&input[136]);
        response->e2e_suite_bits = protocol_read_u32(&input[140]);
        response->max_message_class =
            (ucn_v6_message_class_t)input[144];
        response->max_window = protocol_read_u16(&input[145]);
        response->max_concurrency = protocol_read_u16(&input[147]);
        response->timestamp_capability_bits =
            protocol_read_u16(&input[149]);
        response->timestamp_uncertainty_us =
            protocol_read_u32(&input[151]);
    } else if (kind == UCN_V6_ROUTE_PATH_PROBE ||
               kind == UCN_V6_ROUTE_PATH_PROBE_ACK) {
        decoded.body.probe.path_id = protocol_read_u16(&input[80]);
        decoded.body.probe.path_generation = protocol_read_u32(&input[82]);
        memcpy(decoded.body.probe.proposal_digest, &input[86], 16U);
    } else if (kind == UCN_V6_ROUTE_PATH_ACTIVATE ||
               kind == UCN_V6_ROUTE_PATH_ACTIVATE_ACK) {
        memcpy(decoded.body.activation.proposal_digest, &input[80], 16U);
    } else {
        decoded.body.error.path_id = protocol_read_u16(&input[80]);
        decoded.body.error.path_generation = protocol_read_u32(&input[82]);
        decoded.body.error.reason = input[86];
    }
    if (!protocol_message_is_valid(&decoded)) return UCN_V6_ERR_MALFORMED;
    *message = decoded;
    return UCN_V6_OK;
}
