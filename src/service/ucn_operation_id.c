#include "service/ucn_service_private.h"

#include "internal/ucn_checked.h"
#include "internal/ucn_digest.h"

#include <string.h>

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void write_u64(uint8_t *bytes, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (56U - 8U * index));
    }
}

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | bytes[3];
}

static uint64_t read_u64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index) value = (value << 8U) | bytes[index];
    return value;
}

static bool digest_nonzero(const uint8_t digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    size_t index;
    if (digest == NULL) return false;
    for (index = 0U; index < UCN_I_SERVICE_DIGEST_BYTES; ++index) {
        if (digest[index] != 0U) return true;
    }
    return false;
}

static bool durability_valid(
    const ucn_i_service_owner_t *owner,
    const ucn_i_service_operation_durability_t *durability)
{
    return owner != NULL && durability != NULL && durability->domain_id != 0U &&
           durability->foundation_transaction_id != 0U &&
           durability->expected_record_generation != UINT64_MAX &&
           durability->absolute_deadline_us != 0U &&
           durability->volatile_continuation.runtime_instance ==
               owner->runtime_instance &&
           durability->volatile_continuation.owner_instance ==
               owner->owner_instance &&
           durability->volatile_continuation.generation != 0U &&
           durability->volatile_continuation.object_kind >=
               UCN_OBJECT_KIND_SEND &&
           durability->volatile_continuation.object_kind <=
               UCN_OBJECT_KIND_PERSISTENCE &&
           durability->volatile_continuation.reserved_zero == 0U &&
           durability->domain_generation != 0U &&
           durability->schema_id != 0U && durability->schema_version != 0U;
}

static ucn_result_t encode_body(
    uint32_t parent_generation,
    uint64_t high_water,
    uint8_t body[UCN_I_SERVICE_OPERATION_ID_BODY_BYTES],
    uint8_t digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    ucn_i_sha256_workspace_t workspace;
    if (parent_generation == 0U || high_water == 0U || body == NULL ||
        digest == NULL) return UCN_ERR_ARGUMENT;
    memset(body, 0, UCN_I_SERVICE_OPERATION_ID_BODY_BYTES);
    memcpy(body, "OID1", 4U);
    write_u16(&body[4], UCN_I_SERVICE_OPERATION_SCHEMA);
    write_u16(&body[6], 0U);
    write_u32(&body[8], parent_generation);
    write_u64(&body[12], high_water);
    memset(&workspace, 0, sizeof(workspace));
    return ucn_i_sha256_128(body, UCN_I_SERVICE_OPERATION_ID_BODY_BYTES,
                            digest, &workspace);
}

ucn_result_t ucn_i_service_operation_id_import(
    ucn_i_service_owner_t *owner,
    const uint8_t body[UCN_I_SERVICE_OPERATION_ID_BODY_BYTES],
    const ucn_i_service_operation_durability_t *loaded_durability,
    const uint8_t loaded_published_digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    uint8_t canonical[UCN_I_SERVICE_DIGEST_BYTES];
    uint8_t encoded[UCN_I_SERVICE_OPERATION_ID_BODY_BYTES];
    uint32_t parent_generation;
    uint64_t high_water;
    ucn_result_t result;

    if (body == NULL || loaded_published_digest == NULL ||
        !digest_nonzero(loaded_published_digest) ||
        memcmp(body, "OID1", 4U) != 0 ||
        read_u16(&body[4]) != UCN_I_SERVICE_OPERATION_SCHEMA ||
        read_u16(&body[6]) != 0U) return UCN_ERR_MALFORMED;
    parent_generation = read_u32(&body[8]);
    high_water = read_u64(&body[12]);
    if (encode_body(parent_generation, high_water, encoded, canonical) != UCN_OK ||
        memcmp(encoded, body, sizeof(encoded)) != 0) return UCN_ERR_MALFORMED;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    if (!durability_valid(owner, loaded_durability) ||
        owner->operation_id_ready != 0U ||
        owner->operation_id_persist_pending != 0U ||
        high_water == UINT64_MAX) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->operation_id_parent_generation = parent_generation;
    owner->operation_id_reserved_through = high_water;
    owner->operation_id_next = high_water + 1U;
    owner->operation_id_ready = 1U;
    memcpy(owner->operation_id_current_digest, loaded_published_digest,
           sizeof(owner->operation_id_current_digest));
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_id_prepare_interval(
    ucn_i_service_owner_t *owner,
    uint32_t parent_generation,
    const ucn_i_service_operation_durability_t *durability,
    ucn_i_service_operation_id_requirement_t *requirement_out)
{
    uint64_t proposed;
    uint8_t body[UCN_I_SERVICE_OPERATION_ID_BODY_BYTES];
    uint8_t digest[UCN_I_SERVICE_DIGEST_BYTES];
    ucn_result_t result;

    if (requirement_out == NULL) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    if (!durability_valid(owner, durability) || parent_generation == 0U ||
        owner->operation_id_faulted != 0U ||
        owner->operation_id_persist_pending != 0U ||
        owner->operation_id_next <= owner->operation_id_reserved_through ||
        (owner->operation_id_ready != 0U &&
         owner->operation_id_parent_generation != parent_generation) ||
        owner->operation_id_reserved_through >=
            UINT64_MAX - UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    proposed = owner->operation_id_reserved_through +
               UCN_I_SERVICE_OPERATION_ID_INTERVAL_SIZE;
    if (encode_body(parent_generation, proposed, body, digest) != UCN_OK) {
        owner->operation_id_faulted = 1U;
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(requirement_out, 0, sizeof(*requirement_out));
    requirement_out->durability = *durability;
    memcpy(requirement_out->canonical_body_digest, digest, sizeof(digest));
    memcpy(requirement_out->expected_body_digest,
           owner->operation_id_current_digest,
           sizeof(requirement_out->expected_body_digest));
    memcpy(requirement_out->body, body, sizeof(body));
    requirement_out->proposed_high_water = proposed;
    requirement_out->runtime_instance = owner->runtime_instance;
    requirement_out->parent_generation = parent_generation;
    requirement_out->caller_owner_instance = owner->owner_instance;
    requirement_out->operation_kind = UCN_I_SERVICE_OPERATION_ID_PERSIST_KIND;
    owner->operation_id_durability = *durability;
    owner->operation_id_pending_high_water = proposed;
    owner->operation_id_parent_generation = parent_generation;
    memcpy(owner->operation_id_canonical_digest, digest, sizeof(digest));
    owner->operation_id_persist_pending = 1U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_id_bind_persistence(
    ucn_i_service_owner_t *owner,
    ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    ucn_result_t result;
    if (owner == NULL || published_body_digest == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), published_body_digest,
                             UCN_I_SERVICE_DIGEST_BYTES)) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    if (owner->operation_id_persist_pending == 0U ||
        owner->operation_id_persistence_bound != 0U ||
        persistence_handle.reserved_zero != 0U ||
        persistence_handle.runtime_instance != owner->runtime_instance ||
        persistence_handle.generation == 0U ||
        persistence_handle.object_kind != UCN_OBJECT_KIND_PERSISTENCE ||
        !digest_nonzero(published_body_digest)) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->operation_id_persistence_handle = persistence_handle;
    memcpy(owner->operation_id_pending_digest, published_body_digest,
           sizeof(owner->operation_id_pending_digest));
    owner->operation_id_persistence_bound = 1U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_id_accept_proof(
    ucn_i_service_owner_t *owner,
    const ucn_i_service_operation_id_proof_t *proof)
{
    ucn_result_t result;
    if (proof == NULL) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    if (owner->operation_id_persist_pending == 0U ||
        owner->operation_id_persistence_bound == 0U ||
        memcmp(&proof->persistence_handle,
               &owner->operation_id_persistence_handle,
               sizeof(proof->persistence_handle)) != 0 ||
        proof->domain_id != owner->operation_id_durability.domain_id ||
        proof->foundation_transaction_id !=
            owner->operation_id_durability.foundation_transaction_id ||
        proof->record_generation !=
            owner->operation_id_durability.expected_record_generation + 1U ||
        proof->witness_generation != proof->record_generation ||
        proof->runtime_instance != owner->runtime_instance ||
        proof->body_bytes != UCN_I_SERVICE_OPERATION_ID_BODY_BYTES ||
        proof->domain_generation !=
            owner->operation_id_durability.domain_generation ||
        proof->persistence_owner_instance !=
            owner->operation_id_persistence_handle.owner_instance ||
        proof->caller_owner_instance != owner->owner_instance ||
        proof->schema_id != owner->operation_id_durability.schema_id ||
        proof->schema_version != owner->operation_id_durability.schema_version ||
        proof->operation_kind != UCN_I_SERVICE_OPERATION_ID_PERSIST_KIND ||
        proof->reserved_zero != 0U ||
        memcmp(proof->body_digest, owner->operation_id_pending_digest,
               sizeof(owner->operation_id_pending_digest)) != 0) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->operation_id_reserved_through =
        owner->operation_id_pending_high_water;
    if (owner->operation_id_next == 0U) owner->operation_id_next = 1U;
    owner->operation_id_ready = 1U;
    owner->operation_id_persist_pending = 0U;
    owner->operation_id_persistence_bound = 0U;
    memcpy(owner->operation_id_current_digest,
           owner->operation_id_pending_digest,
           sizeof(owner->operation_id_current_digest));
    memset(owner->operation_id_pending_digest, 0,
           sizeof(owner->operation_id_pending_digest));
    memset(&owner->operation_id_persistence_handle, 0,
           sizeof(owner->operation_id_persistence_handle));
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_id_take(
    ucn_i_service_owner_t *owner,
    uint64_t *operation_id_out)
{
    ucn_result_t result;
    uint64_t value;
    if (operation_id_out == NULL) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    value = owner->operation_id_next;
    if (owner->operation_id_ready == 0U ||
        owner->operation_id_persist_pending != 0U ||
        owner->operation_id_faulted != 0U || value == 0U ||
        value > owner->operation_id_reserved_through) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    *operation_id_out = value;
    owner->operation_id_next = value + 1U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_id_view(
    ucn_i_service_owner_t *owner,
    ucn_i_service_operation_id_view_t *view_out)
{
    ucn_result_t result;
    if (view_out == NULL) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    memset(view_out, 0, sizeof(*view_out));
    view_out->next_id = owner->operation_id_next;
    view_out->reserved_through = owner->operation_id_reserved_through;
    view_out->parent_generation = owner->operation_id_parent_generation;
    view_out->interval_ready = owner->operation_id_ready;
    view_out->persist_pending = owner->operation_id_persist_pending;
    view_out->faulted = owner->operation_id_faulted;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}
