#include "internal/ucn_group.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool object_zero(const void *object, size_t bytes)
{
    const uint8_t *value = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (value[index] != 0U) return false;
    }
    return true;
}

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;

    if (bytes == NULL) return false;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) return true;
    }
    return false;
}

static bool lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | bytes[3];
}

static uint64_t read_be64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | bytes[index];
    }
    return value;
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void write_be64(uint8_t *bytes, uint64_t value)
{
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[7U - index] = (uint8_t)(value >> ((uint32_t)index * 8U));
    }
}

static bool security_ref_valid(const ucn_i_group_security_ref_t *security)
{
    if (security == NULL || security->valid > 1U ||
        security->exact_principal > 1U) return false;
    if (security->valid == 0U) {
        return object_zero(security, sizeof(*security));
    }
    return security->runtime_instance != 0U && security->owner_instance != 0U &&
           security->generation != 0U && security->key_generation != 0U &&
           bytes_nonzero(security->context_digest,
                         sizeof(security->context_digest));
}

static bool tree_ref_valid(const ucn_i_group_tree_ref_t *tree)
{
    if (tree == NULL || tree->valid > 1U || tree->reserved_zero != 0U) {
        return false;
    }
    if (tree->valid == 0U) return object_zero(tree, sizeof(*tree));
    return tree->route_causal_id != 0U && tree->route_generation != 0U &&
           tree->tree_generation != 0U && tree->owner_instance != 0U &&
           tree->generation != 0U;
}

static bool member_valid(const ucn_i_group_member_t *member)
{
    return member != NULL && member->address != 0U &&
           member->binding_generation != 0U && member->sender_slot != 0U &&
           member->weight != 0U && member->reserved_zero == 0U &&
           bytes_nonzero(member->principal, sizeof(member->principal));
}

static bool member_equal(const ucn_i_group_member_t *left,
                         const ucn_i_group_member_t *right)
{
    return left->address == right->address &&
           left->binding_generation == right->binding_generation &&
           left->sender_slot == right->sender_slot &&
           left->weight == right->weight &&
           left->reserved_zero == right->reserved_zero &&
           memcmp(left->principal, right->principal,
                  UCN_I_GROUP_PRINCIPAL_BYTES) == 0;
}

static bool security_ref_equal(const ucn_i_group_security_ref_t *left,
                               const ucn_i_group_security_ref_t *right)
{
    return left->runtime_instance == right->runtime_instance &&
           left->key_generation == right->key_generation &&
           left->owner_instance == right->owner_instance &&
           left->slot == right->slot &&
           left->generation == right->generation &&
           left->valid == right->valid &&
           left->exact_principal == right->exact_principal &&
           memcmp(left->context_digest, right->context_digest,
                  UCN_I_GROUP_DIGEST_BYTES) == 0;
}

static bool tree_ref_equal(const ucn_i_group_tree_ref_t *left,
                           const ucn_i_group_tree_ref_t *right)
{
    return left->route_causal_id == right->route_causal_id &&
           left->route_generation == right->route_generation &&
           left->tree_generation == right->tree_generation &&
           left->owner_instance == right->owner_instance &&
           left->slot == right->slot &&
           left->generation == right->generation &&
           left->valid == right->valid &&
           left->reserved_zero == right->reserved_zero;
}

static bool handle_equal(ucn_handle_t left, ucn_handle_t right)
{
    return left.runtime_instance == right.runtime_instance &&
           left.owner_instance == right.owner_instance &&
           left.slot == right.slot && left.generation == right.generation &&
           left.object_kind == right.object_kind &&
           left.reserved_zero == right.reserved_zero;
}

bool ucn_i_group_p_context_valid(
    const ucn_i_group_context_config_t *config, bool pending_allowed)
{
    uint16_t total_weight = 0U;
    uint8_t left;
    uint8_t right;
    uint8_t legal_scopes = (uint8_t)((1U << UCN_I_GROUP_SCOPE_COUNT) - 1U);

    if (config == NULL || config->realm_id == 0U ||
        (config->group_id == 0U &&
         !(pending_allowed && config->mode == UCN_I_GROUP_DYNAMIC &&
           config->group_generation == 1U)) ||
        config->group_generation == 0U || config->policy_generation == 0U ||
        config->member_generation == 0U || config->endpoint == 0U ||
        config->opcode == 0U ||
        (config->mode != UCN_I_GROUP_STATIC &&
         config->mode != UCN_I_GROUP_DYNAMIC) ||
        config->member_count == 0U ||
        config->member_count > UCN_I_GROUP_MEMBER_COUNT ||
        config->unicast_fanout_limit == 0U ||
        config->unicast_fanout_limit > UCN_I_GROUP_MEMBER_COUNT ||
        config->allowed_scope_mask == 0U ||
        (config->allowed_scope_mask & (uint8_t)~legal_scopes) != 0U ||
        (config->allowed_scope_mask &
         UCN_I_GROUP_SCOPE_BIT(UCN_I_GROUP_SCOPE_LOCAL_ONLY)) == 0U ||
        config->secure_required > 1U || config->public_static > 1U ||
        config->reserved_zero != 0U ||
        !security_ref_valid(&config->security) ||
        !tree_ref_valid(&config->tree) ||
        (config->secure_required != 0U && config->security.valid == 0U) ||
        (config->mode == UCN_I_GROUP_DYNAMIC &&
         (config->public_static != 0U || config->security.valid == 0U)) ||
        (config->mode == UCN_I_GROUP_STATIC &&
         config->security.valid == 0U && config->public_static == 0U)) {
        return false;
    }
    for (left = 0U; left < config->member_count; ++left) {
        if (!member_valid(&config->members[left])) return false;
        total_weight = (uint16_t)(total_weight + config->members[left].weight);
        for (right = (uint8_t)(left + 1U);
             right < config->member_count; ++right) {
            if (config->members[left].address ==
                    config->members[right].address ||
                config->members[left].sender_slot ==
                    config->members[right].sender_slot ||
                memcmp(config->members[left].principal,
                       config->members[right].principal,
                       UCN_I_GROUP_PRINCIPAL_BYTES) == 0) {
                return false;
            }
        }
    }
    for (left = config->member_count;
         left < UCN_I_GROUP_MEMBER_COUNT; ++left) {
        if (!object_zero(&config->members[left],
                         sizeof(config->members[left]))) return false;
    }
    return config->quorum_weight != 0U &&
           config->quorum_weight <= total_weight;
}

static bool config_equal(const ucn_i_group_context_config_t *left,
                         const ucn_i_group_context_config_t *right)
{
    uint8_t index;

    if (left->realm_id != right->realm_id ||
        left->group_id != right->group_id ||
        left->group_generation != right->group_generation ||
        left->policy_generation != right->policy_generation ||
        left->member_generation != right->member_generation ||
        left->endpoint != right->endpoint || left->opcode != right->opcode ||
        left->mode != right->mode ||
        left->member_count != right->member_count ||
        left->unicast_fanout_limit != right->unicast_fanout_limit ||
        left->quorum_weight != right->quorum_weight ||
        left->allowed_scope_mask != right->allowed_scope_mask ||
        left->secure_required != right->secure_required ||
        left->public_static != right->public_static ||
        !security_ref_equal(&left->security, &right->security) ||
        !tree_ref_equal(&left->tree, &right->tree)) {
        return false;
    }
    for (index = 0U; index < left->member_count; ++index) {
        if (!member_equal(&left->members[index], &right->members[index])) {
            return false;
        }
    }
    return true;
}

static bool authority_valid(const ucn_i_group_authority_facts_t *authority,
                            uint32_t realm_id, uint64_t now_us)
{
    return authority != NULL && authority->realm_id == realm_id &&
           authority->authority_generation != 0U &&
           authority->owner_instance != 0U &&
           authority->authenticated == 1U && authority->quorum_met == 1U &&
           authority->current == 1U && authority->fenced == 0U &&
           authority->reserved_zero[0] == 0U &&
           authority->reserved_zero[1] == 0U &&
           authority->lease_deadline_us != 0U &&
           now_us < authority->lease_deadline_us &&
           bytes_nonzero(authority->proof_digest,
                         sizeof(authority->proof_digest));
}

static bool authority_equal(const ucn_i_group_authority_facts_t *left,
                            const ucn_i_group_authority_facts_t *right)
{
    return left->lease_deadline_us == right->lease_deadline_us &&
           left->realm_id == right->realm_id &&
           left->authority_generation == right->authority_generation &&
           left->owner_instance == right->owner_instance &&
           memcmp(left->proof_digest, right->proof_digest,
                  sizeof(left->proof_digest)) == 0;
}

bool ucn_i_group_p_owner_valid(const ucn_i_group_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_I_GROUP_MAGIC &&
           owner->schema == UCN_I_GROUP_SCHEMA &&
           owner->runtime_instance != 0U && owner->realm_id != 0U &&
           owner->owner_instance != 0U &&
           lock_valid(&owner->state_lock);
}

void ucn_i_group_p_make_handle(const ucn_i_group_owner_t *owner,
                               uint16_t slot, uint16_t generation,
                               uint8_t kind, ucn_handle_t *handle_out)
{
    memset(handle_out, 0, sizeof(*handle_out));
    handle_out->runtime_instance = owner->runtime_instance;
    handle_out->owner_instance = owner->owner_instance;
    handle_out->slot = slot;
    handle_out->generation = generation;
    handle_out->object_kind = kind;
}

bool ucn_i_group_p_handle_matches(const ucn_i_group_owner_t *owner,
                                  ucn_handle_t handle,
                                  uint16_t *slot_out)
{
    if (!ucn_i_group_p_owner_valid(owner) ||
        !ucn_i_handle_matches(&handle, owner->runtime_instance,
                              owner->owner_instance,
                              UCN_I_GROUP_CONTEXT_COUNT,
                              UCN_OBJECT_KIND_GROUP) ||
        !owner->contexts[handle.slot].occupied ||
        owner->contexts[handle.slot].handle_generation != handle.generation) {
        return false;
    }
    if (slot_out != NULL) *slot_out = handle.slot;
    return true;
}

static bool durability_valid(const ucn_i_group_owner_t *owner,
                             const ucn_i_group_durability_t *durability,
                             uint64_t now_us)
{
    return durability != NULL && durability->domain_id != 0U &&
           durability->foundation_transaction_id != 0U &&
           durability->expected_record_generation != UINT64_MAX &&
           durability->absolute_deadline_us != 0U &&
           now_us < durability->absolute_deadline_us &&
           durability->persistence_domain_generation != 0U &&
           durability->schema_id == UCN_I_GROUP_RECORD_SCHEMA_ID &&
           durability->schema_version == UCN_I_GROUP_RECORD_SCHEMA &&
           object_zero(&durability->volatile_continuation,
                       sizeof(durability->volatile_continuation)) &&
           owner != NULL;
}

ucn_result_t ucn_i_group_owner_init(ucn_i_group_owner_t *owner,
                                    const ucn_i_group_config_t *config)
{
    if (owner == NULL || config == NULL ||
        !object_zero(owner, sizeof(*owner)) ||
        config->runtime_instance == 0U || config->realm_id == 0U ||
        config->owner_instance == 0U ||
        config->reserved_zero != 0U || !lock_valid(&config->state_lock) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             config->state_lock.context, 1U)) {
        return UCN_ERR_ARGUMENT;
    }
    owner->magic = UCN_I_GROUP_MAGIC;
    owner->runtime_instance = config->runtime_instance;
    owner->realm_id = config->realm_id;
    owner->schema = UCN_I_GROUP_SCHEMA;
    owner->owner_instance = config->owner_instance;
    owner->state_lock = config->state_lock;
    return UCN_OK;
}

ucn_result_t ucn_i_group_owner_destroy(ucn_i_group_owner_t *owner)
{
    ucn_i_lock_ops_t lock;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner)) return UCN_ERR_STATE;
    lock = owner->state_lock;
    result = lock.enter(lock.context);
    if (result != UCN_OK) return result;
    memset(owner, 0, sizeof(*owner));
    lock.leave(lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_group_install_static(
    ucn_i_group_owner_t *owner, uint16_t static_slot,
    const ucn_i_group_context_config_t *config,
    bool manifest_authenticated, bool manifest_anti_rollback,
    ucn_handle_t *group_out)
{
    ucn_i_group_context_slot_t *slot;
    ucn_handle_t handle;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || config == NULL ||
        group_out == NULL || static_slot >= UCN_I_GROUP_STATIC_COUNT ||
        config->mode != UCN_I_GROUP_STATIC ||
        config->realm_id != owner->realm_id ||
        !ucn_i_group_p_context_valid(config, false) ||
        !manifest_authenticated || !manifest_anti_rollback ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), group_out,
                             sizeof(*group_out)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), group_out,
                             sizeof(*group_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    {
        uint16_t index;
        for (index = 0U; index < UCN_I_GROUP_CONTEXT_COUNT; ++index) {
            if (index != static_slot && owner->contexts[index].occupied &&
                owner->contexts[index].current.realm_id == config->realm_id &&
                owner->contexts[index].current.group_id == config->group_id) {
                result = UCN_ERR_STATE;
                goto install_done;
            }
        }
    }
    slot = &owner->contexts[static_slot];
    if (slot->occupied) {
        if (slot->phase == UCN_I_GROUP_ACTIVE &&
            config_equal(&slot->current, config)) {
            ucn_i_group_p_make_handle(owner, static_slot,
                                      slot->handle_generation,
                                      UCN_OBJECT_KIND_GROUP, &handle);
            *group_out = handle;
            result = UCN_OK;
        } else if (slot->phase == UCN_I_GROUP_FENCED &&
                   slot->current.realm_id == config->realm_id &&
                   slot->current.group_id == config->group_id &&
                   slot->current.group_generation != UINT32_MAX &&
                   config->group_generation ==
                       slot->current.group_generation + 1U &&
                   config->policy_generation >=
                       slot->current.policy_generation &&
                   config->member_generation >=
                       slot->current.member_generation) {
            if (slot->handle_generation == UINT16_MAX) {
                slot->phase = UCN_I_GROUP_FAULT;
                result = UCN_ERR_EXHAUSTED;
                goto install_done;
            }
            slot->current = *config;
            slot->handle_generation++;
            slot->phase = UCN_I_GROUP_ACTIVE;
            ucn_i_group_p_make_handle(owner, static_slot,
                                      slot->handle_generation,
                                      UCN_OBJECT_KIND_GROUP, &handle);
            *group_out = handle;
            result = UCN_OK;
        } else {
            result = UCN_ERR_STATE;
        }
    } else {
        slot->current = *config;
        slot->handle_generation = 1U;
        slot->phase = UCN_I_GROUP_ACTIVE;
        slot->occupied = 1U;
        ucn_i_group_p_make_handle(owner, static_slot, 1U,
                                  UCN_OBJECT_KIND_GROUP, &handle);
        *group_out = handle;
        result = UCN_OK;
    }
install_done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_open(ucn_i_group_owner_t *owner,
                              uint32_t group_id,
                              uint32_t group_generation,
                              uint32_t policy_generation,
                              ucn_handle_t *group_out)
{
    ucn_handle_t handle;
    uint16_t index;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || group_out == NULL ||
        group_id == 0U || group_generation == 0U || policy_generation == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), group_out,
                             sizeof(*group_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    result = UCN_ERR_NOT_FOUND;
    for (index = 0U; index < UCN_I_GROUP_CONTEXT_COUNT; ++index) {
        const ucn_i_group_context_slot_t *slot = &owner->contexts[index];
        if (slot->occupied && slot->phase == UCN_I_GROUP_ACTIVE &&
            slot->current.group_id == group_id &&
            slot->current.group_generation == group_generation &&
            slot->current.policy_generation == policy_generation) {
            ucn_i_group_p_make_handle(owner, index, slot->handle_generation,
                                      UCN_OBJECT_KIND_GROUP, &handle);
            *group_out = handle;
            result = UCN_OK;
            break;
        }
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_context_view(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    ucn_i_group_context_config_t *config_out, uint8_t *phase_out)
{
    uint16_t slot_index;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || config_out == NULL ||
        phase_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config_out,
                             sizeof(*config_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), phase_out,
                             sizeof(*phase_out)) ||
        ucn_i_ranges_overlap(config_out, sizeof(*config_out), phase_out,
                             sizeof(*phase_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_handle_matches(owner, group, &slot_index)) {
        result = UCN_ERR_STATE;
    } else {
        *config_out = owner->contexts[slot_index].current;
        *phase_out = owner->contexts[slot_index].phase;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_record_encode(
    const ucn_i_group_context_config_t *config, uint8_t record_phase,
    uint32_t dynamic_id_high_water,
    uint8_t body_out[UCN_I_GROUP_RECORD_BYTES])
{
    uint8_t index;

    if (!ucn_i_group_p_context_valid(config, false) || body_out == NULL ||
        config->mode != UCN_I_GROUP_DYNAMIC ||
        (record_phase != UCN_I_GROUP_ACTIVE &&
         record_phase != UCN_I_GROUP_RETIRED) ||
        dynamic_id_high_water == 0U ||
        config->group_id > dynamic_id_high_water ||
        ucn_i_ranges_overlap(config, sizeof(*config), body_out,
                             UCN_I_GROUP_RECORD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(body_out, 0, UCN_I_GROUP_RECORD_BYTES);
    write_be16(&body_out[0], UCN_I_GROUP_RECORD_SCHEMA);
    body_out[2] = record_phase;
    body_out[3] = config->mode;
    write_be32(&body_out[4], config->realm_id);
    write_be32(&body_out[8], config->group_id);
    write_be32(&body_out[12], config->group_generation);
    write_be32(&body_out[16], config->policy_generation);
    write_be32(&body_out[20], config->member_generation);
    write_be16(&body_out[24], config->endpoint);
    write_be16(&body_out[26], config->opcode);
    body_out[28] = config->member_count;
    body_out[29] = config->unicast_fanout_limit;
    body_out[30] = config->quorum_weight;
    body_out[31] = config->allowed_scope_mask;
    body_out[32] = config->secure_required;
    body_out[33] = config->public_static;
    write_be32(&body_out[36], config->security.runtime_instance);
    write_be32(&body_out[40], config->security.key_generation);
    write_be16(&body_out[44], config->security.owner_instance);
    write_be16(&body_out[46], config->security.slot);
    write_be16(&body_out[48], config->security.generation);
    body_out[50] = config->security.valid;
    body_out[51] = config->security.exact_principal;
    memcpy(&body_out[52], config->security.context_digest,
           UCN_I_GROUP_DIGEST_BYTES);
    write_be64(&body_out[68], config->tree.route_causal_id);
    write_be32(&body_out[76], config->tree.route_generation);
    write_be32(&body_out[80], config->tree.tree_generation);
    write_be16(&body_out[84], config->tree.owner_instance);
    write_be16(&body_out[86], config->tree.slot);
    write_be16(&body_out[88], config->tree.generation);
    body_out[90] = config->tree.valid;
    write_be32(&body_out[92], dynamic_id_high_water);
    for (index = 0U; index < config->member_count; ++index) {
        size_t offset = UCN_I_GROUP_RECORD_HEADER_BYTES +
                        (size_t)index * UCN_I_GROUP_RECORD_MEMBER_BYTES;
        write_be32(&body_out[offset], config->members[index].address);
        write_be32(&body_out[offset + 4U],
                   config->members[index].binding_generation);
        memcpy(&body_out[offset + 8U], config->members[index].principal,
               UCN_I_GROUP_PRINCIPAL_BYTES);
        write_be16(&body_out[offset + 24U],
                   config->members[index].sender_slot);
        body_out[offset + 26U] = config->members[index].weight;
    }
    return UCN_OK;
}

static ucn_result_t record_decode(const uint8_t *body, size_t body_bytes,
                                  ucn_i_group_context_config_t *config_out,
                                  uint8_t *phase_out,
                                  uint32_t *high_water_out)
{
    ucn_i_group_context_config_t *config;
    uint32_t high_water;
    uint8_t phase;
    uint8_t index;

    if (body == NULL || body_bytes != UCN_I_GROUP_RECORD_BYTES ||
        config_out == NULL || phase_out == NULL || high_water_out == NULL ||
        read_be16(&body[0]) != UCN_I_GROUP_RECORD_SCHEMA) {
        return UCN_ERR_MALFORMED;
    }
    for (index = 96U; index < UCN_I_GROUP_RECORD_HEADER_BYTES; ++index) {
        if (body[index] != 0U) return UCN_ERR_MALFORMED;
    }
    config = config_out;
    memset(config, 0, sizeof(*config));
    phase = body[2];
    config->mode = body[3];
    config->realm_id = read_be32(&body[4]);
    config->group_id = read_be32(&body[8]);
    config->group_generation = read_be32(&body[12]);
    config->policy_generation = read_be32(&body[16]);
    config->member_generation = read_be32(&body[20]);
    config->endpoint = read_be16(&body[24]);
    config->opcode = read_be16(&body[26]);
    config->member_count = body[28];
    config->unicast_fanout_limit = body[29];
    config->quorum_weight = body[30];
    config->allowed_scope_mask = body[31];
    config->secure_required = body[32];
    config->public_static = body[33];
    if (body[34] != 0U || body[35] != 0U || body[91] != 0U) {
        return UCN_ERR_MALFORMED;
    }
    config->security.runtime_instance = read_be32(&body[36]);
    config->security.key_generation = read_be32(&body[40]);
    config->security.owner_instance = read_be16(&body[44]);
    config->security.slot = read_be16(&body[46]);
    config->security.generation = read_be16(&body[48]);
    config->security.valid = body[50];
    config->security.exact_principal = body[51];
    memcpy(config->security.context_digest, &body[52],
           UCN_I_GROUP_DIGEST_BYTES);
    config->tree.route_causal_id = read_be64(&body[68]);
    config->tree.route_generation = read_be32(&body[76]);
    config->tree.tree_generation = read_be32(&body[80]);
    config->tree.owner_instance = read_be16(&body[84]);
    config->tree.slot = read_be16(&body[86]);
    config->tree.generation = read_be16(&body[88]);
    config->tree.valid = body[90];
    high_water = read_be32(&body[92]);
    if (config->member_count > UCN_I_GROUP_MEMBER_COUNT) {
        return UCN_ERR_MALFORMED;
    }
    for (index = 0U; index < UCN_I_GROUP_MEMBER_COUNT; ++index) {
        size_t offset = UCN_I_GROUP_RECORD_HEADER_BYTES +
                        (size_t)index * UCN_I_GROUP_RECORD_MEMBER_BYTES;
        if (index < config->member_count) {
            config->members[index].address = read_be32(&body[offset]);
            config->members[index].binding_generation =
                read_be32(&body[offset + 4U]);
            memcpy(config->members[index].principal, &body[offset + 8U],
                   UCN_I_GROUP_PRINCIPAL_BYTES);
            config->members[index].sender_slot =
                read_be16(&body[offset + 24U]);
            config->members[index].weight = body[offset + 26U];
            if (body[offset + 27U] != 0U) return UCN_ERR_MALFORMED;
        } else if (!object_zero(&body[offset],
                                UCN_I_GROUP_RECORD_MEMBER_BYTES)) {
            return UCN_ERR_MALFORMED;
        }
    }
    if ((phase != UCN_I_GROUP_ACTIVE && phase != UCN_I_GROUP_RETIRED) ||
        config->mode != UCN_I_GROUP_DYNAMIC || high_water == 0U ||
        config->group_id > high_water ||
        !ucn_i_group_p_context_valid(config, false)) {
        return UCN_ERR_MALFORMED;
    }
    *phase_out = phase;
    *high_water_out = high_water;
    return UCN_OK;
}

static int find_dynamic_slot(const ucn_i_group_owner_t *owner,
                             uint32_t group_id)
{
    uint16_t index;

    for (index = UCN_I_GROUP_STATIC_COUNT;
         index < UCN_I_GROUP_CONTEXT_COUNT; ++index) {
        if (owner->contexts[index].occupied &&
            owner->contexts[index].current.group_id == group_id) {
            return (int)index;
        }
    }
    return -1;
}

static int find_empty_dynamic_slot(const ucn_i_group_owner_t *owner)
{
    uint16_t index;

    for (index = UCN_I_GROUP_STATIC_COUNT;
         index < UCN_I_GROUP_CONTEXT_COUNT; ++index) {
        if (!owner->contexts[index].occupied) return (int)index;
    }
    return -1;
}

static bool group_id_in_use(const ucn_i_group_owner_t *owner,
                            uint32_t realm_id, uint32_t group_id)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_GROUP_CONTEXT_COUNT; ++index) {
        if (owner->contexts[index].occupied &&
            owner->contexts[index].current.realm_id == realm_id &&
            owner->contexts[index].current.group_id == group_id) {
            return true;
        }
    }
    return false;
}

static ucn_result_t prepare_requirement(
    ucn_i_group_owner_t *owner, uint16_t slot_index,
    uint8_t record_phase, ucn_i_group_requirement_t *requirement_out)
{
    ucn_i_group_context_slot_t *slot = &owner->contexts[slot_index];
    ucn_i_group_requirement_t *requirement = &owner->requirement_staging;
    ucn_result_t result;

    memset(requirement, 0, sizeof(*requirement));
    result = ucn_i_group_record_encode(&slot->pending, record_phase,
                                       slot->pending_high_water,
                                       requirement->body);
    if (result != UCN_OK) return result;
    result = ucn_i_sha256_128(requirement->body,
                              UCN_I_GROUP_RECORD_BYTES,
                              requirement->canonical_body_digest,
                              &owner->digest_workspace);
    if (result != UCN_OK) return result;
    requirement->durability = slot->pending_durability;
    ucn_i_group_p_make_handle(owner, slot_index, slot->handle_generation,
                              UCN_OBJECT_KIND_GROUP,
                              &requirement->durability.volatile_continuation);
    memcpy(requirement->expected_body_digest,
           slot->current_body_digest,
           UCN_I_GROUP_DIGEST_BYTES);
    requirement->runtime_instance = owner->runtime_instance;
    requirement->body_bytes = UCN_I_GROUP_RECORD_BYTES;
    requirement->caller_owner_instance = owner->owner_instance;
    requirement->operation_kind = UCN_I_GROUP_PERSIST_KIND;
    memcpy(slot->pending_body_digest,
           requirement->canonical_body_digest,
           UCN_I_GROUP_DIGEST_BYTES);
    *requirement_out = *requirement;
    return UCN_OK;
}

ucn_result_t ucn_i_group_admin_prepare(
    ucn_i_group_owner_t *owner,
    const ucn_i_group_context_config_t *next_config,
    const ucn_i_group_authority_facts_t *authority,
    const ucn_i_group_durability_t *durability, uint64_t now_us,
    ucn_handle_t *group_out,
    ucn_i_group_requirement_t *requirement_out)
{
    ucn_i_group_context_slot_t *slot;
    ucn_handle_t handle;
    uint32_t next_high_water;
    int found;
    uint16_t slot_index;
    bool create;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || next_config == NULL ||
        authority == NULL || durability == NULL || group_out == NULL ||
        requirement_out == NULL || next_config->mode != UCN_I_GROUP_DYNAMIC ||
        !ucn_i_group_p_context_valid(next_config, true) ||
        next_config->realm_id != owner->realm_id ||
        !authority_valid(authority, next_config->realm_id, now_us) ||
        !durability_valid(owner, durability, now_us) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), next_config,
                             sizeof(*next_config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), authority,
                             sizeof(*authority)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), group_out,
                             sizeof(*group_out)) ||
        ucn_i_ranges_overlap(next_config, sizeof(*next_config),
                             requirement_out, sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(next_config, sizeof(*next_config), group_out,
                             sizeof(*group_out)) ||
        ucn_i_ranges_overlap(authority, sizeof(*authority), group_out,
                             sizeof(*group_out)) ||
        ucn_i_ranges_overlap(authority, sizeof(*authority), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability), group_out,
                             sizeof(*group_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability),
                             requirement_out, sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(group_out, sizeof(*group_out), requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    create = next_config->group_id == 0U;
    found = create ? find_empty_dynamic_slot(owner) :
                     find_dynamic_slot(owner, next_config->group_id);
    if (found < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    slot_index = (uint16_t)found;
    slot = &owner->contexts[slot_index];
    if (slot->pending_valid) {
        result = UCN_ERR_STATE;
        goto done;
    }
    if (create) {
        uint16_t skipped = 0U;
        next_high_water = owner->dynamic_id_high_water;
        do {
            if (next_high_water == UINT32_MAX) {
                result = UCN_ERR_EXHAUSTED;
                goto done;
            }
            next_high_water++;
            skipped++;
        } while (group_id_in_use(owner, next_config->realm_id,
                                 next_high_water) &&
                 skipped <= UCN_I_GROUP_STATIC_COUNT);
        if (group_id_in_use(owner, next_config->realm_id,
                            next_high_water)) {
            result = UCN_ERR_EXHAUSTED;
            goto done;
        }
        if (next_high_water == 0U || next_config->group_generation != 1U) {
            result = UCN_ERR_STATE;
            goto done;
        }
        memset(slot, 0, sizeof(*slot));
        slot->pending = *next_config;
        slot->pending.group_id = next_high_water;
        slot->handle_generation = 1U;
        slot->phase = UCN_I_GROUP_PENDING;
        slot->occupied = 1U;
    } else {
        if (!slot->occupied || slot->phase == UCN_I_GROUP_RETIRED ||
            slot->phase == UCN_I_GROUP_FAULT ||
            slot->current.mode != UCN_I_GROUP_DYNAMIC ||
            slot->current.realm_id != next_config->realm_id ||
            slot->current.group_generation == UINT32_MAX ||
            next_config->group_generation !=
                slot->current.group_generation + 1U ||
            next_config->policy_generation <
                slot->current.policy_generation ||
            next_config->member_generation <
                slot->current.member_generation) {
            result = UCN_ERR_STATE;
            goto done;
        }
        next_high_water = owner->dynamic_id_high_water;
        slot->pending = *next_config;
    }
    slot->pending_authority = *authority;
    slot->pending_durability = *durability;
    slot->pending_high_water = next_high_water;
    slot->pending_valid = 1U;
    slot->pending_retire = 0U;
    slot->persistence_bound = 0U;
    memset(&slot->persistence_handle, 0, sizeof(slot->persistence_handle));
    result = prepare_requirement(owner, slot_index, UCN_I_GROUP_ACTIVE,
                                 requirement_out);
    if (result != UCN_OK) {
        if (create) memset(slot, 0, sizeof(*slot));
        else {
            memset(&slot->pending, 0, sizeof(slot->pending));
            slot->pending_valid = 0U;
        }
        goto done;
    }
    ucn_i_group_p_make_handle(owner, slot_index, slot->handle_generation,
                              UCN_OBJECT_KIND_GROUP, &handle);
    *group_out = handle;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_admin_retire_prepare(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    const ucn_i_group_authority_facts_t *authority,
    const ucn_i_group_durability_t *durability, uint64_t now_us,
    ucn_i_group_requirement_t *requirement_out)
{
    ucn_i_group_context_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || authority == NULL ||
        durability == NULL || requirement_out == NULL ||
        !durability_valid(owner, durability, now_us) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), authority,
                             sizeof(*authority)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(authority, sizeof(*authority), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability),
                             requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_handle_matches(owner, group, &slot_index)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    slot = &owner->contexts[slot_index];
    if (slot->phase != UCN_I_GROUP_ACTIVE ||
        slot->current.mode != UCN_I_GROUP_DYNAMIC || slot->pending_valid ||
        !authority_valid(authority, slot->current.realm_id, now_us)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    slot->pending = slot->current;
    slot->pending_authority = *authority;
    slot->pending_durability = *durability;
    slot->pending_high_water = owner->dynamic_id_high_water;
    slot->pending_valid = 1U;
    slot->pending_retire = 1U;
    slot->persistence_bound = 0U;
    result = prepare_requirement(owner, slot_index, UCN_I_GROUP_RETIRED,
                                 requirement_out);
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_bind_persistence(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_GROUP_DIGEST_BYTES])
{
    ucn_i_group_context_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) ||
        published_body_digest == NULL ||
        persistence_handle.object_kind != UCN_OBJECT_KIND_PERSISTENCE ||
        persistence_handle.runtime_instance != owner->runtime_instance ||
        persistence_handle.owner_instance == 0U ||
        persistence_handle.generation == 0U ||
        persistence_handle.reserved_zero != 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), published_body_digest,
                             UCN_I_GROUP_DIGEST_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_handle_matches(owner, group, &slot_index)) {
        result = UCN_ERR_STATE;
    } else {
        slot = &owner->contexts[slot_index];
        if (!slot->pending_valid || slot->persistence_bound ||
            !bytes_nonzero(published_body_digest,
                           UCN_I_GROUP_DIGEST_BYTES)) {
            result = UCN_ERR_STATE;
        } else {
            slot->persistence_handle = persistence_handle;
            memcpy(slot->pending_body_digest, published_body_digest,
                   UCN_I_GROUP_DIGEST_BYTES);
            slot->persistence_bound = 1U;
            result = UCN_OK;
        }
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_accept_proof(
    ucn_i_group_owner_t *owner, ucn_handle_t group,
    const ucn_i_group_proof_t *proof,
    const ucn_i_group_authority_facts_t *authority, uint64_t now_us)
{
    ucn_i_group_context_slot_t *slot;
    uint16_t slot_index;
    bool update;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || proof == NULL ||
        authority == NULL || ucn_i_ranges_overlap(owner, sizeof(*owner),
                                                  proof, sizeof(*proof)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), authority,
                             sizeof(*authority))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!ucn_i_group_p_handle_matches(owner, group, &slot_index)) {
        result = UCN_ERR_STATE;
        goto done;
    }
    slot = &owner->contexts[slot_index];
    if (!slot->pending_valid || !slot->persistence_bound ||
        !authority_valid(authority, slot->pending.realm_id, now_us) ||
        !authority_equal(authority, &slot->pending_authority) ||
        now_us >= slot->pending_durability.absolute_deadline_us ||
        !handle_equal(proof->persistence_handle, slot->persistence_handle) ||
        proof->domain_id != slot->pending_durability.domain_id ||
        proof->foundation_transaction_id !=
            slot->pending_durability.foundation_transaction_id ||
        proof->record_generation !=
            slot->pending_durability.expected_record_generation + 1U ||
        proof->witness_generation != proof->record_generation ||
        proof->runtime_instance != owner->runtime_instance ||
        proof->body_bytes != UCN_I_GROUP_RECORD_BYTES ||
        proof->persistence_domain_generation !=
            slot->pending_durability.persistence_domain_generation ||
        proof->persistence_owner_instance !=
            slot->persistence_handle.owner_instance ||
        proof->caller_owner_instance != owner->owner_instance ||
        proof->schema_id != UCN_I_GROUP_RECORD_SCHEMA_ID ||
        proof->schema_version != UCN_I_GROUP_RECORD_SCHEMA ||
        proof->operation_kind != UCN_I_GROUP_PERSIST_KIND ||
        proof->reserved_zero != 0U ||
        memcmp(proof->body_digest, slot->pending_body_digest,
               UCN_I_GROUP_DIGEST_BYTES) != 0) {
        result = UCN_ERR_STATE;
        goto done;
    }
    update = slot->phase == UCN_I_GROUP_ACTIVE ||
             slot->phase == UCN_I_GROUP_FENCED;
    if (update && slot->handle_generation == UINT16_MAX) {
        slot->phase = UCN_I_GROUP_FAULT;
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    owner->dynamic_id_high_water = slot->pending_high_water;
    if (slot->pending_retire) {
        /* The durable high-water is the tombstone.  Release the bounded
         * active runtime slot without ever making the retired ID reusable. */
        memset(slot, 0, sizeof(*slot));
        result = UCN_OK;
        goto done;
    }
    slot->current = slot->pending;
    slot->phase = UCN_I_GROUP_ACTIVE;
    if (update) slot->handle_generation++;
    memcpy(slot->current_body_digest, slot->pending_body_digest,
           UCN_I_GROUP_DIGEST_BYTES);
    memset(&slot->pending, 0, sizeof(slot->pending));
    memset(&slot->pending_authority, 0, sizeof(slot->pending_authority));
    memset(&slot->pending_durability, 0, sizeof(slot->pending_durability));
    memset(&slot->persistence_handle, 0, sizeof(slot->persistence_handle));
    memset(slot->pending_body_digest, 0,
           sizeof(slot->pending_body_digest));
    slot->pending_valid = 0U;
    slot->pending_retire = 0U;
    slot->persistence_bound = 0U;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_group_import(
    ucn_i_group_owner_t *owner, const uint8_t *body, size_t body_bytes,
    const ucn_i_group_durability_t *durability,
    const uint8_t published_body_digest[UCN_I_GROUP_DIGEST_BYTES],
    ucn_handle_t *group_out)
{
    ucn_i_group_context_config_t *decoded;
    uint8_t phase;
    uint32_t high_water;
    int found;
    ucn_handle_t handle;
    ucn_result_t result;

    if (!ucn_i_group_p_owner_valid(owner) || body == NULL ||
        durability == NULL || published_body_digest == NULL ||
        group_out == NULL ||
        durability->domain_id == 0U ||
        durability->foundation_transaction_id == 0U ||
        durability->persistence_domain_generation == 0U ||
        durability->schema_id != UCN_I_GROUP_RECORD_SCHEMA_ID ||
        durability->schema_version != UCN_I_GROUP_RECORD_SCHEMA ||
        !object_zero(&durability->volatile_continuation,
                     sizeof(durability->volatile_continuation)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), body, body_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), published_body_digest,
                             UCN_I_GROUP_DIGEST_BYTES) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), group_out,
                             sizeof(*group_out)) ||
        ucn_i_ranges_overlap(body, body_bytes, group_out,
                             sizeof(*group_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability), group_out,
                             sizeof(*group_out)) ||
        ucn_i_ranges_overlap(published_body_digest,
                             UCN_I_GROUP_DIGEST_BYTES, group_out,
                             sizeof(*group_out))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    decoded = &owner->config_staging;
    result = record_decode(body, body_bytes, decoded, &phase, &high_water);
    if (result != UCN_OK) goto done;
    if (decoded->realm_id != owner->realm_id ||
        !bytes_nonzero(published_body_digest,
                       UCN_I_GROUP_DIGEST_BYTES) ||
        high_water < owner->dynamic_id_high_water) {
        result = UCN_ERR_MALFORMED;
        goto done;
    }
    found = find_dynamic_slot(owner, decoded->group_id);
    if (phase == UCN_I_GROUP_RETIRED) {
        if (found >= 0) {
            if (owner->contexts[found].current.group_generation >
                    decoded->group_generation) {
                result = UCN_ERR_REPLAY;
                goto done;
            }
            memset(&owner->contexts[found], 0,
                   sizeof(owner->contexts[found]));
        }
        owner->dynamic_id_high_water = high_water;
        memset(group_out, 0, sizeof(*group_out));
        result = UCN_OK;
        goto done;
    }
    if (found < 0) found = find_empty_dynamic_slot(owner);
    if (found < 0) {
        result = UCN_ERR_NO_SPACE;
        goto done;
    }
    if (owner->contexts[found].occupied) {
        if (owner->contexts[found].current.group_generation >
                decoded->group_generation ||
            (owner->contexts[found].current.group_generation ==
                 decoded->group_generation &&
             (!config_equal(&owner->contexts[found].current, decoded) ||
              owner->contexts[found].phase != phase))) {
            result = UCN_ERR_REPLAY;
            goto done;
        }
    } else {
        memset(&owner->contexts[found], 0,
               sizeof(owner->contexts[found]));
        owner->contexts[found].handle_generation = 1U;
        owner->contexts[found].occupied = 1U;
    }
    owner->contexts[found].current = *decoded;
    owner->contexts[found].phase = phase;
    memcpy(owner->contexts[found].current_body_digest,
           published_body_digest, UCN_I_GROUP_DIGEST_BYTES);
    owner->dynamic_id_high_water = high_water;
    ucn_i_group_p_make_handle(owner, (uint16_t)found,
                              owner->contexts[found].handle_generation,
                              UCN_OBJECT_KIND_GROUP, &handle);
    *group_out = handle;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}
