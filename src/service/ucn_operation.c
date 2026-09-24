#include "service/ucn_service_private.h"

#include "internal/ucn_checked.h"
#include "internal/ucn_digest.h"

#include <string.h>

#define OPERATION_BODY_MAX UCN_I_SERVICE_OPERATION_BODY_BYTES

UCN_STATIC_ASSERT(OPERATION_BODY_MAX <= 256U,
                  operation_body_exceeds_foundation_minimum);

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
        bytes[index] = (uint8_t)(value >> (56U - (8U * index)));
    }
}

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static uint64_t read_u64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | bytes[index];
    }
    return value;
}

static bool digest_nonzero(const uint8_t digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    size_t index;
    for (index = 0U; index < UCN_I_SERVICE_DIGEST_BYTES; ++index) {
        if (digest[index] != 0U) return true;
    }
    return false;
}

static bool operation_key_valid(const ucn_i_service_operation_key_t *key)
{
    return key != NULL && ucn_i_service_p_key_valid(&key->service) &&
           digest_nonzero(key->request_digest);
}

static bool operation_key_equal(const ucn_i_service_operation_key_t *left,
                                const ucn_i_service_operation_key_t *right)
{
    return ucn_i_service_p_key_equal(&left->service, &right->service) &&
           memcmp(left->request_digest, right->request_digest,
                  sizeof(left->request_digest)) == 0;
}

static bool durability_valid(
    const ucn_i_service_operation_durability_t *durability)
{
    return durability != NULL && durability->domain_id != 0U &&
           durability->foundation_transaction_id != 0U &&
           durability->expected_record_generation != UINT64_MAX &&
           durability->absolute_deadline_us != 0U &&
           durability->volatile_continuation.runtime_instance != 0U &&
           durability->volatile_continuation.owner_instance != 0U &&
           durability->volatile_continuation.generation != 0U &&
           durability->volatile_continuation.object_kind >=
               UCN_OBJECT_KIND_SEND &&
           durability->volatile_continuation.object_kind <=
               UCN_OBJECT_KIND_PERSISTENCE &&
           durability->volatile_continuation.reserved_zero == 0U &&
           durability->domain_generation != 0U &&
           durability->schema_id != 0U && durability->schema_version != 0U;
}

static bool durability_owned_by(
    const ucn_i_service_operation_durability_t *durability,
    const ucn_i_service_owner_t *owner)
{
    return durability_valid(durability) && owner != NULL &&
           durability->volatile_continuation.runtime_instance ==
               owner->runtime_instance &&
           durability->volatile_continuation.owner_instance ==
               owner->owner_instance;
}

static void result_freeze(ucn_i_service_result_t *destination,
                          const ucn_i_service_result_t *source)
{
    memset(destination, 0, sizeof(*destination));
    if (source != NULL) {
        destination->application_result = source->application_result;
        destination->bytes = source->bytes;
        if (source->bytes != 0U) {
            memcpy(destination->payload, source->payload, source->bytes);
        }
    }
}

static ucn_result_t body_digest(
    const ucn_i_service_operation_record_t *record,
    uint8_t next_phase,
    const ucn_i_service_result_t *result,
    uint8_t *body_out,
    uint8_t digest_out[UCN_I_SERVICE_DIGEST_BYTES],
    uint32_t *body_bytes_out)
{
    uint8_t body[OPERATION_BODY_MAX];
    ucn_i_sha256_workspace_t workspace;
    size_t offset = 0U;
    const ucn_i_service_key_t *key = &record->key.service;
    uint16_t result_bytes = result == NULL ? 0U : result->bytes;

    if (result_bytes > UCN_I_SERVICE_RESULT_BYTES || digest_out == NULL ||
        body_bytes_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    memset(body, 0, sizeof(body));
    memcpy(&body[offset], "OPS1", 4U); offset += 4U;
    write_u16(&body[offset], UCN_I_SERVICE_OPERATION_SCHEMA); offset += 2U;
    body[offset++] = next_phase;
    body[offset++] = record->executor_observed;
    write_u64(&body[offset], key->operation_id); offset += 8U;
    write_u32(&body[offset], key->realm); offset += 4U;
    write_u16(&body[offset], key->service_id); offset += 2U;
    write_u16(&body[offset], key->opcode); offset += 2U;
    write_u32(&body[offset], key->client.address); offset += 4U;
    write_u32(&body[offset], key->client.generation); offset += 4U;
    memcpy(&body[offset], key->client.principal, 16U); offset += 16U;
    write_u32(&body[offset], key->server.address); offset += 4U;
    write_u32(&body[offset], key->server.generation); offset += 4U;
    memcpy(&body[offset], key->server.principal, 16U); offset += 16U;
    write_u32(&body[offset], key->security.session_generation); offset += 4U;
    write_u32(&body[offset], key->security.key_generation); offset += 4U;
    write_u32(&body[offset], key->security.policy_generation); offset += 4U;
    body[offset++] = key->security.origin_security;
    body[offset++] = key->security.acl_authorized;
    body[offset++] = 0U;
    body[offset++] = 0U;
    memcpy(&body[offset], record->key.request_digest, 16U); offset += 16U;
    write_u32(&body[offset], result == NULL ? 0U :
              (uint32_t)result->application_result); offset += 4U;
    write_u16(&body[offset], result_bytes); offset += 2U;
    if (result_bytes != 0U) {
        memcpy(&body[offset], result->payload, result_bytes);
        offset += result_bytes;
    }
    memset(&workspace, 0, sizeof(workspace));
    *body_bytes_out = (uint32_t)offset;
    if (ucn_i_sha256_128(body, offset, digest_out, &workspace) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (body_out != NULL) {
        memcpy(body_out, body, offset);
    }
    return UCN_OK;
}

static ucn_result_t next_generation(uint16_t current, uint16_t *next_out)
{
    if (next_out == NULL) return UCN_ERR_ARGUMENT;
    if (current == UINT16_MAX) return UCN_ERR_EXHAUSTED;
    *next_out = (uint16_t)(current + 1U);
    return UCN_OK;
}

static ucn_result_t start_transition(
    ucn_i_service_operation_record_t *record,
    uint8_t next_phase,
    const ucn_i_service_result_t *result,
    const ucn_i_service_operation_durability_t *durability)
{
    uint32_t body_bytes;
    ucn_result_t digest_result;

    if (!durability_valid(durability) || record->phase ==
            UCN_I_SERVICE_OPERATION_PERSIST_PENDING) {
        return UCN_ERR_STATE;
    }
    digest_result = body_digest(record, next_phase, result, NULL,
                                record->canonical_body_digest, &body_bytes);
    if (digest_result != UCN_OK) return digest_result;
    (void)body_bytes;
    record->durability = *durability;
    record->pending_body_bytes = body_bytes;
    record->pending_phase = next_phase;
    record->stable_phase = record->phase;
    record->phase = UCN_I_SERVICE_OPERATION_PERSIST_PENDING;
    record->persistence_bound = 0U;
    memset(&record->persistence_handle, 0, sizeof(record->persistence_handle));
    memset(record->pending_published_digest, 0,
           sizeof(record->pending_published_digest));
    if (result != NULL) result_freeze(&record->result, result);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_begin(
    ucn_i_service_owner_t *owner,
    const ucn_i_service_operation_key_t *key,
    const ucn_i_service_operation_durability_t *durability,
    ucn_handle_t *operation_out)
{
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_SERVICE_OPERATION_COUNT;
    uint16_t generation;
    ucn_i_service_operation_record_t candidate;

    if (!operation_key_valid(key) || !durability_owned_by(durability, owner) ||
        operation_out == NULL) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    for (index = 0U; index < UCN_I_SERVICE_OPERATION_COUNT; ++index) {
        ucn_i_service_operation_record_t *record = &owner->operations[index];
        if (record->occupied != 0U &&
            record->key.service.operation_id == key->service.operation_id &&
            record->key.service.realm == key->service.realm) {
            if (!operation_key_equal(&record->key, key)) {
                ucn_i_service_p_unlock(owner);
                return UCN_ERR_REPLAY;
            }
            *operation_out = ucn_i_service_p_handle(
                owner, (uint16_t)index, record->generation,
                UCN_I_SERVICE_OPERATION_KIND);
            ucn_i_service_p_unlock(owner);
            return UCN_OK;
        }
        if (record->occupied == 0U &&
            free_index == UCN_I_SERVICE_OPERATION_COUNT) free_index = index;
    }
    if (free_index == UCN_I_SERVICE_OPERATION_COUNT) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (next_generation(owner->operations[free_index].generation,
                        &generation) != UCN_OK) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.key = *key;
    candidate.generation = generation;
    candidate.occupied = 1U;
    result = start_transition(&candidate, UCN_I_SERVICE_OPERATION_PREPARED,
                              NULL, durability);
    if (result != UCN_OK) {
        ucn_i_service_p_unlock(owner);
        return result;
    }
    owner->operations[free_index] = candidate;
    *operation_out = ucn_i_service_p_handle(owner, (uint16_t)free_index,
                                             generation,
                                             UCN_I_SERVICE_OPERATION_KIND);
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

static ucn_result_t decode_body(
    const uint8_t *body,
    uint32_t body_bytes,
    ucn_i_service_operation_key_t *key_out,
    ucn_i_service_result_t *result_out,
    uint8_t *phase_out,
    uint8_t *executor_observed_out)
{
    size_t offset = 0U;
    uint16_t result_bytes;
    uint32_t app_result;

    if (body == NULL || key_out == NULL || result_out == NULL ||
        phase_out == NULL || executor_observed_out == NULL ||
        body_bytes < 110U || body_bytes > UCN_I_SERVICE_OPERATION_BODY_BYTES ||
        memcmp(body, "OPS1", 4U) != 0 || read_u16(&body[4]) !=
            UCN_I_SERVICE_OPERATION_SCHEMA) {
        return UCN_ERR_MALFORMED;
    }
    memset(key_out, 0, sizeof(*key_out));
    memset(result_out, 0, sizeof(*result_out));
    offset = 6U;
    *phase_out = body[offset++];
    *executor_observed_out = body[offset++];
    key_out->service.operation_id = read_u64(&body[offset]); offset += 8U;
    key_out->service.realm = read_u32(&body[offset]); offset += 4U;
    key_out->service.service_id = read_u16(&body[offset]); offset += 2U;
    key_out->service.opcode = read_u16(&body[offset]); offset += 2U;
    key_out->service.client.address = read_u32(&body[offset]); offset += 4U;
    key_out->service.client.generation = read_u32(&body[offset]); offset += 4U;
    memcpy(key_out->service.client.principal, &body[offset], 16U); offset += 16U;
    key_out->service.server.address = read_u32(&body[offset]); offset += 4U;
    key_out->service.server.generation = read_u32(&body[offset]); offset += 4U;
    memcpy(key_out->service.server.principal, &body[offset], 16U); offset += 16U;
    key_out->service.security.session_generation = read_u32(&body[offset]); offset += 4U;
    key_out->service.security.key_generation = read_u32(&body[offset]); offset += 4U;
    key_out->service.security.policy_generation = read_u32(&body[offset]); offset += 4U;
    key_out->service.security.origin_security = body[offset++];
    key_out->service.security.acl_authorized = body[offset++];
    if (body[offset++] != 0U || body[offset++] != 0U) return UCN_ERR_MALFORMED;
    memcpy(key_out->request_digest, &body[offset], 16U); offset += 16U;
    app_result = read_u32(&body[offset]); offset += 4U;
    result_bytes = read_u16(&body[offset]); offset += 2U;
    if (result_bytes > UCN_I_SERVICE_RESULT_BYTES ||
        offset + result_bytes != body_bytes ||
        *phase_out < UCN_I_SERVICE_OPERATION_PREPARED ||
        *phase_out > UCN_I_SERVICE_OPERATION_TOMBSTONED ||
        *executor_observed_out > 1U) {
        return UCN_ERR_MALFORMED;
    }
    if (app_result == 0U) {
        result_out->application_result = UCN_OK;
    } else if (app_result >= UINT32_C(0xFFFFFFF1)) {
        result_out->application_result =
            -(ucn_result_t)(UINT32_MAX - app_result + 1U);
    } else {
        return UCN_ERR_MALFORMED;
    }
    result_out->bytes = result_bytes;
    if (result_bytes != 0U) {
        memcpy(result_out->payload, &body[offset], result_bytes);
    }
    if (((*phase_out == UCN_I_SERVICE_OPERATION_PREPARED ||
          *phase_out == UCN_I_SERVICE_OPERATION_EXECUTING) &&
         (*executor_observed_out != 0U || result_bytes != 0U ||
          result_out->application_result != UCN_OK)) ||
        (*phase_out == UCN_I_SERVICE_OPERATION_COMMITTED_RESULT &&
         *executor_observed_out == 0U) ||
        (*phase_out == UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT &&
         (*executor_observed_out != 0U || result_bytes != 0U ||
          result_out->application_result != UCN_ERR_CANCELLED)) ||
        (*phase_out == UCN_I_SERVICE_OPERATION_IN_DOUBT &&
         (result_bytes != 0U ||
          result_out->application_result != UCN_ERR_IN_DOUBT))) {
        return UCN_ERR_MALFORMED;
    }
    if (!operation_key_valid(key_out)) return UCN_ERR_MALFORMED;
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_import(
    ucn_i_service_owner_t *owner,
    const uint8_t *body,
    uint32_t body_bytes,
    const ucn_i_service_operation_durability_t *loaded_durability,
    const uint8_t loaded_published_digest[UCN_I_SERVICE_DIGEST_BYTES],
    ucn_handle_t *operation_out)
{
    ucn_i_service_operation_key_t key;
    ucn_i_service_result_t operation_result;
    ucn_i_service_operation_record_t candidate;
    ucn_result_t result;
    uint8_t phase;
    uint8_t executor_observed;
    size_t index;
    size_t free_index = UCN_I_SERVICE_OPERATION_COUNT;
    uint16_t generation;

    if (!durability_valid(loaded_durability) ||
        loaded_published_digest == NULL ||
        !digest_nonzero(loaded_published_digest) || operation_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = decode_body(body, body_bytes, &key, &operation_result,
                         &phase, &executor_observed);
    if (result != UCN_OK) return result;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    for (index = 0U; index < UCN_I_SERVICE_OPERATION_COUNT; ++index) {
        ucn_i_service_operation_record_t *record = &owner->operations[index];
        if (record->occupied != 0U &&
            record->key.service.operation_id == key.service.operation_id &&
            record->key.service.realm == key.service.realm) {
            if (!operation_key_equal(&record->key, &key) ||
                record->phase != phase ||
                memcmp(&record->result, &operation_result,
                       sizeof(operation_result)) != 0) {
                ucn_i_service_p_unlock(owner);
                return UCN_ERR_REPLAY;
            }
            *operation_out = ucn_i_service_p_handle(
                owner, (uint16_t)index, record->generation,
                UCN_I_SERVICE_OPERATION_KIND);
            ucn_i_service_p_unlock(owner);
            return UCN_OK;
        }
        if (record->occupied == 0U &&
            free_index == UCN_I_SERVICE_OPERATION_COUNT) free_index = index;
    }
    if (free_index == UCN_I_SERVICE_OPERATION_COUNT) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (next_generation(owner->operations[free_index].generation,
                        &generation) != UCN_OK) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.key = key;
    candidate.durability = *loaded_durability;
    candidate.result = operation_result;
    memcpy(candidate.current_published_digest, loaded_published_digest,
           sizeof(candidate.current_published_digest));
    candidate.generation = generation;
    candidate.occupied = 1U;
    candidate.phase = phase;
    candidate.stable_phase = phase;
    candidate.executor_observed =
        phase == UCN_I_SERVICE_OPERATION_EXECUTING ? 1U : executor_observed;
    candidate.reply_ready =
        phase == UCN_I_SERVICE_OPERATION_COMMITTED_RESULT ||
        phase == UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT ||
        phase == UCN_I_SERVICE_OPERATION_IN_DOUBT;
    owner->operations[free_index] = candidate;
    *operation_out = ucn_i_service_p_handle(owner, (uint16_t)free_index,
                                             generation,
                                             UCN_I_SERVICE_OPERATION_KIND);
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_requirement_get(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    ucn_i_service_operation_requirement_t *requirement_out)
{
    ucn_result_t result;
    ucn_i_service_operation_record_t *record;
    uint32_t body_bytes;
    uint8_t digest[UCN_I_SERVICE_DIGEST_BYTES];

    if (requirement_out == NULL) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    record = ucn_i_service_p_operation(owner, operation);
    if (record == NULL || record->phase != UCN_I_SERVICE_OPERATION_PERSIST_PENDING) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(requirement_out, 0, sizeof(*requirement_out));
    result = body_digest(record, record->pending_phase,
                         record->pending_phase >=
                             UCN_I_SERVICE_OPERATION_COMMITTED_RESULT ?
                             &record->result : NULL,
                         requirement_out->body, digest, &body_bytes);
    if (result != UCN_OK ||
        memcmp(digest, record->canonical_body_digest, sizeof(digest)) != 0) {
        record->phase = UCN_I_SERVICE_OPERATION_FAULTED;
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    requirement_out->continuation = operation;
    requirement_out->durability = record->durability;
    memcpy(requirement_out->canonical_body_digest, digest, sizeof(digest));
    memcpy(requirement_out->expected_body_digest,
           record->current_published_digest,
           sizeof(requirement_out->expected_body_digest));
    requirement_out->runtime_instance = owner->runtime_instance;
    requirement_out->body_bytes = body_bytes;
    requirement_out->caller_owner_instance = owner->owner_instance;
    requirement_out->operation_kind = UCN_I_SERVICE_OPERATION_PERSIST_KIND;
    requirement_out->next_phase = record->pending_phase;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_bind_persistence(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_SERVICE_DIGEST_BYTES])
{
    ucn_result_t result;
    ucn_i_service_operation_record_t *record;

    if (owner == NULL || published_body_digest == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), published_body_digest,
                             UCN_I_SERVICE_DIGEST_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    record = ucn_i_service_p_operation(owner, operation);
    if (record == NULL || record->phase != UCN_I_SERVICE_OPERATION_PERSIST_PENDING ||
        record->persistence_bound != 0U || persistence_handle.reserved_zero != 0U ||
        persistence_handle.runtime_instance != owner->runtime_instance ||
        persistence_handle.generation == 0U ||
        persistence_handle.object_kind != UCN_OBJECT_KIND_PERSISTENCE ||
        !digest_nonzero(published_body_digest)) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->persistence_handle = persistence_handle;
    memcpy(record->pending_published_digest, published_body_digest,
           sizeof(record->pending_published_digest));
    record->persistence_bound = 1U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_accept_proof(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    const ucn_i_service_operation_proof_t *proof)
{
    ucn_result_t result;
    ucn_i_service_operation_record_t *record;

    if (proof == NULL) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    record = ucn_i_service_p_operation(owner, operation);
    if (record == NULL || record->phase != UCN_I_SERVICE_OPERATION_PERSIST_PENDING ||
        record->persistence_bound == 0U ||
        memcmp(&proof->continuation, &operation, sizeof(operation)) != 0 ||
        memcmp(&proof->persistence_handle, &record->persistence_handle,
               sizeof(record->persistence_handle)) != 0 ||
        proof->domain_id != record->durability.domain_id ||
        proof->foundation_transaction_id !=
            record->durability.foundation_transaction_id ||
        proof->record_generation !=
            record->durability.expected_record_generation + 1U ||
        proof->witness_generation != proof->record_generation ||
        proof->runtime_instance != owner->runtime_instance ||
        proof->body_bytes != record->pending_body_bytes ||
        proof->domain_generation != record->durability.domain_generation ||
        proof->persistence_owner_instance !=
            record->persistence_handle.owner_instance ||
        proof->caller_owner_instance != owner->owner_instance ||
        proof->schema_id != record->durability.schema_id ||
        proof->schema_version != record->durability.schema_version ||
        proof->operation_kind != UCN_I_SERVICE_OPERATION_PERSIST_KIND ||
        proof->reserved_zero16 != 0U ||
        proof->next_phase != record->pending_phase ||
        proof->reserved_zero[0] != 0U || proof->reserved_zero[1] != 0U ||
        proof->reserved_zero[2] != 0U ||
        memcmp(proof->body_digest, record->pending_published_digest,
               sizeof(record->pending_published_digest)) != 0) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->phase = record->pending_phase;
    record->stable_phase = record->phase;
    record->pending_phase = 0U;
    record->pending_body_bytes = 0U;
    record->persistence_bound = 0U;
    memcpy(record->current_published_digest,
           record->pending_published_digest,
           sizeof(record->current_published_digest));
    memset(record->pending_published_digest, 0,
           sizeof(record->pending_published_digest));
    memset(&record->persistence_handle, 0, sizeof(record->persistence_handle));
    if (record->phase == UCN_I_SERVICE_OPERATION_COMMITTED_RESULT ||
        record->phase == UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT ||
        record->phase == UCN_I_SERVICE_OPERATION_IN_DOUBT) {
        record->reply_ready = 1U;
    }
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_prepare_executing(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    const ucn_i_service_operation_durability_t *durability)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_operation_record_t *record;

    if (result != UCN_OK) return result;
    record = ucn_i_service_p_operation(owner, operation);
    if (record == NULL || record->phase != UCN_I_SERVICE_OPERATION_PREPARED) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (!durability_owned_by(durability, owner)) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = start_transition(record, UCN_I_SERVICE_OPERATION_EXECUTING,
                              NULL, durability);
    ucn_i_service_p_unlock(owner);
    return result;
}

ucn_result_t ucn_i_service_operation_mark_executor_observed(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_operation_record_t *record;
    if (result != UCN_OK) return result;
    record = ucn_i_service_p_operation(owner, operation);
    if (record == NULL || record->phase != UCN_I_SERVICE_OPERATION_EXECUTING ||
        record->executor_observed != 0U) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->executor_observed = 1U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_prepare_terminal(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    ucn_i_service_operation_phase_t terminal_phase,
    const ucn_i_service_result_t *terminal_result,
    const ucn_i_service_operation_durability_t *durability)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_operation_record_t *record;
    ucn_i_service_result_t canonical;

    if (result != UCN_OK) return result;
    record = ucn_i_service_p_operation(owner, operation);
    if (record == NULL || terminal_phase < UCN_I_SERVICE_OPERATION_COMMITTED_RESULT ||
        terminal_phase > UCN_I_SERVICE_OPERATION_TOMBSTONED) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (!durability_owned_by(durability, owner)) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&canonical, 0, sizeof(canonical));
    if (terminal_phase == UCN_I_SERVICE_OPERATION_COMMITTED_RESULT) {
        if (record->phase != UCN_I_SERVICE_OPERATION_EXECUTING ||
            record->executor_observed == 0U || terminal_result == NULL ||
            terminal_result->bytes > UCN_I_SERVICE_RESULT_BYTES) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        result_freeze(&canonical, terminal_result);
    } else if (terminal_phase == UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT) {
        if ((record->phase != UCN_I_SERVICE_OPERATION_PREPARED &&
             record->phase != UCN_I_SERVICE_OPERATION_EXECUTING) ||
            record->executor_observed != 0U) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        canonical.application_result = UCN_ERR_CANCELLED;
    } else if (terminal_phase == UCN_I_SERVICE_OPERATION_IN_DOUBT) {
        if (record->phase != UCN_I_SERVICE_OPERATION_EXECUTING) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        canonical.application_result = UCN_ERR_IN_DOUBT;
    } else {
        if (record->phase != UCN_I_SERVICE_OPERATION_COMMITTED_RESULT &&
            record->phase != UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT &&
            record->phase != UCN_I_SERVICE_OPERATION_IN_DOUBT) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        canonical = record->result;
    }
    result = start_transition(record, terminal_phase, &canonical, durability);
    ucn_i_service_p_unlock(owner);
    return result;
}

ucn_result_t ucn_i_service_operation_view(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    ucn_i_service_operation_view_t *view_out)
{
    ucn_result_t result;
    ucn_i_service_operation_record_t *record;
    if (view_out == NULL) return UCN_ERR_ARGUMENT;
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) return result;
    record = ucn_i_service_p_operation(owner, operation);
    if (record == NULL) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    memset(view_out, 0, sizeof(*view_out));
    view_out->key = record->key;
    view_out->result = record->result;
    view_out->phase = record->phase;
    view_out->executor_observed = record->executor_observed;
    view_out->reply_ready = record->reply_ready;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_retire(
    ucn_i_service_owner_t *owner,
    ucn_handle_t operation,
    bool authenticated_result_ack,
    bool retention_elapsed,
    uint64_t retired_operation_floor)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_operation_record_t *record;
    if (result != UCN_OK) return result;
    record = ucn_i_service_p_operation(owner, operation);
    if (record == NULL || record->phase != UCN_I_SERVICE_OPERATION_TOMBSTONED ||
        !authenticated_result_ack || !retention_elapsed ||
        retired_operation_floor < record->key.service.operation_id) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->occupied = 0U;
    record->phase = 0U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}
