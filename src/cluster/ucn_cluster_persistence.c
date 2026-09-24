#include "internal/ucn_cluster.h"

#include "internal/ucn_checked.h"

#include <string.h>

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
    for (index = 0U; index < 8U; ++index) value = (value << 8U) | bytes[index];
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
        bytes[7U - index] =
            (uint8_t)(value >> ((uint32_t)index * 8U));
    }
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

static bool object_zero(const void *object, size_t bytes)
{
    const uint8_t *value = (const uint8_t *)object;
    size_t index;
    for (index = 0U; index < bytes; ++index) {
        if (value[index] != 0U) return false;
    }
    return true;
}

static bool handle_equal(ucn_handle_t left, ucn_handle_t right)
{
    return left.runtime_instance == right.runtime_instance &&
           left.owner_instance == right.owner_instance &&
           left.slot == right.slot && left.generation == right.generation &&
           left.object_kind == right.object_kind &&
           left.reserved_zero == right.reserved_zero;
}

static void encode_epoch(uint8_t *bytes,
                         const ucn_i_cluster_epoch_t *epoch)
{
    write_be32(&bytes[0], epoch->cluster_id);
    write_be32(&bytes[4], epoch->term);
    write_be32(&bytes[8], epoch->head_binding_generation);
    memcpy(&bytes[12], epoch->head_principal, UCN_I_CLUSTER_PRINCIPAL_BYTES);
}

static void decode_epoch(const uint8_t *bytes,
                         ucn_i_cluster_epoch_t *epoch)
{
    epoch->cluster_id = read_be32(&bytes[0]);
    epoch->term = read_be32(&bytes[4]);
    epoch->head_binding_generation = read_be32(&bytes[8]);
    memcpy(epoch->head_principal, &bytes[12], UCN_I_CLUSTER_PRINCIPAL_BYTES);
}

static void encode_member(uint8_t *bytes,
                          const ucn_i_cluster_config_member_t *member)
{
    memcpy(&bytes[0], member->principal, UCN_I_CLUSTER_PRINCIPAL_BYTES);
    write_be32(&bytes[16], member->binding_generation);
    bytes[20] = member->flags;
}

static ucn_result_t decode_member(
    const uint8_t *bytes, ucn_i_cluster_config_member_t *member)
{
    if (bytes[21] != 0U || bytes[22] != 0U || bytes[23] != 0U) {
        return UCN_ERR_MALFORMED;
    }
    memcpy(member->principal, &bytes[0], UCN_I_CLUSTER_PRINCIPAL_BYTES);
    member->binding_generation = read_be32(&bytes[16]);
    member->flags = bytes[20];
    return UCN_OK;
}

static void encode_vote(uint8_t *bytes,
                        const ucn_i_cluster_vote_evidence_t *vote)
{
    if (vote->valid == 0U) return;
    write_be32(&bytes[0], vote->binding_generation);
    write_be32(&bytes[4], vote->session_generation);
    write_be32(&bytes[8], vote->capability_generation);
    write_be32(&bytes[12], vote->vote_id);
    memcpy(&bytes[16], vote->canonical_digest,
           UCN_I_CLUSTER_DIGEST_BYTES);
}

static void decode_vote(const uint8_t *bytes,
                        const ucn_i_cluster_config_view_t *config,
                        uint8_t index,
                        ucn_i_cluster_vote_evidence_t *vote)
{
    if (object_zero(bytes, UCN_I_CLUSTER_VOTE_EVIDENCE_BYTES)) return;
    vote->binding_generation = read_be32(&bytes[0]);
    vote->session_generation = read_be32(&bytes[4]);
    vote->capability_generation = read_be32(&bytes[8]);
    vote->vote_id = read_be32(&bytes[12]);
    memcpy(vote->canonical_digest, &bytes[16], UCN_I_CLUSTER_DIGEST_BYTES);
    vote->valid = 1U;
    vote->voter_index = index;
    if (index < config->member_count) {
        memcpy(vote->principal, config->members[index].principal,
               UCN_I_CLUSTER_PRINCIPAL_BYTES);
    }
}

ucn_result_t ucn_i_cluster_p_record_encode(
    const ucn_i_cluster_state_t *state,
    uint8_t body_out[UCN_I_CLUSTER_RECORD_BYTES])
{
    uint8_t index;
    size_t offset;
    if (!ucn_i_cluster_p_state_valid(state) || body_out == NULL ||
        ucn_i_ranges_overlap(state, sizeof(*state), body_out,
                             UCN_I_CLUSTER_RECORD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(body_out, 0, UCN_I_CLUSTER_RECORD_BYTES);
    write_be32(&body_out[0], UCN_I_CLUSTER_MAGIC);
    write_be16(&body_out[4], UCN_I_CLUSTER_RECORD_SCHEMA);
    body_out[6] = state->phase;
    body_out[7] = state->role;
    encode_epoch(&body_out[8], &state->epoch);
    encode_epoch(&body_out[36], &state->transition.target_epoch);
    write_be32(&body_out[64], state->stable_config.config_id);
    write_be32(&body_out[68], state->stable_config.generation);
    write_be32(&body_out[72], state->transition.target_config.config_id);
    write_be32(&body_out[76], state->transition.target_config.generation);
    write_be64(&body_out[80], state->transition.transaction_id);
    write_be64(&body_out[88], state->transition.absolute_deadline_us);
    write_be64(&body_out[96], state->transaction_high_water);
    write_be32(&body_out[104], state->lineage_generation);
    write_be32(&body_out[108], state->retired_cluster_high_water);
    write_be32(&body_out[112], state->backup_assignment_generation);
    write_be32(&body_out[116], state->backup_coverage_mask);
    if (state->transition.handover_ready_valid != 0U) {
        memcpy(&body_out[120], state->transition.handover_ready.proof_digest,
               UCN_I_CLUSTER_DIGEST_BYTES);
    } else if (state->backup_ready != 0U) {
        memcpy(&body_out[120], state->backup_snapshot_digest,
               UCN_I_CLUSTER_DIGEST_BYTES);
    }
    body_out[136] = state->stable_config.member_count;
    body_out[137] = state->transition.target_config.member_count;
    body_out[138] = state->transition.kind;
    body_out[139] = state->transition.phase;
    body_out[140] = state->joint_valid;
    body_out[141] = state->authority_fenced;
    body_out[142] = state->backup_ready;
    body_out[143] = state->transition.handover_ready_valid;
    offset = UCN_I_CLUSTER_RECORD_HEADER_BYTES;
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        if (index < state->stable_config.member_count) {
            encode_member(&body_out[offset],
                          &state->stable_config.members[index]);
        }
        offset += UCN_I_CLUSTER_CONFIG_MEMBER_BYTES;
    }
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        if (index < state->transition.target_config.member_count) {
            encode_member(&body_out[offset],
                          &state->transition.target_config.members[index]);
        }
        offset += UCN_I_CLUSTER_CONFIG_MEMBER_BYTES;
    }
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        encode_vote(&body_out[offset], &state->transition.old_votes[index]);
        offset += UCN_I_CLUSTER_VOTE_EVIDENCE_BYTES;
    }
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        encode_vote(&body_out[offset], &state->transition.new_votes[index]);
        offset += UCN_I_CLUSTER_VOTE_EVIDENCE_BYTES;
    }
    return UCN_OK;
}

ucn_result_t ucn_i_cluster_p_record_decode(
    const uint8_t *body, size_t body_bytes,
    ucn_i_cluster_state_t *state_out)
{
    ucn_i_cluster_state_t *state = state_out;
    uint8_t index;
    size_t offset;
    ucn_result_t result;
    if (body == NULL || state_out == NULL ||
        body_bytes != UCN_I_CLUSTER_RECORD_BYTES ||
        ucn_i_ranges_overlap(body, body_bytes, state_out,
                             sizeof(*state_out))) return UCN_ERR_ARGUMENT;
    memset(state, 0, sizeof(*state));
    if (read_be32(&body[0]) != UCN_I_CLUSTER_MAGIC ||
        read_be16(&body[4]) != UCN_I_CLUSTER_RECORD_SCHEMA) {
        return UCN_ERR_MALFORMED;
    }
    state->phase = body[6];
    state->role = body[7];
    decode_epoch(&body[8], &state->epoch);
    decode_epoch(&body[36], &state->transition.target_epoch);
    state->stable_config.config_id = read_be32(&body[64]);
    state->stable_config.generation = read_be32(&body[68]);
    state->transition.target_config.config_id = read_be32(&body[72]);
    state->transition.target_config.generation = read_be32(&body[76]);
    state->transition.transaction_id = read_be64(&body[80]);
    state->transition.absolute_deadline_us = read_be64(&body[88]);
    state->transaction_high_water = read_be64(&body[96]);
    state->lineage_generation = read_be32(&body[104]);
    state->retired_cluster_high_water = read_be32(&body[108]);
    state->backup_assignment_generation = read_be32(&body[112]);
    state->backup_coverage_mask = read_be32(&body[116]);
    state->stable_config.member_count = body[136];
    state->transition.target_config.member_count = body[137];
    state->transition.kind = body[138];
    state->transition.phase = body[139];
    state->joint_valid = body[140];
    state->authority_fenced = body[141];
    state->backup_ready = body[142];
    state->transition.handover_ready_valid = body[143];
    if (state->transition.handover_ready_valid != 0U) {
        memcpy(state->transition.handover_ready.proof_digest, &body[120],
               UCN_I_CLUSTER_DIGEST_BYTES);
        state->transition.handover_ready.transaction_id =
            state->transition.transaction_id;
        state->transition.handover_ready.target_cluster_id =
            state->transition.target_epoch.cluster_id;
        state->transition.handover_ready.target_term =
            state->transition.target_epoch.term;
        state->transition.handover_ready.target_binding_generation =
            state->transition.target_epoch.head_binding_generation;
        state->transition.handover_ready.target_config_id =
            state->transition.target_config.config_id;
        state->transition.handover_ready.target_config_generation =
            state->transition.target_config.generation;
        memcpy(state->transition.handover_ready.target_principal,
               state->transition.target_epoch.head_principal,
               UCN_I_CLUSTER_PRINCIPAL_BYTES);
    } else if (state->backup_ready != 0U) {
        memcpy(state->backup_snapshot_digest, &body[120],
               UCN_I_CLUSTER_DIGEST_BYTES);
    } else if (!object_zero(&body[120], UCN_I_CLUSTER_DIGEST_BYTES)) {
        return UCN_ERR_MALFORMED;
    }
    offset = UCN_I_CLUSTER_RECORD_HEADER_BYTES;
    if (state->stable_config.member_count >
            UCN_I_CLUSTER_CONFIG_MEMBER_COUNT ||
        state->transition.target_config.member_count >
            UCN_I_CLUSTER_CONFIG_MEMBER_COUNT) return UCN_ERR_MALFORMED;
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        if (index < state->stable_config.member_count) {
            result = decode_member(&body[offset],
                                   &state->stable_config.members[index]);
            if (result != UCN_OK) return result;
        } else if (!object_zero(&body[offset],
                                UCN_I_CLUSTER_CONFIG_MEMBER_BYTES)) {
            return UCN_ERR_MALFORMED;
        }
        offset += UCN_I_CLUSTER_CONFIG_MEMBER_BYTES;
    }
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        if (index < state->transition.target_config.member_count) {
            result = decode_member(
                &body[offset], &state->transition.target_config.members[index]);
            if (result != UCN_OK) return result;
        } else if (!object_zero(&body[offset],
                                UCN_I_CLUSTER_CONFIG_MEMBER_BYTES)) {
            return UCN_ERR_MALFORMED;
        }
        offset += UCN_I_CLUSTER_CONFIG_MEMBER_BYTES;
    }
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        decode_vote(&body[offset], &state->stable_config, index,
                    &state->transition.old_votes[index]);
        if (state->transition.old_votes[index].valid != 0U) {
            state->transition.old_vote_mask |= UINT32_C(1) << index;
        }
        offset += UCN_I_CLUSTER_VOTE_EVIDENCE_BYTES;
    }
    for (index = 0U; index < UCN_I_CLUSTER_CONFIG_MEMBER_COUNT; ++index) {
        decode_vote(&body[offset], &state->transition.target_config, index,
                    &state->transition.new_votes[index]);
        if (state->transition.new_votes[index].valid != 0U) {
            state->transition.new_vote_mask |= UINT32_C(1) << index;
        }
        offset += UCN_I_CLUSTER_VOTE_EVIDENCE_BYTES;
    }
    if (!ucn_i_cluster_p_state_valid(state)) return UCN_ERR_MALFORMED;
    return UCN_OK;
}

static bool durability_valid(const ucn_i_cluster_owner_t *owner,
                             const ucn_i_cluster_durability_t *durability)
{
    return durability != NULL && durability->domain_id != 0U &&
           durability->foundation_transaction_id != 0U &&
           durability->absolute_deadline_us != 0U &&
           durability->persistence_domain_generation != 0U &&
           durability->schema_id == UCN_I_CLUSTER_RECORD_SCHEMA_ID &&
           durability->schema_version == UCN_I_CLUSTER_RECORD_SCHEMA &&
           durability->volatile_continuation.runtime_instance == 0U &&
           durability->volatile_continuation.owner_instance == 0U &&
           durability->volatile_continuation.slot == 0U &&
           durability->volatile_continuation.generation == 0U &&
           durability->volatile_continuation.object_kind == 0U &&
           durability->volatile_continuation.reserved_zero == 0U &&
           owner != NULL;
}

ucn_result_t ucn_i_cluster_p_prepare_requirement(
    ucn_i_cluster_owner_t *owner, uint16_t operation_kind,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        !durability_valid(owner, durability) || requirement_out == NULL ||
        owner->pending_valid != 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability),
                             requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(requirement_out, 0, sizeof(*requirement_out));
    result = ucn_i_cluster_p_record_encode(&owner->pending_state,
                                            requirement_out->body);
    if (result != UCN_OK) return result;
    result = ucn_i_sha256_128(requirement_out->body,
                              UCN_I_CLUSTER_RECORD_BYTES,
                              requirement_out->canonical_body_digest,
                              &owner->hash_workspace);
    if (result != UCN_OK) return result;
    requirement_out->durability = *durability;
    requirement_out->durability.volatile_continuation.runtime_instance =
        owner->runtime_instance;
    requirement_out->durability.volatile_continuation.owner_instance =
        owner->owner_instance;
    requirement_out->durability.volatile_continuation.slot = 0U;
    requirement_out->durability.volatile_continuation.generation =
        (uint16_t)((durability->foundation_transaction_id - 1U) %
                   UINT16_MAX + 1U);
    requirement_out->durability.volatile_continuation.object_kind =
        UCN_OBJECT_KIND_CLUSTER;
    memcpy(requirement_out->expected_body_digest,
           owner->current_body_digest, UCN_I_CLUSTER_DIGEST_BYTES);
    requirement_out->runtime_instance = owner->runtime_instance;
    requirement_out->body_bytes = UCN_I_CLUSTER_RECORD_BYTES;
    requirement_out->caller_owner_instance = owner->owner_instance;
    requirement_out->operation_kind = operation_kind;
    owner->pending_durability = requirement_out->durability;
    owner->pending_operation_kind = operation_kind;
    memcpy(owner->pending_body_digest,
           requirement_out->canonical_body_digest,
           UCN_I_CLUSTER_DIGEST_BYTES);
    owner->pending_valid = 1U;
    owner->persistence_bound = 0U;
    memset(&owner->persistence_handle, 0, sizeof(owner->persistence_handle));
    return UCN_OK;
}

ucn_result_t ucn_i_cluster_bind_persistence(
    ucn_i_cluster_owner_t *owner, ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_CLUSTER_DIGEST_BYTES])
{
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) ||
        published_body_digest == NULL ||
        persistence_handle.runtime_instance != owner->runtime_instance ||
        persistence_handle.owner_instance == 0U ||
        persistence_handle.generation == 0U ||
        persistence_handle.object_kind != UCN_OBJECT_KIND_PERSISTENCE ||
        persistence_handle.reserved_zero != 0U ||
        !bytes_nonzero(published_body_digest,
                       UCN_I_CLUSTER_DIGEST_BYTES) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), published_body_digest,
                             UCN_I_CLUSTER_DIGEST_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->pending_valid == 0U || owner->persistence_bound != 0U) {
        result = UCN_ERR_STATE;
    } else {
        owner->persistence_handle = persistence_handle;
        memcpy(owner->pending_body_digest, published_body_digest,
               UCN_I_CLUSTER_DIGEST_BYTES);
        owner->persistence_bound = 1U;
        result = UCN_OK;
    }
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

static bool proof_matches(const ucn_i_cluster_owner_t *owner,
                          const ucn_i_cluster_proof_t *proof)
{
    return proof != NULL && owner->pending_valid != 0U &&
           owner->persistence_bound != 0U &&
           handle_equal(proof->persistence_handle,
                        owner->persistence_handle) &&
           proof->domain_id == owner->pending_durability.domain_id &&
           proof->foundation_transaction_id ==
               owner->pending_durability.foundation_transaction_id &&
           proof->record_generation ==
               owner->pending_durability.expected_record_generation + 1U &&
           proof->witness_generation == proof->record_generation &&
           proof->runtime_instance == owner->runtime_instance &&
           proof->body_bytes == UCN_I_CLUSTER_RECORD_BYTES &&
           proof->persistence_domain_generation ==
               owner->pending_durability.persistence_domain_generation &&
           proof->persistence_owner_instance ==
               owner->persistence_handle.owner_instance &&
           proof->caller_owner_instance == owner->owner_instance &&
           proof->schema_id == UCN_I_CLUSTER_RECORD_SCHEMA_ID &&
           proof->schema_version == UCN_I_CLUSTER_RECORD_SCHEMA &&
           proof->operation_kind == owner->pending_operation_kind &&
           proof->reserved_zero == 0U &&
           memcmp(proof->body_digest, owner->pending_body_digest,
                  UCN_I_CLUSTER_DIGEST_BYTES) == 0;
}

ucn_result_t ucn_i_cluster_accept_proof(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_proof_t *proof, uint64_t now_us)
{
    uint32_t old_live;
    uint32_t new_live;
    uint16_t operation_kind;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || proof == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), proof,
                             sizeof(*proof))) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (!proof_matches(owner, proof) ||
        now_us >= owner->pending_durability.absolute_deadline_us) {
        result = UCN_ERR_STATE;
        goto done;
    }
    operation_kind = owner->pending_operation_kind;
    owner->state = owner->pending_state;
    memcpy(owner->current_body_digest, owner->pending_body_digest,
           UCN_I_CLUSTER_DIGEST_BYTES);
    memset(&owner->pending_state, 0, sizeof(owner->pending_state));
    memset(&owner->pending_durability, 0,
           sizeof(owner->pending_durability));
    memset(&owner->persistence_handle, 0,
           sizeof(owner->persistence_handle));
    memset(owner->pending_body_digest, 0,
           sizeof(owner->pending_body_digest));
    owner->pending_operation_kind = 0U;
    owner->pending_valid = 0U;
    owner->persistence_bound = 0U;
    if (operation_kind == UCN_I_CLUSTER_PERSIST_CONFIG_COMMIT ||
        operation_kind == UCN_I_CLUSTER_PERSIST_EPOCH_COMMIT ||
        operation_kind == UCN_I_CLUSTER_PERSIST_REKEY_COMMIT ||
        operation_kind == UCN_I_CLUSTER_PERSIST_MERGE_RETIRE) {
        memset(&owner->snapshot_sync, 0,
               sizeof(owner->snapshot_sync));
        memset(owner->directory, 0, sizeof(owner->directory));
        memset(owner->tunnels, 0, sizeof(owner->tunnels));
    }
    result = ucn_i_cluster_p_refresh(owner, now_us, &old_live, &new_live);
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_import(
    ucn_i_cluster_owner_t *owner, const uint8_t *body, size_t body_bytes,
    const ucn_i_cluster_durability_t *durability,
    const uint8_t published_body_digest[UCN_I_CLUSTER_DIGEST_BYTES])
{
    ucn_i_cluster_state_t *decoded;
    ucn_result_t result;
    if (!ucn_i_cluster_p_owner_valid(owner) || body == NULL ||
        !durability_valid(owner, durability) ||
        published_body_digest == NULL ||
        !bytes_nonzero(published_body_digest,
                       UCN_I_CLUSTER_DIGEST_BYTES) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), body, body_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), published_body_digest,
                             UCN_I_CLUSTER_DIGEST_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->pending_valid != 0U) {
        result = UCN_ERR_STATE;
        goto done;
    }
    decoded = &owner->pending_state;
    result = ucn_i_cluster_p_record_decode(body, body_bytes, decoded);
    if (result != UCN_OK) {
        memset(decoded, 0, sizeof(*decoded));
        goto done;
    }
    if (owner->state.epoch.cluster_id != 0U &&
        (decoded->transaction_high_water <
             owner->state.transaction_high_water ||
         (decoded->epoch.cluster_id == owner->state.epoch.cluster_id &&
         decoded->epoch.term < owner->state.epoch.term))) {
        result = UCN_ERR_REPLAY;
        memset(decoded, 0, sizeof(*decoded));
        goto done;
    }
    owner->state = *decoded;
    memset(decoded, 0, sizeof(*decoded));
    memset(&owner->snapshot_sync, 0, sizeof(owner->snapshot_sync));
    memset(owner->directory, 0, sizeof(owner->directory));
    memset(owner->tunnels, 0, sizeof(owner->tunnels));
    memcpy(owner->current_body_digest, published_body_digest,
           UCN_I_CLUSTER_DIGEST_BYTES);
    owner->authority_active = 0U;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}
