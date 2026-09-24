#include "transport/ucn_transport_private.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_TRANSPORT_PARENT_MAGIC UINT32_C(0x55435054)

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

static uint16_t get16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t)(((uint16_t)bytes[offset] << 8U) |
                      bytes[offset + 1U]);
}

static uint32_t get32(const uint8_t *bytes, size_t offset)
{
    return ((uint32_t)bytes[offset] << 24U) |
           ((uint32_t)bytes[offset + 1U] << 16U) |
           ((uint32_t)bytes[offset + 2U] << 8U) | bytes[offset + 3U];
}

static void put64(uint8_t *bytes, size_t offset, uint64_t value)
{
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        bytes[offset + index] =
            (uint8_t)(value >> (uint8_t)((7U - index) * 8U));
    }
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

static bool binding_valid(const ucn_i_transport_binding_t *binding)
{
    return binding != NULL && binding->address != 0U &&
           binding->generation != 0U &&
           ucn_i_transport_p_bytes_nonzero(binding->principal,
                                           sizeof(binding->principal));
}

static bool context_valid(const ucn_i_transport_parent_context_t *context)
{
    return context != NULL && context->realm != 0U &&
           binding_valid(&context->source) &&
           binding_valid(&context->destination) &&
           context->transport_policy_generation != 0U &&
           context->parent_generation != 0U &&
           context->origin_security <= 2U &&
           context->reserved_zero[0] == 0U &&
           context->reserved_zero[1] == 0U &&
           context->reserved_zero[2] == 0U &&
           ((context->origin_security == 0U &&
             context->security_session_generation == 0U &&
             context->security_key_generation == 0U) ||
            (context->origin_security != 0U &&
             context->security_session_generation != 0U &&
             context->security_key_generation != 0U));
}

static bool context_equal(const ucn_i_transport_parent_context_t *left,
                          const ucn_i_transport_parent_context_t *right)
{
    return left->realm == right->realm &&
           left->source.address == right->source.address &&
           left->source.generation == right->source.generation &&
           memcmp(left->source.principal, right->source.principal, 16U) == 0 &&
           left->destination.address == right->destination.address &&
           left->destination.generation == right->destination.generation &&
           memcmp(left->destination.principal,
                  right->destination.principal, 16U) == 0 &&
           left->transport_policy_generation ==
               right->transport_policy_generation &&
           left->security_session_generation ==
               right->security_session_generation &&
           left->security_key_generation == right->security_key_generation &&
           left->parent_generation == right->parent_generation &&
           left->origin_security == right->origin_security;
}

static uint64_t fingerprint_from_digest(const uint8_t digest[16])
{
    uint64_t value = 0U;
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | digest[index];
    }
    return value;
}

static bool persistence_handle_valid(ucn_handle_t handle,
                                     uint32_t runtime_instance)
{
    return handle.runtime_instance == runtime_instance &&
           handle.owner_instance != 0U && handle.generation != 0U &&
           handle.object_kind == UCN_OBJECT_KIND_PERSISTENCE &&
           handle.reserved_zero == 0U;
}

static ucn_result_t next_transfer_id(uint32_t current, uint32_t *value_out)
{
    return current == 0U ? ucn_i_u32_allocate_first(true, value_out) :
                           ucn_i_u32_checked_next(current, value_out);
}

ucn_result_t ucn_i_transport_parent_record_encode(
    const ucn_i_transport_parent_context_t *context,
    uint32_t transfer_high_water,
    uint64_t foundation_transaction_id,
    uint8_t output[UCN_I_TRANSPORT_PARENT_RECORD_BYTES])
{
    uint8_t body[UCN_I_TRANSPORT_PARENT_RECORD_BYTES];

    if (!context_valid(context) || foundation_transaction_id == 0U ||
        output == NULL ||
        ucn_i_ranges_overlap(context, sizeof(*context), output,
                             sizeof(body))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(body, 0, sizeof(body));
    put32(body, 0U, UCN_I_TRANSPORT_PARENT_MAGIC);
    put16(body, 4U, UCN_I_TRANSPORT_SCHEMA);
    body[6] = context->origin_security;
    put32(body, 8U, context->realm);
    put32(body, 12U, context->source.address);
    put32(body, 16U, context->source.generation);
    memcpy(&body[20], context->source.principal, 16U);
    put32(body, 36U, context->destination.address);
    put32(body, 40U, context->destination.generation);
    memcpy(&body[44], context->destination.principal, 16U);
    put32(body, 60U, context->transport_policy_generation);
    put32(body, 64U, context->security_session_generation);
    put32(body, 68U, context->security_key_generation);
    put32(body, 72U, context->parent_generation);
    put32(body, 76U, transfer_high_water);
    put64(body, 80U, foundation_transaction_id);
    memcpy(output, body, sizeof(body));
    return UCN_OK;
}

ucn_result_t ucn_i_transport_parent_record_decode(
    const uint8_t input[UCN_I_TRANSPORT_PARENT_RECORD_BYTES],
    ucn_i_transport_parent_context_t *context_out,
    uint32_t *transfer_high_water_out,
    uint64_t *foundation_transaction_id_out)
{
    ucn_i_transport_parent_context_t context;
    uint32_t high_water;

    if (input == NULL || context_out == NULL ||
        transfer_high_water_out == NULL || foundation_transaction_id_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_TRANSPORT_PARENT_RECORD_BYTES,
                             context_out, sizeof(*context_out)) ||
        ucn_i_ranges_overlap(input, UCN_I_TRANSPORT_PARENT_RECORD_BYTES,
                             transfer_high_water_out,
                             sizeof(*transfer_high_water_out)) ||
        ucn_i_ranges_overlap(context_out, sizeof(*context_out),
                             transfer_high_water_out,
                             sizeof(*transfer_high_water_out)) ||
        ucn_i_ranges_overlap(input, UCN_I_TRANSPORT_PARENT_RECORD_BYTES,
                             foundation_transaction_id_out,
                             sizeof(*foundation_transaction_id_out)) ||
        ucn_i_ranges_overlap(context_out, sizeof(*context_out),
                             foundation_transaction_id_out,
                             sizeof(*foundation_transaction_id_out)) ||
        ucn_i_ranges_overlap(transfer_high_water_out,
                             sizeof(*transfer_high_water_out),
                             foundation_transaction_id_out,
                             sizeof(*foundation_transaction_id_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (get32(input, 0U) != UCN_I_TRANSPORT_PARENT_MAGIC ||
        get16(input, 4U) != UCN_I_TRANSPORT_SCHEMA || input[6] > 2U ||
        input[7] != 0U) {
        return UCN_ERR_MALFORMED;
    }
    memset(&context, 0, sizeof(context));
    context.realm = get32(input, 8U);
    context.origin_security = input[6];
    context.source.address = get32(input, 12U);
    context.source.generation = get32(input, 16U);
    memcpy(context.source.principal, &input[20], 16U);
    context.destination.address = get32(input, 36U);
    context.destination.generation = get32(input, 40U);
    memcpy(context.destination.principal, &input[44], 16U);
    context.transport_policy_generation = get32(input, 60U);
    context.security_session_generation = get32(input, 64U);
    context.security_key_generation = get32(input, 68U);
    context.parent_generation = get32(input, 72U);
    high_water = get32(input, 76U);
    if (!context_valid(&context) || get64(input, 80U) == 0U) {
        return UCN_ERR_MALFORMED;
    }
    *context_out = context;
    *transfer_high_water_out = high_water;
    *foundation_transaction_id_out = get64(input, 80U);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_parent_import(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_parent_context_t *context,
    uint32_t transfer_high_water,
    uint64_t domain_id,
    uint64_t record_generation,
    uint64_t foundation_transaction_id,
    uint16_t domain_generation,
    const uint8_t published_body_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    ucn_handle_t *handle_out)
{
    uint8_t body[UCN_I_TRANSPORT_PARENT_RECORD_BYTES];
    uint8_t digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_TRANSPORT_PARENT_COUNT;
    bool exhausted_slot = false;

    if (!context_valid(context) || domain_id == 0U ||
        record_generation == 0U || foundation_transaction_id == 0U ||
        domain_generation == 0U || published_body_digest == NULL ||
        !ucn_i_transport_p_bytes_nonzero(published_body_digest, 16U) ||
        handle_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), context,
                             sizeof(*context)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), published_body_digest,
                             UCN_I_TRANSPORT_DIGEST_BYTES) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(context, sizeof(*context), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(published_body_digest,
                             UCN_I_TRANSPORT_DIGEST_BYTES, handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_parent_record_encode(
        context, transfer_high_water, foundation_transaction_id, body);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_sha256_128(body, sizeof(body), digest,
                              &owner->hash_workspace);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    for (index = 0U; index < UCN_I_TRANSPORT_PARENT_COUNT; ++index) {
        ucn_i_transport_parent_record_t *record = &owner->parents[index];
        if (record->occupied == 0U) {
            if (record->generation == UINT16_MAX) {
                exhausted_slot = true;
            } else if (free_index == UCN_I_TRANSPORT_PARENT_COUNT) {
                free_index = index;
            }
        } else if (context_equal(&record->context, context)) {
            if (record->active != 0U && record->prepared == 0U &&
                record->transfer_high_water == transfer_high_water &&
                record->domain_id == domain_id &&
                record->record_generation == record_generation &&
                record->foundation_transaction_id ==
                    foundation_transaction_id &&
                record->domain_generation == domain_generation &&
                memcmp(record->current_published_digest,
                       published_body_digest, 16U) == 0) {
                *handle_out = ucn_i_transport_p_handle(
                    owner, (uint16_t)index, record->generation,
                    UCN_I_TRANSPORT_PARENT_KIND);
                ucn_i_transport_p_unlock(owner);
                return UCN_OK;
            }
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    if (free_index == UCN_I_TRANSPORT_PARENT_COUNT) {
        ucn_i_transport_p_unlock(owner);
        return exhausted_slot ? UCN_ERR_EXHAUSTED : UCN_ERR_NO_SPACE;
    }
    {
        ucn_i_transport_parent_record_t *record = &owner->parents[free_index];
        uint16_t generation = (uint16_t)(record->generation + 1U);
        memset(record, 0, sizeof(*record));
        record->context = *context;
        memcpy(record->body, body, sizeof(body));
        memcpy(record->canonical_body_digest, digest, sizeof(digest));
        memcpy(record->current_published_digest, published_body_digest, 16U);
        record->domain_id = domain_id;
        record->record_generation = record_generation;
        record->foundation_transaction_id = foundation_transaction_id;
        record->transfer_high_water = transfer_high_water;
        record->domain_generation = domain_generation;
        record->generation = generation;
        record->occupied = 1U;
        record->active = 1U;
        *handle_out = ucn_i_transport_p_handle(
            owner, (uint16_t)free_index, generation,
            UCN_I_TRANSPORT_PARENT_KIND);
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

static bool durability_valid(
    const ucn_i_transport_parent_record_t *record,
    const ucn_i_transport_parent_durability_base_t *durability,
    uint64_t now_us)
{
    uint64_t expected_record;
    uint64_t next_transaction;

    if (durability == NULL || durability->domain_id != record->domain_id ||
        durability->domain_generation != record->domain_generation ||
        durability->reserved_zero != 0U ||
        durability->absolute_deadline_us == 0U ||
        ucn_i_deadline_expired_us(now_us,
                                  durability->absolute_deadline_us) ||
        durability->prior_transfer_high_water !=
            record->transfer_high_water ||
        durability->prior_foundation_transaction_id !=
            record->foundation_transaction_id ||
        durability->expected_record_generation !=
            record->record_generation ||
        ucn_i_u64_checked_next(record->record_generation,
                               &expected_record) != UCN_OK ||
        ucn_i_u64_checked_next(record->foundation_transaction_id,
                               &next_transaction) != UCN_OK) {
        return false;
    }
    (void)expected_record;
    return durability->next_foundation_transaction_id == next_transaction &&
           memcmp(durability->expected_body_digest,
                  record->current_published_digest,
                  UCN_I_TRANSPORT_DIGEST_BYTES) == 0 &&
           durability->volatile_continuation.runtime_instance != 0U &&
           durability->volatile_continuation.owner_instance != 0U &&
           durability->volatile_continuation.generation != 0U &&
           durability->volatile_continuation.object_kind != 0U &&
           durability->volatile_continuation.reserved_zero == 0U;
}

static bool first_durability_valid(
    const ucn_i_transport_parent_durability_base_t *durability,
    uint64_t now_us)
{
    uint8_t zero_digest[UCN_I_TRANSPORT_DIGEST_BYTES];

    memset(zero_digest, 0, sizeof(zero_digest));
    return durability != NULL && durability->domain_id != 0U &&
           durability->domain_generation != 0U &&
           durability->reserved_zero == 0U &&
           durability->prior_foundation_transaction_id == 0U &&
           durability->next_foundation_transaction_id == 1U &&
           durability->expected_record_generation == 0U &&
           durability->prior_transfer_high_water == 0U &&
           durability->absolute_deadline_us != 0U &&
           !ucn_i_deadline_expired_us(now_us,
                                      durability->absolute_deadline_us) &&
           memcmp(durability->expected_body_digest, zero_digest,
                  sizeof(zero_digest)) == 0 &&
           durability->volatile_continuation.runtime_instance != 0U &&
           durability->volatile_continuation.owner_instance != 0U &&
           durability->volatile_continuation.generation != 0U &&
           durability->volatile_continuation.object_kind != 0U &&
           durability->volatile_continuation.reserved_zero == 0U;
}

ucn_result_t ucn_i_transport_parent_prepare_first(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_parent_context_t *context,
    const ucn_i_transport_parent_durability_base_t *durability,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    uint8_t body[UCN_I_TRANSPORT_PARENT_RECORD_BYTES];
    uint8_t digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_TRANSPORT_PARENT_COUNT;
    bool exhausted_slot = false;

    if (!context_valid(context) || durability == NULL || handle_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), context,
                             sizeof(*context)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(context, sizeof(*context), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_parent_record_encode(
        context, 1U, durability != NULL ?
            durability->next_foundation_transaction_id : 0U, body);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!first_durability_valid(durability, now_us) ||
        durability->volatile_continuation.runtime_instance !=
            owner->runtime_instance ||
        durability->volatile_continuation.owner_instance !=
            owner->owner_instance) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = ucn_i_sha256_128(body, sizeof(body), digest,
                              &owner->hash_workspace);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    for (index = 0U; index < UCN_I_TRANSPORT_PARENT_COUNT; ++index) {
        ucn_i_transport_parent_record_t *record = &owner->parents[index];
        if (record->occupied != 0U && context_equal(&record->context, context)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        if (record->occupied == 0U) {
            if (record->generation == UINT16_MAX) {
                exhausted_slot = true;
            } else if (free_index == UCN_I_TRANSPORT_PARENT_COUNT) {
                free_index = index;
            }
        }
    }
    if (free_index == UCN_I_TRANSPORT_PARENT_COUNT) {
        ucn_i_transport_p_unlock(owner);
        return exhausted_slot ? UCN_ERR_EXHAUSTED : UCN_ERR_NO_SPACE;
    }
    {
        ucn_i_transport_parent_record_t *record = &owner->parents[free_index];
        uint16_t generation = (uint16_t)(record->generation + 1U);

        memset(record, 0, sizeof(*record));
        record->context = *context;
        record->durability = *durability;
        memcpy(record->body, body, sizeof(body));
        memcpy(record->canonical_body_digest, digest, sizeof(digest));
        record->domain_id = durability->domain_id;
        record->domain_generation = durability->domain_generation;
        record->transition_fingerprint = fingerprint_from_digest(digest);
        record->generation = generation;
        record->occupied = 1U;
        record->prepared = 1U;
        *handle_out = ucn_i_transport_p_handle(
            owner, (uint16_t)free_index, generation,
            UCN_I_TRANSPORT_PARENT_KIND);
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_parent_prepare_next(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_parent_durability_base_t *durability,
    uint64_t now_us)
{
    ucn_i_transport_parent_record_t *record;
    uint8_t body[UCN_I_TRANSPORT_PARENT_RECORD_BYTES];
    uint8_t digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint32_t next_high_water;
    ucn_result_t result;

    if (durability == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_parent(owner, parent);
    if (record == NULL || record->active == 0U || record->prepared != 0U ||
        record->grant_available != 0U ||
        !durability_valid(record, durability, now_us)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = next_transfer_id(record->transfer_high_water, &next_high_water);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    result = ucn_i_transport_parent_record_encode(
        &record->context, next_high_water,
        durability->next_foundation_transaction_id, body);
    if (result == UCN_OK) {
        result = ucn_i_sha256_128(body, sizeof(body), digest,
                                  &owner->hash_workspace);
    }
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    record->durability = *durability;
    memcpy(record->body, body, sizeof(body));
    memcpy(record->canonical_body_digest, digest, sizeof(digest));
    record->transition_fingerprint = fingerprint_from_digest(digest);
    record->prepared = 1U;
    record->persistence_bound = 0U;
    memset(&record->persistence_handle, 0,
           sizeof(record->persistence_handle));
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_parent_requirement_get(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    ucn_i_transport_parent_requirement_t *requirement_out)
{
    ucn_i_transport_parent_record_t *record;
    ucn_i_transport_parent_requirement_t requirement;
    ucn_result_t result;

    if (requirement_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_parent(owner, parent);
    if (record == NULL || record->prepared == 0U) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&requirement, 0, sizeof(requirement));
    requirement.canonical_body = record->body;
    requirement.domain_id = record->durability.domain_id;
    requirement.foundation_transaction_id =
        record->durability.next_foundation_transaction_id;
    requirement.expected_record_generation =
        record->durability.expected_record_generation;
    requirement.absolute_deadline_us =
        record->durability.absolute_deadline_us;
    requirement.transition_fingerprint = record->transition_fingerprint;
    requirement.runtime_instance = owner->runtime_instance;
    requirement.body_bytes = UCN_I_TRANSPORT_PARENT_RECORD_BYTES;
    requirement.volatile_continuation =
        record->durability.volatile_continuation;
    requirement.caller_owner_instance = owner->owner_instance;
    requirement.domain_generation = record->durability.domain_generation;
    requirement.schema_id = UCN_I_TRANSPORT_PARENT_SCHEMA_ID;
    requirement.schema_version = UCN_I_TRANSPORT_SCHEMA;
    requirement.operation_kind = UCN_I_TRANSPORT_PARENT_OPERATION_KIND;
    memcpy(requirement.expected_body_digest,
           record->current_published_digest, 16U);
    *requirement_out = requirement;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_parent_bind_persistence(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    ucn_handle_t persistence_handle,
    const uint8_t expected_published_digest[UCN_I_TRANSPORT_DIGEST_BYTES])
{
    ucn_i_transport_parent_record_t *record;
    ucn_result_t result;

    if (expected_published_digest == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             expected_published_digest,
                             UCN_I_TRANSPORT_DIGEST_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_parent(owner, parent);
    if (record == NULL || record->prepared == 0U ||
        !persistence_handle_valid(persistence_handle,
                                  owner->runtime_instance) ||
        !ucn_i_transport_p_bytes_nonzero(expected_published_digest, 16U)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (record->persistence_bound != 0U) {
        bool exact = memcmp(&record->persistence_handle, &persistence_handle,
                            sizeof(persistence_handle)) == 0 &&
                     memcmp(record->pending_published_digest,
                            expected_published_digest, 16U) == 0;
        ucn_i_transport_p_unlock(owner);
        return exact ? UCN_OK : UCN_ERR_STATE;
    }
    record->persistence_handle = persistence_handle;
    memcpy(record->pending_published_digest, expected_published_digest, 16U);
    record->persistence_bound = 1U;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

static bool proof_matches(const ucn_i_transport_owner_t *owner,
                          const ucn_i_transport_parent_record_t *record,
                          const ucn_i_transport_parent_proof_t *proof)
{
    uint64_t next_record;

    if (proof == NULL || record->prepared == 0U ||
        record->persistence_bound == 0U ||
        (record->record_generation == 0U ?
             ucn_i_u64_allocate_first(true, &next_record) :
             ucn_i_u64_checked_next(record->record_generation,
                                    &next_record)) != UCN_OK) {
        return false;
    }
    return memcmp(&proof->persistence_handle, &record->persistence_handle,
                  sizeof(proof->persistence_handle)) == 0 &&
           proof->domain_id == record->durability.domain_id &&
           proof->record_generation == next_record &&
           proof->foundation_transaction_id ==
               record->durability.next_foundation_transaction_id &&
           proof->witness_generation == next_record &&
           proof->transition_fingerprint == record->transition_fingerprint &&
           proof->runtime_instance == owner->runtime_instance &&
           proof->body_bytes == UCN_I_TRANSPORT_PARENT_RECORD_BYTES &&
           memcmp(&proof->volatile_continuation,
                  &record->durability.volatile_continuation,
                  sizeof(proof->volatile_continuation)) == 0 &&
           proof->persistence_owner_instance ==
               record->persistence_handle.owner_instance &&
           proof->caller_owner_instance == owner->owner_instance &&
           proof->domain_generation == record->durability.domain_generation &&
           proof->schema_id == UCN_I_TRANSPORT_PARENT_SCHEMA_ID &&
           proof->schema_version == UCN_I_TRANSPORT_SCHEMA &&
           proof->operation_kind == UCN_I_TRANSPORT_PARENT_OPERATION_KIND &&
           proof->reserved_zero == 0U &&
           memcmp(proof->body_digest, record->pending_published_digest,
                  16U) == 0;
}

ucn_result_t ucn_i_transport_parent_activate_next(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_parent_proof_t *proof,
    uint64_t now_us,
    uint32_t *transfer_id_out)
{
    ucn_i_transport_parent_record_t *record;
    uint32_t next_high_water;
    ucn_result_t result;

    if (transfer_id_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), proof, sizeof(*proof)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), transfer_id_out,
                             sizeof(*transfer_id_out)) ||
        ucn_i_ranges_overlap(proof, sizeof(*proof), transfer_id_out,
                             sizeof(*transfer_id_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_parent(owner, parent);
    if (record == NULL ||
        record->grant_available != 0U ||
        ucn_i_deadline_expired_us(now_us,
                                  record->durability.absolute_deadline_us) ||
        !proof_matches(owner, record, proof) ||
        next_transfer_id(record->transfer_high_water,
                         &next_high_water) != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->transfer_high_water = next_high_water;
    record->record_generation = proof->record_generation;
    record->foundation_transaction_id = proof->foundation_transaction_id;
    memcpy(record->current_published_digest,
           record->pending_published_digest, 16U);
    memset(record->pending_published_digest, 0,
           sizeof(record->pending_published_digest));
    memset(&record->durability, 0, sizeof(record->durability));
    memset(&record->persistence_handle, 0,
           sizeof(record->persistence_handle));
    record->transition_fingerprint = 0U;
    record->prepared = 0U;
    record->persistence_bound = 0U;
    record->active = 1U;
    record->granted_transfer_id = next_high_water;
    record->grant_available = 1U;
    *transfer_id_out = next_high_water;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_parent_discard_grant(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    uint32_t transfer_id)
{
    ucn_i_transport_parent_record_t *record;
    ucn_result_t result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_parent(owner, parent);
    if (record == NULL || record->active == 0U ||
        record->prepared != 0U || record->grant_available == 0U ||
        transfer_id == 0U || record->granted_transfer_id != transfer_id) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->granted_transfer_id = 0U;
    record->grant_available = 0U;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

static ucn_result_t restore_active_record(
    ucn_i_transport_owner_t *owner,
    ucn_i_transport_parent_record_t *record)
{
    ucn_result_t result;

    if (record->active == 0U) {
        uint16_t generation = record->generation;
        memset(record, 0, sizeof(*record));
        record->generation = generation;
        return UCN_OK;
    }
    result = ucn_i_transport_parent_record_encode(
        &record->context, record->transfer_high_water,
        record->foundation_transaction_id, record->body);

    if (result == UCN_OK) {
        result = ucn_i_sha256_128(record->body, sizeof(record->body),
                                  record->canonical_body_digest,
                                  &owner->hash_workspace);
    }
    if (result != UCN_OK) {
        return result;
    }
    memset(&record->durability, 0, sizeof(record->durability));
    memset(&record->persistence_handle, 0,
           sizeof(record->persistence_handle));
    memset(record->pending_published_digest, 0,
           sizeof(record->pending_published_digest));
    record->transition_fingerprint = 0U;
    record->prepared = 0U;
    record->persistence_bound = 0U;
    return UCN_OK;
}

ucn_result_t ucn_i_transport_parent_abort_unsubmitted(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent)
{
    ucn_i_transport_parent_record_t *record;
    ucn_result_t result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_parent(owner, parent);
    if (record == NULL || record->prepared == 0U ||
        record->persistence_bound != 0U) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = restore_active_record(owner, record);
    ucn_i_transport_p_unlock(owner);
    return result;
}

static bool failure_matches(
    const ucn_i_transport_owner_t *owner,
    const ucn_i_transport_parent_record_t *record,
    const ucn_i_transport_parent_failure_t *failure)
{
    return failure != NULL && record->prepared != 0U &&
           record->persistence_bound != 0U &&
           failure->request_terminal != 0U &&
           failure->terminal_result != UCN_OK &&
           memcmp(&failure->persistence_handle,
                  &record->persistence_handle,
                  sizeof(failure->persistence_handle)) == 0 &&
           memcmp(&failure->volatile_continuation,
                  &record->durability.volatile_continuation,
                  sizeof(failure->volatile_continuation)) == 0 &&
           failure->domain_id == record->domain_id &&
           failure->foundation_transaction_id ==
               record->durability.next_foundation_transaction_id &&
           failure->transition_fingerprint == record->transition_fingerprint &&
           failure->runtime_instance == owner->runtime_instance &&
           failure->persistence_owner_instance ==
               record->persistence_handle.owner_instance &&
           failure->caller_owner_instance == owner->owner_instance &&
           failure->domain_generation == record->domain_generation &&
           failure->schema_id == UCN_I_TRANSPORT_PARENT_SCHEMA_ID &&
           failure->schema_version == UCN_I_TRANSPORT_SCHEMA &&
           failure->operation_kind == UCN_I_TRANSPORT_PARENT_OPERATION_KIND &&
           failure->reserved_zero[0] == 0U &&
           failure->reserved_zero[1] == 0U &&
           failure->reserved_zero[2] == 0U;
}

ucn_result_t ucn_i_transport_parent_fail_persistence(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_parent_failure_t *failure)
{
    ucn_i_transport_parent_record_t *record;
    ucn_result_t result;

    if (failure == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), failure,
                             sizeof(*failure))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_parent(owner, parent);
    if (record == NULL || !failure_matches(owner, record, failure)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = restore_active_record(owner, record);
    ucn_i_transport_p_unlock(owner);
    return result;
}

ucn_result_t ucn_i_transport_parent_view(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    ucn_i_transport_parent_view_t *view_out)
{
    ucn_i_transport_parent_record_t *record;
    ucn_i_transport_parent_view_t view;
    ucn_result_t result;

    if (view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_parent(owner, parent);
    if (record == NULL) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    memset(&view, 0, sizeof(view));
    view.context = record->context;
    view.domain_id = record->domain_id;
    view.record_generation = record->record_generation;
    view.foundation_transaction_id = record->foundation_transaction_id;
    view.transfer_high_water = record->transfer_high_water;
    view.granted_transfer_id = record->granted_transfer_id;
    view.domain_generation = record->domain_generation;
    view.active = record->active;
    view.grant_available = record->grant_available;
    *view_out = view;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}
