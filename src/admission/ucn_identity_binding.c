#include "internal/ucn_identity.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_IDENTITY_MAGIC UINT32_C(0x55434944)

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
    return !bytes_nonzero(bytes, length);
}

static bool object_zero(const void *object, size_t bytes)
{
    return bytes_zero((const uint8_t *)object, bytes);
}

static bool lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

static bool owner_valid(const ucn_i_identity_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_I_IDENTITY_MAGIC &&
           owner->schema == UCN_I_IDENTITY_SCHEMA &&
           owner->runtime_instance != 0U && owner->realm_id != 0U &&
           owner->owner_instance != 0U &&
           owner->authority_owner_instance != 0U &&
           owner->persistence_business_owner_instance != 0U &&
           owner->persistence_owner_instance != 0U &&
           owner->domain_rules != NULL && owner->domain_rule_count != 0U &&
           lock_valid(&owner->state_lock);
}

static ucn_result_t owner_lock(ucn_i_identity_owner_t *owner)
{
    if (!owner_valid(owner)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

static void owner_unlock(ucn_i_identity_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static uint32_t address_limit(uint8_t width)
{
    return width == 4U ? UINT32_MAX :
           (UINT32_C(1) << ((uint32_t)width * 8U)) - UINT32_C(1);
}

static bool mode_valid(uint8_t mode)
{
    return mode == UCN_I_IDENTITY_ADDRESS_STATIC ||
           mode == UCN_I_IDENTITY_ADDRESS_LEASED ||
           mode == UCN_I_IDENTITY_ADDRESS_SELF_PROPOSED;
}

static bool issue_durable_fields_valid(
    const ucn_i_identity_binding_issue_t *issue)
{
    uint32_t limit;

    if (issue == NULL || issue->realm_id == 0U ||
        issue->address_width == 0U || issue->address_width > 4U ||
        !mode_valid(issue->mode)) {
        return false;
    }
    limit = address_limit(issue->address_width);
    return issue->address != 0U && issue->address < limit &&
           issue->binding_generation != 0U &&
           issue->authority_generation != 0U &&
           issue->lease_duration_us != 0U &&
           issue->authority_lease_sequence != 0U &&
           issue->authority_lease_sequence != UINT64_MAX &&
           bytes_nonzero(issue->principal, sizeof(issue->principal)) &&
           bytes_nonzero(issue->authority_principal,
                         sizeof(issue->authority_principal)) &&
           memcmp(issue->principal, issue->authority_principal,
                  sizeof(issue->principal)) != 0 &&
           bytes_nonzero(issue->lease_id, sizeof(issue->lease_id));
}

static bool issue_runtime_fields_valid(
    const ucn_i_identity_binding_issue_t *issue,
    const ucn_i_identity_owner_t *owner,
    uint64_t now_us,
    uint64_t *local_deadline_out)
{
    uint64_t local_deadline;

    if (!issue_durable_fields_valid(issue) || owner == NULL ||
        now_us == 0U || issue->runtime_instance != owner->runtime_instance ||
        issue->realm_id != owner->realm_id ||
        issue->address_width != owner->address_width ||
        issue->transaction_id == 0U ||
        issue->challenge_started_local_us == 0U ||
        issue->challenge_deadline_us == 0U ||
        now_us < issue->challenge_started_local_us ||
        now_us >= issue->challenge_deadline_us ||
        issue->link_generation == 0U || issue->admission_generation == 0U ||
        issue->admission_owner_instance == 0U ||
        !bytes_nonzero(issue->transcript_digest,
                       sizeof(issue->transcript_digest)) ||
        ucn_i_deadline_from_duration_us(issue->challenge_started_local_us,
                                        issue->lease_duration_us,
                                        &local_deadline) != UCN_OK ||
        local_deadline > issue->challenge_deadline_us ||
        now_us >= local_deadline) {
        return false;
    }
    *local_deadline_out = local_deadline;
    return true;
}

static bool authority_valid(
    const ucn_i_identity_owner_t *owner,
    const ucn_i_identity_authority_view_t *authority,
    uint64_t now_us)
{
    return authority != NULL && authority->runtime_instance ==
               owner->runtime_instance &&
           authority->realm_id == owner->realm_id &&
           authority->authority_owner_instance ==
               owner->authority_owner_instance &&
           authority->persistence_owner_instance ==
               owner->persistence_owner_instance &&
           authority->authority_generation != 0U &&
           authority->lease_sequence != 0U &&
           authority->lease_sequence != UINT64_MAX &&
           authority->local_deadline_us != 0U && now_us != 0U &&
           now_us < authority->local_deadline_us &&
           authority->record_generation != 0U &&
           authority->foundation_transaction_id != 0U &&
           authority->witness_generation == authority->record_generation &&
           authority->schema_id == UCN_I_IDENTITY_AUTHORITY_SCHEMA_ID &&
           authority->schema_version == 1U &&
           bytes_nonzero(authority->principal,
                         sizeof(authority->principal)) &&
           bytes_nonzero(authority->body_digest,
                         sizeof(authority->body_digest));
}

static bool authority_matches_issue(
    const ucn_i_identity_authority_view_t *authority,
    const ucn_i_identity_binding_issue_t *issue)
{
    return authority->authority_generation == issue->authority_generation &&
           authority->lease_sequence == issue->authority_lease_sequence &&
           memcmp(authority->principal, issue->authority_principal,
                  sizeof(authority->principal)) == 0;
}

static void put16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (uint8_t)(value >> 8U);
    bytes[offset + 1U] = (uint8_t)value;
}

static void put32(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (uint8_t)(value >> 24U);
    bytes[offset + 1U] = (uint8_t)(value >> 16U);
    bytes[offset + 2U] = (uint8_t)(value >> 8U);
    bytes[offset + 3U] = (uint8_t)value;
}

static void put64(uint8_t *bytes, size_t offset, uint64_t value)
{
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[offset + index] =
            (uint8_t)(value >> ((uint32_t)(7U - index) * 8U));
    }
}

static uint16_t get16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t)(((uint16_t)bytes[offset] << 8U) |
                      bytes[offset + 1U]);
}

static uint32_t get32(const uint8_t *bytes, size_t offset)
{
    return ((uint32_t)bytes[offset] << 24U) |
           ((uint32_t)bytes[offset + 1U] << 16U) |
           ((uint32_t)bytes[offset + 2U] << 8U) |
           bytes[offset + 3U];
}

static uint64_t get64(const uint8_t *bytes, size_t offset)
{
    uint64_t value = 0U;
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

ucn_result_t ucn_i_identity_binding_record_encode(
    const ucn_i_identity_binding_issue_t *issue,
    uint8_t output[UCN_I_IDENTITY_BINDING_RECORD_BYTES])
{
    uint8_t bytes[UCN_I_IDENTITY_BINDING_RECORD_BYTES];

    if (!issue_durable_fields_valid(issue) || output == NULL ||
        ucn_i_ranges_overlap(issue, sizeof(*issue), output,
                             UCN_I_IDENTITY_BINDING_RECORD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, "UC6B", 4U);
    put16(bytes, 4U, UCN_I_IDENTITY_BINDING_RECORD_SCHEMA);
    bytes[6] = 1U;
    bytes[7] = issue->mode;
    put32(bytes, 8U, issue->realm_id);
    bytes[12] = issue->address_width;
    put32(bytes, 16U, issue->address);
    put32(bytes, 20U, issue->binding_generation);
    memcpy(&bytes[24], issue->principal, 16U);
    memcpy(&bytes[40], issue->authority_principal, 16U);
    put32(bytes, 56U, issue->authority_generation);
    memcpy(&bytes[60], issue->lease_id, 16U);
    put64(bytes, 76U, issue->lease_duration_us);
    put64(bytes, 84U, issue->authority_lease_sequence);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_identity_binding_record_decode(
    const uint8_t input[UCN_I_IDENTITY_BINDING_RECORD_BYTES],
    ucn_i_identity_binding_issue_t *issue_out)
{
    ucn_i_identity_binding_issue_t issue;

    if (input == NULL || issue_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_IDENTITY_BINDING_RECORD_BYTES,
                             issue_out, sizeof(*issue_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (memcmp(input, "UC6B", 4U) != 0 ||
        get16(input, 4U) != UCN_I_IDENTITY_BINDING_RECORD_SCHEMA ||
        input[6] != 1U || !bytes_zero(&input[13], 3U) ||
        !bytes_zero(&input[92], 4U)) {
        return UCN_ERR_MALFORMED;
    }
    memset(&issue, 0, sizeof(issue));
    issue.mode = input[7];
    issue.realm_id = get32(input, 8U);
    issue.address_width = input[12];
    issue.address = get32(input, 16U);
    issue.binding_generation = get32(input, 20U);
    memcpy(issue.principal, &input[24], 16U);
    memcpy(issue.authority_principal, &input[40], 16U);
    issue.authority_generation = get32(input, 56U);
    memcpy(issue.lease_id, &input[60], 16U);
    issue.lease_duration_us = get64(input, 76U);
    issue.authority_lease_sequence = get64(input, 84U);
    if (!issue_durable_fields_valid(&issue)) {
        return UCN_ERR_MALFORMED;
    }
    *issue_out = issue;
    return UCN_OK;
}

static uint64_t transition_fingerprint(const uint8_t *bytes, size_t length)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    size_t index;

    for (index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(0x00000100000001B3);
    }
    return hash;
}

static bool persistence_handle_valid(ucn_handle_t handle)
{
    return handle.runtime_instance != 0U && handle.owner_instance != 0U &&
           handle.generation != 0U &&
           handle.object_kind == UCN_OBJECT_KIND_PERSISTENCE &&
           handle.reserved_zero == 0U;
}

static bool continuation_valid(const ucn_i_identity_owner_t *owner,
                               ucn_handle_t handle)
{
    return handle.runtime_instance == owner->runtime_instance &&
           handle.owner_instance == owner->owner_instance &&
           handle.generation != 0U &&
           handle.object_kind == UCN_OBJECT_KIND_SEND &&
           handle.reserved_zero == 0U;
}

static uint16_t find_domain(
    const ucn_i_identity_owner_t *owner,
    uint64_t domain_id)
{
    uint16_t index;

    for (index = 0U; index < owner->domain_rule_count; ++index) {
        if (owner->domain_rules[index].domain_id == domain_id) {
            return index;
        }
    }
    return UINT16_MAX;
}

static bool handle_matches(
    const ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    uint16_t *slot_out)
{
    uint16_t slot = handle.slot;

    if (slot >= owner->domain_rule_count ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        owner->bindings[slot].occupied == 0U ||
        owner->bindings[slot].slot_generation != handle.slot_generation ||
        owner->bindings[slot].issue.binding_generation !=
            handle.binding_generation) {
        return false;
    }
    *slot_out = slot;
    return true;
}

static bool durability_valid(
    const ucn_i_identity_durability_base_t *durability,
    const ucn_i_identity_binding_issue_t *issue,
    const ucn_i_identity_owner_t *owner,
    uint64_t now_us)
{
    if (durability == NULL || durability->domain_id == 0U ||
        durability->next_transaction_id == 0U ||
        durability->next_transaction_id == UINT64_MAX ||
        durability->absolute_deadline_us == 0U ||
        now_us >= durability->absolute_deadline_us ||
        durability->domain_generation == 0U ||
        !continuation_valid(owner, durability->volatile_continuation) ||
        durability->reserved_zero != 0U) {
        return false;
    }
    if (durability->prior_binding_generation == 0U) {
        return durability->prior_foundation_transaction_id == 0U &&
               durability->next_transaction_id == 1U &&
               durability->expected_record_generation == 0U &&
               durability->prior_address == 0U &&
               durability->prior_authority_lease_sequence == 0U &&
               durability->prior_address_width == 0U &&
               bytes_zero(durability->prior_principal,
                          sizeof(durability->prior_principal)) &&
               bytes_zero(durability->prior_lease_id,
                          sizeof(durability->prior_lease_id)) &&
               bytes_zero(durability->expected_body_digest,
                          sizeof(durability->expected_body_digest)) &&
               issue->binding_generation == 1U;
    }
    return durability->prior_foundation_transaction_id != 0U &&
           durability->prior_foundation_transaction_id != UINT64_MAX &&
           durability->next_transaction_id ==
               durability->prior_foundation_transaction_id + 1U &&
           durability->expected_record_generation != 0U &&
           durability->prior_binding_generation != UINT32_MAX &&
           issue->binding_generation ==
               durability->prior_binding_generation + 1U &&
           durability->prior_address == issue->address &&
           durability->prior_address_width == issue->address_width &&
           durability->prior_authority_lease_sequence != 0U &&
           issue->authority_lease_sequence >=
               durability->prior_authority_lease_sequence &&
           bytes_nonzero(durability->prior_principal,
                         sizeof(durability->prior_principal)) &&
           bytes_nonzero(durability->prior_lease_id,
                         sizeof(durability->prior_lease_id)) &&
           memcmp(durability->prior_lease_id, issue->lease_id,
                  sizeof(durability->prior_lease_id)) != 0 &&
           bytes_nonzero(durability->expected_body_digest,
                         sizeof(durability->expected_body_digest));
}

static bool config_valid(const ucn_i_identity_config_t *config)
{
    uint16_t left;
    uint16_t right;

    if (config == NULL || config->struct_size != sizeof(*config) ||
        config->api_version != UCN_API_VERSION ||
        config->runtime_instance == 0U || config->realm_id == 0U ||
        config->realm_id == UINT32_MAX || config->owner_instance == 0U ||
        config->authority_owner_instance == 0U ||
        config->persistence_business_owner_instance == 0U ||
        config->persistence_owner_instance == 0U ||
        config->address_width == 0U || config->address_width > 4U ||
        !bytes_zero(config->reserved_zero, sizeof(config->reserved_zero)) ||
        config->reserved_zero2 != 0U || config->domain_rules == NULL ||
        config->domain_rule_count == 0U ||
        config->domain_rule_count > UCN_BINDING_COUNT ||
        !lock_valid(&config->state_lock)) {
        return false;
    }
    for (left = 0U; left < config->domain_rule_count; ++left) {
        if (config->domain_rules[left].domain_id == 0U) {
            return false;
        }
        for (right = (uint16_t)(left + 1U);
             right < config->domain_rule_count; ++right) {
            if (config->domain_rules[left].domain_id ==
                config->domain_rules[right].domain_id) {
                return false;
            }
        }
    }
    return true;
}

ucn_result_t ucn_i_identity_owner_init(
    ucn_i_identity_owner_t *owner,
    const ucn_i_identity_config_t *config)
{
    ucn_result_t result;

    if (owner == NULL || !config_valid(config) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config->domain_rules,
                             sizeof(config->domain_rules[0]) *
                                 config->domain_rule_count) ||
        (config->state_lock.context != NULL &&
         ucn_i_ranges_overlap(owner, sizeof(*owner),
                              config->state_lock.context, 1U))) {
        return UCN_ERR_CONFIG;
    }
    result = config->state_lock.enter(config->state_lock.context);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (!object_zero(owner, sizeof(*owner))) {
        config->state_lock.leave(config->state_lock.context);
        return UCN_ERR_STATE;
    }
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_I_IDENTITY_MAGIC;
    owner->schema = UCN_I_IDENTITY_SCHEMA;
    owner->runtime_instance = config->runtime_instance;
    owner->realm_id = config->realm_id;
    owner->owner_instance = config->owner_instance;
    owner->authority_owner_instance = config->authority_owner_instance;
    owner->persistence_business_owner_instance =
        config->persistence_business_owner_instance;
    owner->persistence_owner_instance = config->persistence_owner_instance;
    owner->address_width = config->address_width;
    owner->domain_rules = config->domain_rules;
    owner->domain_rule_count = config->domain_rule_count;
    owner->state_lock = config->state_lock;
    config->state_lock.leave(config->state_lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_identity_prepare_binding(
    ucn_i_identity_owner_t *owner,
    const ucn_i_identity_binding_issue_t *issue,
    const ucn_i_identity_authority_view_t *authority,
    const ucn_i_identity_durability_base_t *durability,
    uint64_t now_us,
    ucn_i_identity_handle_t *handle_out)
{
    ucn_i_identity_slot_t *slot;
    ucn_i_identity_handle_t handle;
    uint64_t local_deadline;
    uint16_t slot_index;
    uint16_t index;
    uint32_t slot_generation;
    uint8_t encoded_body[UCN_I_IDENTITY_BINDING_RECORD_BYTES];
    ucn_result_t result;

    if (owner == NULL || issue == NULL || authority == NULL ||
        durability == NULL || handle_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), issue, sizeof(*issue)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), authority,
                             sizeof(*authority)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(issue, sizeof(*issue), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(authority, sizeof(*authority), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!issue_runtime_fields_valid(issue, owner, now_us,
                                    &local_deadline) ||
        !authority_valid(owner, authority, now_us) ||
        !authority_matches_issue(authority, issue) ||
        local_deadline > authority->local_deadline_us ||
        !durability_valid(durability, issue, owner, now_us)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot_index = find_domain(owner, durability->domain_id);
    if (slot_index == UINT16_MAX) {
        owner_unlock(owner);
        return UCN_ERR_ACCESS;
    }
    slot = &owner->bindings[slot_index];
    if (slot->occupied != 0U &&
        slot->phase != UCN_I_IDENTITY_BINDING_ACTIVE) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < owner->domain_rule_count; ++index) {
        const ucn_i_identity_slot_t *existing = &owner->bindings[index];

        if (index != slot_index && existing->occupied != 0U &&
            existing->phase == UCN_I_IDENTITY_BINDING_ACTIVE &&
            (existing->active_view.address == issue->address ||
             memcmp(existing->active_view.principal, issue->principal,
                    sizeof(issue->principal)) == 0)) {
            owner_unlock(owner);
            return UCN_ERR_ACCESS;
        }
    }
    slot_generation = slot->generation_high_water + 1U;
    if (slot_generation == 0U) {
        owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    result = ucn_i_identity_binding_record_encode(issue, encoded_body);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return result;
    }
    if (slot->phase == UCN_I_IDENTITY_BINDING_ACTIVE) {
        slot->previous_view = slot->active_view;
        slot->has_previous = 1U;
    } else {
        memset(&slot->previous_view, 0, sizeof(slot->previous_view));
        slot->has_previous = 0U;
    }
    slot->issue = *issue;
    slot->durability = *durability;
    memset(&slot->active_view, 0, sizeof(slot->active_view));
    memset(&slot->persistence_handle, 0, sizeof(slot->persistence_handle));
    memset(slot->expected_published_digest, 0,
           sizeof(slot->expected_published_digest));
    memcpy(slot->body, encoded_body, sizeof(slot->body));
    slot->transition_fingerprint =
        transition_fingerprint(slot->body, sizeof(slot->body));
    slot->active_view.local_deadline_us = local_deadline;
    slot->slot_generation = slot_generation;
    slot->generation_high_water = slot_generation;
    slot->occupied = 1U;
    slot->phase = UCN_I_IDENTITY_BINDING_AWAITING_DURABILITY;
    slot->persistence_bound = 0U;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = slot_index;
    handle.slot_generation = slot_generation;
    handle.binding_generation = issue->binding_generation;
    *handle_out = handle;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_identity_requirement_get(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    ucn_i_identity_requirement_view_t *requirement_out)
{
    ucn_i_identity_requirement_view_t view;
    ucn_i_identity_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->bindings[slot_index];
    if (slot->phase != UCN_I_IDENTITY_BINDING_AWAITING_DURABILITY) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&view, 0, sizeof(view));
    view.canonical_body = slot->body;
    view.domain_id = slot->durability.domain_id;
    view.foundation_transaction_id = slot->durability.next_transaction_id;
    view.expected_record_generation =
        slot->durability.expected_record_generation;
    view.absolute_deadline_us = slot->durability.absolute_deadline_us;
    view.transition_fingerprint = slot->transition_fingerprint;
    view.runtime_instance = owner->runtime_instance;
    view.body_bytes = UCN_I_IDENTITY_BINDING_RECORD_BYTES;
    view.volatile_continuation = slot->durability.volatile_continuation;
    view.caller_owner_instance =
        owner->persistence_business_owner_instance;
    view.domain_generation = slot->durability.domain_generation;
    view.schema_id = UCN_I_IDENTITY_BINDING_SCHEMA_ID;
    view.schema_version = UCN_I_IDENTITY_BINDING_RECORD_SCHEMA;
    view.operation_kind = UCN_I_IDENTITY_BINDING_OPERATION_KIND;
    memcpy(view.expected_body_digest,
           slot->durability.expected_body_digest,
           sizeof(view.expected_body_digest));
    *requirement_out = view;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_identity_bind_persistence(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    ucn_handle_t persistence_handle,
    const uint8_t expected_published_digest[16])
{
    ucn_i_identity_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || !persistence_handle_valid(persistence_handle) ||
        expected_published_digest == NULL ||
        !bytes_nonzero(expected_published_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             expected_published_digest, 16U)) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->bindings[slot_index];
    if (slot->phase != UCN_I_IDENTITY_BINDING_AWAITING_DURABILITY) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (slot->persistence_bound != 0U) {
        bool exact = memcmp(&slot->persistence_handle, &persistence_handle,
                            sizeof(persistence_handle)) == 0 &&
                     memcmp(slot->expected_published_digest,
                            expected_published_digest, 16U) == 0;

        owner_unlock(owner);
        return exact ? UCN_OK : UCN_ERR_STATE;
    }
    slot->persistence_handle = persistence_handle;
    memcpy(slot->expected_published_digest, expected_published_digest, 16U);
    slot->persistence_bound = 1U;
    owner_unlock(owner);
    return UCN_OK;
}

static bool durability_proof_matches(
    const ucn_i_identity_owner_t *owner,
    const ucn_i_identity_slot_t *slot,
    const ucn_i_identity_durability_proof_t *proof)
{
    uint64_t expected_generation;

    if (proof == NULL || slot->persistence_bound == 0U ||
        slot->durability.expected_record_generation == UINT64_MAX) {
        return false;
    }
    expected_generation = slot->durability.expected_record_generation + 1U;
    return memcmp(&proof->persistence_handle, &slot->persistence_handle,
                  sizeof(proof->persistence_handle)) == 0 &&
           proof->domain_id == slot->durability.domain_id &&
           proof->record_generation == expected_generation &&
           proof->foundation_transaction_id ==
               slot->durability.next_transaction_id &&
           proof->witness_generation == proof->record_generation &&
           proof->transition_fingerprint == slot->transition_fingerprint &&
           proof->runtime_instance == owner->runtime_instance &&
           proof->body_bytes == UCN_I_IDENTITY_BINDING_RECORD_BYTES &&
           memcmp(&proof->volatile_continuation,
                  &slot->durability.volatile_continuation,
                  sizeof(proof->volatile_continuation)) == 0 &&
           proof->persistence_owner_instance ==
               owner->persistence_owner_instance &&
           proof->persistence_owner_instance ==
               slot->persistence_handle.owner_instance &&
           proof->caller_owner_instance ==
               owner->persistence_business_owner_instance &&
           proof->domain_generation == slot->durability.domain_generation &&
           proof->schema_id == UCN_I_IDENTITY_BINDING_SCHEMA_ID &&
           proof->schema_version == UCN_I_IDENTITY_BINDING_RECORD_SCHEMA &&
           proof->operation_kind == UCN_I_IDENTITY_BINDING_OPERATION_KIND &&
           proof->reserved_zero == 0U &&
           memcmp(proof->body_digest, slot->expected_published_digest,
                  sizeof(proof->body_digest)) == 0;
}

static void binding_view_build(
    const ucn_i_identity_owner_t *owner,
    const ucn_i_identity_slot_t *slot,
    const ucn_i_identity_durability_proof_t *proof,
    ucn_i_identity_binding_view_t *view)
{
    memset(view, 0, sizeof(*view));
    view->runtime_instance = owner->runtime_instance;
    view->realm_id = owner->realm_id;
    view->address = slot->issue.address;
    view->binding_generation = slot->issue.binding_generation;
    view->authority_generation = slot->issue.authority_generation;
    view->link_generation = slot->issue.link_generation;
    view->local_deadline_us = slot->active_view.local_deadline_us;
    view->record_generation = proof->record_generation;
    view->foundation_transaction_id = proof->foundation_transaction_id;
    view->witness_generation = proof->witness_generation;
    view->slot_generation = slot->slot_generation;
    view->identity_owner_instance = owner->owner_instance;
    view->persistence_owner_instance = owner->persistence_owner_instance;
    view->schema_id = proof->schema_id;
    view->schema_version = proof->schema_version;
    memcpy(view->principal, slot->issue.principal,
           sizeof(view->principal));
    memcpy(view->body_digest, proof->body_digest,
           sizeof(view->body_digest));
}

ucn_result_t ucn_i_identity_activate_binding(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    const ucn_i_identity_durability_proof_t *proof,
    const ucn_i_identity_authority_view_t *authority,
    uint64_t now_us,
    ucn_i_identity_binding_view_t *view_out)
{
    ucn_i_identity_slot_t *slot;
    uint16_t slot_index;
    uint16_t index;
    ucn_result_t result;

    if (owner == NULL || proof == NULL || authority == NULL ||
        view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), proof, sizeof(*proof)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), authority,
                             sizeof(*authority)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out)) ||
        ucn_i_ranges_overlap(proof, sizeof(*proof), view_out,
                             sizeof(*view_out)) ||
        ucn_i_ranges_overlap(authority, sizeof(*authority), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->bindings[slot_index];
    if (slot->phase != UCN_I_IDENTITY_BINDING_AWAITING_DURABILITY ||
        now_us < slot->issue.challenge_started_local_us ||
        now_us >= slot->issue.challenge_deadline_us ||
        now_us >= slot->active_view.local_deadline_us ||
        now_us >= slot->durability.absolute_deadline_us ||
        !authority_valid(owner, authority, now_us) ||
        !authority_matches_issue(authority, &slot->issue) ||
        slot->active_view.local_deadline_us > authority->local_deadline_us ||
        !durability_proof_matches(owner, slot, proof)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < owner->domain_rule_count; ++index) {
        const ucn_i_identity_slot_t *existing = &owner->bindings[index];

        if (index != slot_index && existing->occupied != 0U &&
            existing->phase == UCN_I_IDENTITY_BINDING_ACTIVE &&
            (existing->active_view.address == slot->issue.address ||
             memcmp(existing->active_view.principal,
                    slot->issue.principal,
                    sizeof(slot->issue.principal)) == 0)) {
            owner_unlock(owner);
            return UCN_ERR_REPLAY;
        }
    }
    binding_view_build(owner, slot, proof, view_out);
    slot->active_view = *view_out;
    memset(&slot->previous_view, 0, sizeof(slot->previous_view));
    memset(&slot->persistence_handle, 0, sizeof(slot->persistence_handle));
    memset(slot->expected_published_digest, 0,
           sizeof(slot->expected_published_digest));
    slot->has_previous = 0U;
    slot->persistence_bound = 0U;
    slot->phase = UCN_I_IDENTITY_BINDING_ACTIVE;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_identity_abort_unsubmitted(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle)
{
    ucn_i_identity_slot_t *slot;
    uint16_t slot_index;
    uint32_t high_water;
    ucn_result_t result;

    if (owner == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->bindings[slot_index];
    if (slot->phase != UCN_I_IDENTITY_BINDING_AWAITING_DURABILITY ||
        slot->persistence_bound != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    high_water = slot->generation_high_water;
    if (slot->has_previous != 0U) {
        ucn_i_identity_binding_view_t previous = slot->previous_view;

        memset(slot, 0, sizeof(*slot));
        slot->occupied = 1U;
        slot->phase = UCN_I_IDENTITY_BINDING_ACTIVE;
        slot->slot_generation = previous.slot_generation;
        slot->generation_high_water = high_water;
        slot->active_view = previous;
        slot->issue.runtime_instance = previous.runtime_instance;
        slot->issue.realm_id = previous.realm_id;
        slot->issue.address = previous.address;
        slot->issue.binding_generation = previous.binding_generation;
        slot->issue.authority_generation = previous.authority_generation;
        slot->issue.link_generation = previous.link_generation;
        memcpy(slot->issue.principal, previous.principal,
               sizeof(slot->issue.principal));
    } else {
        memset(slot, 0, sizeof(*slot));
        slot->generation_high_water = high_water;
    }
    owner_unlock(owner);
    return UCN_OK;
}

static void restore_previous_or_clear(ucn_i_identity_slot_t *slot)
{
    uint32_t high_water = slot->generation_high_water;

    if (slot->has_previous != 0U) {
        ucn_i_identity_binding_view_t previous = slot->previous_view;

        memset(slot, 0, sizeof(*slot));
        slot->occupied = 1U;
        slot->phase = UCN_I_IDENTITY_BINDING_ACTIVE;
        slot->slot_generation = previous.slot_generation;
        slot->generation_high_water = high_water;
        slot->active_view = previous;
        slot->issue.runtime_instance = previous.runtime_instance;
        slot->issue.realm_id = previous.realm_id;
        slot->issue.address = previous.address;
        slot->issue.binding_generation = previous.binding_generation;
        slot->issue.authority_generation = previous.authority_generation;
        slot->issue.link_generation = previous.link_generation;
        memcpy(slot->issue.principal, previous.principal,
               sizeof(slot->issue.principal));
    } else {
        memset(slot, 0, sizeof(*slot));
        slot->generation_high_water = high_water;
    }
}

ucn_result_t ucn_i_identity_fail_persistence(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    const ucn_i_identity_persistence_failure_t *failure)
{
    ucn_i_identity_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || failure == NULL ||
        !persistence_handle_valid(failure->persistence_handle) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), failure,
                             sizeof(*failure))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->bindings[slot_index];
    if ((slot->phase != UCN_I_IDENTITY_BINDING_AWAITING_DURABILITY &&
        slot->phase != UCN_I_IDENTITY_BINDING_FENCED) ||
        slot->persistence_bound == 0U ||
        memcmp(&slot->persistence_handle, &failure->persistence_handle,
               sizeof(failure->persistence_handle)) != 0 ||
        memcmp(&slot->durability.volatile_continuation,
               &failure->volatile_continuation,
               sizeof(failure->volatile_continuation)) != 0 ||
        failure->domain_id != slot->durability.domain_id ||
        failure->foundation_transaction_id !=
            slot->durability.next_transaction_id ||
        failure->transition_fingerprint != slot->transition_fingerprint ||
        failure->runtime_instance != owner->runtime_instance ||
        failure->terminal_result >= UCN_OK ||
        failure->persistence_owner_instance !=
            owner->persistence_owner_instance ||
        failure->persistence_owner_instance !=
            failure->persistence_handle.owner_instance ||
        failure->caller_owner_instance !=
            owner->persistence_business_owner_instance ||
        failure->domain_generation != slot->durability.domain_generation ||
        failure->schema_id != UCN_I_IDENTITY_BINDING_SCHEMA_ID ||
        failure->schema_version != UCN_I_IDENTITY_BINDING_RECORD_SCHEMA ||
        failure->operation_kind != UCN_I_IDENTITY_BINDING_OPERATION_KIND ||
        failure->request_state !=
            UCN_I_IDENTITY_PERSISTENCE_REQUEST_FAILED ||
        !bytes_zero(failure->reserved_zero,
                    sizeof(failure->reserved_zero))) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    restore_previous_or_clear(slot);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_identity_binding_get(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle,
    uint64_t now_us,
    ucn_i_identity_binding_view_t *view_out)
{
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || view_out == NULL || now_us == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(owner, handle, &slot_index) ||
        owner->bindings[slot_index].phase !=
            UCN_I_IDENTITY_BINDING_ACTIVE ||
        now_us >= owner->bindings[slot_index].active_view.local_deadline_us) {
        owner_unlock(owner);
        return UCN_ERR_ACCESS;
    }
    *view_out = owner->bindings[slot_index].active_view;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_identity_expire(
    ucn_i_identity_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *fenced_out)
{
    uint16_t inspected = 0U;
    uint16_t fenced = 0U;
    ucn_result_t result;

    if (owner == NULL || now_us == 0U || budget == 0U ||
        inspected_out == NULL || fenced_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), fenced_out,
                             sizeof(*fenced_out)) ||
        ucn_i_ranges_overlap(inspected_out, sizeof(*inspected_out),
                             fenced_out, sizeof(*fenced_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    while (inspected < budget && inspected < owner->domain_rule_count) {
        uint16_t index = owner->maintenance_cursor;
        ucn_i_identity_slot_t *slot = &owner->bindings[index];

        owner->maintenance_cursor =
            (uint16_t)((index + 1U) % owner->domain_rule_count);
        inspected++;
        if (slot->phase == UCN_I_IDENTITY_BINDING_ACTIVE &&
            now_us >= slot->active_view.local_deadline_us) {
            slot->phase = UCN_I_IDENTITY_BINDING_FENCED;
            fenced++;
        } else if (slot->phase ==
                       UCN_I_IDENTITY_BINDING_AWAITING_DURABILITY &&
                   (now_us >= slot->issue.challenge_deadline_us ||
                    now_us >= slot->active_view.local_deadline_us ||
                    now_us >= slot->durability.absolute_deadline_us)) {
            slot->phase = UCN_I_IDENTITY_BINDING_FENCED;
            fenced++;
        }
    }
    *inspected_out = inspected;
    *fenced_out = fenced;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_identity_retire_fenced(
    ucn_i_identity_owner_t *owner,
    ucn_i_identity_handle_t handle)
{
    ucn_i_identity_slot_t *slot;
    uint16_t slot_index;
    uint32_t high_water;
    ucn_result_t result;

    if (owner == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->bindings[slot_index];
    if (slot->phase != UCN_I_IDENTITY_BINDING_FENCED ||
        slot->persistence_bound != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    high_water = slot->generation_high_water;
    memset(slot, 0, sizeof(*slot));
    slot->generation_high_water = high_water;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_identity_owner_destroy(
    ucn_i_identity_owner_t *owner)
{
    ucn_i_lock_ops_t lock;
    uint16_t index;
    ucn_result_t result;

    if (owner == NULL || !owner_valid(owner)) {
        return UCN_ERR_STATE;
    }
    lock = owner->state_lock;
    result = lock.enter(lock.context);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < owner->domain_rule_count; ++index) {
        if (owner->bindings[index].occupied != 0U) {
            lock.leave(lock.context);
            return UCN_ERR_STATE;
        }
    }
    memset(owner, 0, sizeof(*owner));
    lock.leave(lock.context);
    return UCN_OK;
}
