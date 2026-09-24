#include "internal/ucn_cluster.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool bytes_nonzero(const uint8_t *bytes, size_t count)
{
    size_t index;
    uint8_t value = 0U;
    for (index = 0U; index < count; ++index) value |= bytes[index];
    return value != 0U;
}

static bool directory_valid(const ucn_i_cluster_directory_fact_t *fact,
                            uint64_t now_us)
{
    return fact != NULL &&
           ucn_i_cluster_p_epoch_valid(&fact->remote_epoch) &&
           fact->origin_sequence != 0U &&
           fact->authority_deadline_us > now_us &&
           fact->capability_deadline_us > now_us &&
           fact->flow_deadline_us > now_us &&
           fact->remote_config_id != 0U &&
           fact->remote_config_generation != 0U &&
           fact->authority_generation != 0U &&
           fact->source_session_generation != 0U &&
           fact->capability_generation != 0U &&
           fact->route_generation != 0U && fact->path_generation != 0U &&
           fact->link_generation != 0U && fact->path_id != 0U &&
           fact->path_id != UINT16_MAX && fact->link_id != 0U &&
           fact->link_id != UINT16_MAX && fact->authenticated == 1U &&
           fact->quorum_verified == 1U && fact->flow_active == 1U &&
           fact->reserved_zero == 0U &&
           bytes_nonzero(fact->capability_digest,
                         sizeof(fact->capability_digest)) &&
           bytes_nonzero(fact->authority_digest,
                         sizeof(fact->authority_digest));
}

static bool directory_equal(const ucn_i_cluster_directory_fact_t *left,
                            const ucn_i_cluster_directory_fact_t *right)
{
    return left->remote_epoch.cluster_id == right->remote_epoch.cluster_id &&
           left->remote_epoch.term == right->remote_epoch.term &&
           left->remote_epoch.head_binding_generation ==
               right->remote_epoch.head_binding_generation &&
           ucn_i_cluster_p_principal_equal(
               left->remote_epoch.head_principal,
               right->remote_epoch.head_principal) &&
           left->origin_sequence == right->origin_sequence &&
           left->authority_deadline_us == right->authority_deadline_us &&
           left->capability_deadline_us == right->capability_deadline_us &&
           left->flow_deadline_us == right->flow_deadline_us &&
           left->remote_config_id == right->remote_config_id &&
           left->remote_config_generation ==
               right->remote_config_generation &&
           left->authority_generation == right->authority_generation &&
           left->source_session_generation ==
               right->source_session_generation &&
           left->capability_generation == right->capability_generation &&
           left->route_generation == right->route_generation &&
           left->path_generation == right->path_generation &&
           left->link_generation == right->link_generation &&
           left->path_id == right->path_id && left->link_id == right->link_id &&
           memcmp(left->capability_digest, right->capability_digest,
                  UCN_I_CLUSTER_DIGEST_BYTES) == 0 &&
           memcmp(left->authority_digest, right->authority_digest,
                  UCN_I_CLUSTER_DIGEST_BYTES) == 0 &&
           left->authenticated == right->authenticated &&
           left->quorum_verified == right->quorum_verified &&
           left->flow_active == right->flow_active;
}

static bool authority_domain_equal(
    const ucn_i_cluster_directory_fact_t *left,
    const ucn_i_cluster_directory_fact_t *right)
{
    return left->remote_epoch.cluster_id == right->remote_epoch.cluster_id &&
           left->remote_epoch.term == right->remote_epoch.term &&
           left->remote_epoch.head_binding_generation ==
               right->remote_epoch.head_binding_generation &&
           ucn_i_cluster_p_principal_equal(
               left->remote_epoch.head_principal,
               right->remote_epoch.head_principal) &&
           left->remote_config_id == right->remote_config_id &&
           left->remote_config_generation ==
               right->remote_config_generation &&
           left->authority_generation == right->authority_generation &&
           memcmp(left->authority_digest, right->authority_digest,
                  UCN_I_CLUSTER_DIGEST_BYTES) == 0;
}

static bool directory_live(const ucn_i_cluster_directory_slot_t *slot,
                           uint64_t now_us)
{
    return slot->occupied != 0U && directory_valid(&slot->fact, now_us);
}

static bool flow_valid(const ucn_i_cluster_flow_fact_t *flow,
                       uint64_t now_us)
{
    return flow != NULL && flow->deadline_us > now_us &&
           flow->source_binding_generation != 0U &&
           flow->source_session_generation != 0U &&
           flow->destination_binding_generation != 0U &&
           flow->destination_session_generation != 0U &&
           flow->capability_generation != 0U &&
           flow->route_generation != 0U && flow->path_generation != 0U &&
           flow->link_generation != 0U && flow->path_id != 0U &&
           flow->path_id != UINT16_MAX && flow->link_id != 0U &&
           flow->link_id != UINT16_MAX && flow->active == 1U &&
           flow->authenticated == 1U &&
           flow->reserved_zero[0] == 0U && flow->reserved_zero[1] == 0U &&
           flow->reserved_zero[2] == 0U && flow->reserved_zero[3] == 0U &&
           flow->reserved_zero[4] == 0U && flow->reserved_zero[5] == 0U &&
           bytes_nonzero(flow->source_principal,
                         sizeof(flow->source_principal)) &&
           bytes_nonzero(flow->destination_principal,
                         sizeof(flow->destination_principal)) &&
           bytes_nonzero(flow->capability_digest,
                         sizeof(flow->capability_digest));
}

static bool flow_equal(const ucn_i_cluster_flow_fact_t *left,
                       const ucn_i_cluster_flow_fact_t *right)
{
    return left->deadline_us == right->deadline_us &&
           left->source_binding_generation ==
               right->source_binding_generation &&
           left->source_session_generation ==
               right->source_session_generation &&
           left->destination_binding_generation ==
               right->destination_binding_generation &&
           left->destination_session_generation ==
               right->destination_session_generation &&
           left->capability_generation == right->capability_generation &&
           left->route_generation == right->route_generation &&
           left->path_generation == right->path_generation &&
           left->link_generation == right->link_generation &&
           left->path_id == right->path_id && left->link_id == right->link_id &&
           ucn_i_cluster_p_principal_equal(left->source_principal,
                                           right->source_principal) &&
           ucn_i_cluster_p_principal_equal(left->destination_principal,
                                           right->destination_principal) &&
           memcmp(left->capability_digest, right->capability_digest,
                  UCN_I_CLUSTER_DIGEST_BYTES) == 0 &&
           left->active == right->active &&
           left->authenticated == right->authenticated;
}

static bool tunnel_equal(const ucn_i_cluster_tunnel_request_t *left,
                         const ucn_i_cluster_tunnel_request_t *right)
{
    return left->tunnel_id == right->tunnel_id &&
           left->directory_origin_sequence ==
               right->directory_origin_sequence &&
           left->absolute_deadline_us == right->absolute_deadline_us &&
           left->source_cluster_id == right->source_cluster_id &&
           left->destination_cluster_id == right->destination_cluster_id &&
           flow_equal(&left->flow, &right->flow);
}

static ucn_result_t authority_locked(ucn_i_cluster_owner_t *owner,
                                     uint64_t now_us)
{
    uint32_t old_live;
    uint32_t new_live;
    ucn_result_t result = ucn_i_cluster_p_refresh(owner, now_us,
                                                  &old_live, &new_live);
    if (result != UCN_OK) return result;
    return owner->authority_active != 0U ? UCN_OK : UCN_ERR_ACCESS;
}

static int directory_find(const ucn_i_cluster_owner_t *owner,
                          uint32_t cluster_id)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT; ++index) {
        if (owner->directory[index].occupied != 0U &&
            owner->directory[index].fact.remote_epoch.cluster_id ==
                cluster_id) return (int)index;
    }
    return -1;
}

static bool local_source_session_current(
    const ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_flow_fact_t *flow, uint64_t now_us)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT; ++index) {
        const ucn_i_cluster_member_slot_t *slot = &owner->members[index];
        if (slot->occupied != 0U && slot->fact.authenticated == 1U &&
            slot->fact.current == 1U &&
            slot->fact.lease_deadline_us > now_us &&
            slot->fact.capability_deadline_us > now_us &&
            slot->fact.binding_generation ==
                flow->source_binding_generation &&
            slot->fact.session_generation ==
                flow->source_session_generation &&
            ucn_i_cluster_p_principal_equal(
                slot->fact.principal, flow->source_principal)) {
            return true;
        }
    }
    return false;
}

ucn_result_t ucn_i_cluster_directory_install(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_directory_fact_t *fact, uint64_t now_us)
{
    int existing;
    int empty = -1;
    uint8_t index;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        !directory_valid(fact, now_us) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), fact,
                             sizeof(*fact))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (fact->remote_epoch.cluster_id == owner->state.epoch.cluster_id ||
        authority_locked(owner, now_us) != UCN_OK) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    existing = directory_find(owner, fact->remote_epoch.cluster_id);
    if (existing >= 0) {
        ucn_i_cluster_directory_slot_t *slot = &owner->directory[existing];
        if (!directory_live(slot, now_us)) {
            empty = existing;
        } else if (fact->authority_generation <
                       slot->fact.authority_generation ||
                   (fact->authority_generation ==
                        slot->fact.authority_generation &&
                    fact->origin_sequence < slot->fact.origin_sequence)) {
            result = UCN_ERR_REPLAY;
            goto done;
        } else if (fact->authority_generation ==
                       slot->fact.authority_generation &&
                   fact->origin_sequence == slot->fact.origin_sequence) {
            result = directory_equal(fact, &slot->fact) ? UCN_OK :
                     UCN_ERR_SECURITY;
            goto done;
        } else if ((fact->authority_generation ==
                        slot->fact.authority_generation &&
                    !authority_domain_equal(fact, &slot->fact)) ||
                   (fact->authority_generation >
                        slot->fact.authority_generation &&
                    (fact->remote_epoch.term < slot->fact.remote_epoch.term ||
                     fact->remote_config_generation <
                         slot->fact.remote_config_generation ||
                     (fact->remote_epoch.term ==
                          slot->fact.remote_epoch.term &&
                      (!ucn_i_cluster_p_principal_equal(
                           fact->remote_epoch.head_principal,
                           slot->fact.remote_epoch.head_principal) ||
                       fact->remote_epoch.head_binding_generation !=
                           slot->fact.remote_epoch.head_binding_generation))))) {
            result = UCN_ERR_SECURITY;
            goto done;
        } else {
            slot->fact = *fact;
            result = UCN_OK;
            goto done;
        }
    }
    if (empty < 0) {
        for (index = 0U; index < UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT;
             ++index) {
            if (owner->directory[index].occupied == 0U ||
                !directory_live(&owner->directory[index], now_us)) {
                empty = (int)index;
                break;
            }
        }
    }
    if (empty < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    memset(&owner->directory[empty], 0, sizeof(owner->directory[empty]));
    owner->directory[empty].fact = *fact;
    owner->directory[empty].occupied = 1U;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_directory_copy(
    ucn_i_cluster_owner_t *owner, uint32_t remote_cluster_id,
    uint64_t now_us, ucn_i_cluster_directory_fact_t *fact_out)
{
    int index;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || remote_cluster_id == 0U ||
        fact_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), fact_out,
                             sizeof(*fact_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    index = directory_find(owner, remote_cluster_id);
    if (authority_locked(owner, now_us) != UCN_OK) {
        result = UCN_ERR_ACCESS;
    } else if (index < 0 ||
               !directory_live(&owner->directory[index], now_us)) {
        result = UCN_ERR_NOT_FOUND;
    } else {
        *fact_out = owner->directory[index].fact;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

static bool request_matches_directory(
    const ucn_i_cluster_tunnel_request_t *request,
    const ucn_i_cluster_directory_fact_t *directory, uint64_t now_us)
{
    return flow_valid(&request->flow, now_us) &&
           request->directory_origin_sequence == directory->origin_sequence &&
           request->destination_cluster_id ==
               directory->remote_epoch.cluster_id &&
           request->absolute_deadline_us > now_us &&
           request->absolute_deadline_us <= request->flow.deadline_us &&
           request->absolute_deadline_us <= directory->authority_deadline_us &&
           request->absolute_deadline_us <=
               directory->capability_deadline_us &&
           request->absolute_deadline_us <= directory->flow_deadline_us &&
           ucn_i_cluster_p_principal_equal(
               request->flow.destination_principal,
               directory->remote_epoch.head_principal) &&
           request->flow.destination_binding_generation ==
               directory->remote_epoch.head_binding_generation &&
           request->flow.destination_session_generation ==
               directory->source_session_generation &&
           request->flow.capability_generation ==
               directory->capability_generation &&
           request->flow.route_generation == directory->route_generation &&
           request->flow.path_generation == directory->path_generation &&
           request->flow.link_generation == directory->link_generation &&
           request->flow.path_id == directory->path_id &&
           request->flow.link_id == directory->link_id &&
           memcmp(request->flow.capability_digest,
                  directory->capability_digest,
                  UCN_I_CLUSTER_DIGEST_BYTES) == 0;
}

ucn_result_t ucn_i_cluster_tunnel_install(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_tunnel_request_t *request, uint64_t now_us)
{
    int directory_index;
    int empty = -1;
    uint8_t index;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || request == NULL ||
        request->tunnel_id == 0U || request->directory_origin_sequence == 0U ||
        request->source_cluster_id == 0U ||
        request->destination_cluster_id == 0U ||
        request->source_cluster_id == request->destination_cluster_id ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    directory_index = directory_find(owner, request->destination_cluster_id);
    if (authority_locked(owner, now_us) != UCN_OK ||
        request->source_cluster_id != owner->state.epoch.cluster_id ||
        !ucn_i_cluster_p_principal_equal(
            request->flow.source_principal,
            owner->state.epoch.head_principal) ||
        request->flow.source_binding_generation !=
            owner->state.epoch.head_binding_generation ||
        !local_source_session_current(owner, &request->flow, now_us) ||
        directory_index < 0 ||
        !directory_live(&owner->directory[directory_index], now_us) ||
        !request_matches_directory(
            request, &owner->directory[directory_index].fact, now_us)) {
        result = UCN_ERR_ACCESS;
        goto done;
    }
    for (index = 0U; index < UCN_I_CLUSTER_TUNNEL_SLOT_COUNT; ++index) {
        ucn_i_cluster_tunnel_slot_t *slot = &owner->tunnels[index];
        if (slot->occupied != 0U &&
            slot->value.tunnel_id == request->tunnel_id) {
            if (slot->value.absolute_deadline_us <= now_us) {
                empty = (int)index;
                break;
            }
            result = tunnel_equal(&slot->value, request) ? UCN_OK :
                     UCN_ERR_SECURITY;
            goto done;
        }
        if (empty < 0 && (slot->occupied == 0U ||
                          slot->value.absolute_deadline_us <= now_us)) {
            empty = (int)index;
        }
    }
    if (empty < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    memset(&owner->tunnels[empty], 0, sizeof(owner->tunnels[empty]));
    owner->tunnels[empty].value = *request;
    owner->tunnels[empty].occupied = 1U;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_tunnel_preflight(
    ucn_i_cluster_owner_t *owner, uint64_t tunnel_id,
    const ucn_i_cluster_flow_fact_t *current_flow, uint64_t now_us,
    ucn_i_cluster_tunnel_request_t *tunnel_out)
{
    uint8_t index;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || tunnel_id == 0U ||
        !flow_valid(current_flow, now_us) || tunnel_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), current_flow,
                             sizeof(*current_flow)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), tunnel_out,
                             sizeof(*tunnel_out)) ||
        ucn_i_ranges_overlap(current_flow, sizeof(*current_flow), tunnel_out,
                             sizeof(*tunnel_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    result = UCN_ERR_NOT_FOUND;
    for (index = 0U; index < UCN_I_CLUSTER_TUNNEL_SLOT_COUNT; ++index) {
        ucn_i_cluster_tunnel_slot_t *slot = &owner->tunnels[index];
        int directory_index;
        if (slot->occupied == 0U ||
            slot->value.tunnel_id != tunnel_id) continue;
        directory_index = directory_find(
            owner, slot->value.destination_cluster_id);
        if (authority_locked(owner, now_us) != UCN_OK ||
            slot->value.absolute_deadline_us <= now_us ||
            slot->value.source_cluster_id != owner->state.epoch.cluster_id ||
            !ucn_i_cluster_p_principal_equal(
                slot->value.flow.source_principal,
                owner->state.epoch.head_principal) ||
            slot->value.flow.source_binding_generation !=
                owner->state.epoch.head_binding_generation ||
            !local_source_session_current(owner, current_flow, now_us) ||
            directory_index < 0 ||
            !directory_live(&owner->directory[directory_index], now_us) ||
            !flow_equal(&slot->value.flow, current_flow) ||
            !request_matches_directory(
                &slot->value, &owner->directory[directory_index].fact,
                now_us)) {
            result = UCN_ERR_ACCESS;
        } else {
            *tunnel_out = slot->value;
            result = UCN_OK;
        }
        break;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_dependency_revoke(
    ucn_i_cluster_owner_t *owner,
    const uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES],
    uint32_t binding_generation, uint32_t session_generation,
    uint32_t capability_generation, uint16_t link_id,
    uint32_t link_generation)
{
    uint8_t index;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || principal == NULL ||
        !bytes_nonzero(principal, UCN_I_CLUSTER_PRINCIPAL_BYTES) ||
        binding_generation == 0U || session_generation == 0U ||
        capability_generation == 0U || link_id == 0U ||
        link_id == UINT16_MAX || link_generation == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), principal,
                             UCN_I_CLUSTER_PRINCIPAL_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    for (index = 0U; index < UCN_I_CLUSTER_DIRECTORY_SLOT_COUNT; ++index) {
        ucn_i_cluster_directory_fact_t *fact = &owner->directory[index].fact;
        if (owner->directory[index].occupied != 0U &&
            ucn_i_cluster_p_principal_equal(
                fact->remote_epoch.head_principal, principal) &&
            fact->remote_epoch.head_binding_generation ==
                binding_generation &&
            fact->source_session_generation == session_generation &&
            fact->capability_generation == capability_generation &&
            fact->link_id == link_id &&
            fact->link_generation == link_generation) {
            memset(&owner->directory[index], 0,
                   sizeof(owner->directory[index]));
        }
    }
    for (index = 0U; index < UCN_I_CLUSTER_TUNNEL_SLOT_COUNT; ++index) {
        ucn_i_cluster_flow_fact_t *flow = &owner->tunnels[index].value.flow;
        if (owner->tunnels[index].occupied != 0U &&
            ucn_i_cluster_p_principal_equal(
                flow->destination_principal, principal) &&
            flow->destination_binding_generation == binding_generation &&
            flow->destination_session_generation == session_generation &&
            flow->capability_generation == capability_generation &&
            flow->link_id == link_id &&
            flow->link_generation == link_generation) {
            memset(&owner->tunnels[index], 0,
                   sizeof(owner->tunnels[index]));
        }
    }
    owner->state_lock.leave(owner->state_lock.context);
    return UCN_OK;
}
