#include "../internal/ucn_v6_identity_private.h"

#include "ucn/v6/ucn_v6_config.h"

#include <limits.h>
#include <string.h>

static bool bytes_are_nontrivial(const uint8_t *bytes, size_t length)
{
    size_t index;
    bool any_nonzero = false;
    bool any_not_ff = false;

    if (bytes == NULL || length == 0U) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        any_nonzero = any_nonzero || bytes[index] != 0U;
        any_not_ff = any_not_ff || bytes[index] != UINT8_MAX;
    }
    return any_nonzero && any_not_ff;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
    size_t index;

    if (bytes == NULL) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool address_is_valid(uint32_t address)
{
    return address != 0U && address != UINT32_MAX;
}

static bool address_mode_is_valid(ucn_v6_address_mode_t mode)
{
    return mode == UCN_V6_ADDRESS_STATIC || mode == UCN_V6_ADDRESS_LEASED ||
           mode == UCN_V6_ADDRESS_SELF_PROPOSED;
}

static bool authority_epoch_is_valid(const ucn_v6_authority_epoch_t *epoch)
{
    return epoch != NULL &&
           epoch->realm_id != 0U && epoch->realm_id != UINT32_MAX &&
           ucn_v6_principal_is_valid(&epoch->authority_principal) &&
           epoch->authority_generation != 0U &&
           epoch->authority_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_are_nontrivial(epoch->durable_fence_token,
                                sizeof(epoch->durable_fence_token)) &&
           epoch->lease_sequence != 0U &&
           epoch->lease_sequence <= UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           epoch->lease_duration_us != 0U &&
           bytes_are_nontrivial(epoch->allocation_high_water_digest,
                                sizeof(epoch->allocation_high_water_digest)) &&
           bytes_are_nontrivial(epoch->quorum_config_digest,
                                sizeof(epoch->quorum_config_digest)) &&
           bytes_are_nontrivial(epoch->signer_set_digest,
                                sizeof(epoch->signer_set_digest)) &&
           bytes_are_nontrivial(epoch->threshold_proof_digest,
                                sizeof(epoch->threshold_proof_digest)) &&
           epoch->signer_count != 0U && epoch->quorum_threshold != 0U &&
           epoch->quorum_threshold <= epoch->signer_count;
}

static bool authority_freshness_is_valid(
    const ucn_v6_authority_freshness_t *freshness,
    const ucn_v6_authority_epoch_t *epoch,
    bool require_binding)
{
    bool binding_present;

    if (freshness == NULL || !authority_epoch_is_valid(epoch) ||
        !ucn_v6_principal_is_valid(&freshness->verifier_device_principal) ||
        freshness->challenge_nonce == 0U ||
        freshness->transaction_id == 0U ||
        freshness->transaction_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        freshness->authority_lease_sequence != epoch->lease_sequence ||
        freshness->max_remaining_lease_us == 0U ||
        freshness->max_remaining_lease_us > epoch->lease_duration_us ||
        !bytes_are_nontrivial(freshness->proof_transcript_hash,
                              sizeof(freshness->proof_transcript_hash))) {
        return false;
    }
    binding_present = freshness->binding_generation != 0U;
    if (binding_present != require_binding) {
        return false;
    }
    if (!binding_present) {
        uint8_t zero[16] = {0};
        return memcmp(freshness->binding_lease_id, zero,
                      sizeof(zero)) == 0;
    }
    return freshness->binding_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_are_nontrivial(freshness->binding_lease_id,
                                sizeof(freshness->binding_lease_id));
}

typedef char ucn_v6_identity_storage_size_check[
    sizeof(ucn_v6_identity_authority_t) <=
            UCN_V6_IDENTITY_AUTHORITY_STORAGE_BYTES ? 1 : -1];

static bool authority_verifier_is_valid(
    const ucn_v6_identity_authority_verifier_ops_t *verifier)
{
    return verifier != NULL && verifier->verify_epoch_transition != NULL;
}

static bool authority_storage_is_valid(
    const ucn_v6_identity_authority_t *authority)
{
    return authority != NULL &&
           authority->magic == UCN_V6_IDENTITY_AUTHORITY_MAGIC &&
           authority->schema == UCN_V6_STORAGE_LAYOUT &&
           authority->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           authority_verifier_is_valid(&authority->verifier) &&
           authority->canary == UCN_V6_IDENTITY_AUTHORITY_CANARY;
}

static bool authority_proof_is_valid(
    const ucn_v6_authority_proof_t *proof)
{
    size_t index;
    bool any_nonzero = false;

    if (proof == NULL || proof->length == 0U ||
        proof->length > UCN_V6_AUTHORITY_PROOF_MAX_BYTES) {
        return false;
    }
    for (index = 0U; index < proof->length; ++index) {
        any_nonzero = any_nonzero || proof->bytes[index] != 0U;
    }
    if (!any_nonzero) {
        return false;
    }
    for (; index < UCN_V6_AUTHORITY_PROOF_MAX_BYTES; ++index) {
        if (proof->bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool authority_epoch_identity_equal(
    const ucn_v6_authority_epoch_t *left,
    const ucn_v6_authority_epoch_t *right)
{
    return left->authority_generation == right->authority_generation &&
           left->realm_id == right->realm_id &&
           left->lease_sequence == right->lease_sequence &&
           left->lease_duration_us == right->lease_duration_us &&
           memcmp(left->authority_principal.bytes,
                  right->authority_principal.bytes,
                  sizeof(left->authority_principal.bytes)) == 0 &&
           memcmp(left->durable_fence_token,
                  right->durable_fence_token,
                  sizeof(left->durable_fence_token)) == 0 &&
           memcmp(left->allocation_high_water_digest,
                  right->allocation_high_water_digest,
                  sizeof(left->allocation_high_water_digest)) == 0 &&
           memcmp(left->quorum_config_digest, right->quorum_config_digest,
                  sizeof(left->quorum_config_digest)) == 0 &&
           memcmp(left->signer_set_digest, right->signer_set_digest,
                  sizeof(left->signer_set_digest)) == 0 &&
           memcmp(left->threshold_proof_digest,
                  right->threshold_proof_digest,
                  sizeof(left->threshold_proof_digest)) == 0 &&
           left->signer_count == right->signer_count &&
           left->quorum_threshold == right->quorum_threshold;
}

/* EN: Lease renewal keeps the same logical Authority owner and durable
 * Fence. Proof and allocation digests may advance with the renewed lease.
 * 中文：租约续期保持同一逻辑 Authority Owner 与持久 Fence；证明和分配
 * 摘要可以随新租约推进。 */
static bool authority_epoch_owner_equal(
    const ucn_v6_authority_epoch_t *left,
    const ucn_v6_authority_epoch_t *right)
{
    return left->realm_id == right->realm_id &&
           left->authority_generation == right->authority_generation &&
           memcmp(left->authority_principal.bytes,
                  right->authority_principal.bytes,
                  sizeof(left->authority_principal.bytes)) == 0 &&
           memcmp(left->durable_fence_token,
                  right->durable_fence_token,
                  sizeof(left->durable_fence_token)) == 0;
}

static ucn_v6_result_t serial64_checked_next(
    uint64_t current,
    uint64_t *next)
{
    if (next == NULL || current == 0U ||
        current >= UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *next = current + 1U;
    return UCN_V6_OK;
}

static bool authority_epoch_semantic_equal(
    const ucn_v6_authority_epoch_t *left,
    const ucn_v6_authority_epoch_t *right)
{
    return authority_epoch_identity_equal(left, right);
}

static bool authority_epoch_is_zero(
    const ucn_v6_authority_epoch_t *epoch)
{
    return epoch != NULL && epoch->realm_id == 0U &&
           bytes_are_zero(epoch->authority_principal.bytes,
                          sizeof(epoch->authority_principal.bytes)) &&
           epoch->authority_generation == 0U &&
           bytes_are_zero(epoch->durable_fence_token,
                          sizeof(epoch->durable_fence_token)) &&
           epoch->lease_sequence == 0U && epoch->lease_duration_us == 0U &&
           bytes_are_zero(epoch->allocation_high_water_digest,
                          sizeof(epoch->allocation_high_water_digest)) &&
           bytes_are_zero(epoch->quorum_config_digest,
                          sizeof(epoch->quorum_config_digest)) &&
           bytes_are_zero(epoch->signer_set_digest,
                          sizeof(epoch->signer_set_digest)) &&
           bytes_are_zero(epoch->threshold_proof_digest,
                          sizeof(epoch->threshold_proof_digest)) &&
           epoch->signer_count == 0U && epoch->quorum_threshold == 0U;
}

static bool authority_transition_kind_is_valid(
    ucn_v6_authority_transition_kind_t kind)
{
    return kind == UCN_V6_AUTHORITY_TRANSITION_INITIAL ||
           kind == UCN_V6_AUTHORITY_TRANSITION_FRESHNESS ||
           kind == UCN_V6_AUTHORITY_TRANSITION_RENEWAL ||
           kind == UCN_V6_AUTHORITY_TRANSITION_TRANSFER;
}

static bool authority_transition_request_is_valid(
    const ucn_v6_authority_transition_request_t *request)
{
    uint64_t expected_lease_sequence = 0U;
    uint64_t derived_deadline = 0U;
    uint32_t expected_generation = 0U;

    if (request == NULL ||
        !authority_transition_kind_is_valid(request->kind) ||
        request->realm_id == 0U || request->realm_id == UINT32_MAX ||
        !authority_epoch_is_valid(&request->proposed_epoch) ||
        request->proposed_epoch.realm_id != request->realm_id ||
        !authority_freshness_is_valid(&request->freshness,
                                      &request->proposed_epoch, false) ||
        memcmp(request->freshness.verifier_device_principal.bytes,
               request->proposed_epoch.authority_principal.bytes,
               sizeof(request->freshness.verifier_device_principal.bytes)) !=
            0 ||
        ucn_v6_lease_deadline_build(
            request->challenge_started_local_us,
            request->freshness.max_remaining_lease_us,
            &request->lease_policy, &derived_deadline) != UCN_V6_OK ||
        derived_deadline != request->derived_local_deadline_us) {
        return false;
    }
    if (!request->committed_epoch_valid) {
        return request->kind == UCN_V6_AUTHORITY_TRANSITION_INITIAL &&
               authority_epoch_is_zero(&request->committed_epoch) &&
               request->proposed_epoch.authority_generation == 1U &&
               request->proposed_epoch.lease_sequence == 1U;
    }
    if (!authority_epoch_is_valid(&request->committed_epoch) ||
        request->committed_epoch.realm_id != request->realm_id ||
        request->kind == UCN_V6_AUTHORITY_TRANSITION_INITIAL) {
        return false;
    }
    if (request->kind == UCN_V6_AUTHORITY_TRANSITION_FRESHNESS) {
        return authority_epoch_identity_equal(&request->committed_epoch,
                                               &request->proposed_epoch);
    }
    if (serial64_checked_next(request->committed_epoch.lease_sequence,
                              &expected_lease_sequence) != UCN_V6_OK ||
        request->proposed_epoch.lease_sequence != expected_lease_sequence) {
        return false;
    }
    if (request->kind == UCN_V6_AUTHORITY_TRANSITION_RENEWAL) {
        return authority_epoch_owner_equal(&request->committed_epoch,
                                            &request->proposed_epoch) &&
               !authority_epoch_identity_equal(&request->committed_epoch,
                                                &request->proposed_epoch);
    }
    return !authority_epoch_owner_equal(&request->committed_epoch,
                                         &request->proposed_epoch) &&
           ucn_v6_serial_checked_next(
               request->committed_epoch.authority_generation,
               &expected_generation) == UCN_V6_OK &&
           request->proposed_epoch.authority_generation ==
               expected_generation &&
           memcmp(request->committed_epoch.durable_fence_token,
                  request->proposed_epoch.durable_fence_token,
                  sizeof(request->committed_epoch.durable_fence_token)) != 0;
}

static void canonical_put_u16(
    uint8_t *output,
    size_t *offset,
    uint16_t value)
{
    output[(*offset)++] = (uint8_t)(value >> 8U);
    output[(*offset)++] = (uint8_t)value;
}

static void canonical_put_u32(
    uint8_t *output,
    size_t *offset,
    uint32_t value)
{
    output[(*offset)++] = (uint8_t)(value >> 24U);
    output[(*offset)++] = (uint8_t)(value >> 16U);
    output[(*offset)++] = (uint8_t)(value >> 8U);
    output[(*offset)++] = (uint8_t)value;
}

static void canonical_put_u64(
    uint8_t *output,
    size_t *offset,
    uint64_t value)
{
    unsigned shift;

    for (shift = 56U;; shift -= 8U) {
        output[(*offset)++] = (uint8_t)(value >> shift);
        if (shift == 0U) {
            break;
        }
    }
}

static void canonical_put_bytes(
    uint8_t *output,
    size_t *offset,
    const uint8_t *value,
    size_t value_bytes)
{
    memcpy(output + *offset, value, value_bytes);
    *offset += value_bytes;
}

static void canonical_put_epoch(
    uint8_t *output,
    size_t *offset,
    const ucn_v6_authority_epoch_t *epoch)
{
    canonical_put_u32(output, offset, epoch->realm_id);
    canonical_put_bytes(output, offset, epoch->authority_principal.bytes,
                        sizeof(epoch->authority_principal.bytes));
    canonical_put_u32(output, offset, epoch->authority_generation);
    canonical_put_bytes(output, offset, epoch->durable_fence_token,
                        sizeof(epoch->durable_fence_token));
    canonical_put_u64(output, offset, epoch->lease_sequence);
    canonical_put_u64(output, offset, epoch->lease_duration_us);
    canonical_put_bytes(output, offset, epoch->allocation_high_water_digest,
                        sizeof(epoch->allocation_high_water_digest));
    canonical_put_bytes(output, offset, epoch->quorum_config_digest,
                        sizeof(epoch->quorum_config_digest));
    canonical_put_bytes(output, offset, epoch->signer_set_digest,
                        sizeof(epoch->signer_set_digest));
    canonical_put_bytes(output, offset, epoch->threshold_proof_digest,
                        sizeof(epoch->threshold_proof_digest));
    canonical_put_u16(output, offset, epoch->signer_count);
    canonical_put_u16(output, offset, epoch->quorum_threshold);
}

ucn_v6_result_t ucn_v6_authority_transition_encode_canonical(
    const ucn_v6_authority_transition_request_t *request,
    uint8_t *output,
    size_t output_bytes,
    size_t *written_bytes)
{
    uint8_t encoded[UCN_V6_AUTHORITY_TRANSITION_CANONICAL_BYTES];
    size_t offset = 0U;

    if (output == NULL || written_bytes == NULL ||
        output_bytes < sizeof(encoded) ||
        !authority_transition_request_is_valid(request)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    encoded[offset++] = UCN_V6_PROTOCOL_VERSION;
    encoded[offset++] = UCN_V6_AUTHORITY_TRANSITION_CANONICAL_VERSION;
    encoded[offset++] = (uint8_t)request->kind;
    encoded[offset++] = request->committed_epoch_valid ? 1U : 0U;
    canonical_put_u32(encoded, &offset, request->realm_id);
    canonical_put_epoch(encoded, &offset, &request->committed_epoch);
    canonical_put_epoch(encoded, &offset, &request->proposed_epoch);
    canonical_put_bytes(encoded, &offset,
                        request->freshness.verifier_device_principal.bytes,
                        sizeof(request->freshness.verifier_device_principal.bytes));
    canonical_put_u64(encoded, &offset,
                      request->freshness.challenge_nonce);
    canonical_put_u64(encoded, &offset,
                      request->freshness.transaction_id);
    canonical_put_u64(encoded, &offset,
                      request->freshness.authority_lease_sequence);
    canonical_put_u64(encoded, &offset,
                      request->freshness.max_remaining_lease_us);
    canonical_put_bytes(encoded, &offset,
                        request->freshness.binding_lease_id,
                        sizeof(request->freshness.binding_lease_id));
    canonical_put_u32(encoded, &offset,
                      request->freshness.binding_generation);
    canonical_put_bytes(encoded, &offset,
                        request->freshness.proof_transcript_hash,
                        sizeof(request->freshness.proof_transcript_hash));
    canonical_put_u64(encoded, &offset,
                      request->challenge_started_local_us);
    canonical_put_u32(encoded, &offset,
                      request->lease_policy.local_timer_max_slow_ppm);
    canonical_put_u64(encoded, &offset,
                      request->lease_policy.local_timer_resolution_us);
    canonical_put_u64(
        encoded, &offset,
        request->lease_policy.local_timer_read_uncertainty_us);
    encoded[offset++] =
        request->lease_policy.timer_read_uncertainty_known ? 1U : 0U;
    canonical_put_u64(encoded, &offset,
                      request->lease_policy.local_policy_max_lease_us);
    canonical_put_u64(encoded, &offset,
                      request->derived_local_deadline_us);
    if (offset != sizeof(encoded)) {
        return UCN_V6_ERR_STATE;
    }
    memcpy(output, encoded, sizeof(encoded));
    *written_bytes = sizeof(encoded);
    return UCN_V6_OK;
}

static bool callback_gate_is_valid(const ucn_v6_callback_gate_t *gate)
{
    return gate != NULL && gate->initialized && gate->lock != NULL &&
           gate->unlock != NULL;
}

static bool callback_result_is_declared(ucn_v6_result_t result)
{
    return result <= UCN_V6_OK && result >= UCN_V6_ERR_CANCELLED;
}

static bool callback_scope_is_clean(
    ucn_v6_callback_gate_t *gate,
    const void *owner,
    uint64_t violations_before)
{
    bool clean;

    if (!callback_gate_is_valid(gate) || owner == NULL ||
        violations_before == UINT64_MAX) {
        return false;
    }
    gate->lock(gate->context);
    clean = gate->active && gate->active_owner == owner &&
            gate->violation_count == violations_before &&
            gate->violation_count != UINT64_MAX;
    gate->unlock(gate->context);
    return clean;
}

static ucn_v6_result_t callback_scope_finish(
    ucn_v6_callback_gate_t *gate,
    const void *owner,
    uint64_t violations_before,
    ucn_v6_result_t result)
{
    bool clean;

    if (!callback_gate_is_valid(gate) || owner == NULL ||
        violations_before == UINT64_MAX) {
        return UCN_V6_ERR_STATE;
    }
    gate->lock(gate->context);
    clean = gate->active && gate->active_owner == owner &&
            gate->violation_count == violations_before &&
            gate->violation_count != UINT64_MAX;
    if (gate->active && gate->active_owner == owner) {
        gate->active = false;
        gate->active_owner = NULL;
    }
    gate->unlock(gate->context);
    return clean && callback_result_is_declared(result) ? result :
                                                         UCN_V6_ERR_STATE;
}

static bool callback_enter(ucn_v6_identity_authority_t *authority)
{
    return ucn_v6_callback_gate_try_enter(authority->callback_gate,
                                           authority) == UCN_V6_OK;
}

static ucn_v6_result_t callback_exit(
    ucn_v6_identity_authority_t *authority,
    uint64_t violations_before,
    ucn_v6_result_t result)
{
    return callback_scope_finish(authority->callback_gate, authority,
                                 violations_before, result);
}

static bool authority_can_write(
    const ucn_v6_identity_authority_t *authority,
    uint64_t now_us)
{
    if (!authority_storage_is_valid(authority) || !authority->epoch_valid ||
        authority->faulted ||
        !callback_gate_is_valid(authority->callback_gate)) {
        return false;
    }
    if (ucn_v6_callback_gate_is_active(authority->callback_gate)) {
        /* EN: Record a valid owner-API re-entry attempt so the outer trusted
         * callback cannot hide it by discarding this inner error.
         * 中文：记录有效 Owner API 的重入尝试，避免外层可信回调通过丢弃
         * 内层错误来隐藏重入。 */
        (void)ucn_v6_callback_gate_try_enter(authority->callback_gate,
                                              authority);
        return false;
    }
    return ucn_v6_lease_deadline_is_live(
        now_us, authority->local_lease_deadline_us);
}

static ucn_v6_result_t verify_authority_transition(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_transition_request_t *request,
    const ucn_v6_authority_proof_t *proof)
{
    ucn_v6_authority_transition_request_t request_copy;
    ucn_v6_authority_proof_t proof_copy;
    uint8_t canonical[UCN_V6_AUTHORITY_TRANSITION_CANONICAL_BYTES];
    size_t canonical_bytes = 0U;
    uint64_t violations_before;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    if (!authority_storage_is_valid(authority) || request == NULL ||
        !authority_proof_is_valid(proof)) {
        return UCN_V6_ERR_SECURITY;
    }
    request_copy = *request;
    proof_copy = *proof;
    if (ucn_v6_authority_transition_encode_canonical(
            &request_copy, canonical, sizeof(canonical),
            &canonical_bytes) != UCN_V6_OK ||
        canonical_bytes != sizeof(canonical)) {
        return UCN_V6_ERR_STATE;
    }
    violations_before = ucn_v6_callback_gate_violation_count(
        authority->callback_gate);
    if (violations_before == UINT64_MAX ||
        ucn_v6_callback_gate_try_enter(authority->callback_gate, authority) !=
            UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    result = authority->verifier.verify_epoch_transition(
        authority->verifier.context, &request_copy, canonical,
        canonical_bytes, &proof_copy);
    leave_result = callback_scope_finish(
        authority->callback_gate, authority, violations_before, result);
    if (leave_result == UCN_V6_ERR_STATE && result == UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    return leave_result == UCN_V6_OK ? UCN_V6_OK : UCN_V6_ERR_SECURITY;
}

static ucn_v6_binding_slot_t *find_binding_slot(
    ucn_v6_identity_authority_t *authority,
    uint32_t address,
    bool allow_empty)
{
    size_t index;
    ucn_v6_binding_slot_t *empty = NULL;

    for (index = 0U; index < UCN_V6_MAX_BINDING_SLOTS; ++index) {
        ucn_v6_binding_slot_t *slot = &authority->bindings[index];
        if (slot->occupied && slot->node_address == address) {
            return slot;
        }
        if (!slot->occupied && empty == NULL) {
            empty = slot;
        }
    }
    return allow_empty ? empty : NULL;
}

static bool identity_store_is_valid(const ucn_v6_identity_store_ops_t *store)
{
    return store != NULL && store->load_witness != NULL &&
           store->reserve_witness != NULL && store->load != NULL &&
           store->submit != NULL;
}

static void witness_make_factory(
    ucn_v6_durable_generation_witness_t *witness)
{
    memset(witness, 0, sizeof(*witness));
    witness->magic = UCN_V6_DURABLE_WITNESS_MAGIC;
    witness->schema = UCN_V6_DURABLE_WITNESS_SCHEMA;
    witness->domain = (uint8_t)UCN_V6_DURABLE_WITNESS_IDENTITY;
}

static void snapshot_make_factory(
    ucn_v6_identity_snapshot_t *snapshot,
    uint32_t realm_id,
    uint64_t record_generation)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = UCN_V6_IDENTITY_SNAPSHOT_MAGIC;
    snapshot->schema = UCN_V6_IDENTITY_SNAPSHOT_SCHEMA;
    snapshot->realm_id = realm_id;
    snapshot->record_generation = record_generation;
}

static bool witness_is_valid(
    const ucn_v6_durable_generation_witness_t *witness,
    bool allow_factory)
{
    uint8_t zero[sizeof(witness->reserved)] = {0};
    bool commissioned;

    if (witness == NULL || witness->magic != UCN_V6_DURABLE_WITNESS_MAGIC ||
        witness->schema != UCN_V6_DURABLE_WITNESS_SCHEMA ||
        witness->domain != (uint8_t)UCN_V6_DURABLE_WITNESS_IDENTITY ||
        memcmp(witness->reserved, zero, sizeof(zero)) != 0 ||
        (witness->flags &
         (uint16_t)~UCN_V6_DURABLE_WITNESS_COMMISSIONED) != 0U ||
        witness->witness_generation > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        witness->committed_generation > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        witness->pending_generation > UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return false;
    }
    commissioned = (witness->flags &
                    UCN_V6_DURABLE_WITNESS_COMMISSIONED) != 0U;
    if (!commissioned) {
        return allow_factory && witness->witness_generation == 0U &&
               witness->committed_generation == 0U &&
               witness->pending_generation == 0U;
    }
    if (witness->witness_generation == 0U) {
        return false;
    }
    return witness->pending_generation == 0U ||
           (witness->pending_generation ==
                witness->committed_generation + 1U &&
            witness->pending_generation != 0U);
}

static bool witness_equal(
    const ucn_v6_durable_generation_witness_t *left,
    const ucn_v6_durable_generation_witness_t *right)
{
    return left->magic == right->magic && left->schema == right->schema &&
           left->flags == right->flags && left->domain == right->domain &&
           memcmp(left->reserved, right->reserved,
                  sizeof(left->reserved)) == 0 &&
           left->witness_generation == right->witness_generation &&
           left->committed_generation == right->committed_generation &&
           left->pending_generation == right->pending_generation;
}

static ucn_v6_result_t reserve_witness_transition(
    const ucn_v6_identity_store_ops_t *store,
    const ucn_v6_durable_generation_witness_t *previous,
    ucn_v6_durable_generation_witness_t *candidate,
    ucn_v6_callback_gate_t *callback_gate,
    const void *callback_owner,
    uint64_t violations_before)
{
    ucn_v6_durable_generation_witness_t loaded;
    ucn_v6_result_t result;

    if (!witness_is_valid(previous, true) ||
        previous->witness_generation >= UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_STATE;
    }
    candidate->witness_generation = previous->witness_generation + 1U;
    if (!witness_is_valid(candidate, false)) {
        return UCN_V6_ERR_STATE;
    }
    result = store->reserve_witness(store->context, candidate);
    if (!callback_scope_is_clean(callback_gate, callback_owner,
                                 violations_before)) {
        return UCN_V6_ERR_STATE;
    }
    if (result != UCN_V6_OK) {
        return result;
    }
    memset(&loaded, 0, sizeof(loaded));
    result = store->load_witness(store->context, &loaded);
    if (!callback_scope_is_clean(callback_gate, callback_owner,
                                 violations_before)) {
        return UCN_V6_ERR_STATE;
    }
    if (result != UCN_V6_OK || !witness_equal(&loaded, candidate)) {
        return result == UCN_V6_OK ? UCN_V6_ERR_STATE : result;
    }
    return UCN_V6_OK;
}

static bool binding_certificate_is_valid(
    const ucn_v6_binding_certificate_t *certificate,
    uint32_t realm_id)
{
    return certificate != NULL &&
           ucn_v6_principal_is_valid(&certificate->device_principal) &&
           ucn_v6_principal_is_valid(&certificate->authority_principal) &&
           ucn_v6_binding_key_is_valid(&certificate->binding) &&
           certificate->binding.realm_id == realm_id &&
           certificate->authority_generation != 0U &&
           certificate->authority_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_are_nontrivial(certificate->lease_id,
                                sizeof(certificate->lease_id)) &&
           certificate->lease_duration_us != 0U &&
           certificate->authority_lease_sequence != 0U &&
           certificate->authority_lease_sequence <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           address_mode_is_valid(certificate->mode);
}

static bool binding_certificate_semantic_equal(
    const ucn_v6_binding_certificate_t *left,
    const ucn_v6_binding_certificate_t *right)
{
    return memcmp(left->device_principal.bytes,
                  right->device_principal.bytes,
                  sizeof(left->device_principal.bytes)) == 0 &&
           memcmp(left->authority_principal.bytes,
                  right->authority_principal.bytes,
                  sizeof(left->authority_principal.bytes)) == 0 &&
           ucn_v6_binding_key_equal(&left->binding, &right->binding) &&
           left->authority_generation == right->authority_generation &&
           memcmp(left->lease_id, right->lease_id,
                  sizeof(left->lease_id)) == 0 &&
           left->lease_duration_us == right->lease_duration_us &&
           left->authority_lease_sequence ==
               right->authority_lease_sequence &&
           left->mode == right->mode;
}

static bool binding_slot_semantic_equal(
    const ucn_v6_binding_slot_t *left,
    const ucn_v6_binding_slot_t *right)
{
    return left->occupied == right->occupied &&
           left->active == right->active &&
           left->node_address == right->node_address &&
           left->generation_high_water == right->generation_high_water &&
           binding_certificate_semantic_equal(&left->certificate,
                                              &right->certificate);
}

static bool group_allocator_is_valid(
    const ucn_v6_group_allocator_t *groups)
{
    size_t left;
    size_t right;

    if (groups == NULL || groups->dynamic_group_id_high_water >
                              UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return false;
    }
    for (left = 0U; left < UCN_V6_MAX_ACTIVE_GROUPS; ++left) {
        const uint32_t group_id = groups->active_group_ids[left];
        if (group_id == 0U) {
            continue;
        }
        if (!address_is_valid(group_id) ||
            group_id > groups->dynamic_group_id_high_water) {
            return false;
        }
        for (right = left + 1U; right < UCN_V6_MAX_ACTIVE_GROUPS; ++right) {
            if (groups->active_group_ids[right] == group_id) {
                return false;
            }
        }
    }
    return true;
}

static bool identity_snapshot_semantic_equal(
    const ucn_v6_identity_snapshot_t *left,
    const ucn_v6_identity_snapshot_t *right)
{
    size_t index;

    if (left->magic != right->magic || left->schema != right->schema ||
        left->reserved != right->reserved ||
        left->record_generation != right->record_generation ||
        left->realm_id != right->realm_id ||
        left->epoch_valid != right->epoch_valid ||
        !authority_epoch_semantic_equal(&left->epoch, &right->epoch) ||
        left->groups.dynamic_group_id_high_water !=
            right->groups.dynamic_group_id_high_water) {
        return false;
    }
    for (index = 0U; index < UCN_V6_MAX_BINDING_SLOTS; ++index) {
        if (!binding_slot_semantic_equal(&left->bindings[index],
                                         &right->bindings[index])) {
            return false;
        }
    }
    for (index = 0U; index < UCN_V6_MAX_ACTIVE_GROUPS; ++index) {
        if (left->groups.active_group_ids[index] !=
            right->groups.active_group_ids[index]) {
            return false;
        }
    }
    return true;
}

static bool identity_snapshot_is_valid(
    const ucn_v6_identity_snapshot_t *snapshot,
    uint32_t realm_id,
    bool allow_factory_empty)
{
    size_t left;
    size_t right;
    ucn_v6_identity_snapshot_t empty;

    if (snapshot == NULL || snapshot->magic != UCN_V6_IDENTITY_SNAPSHOT_MAGIC ||
        snapshot->schema != UCN_V6_IDENTITY_SNAPSHOT_SCHEMA ||
        snapshot->reserved != 0U || snapshot->realm_id != realm_id ||
        snapshot->record_generation > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        !group_allocator_is_valid(&snapshot->groups)) {
        return false;
    }
    if (snapshot->record_generation == 0U) {
        if (!allow_factory_empty) {
            return false;
        }
        memset(&empty, 0, sizeof(empty));
        empty.magic = UCN_V6_IDENTITY_SNAPSHOT_MAGIC;
        empty.schema = UCN_V6_IDENTITY_SNAPSHOT_SCHEMA;
        empty.realm_id = realm_id;
        return identity_snapshot_semantic_equal(snapshot, &empty);
    }
    if (snapshot->epoch_valid) {
        if (!authority_epoch_is_valid(&snapshot->epoch) ||
            snapshot->epoch.realm_id != realm_id) {
            return false;
        }
    } else {
        ucn_v6_authority_epoch_t empty_epoch;
        memset(&empty_epoch, 0, sizeof(empty_epoch));
        if (!authority_epoch_semantic_equal(&snapshot->epoch,
                                            &empty_epoch)) {
            return false;
        }
    }
    for (left = 0U; left < UCN_V6_MAX_BINDING_SLOTS; ++left) {
        const ucn_v6_binding_slot_t *slot = &snapshot->bindings[left];
        ucn_v6_binding_slot_t empty_slot;

        memset(&empty_slot, 0, sizeof(empty_slot));
        if (!slot->occupied) {
            if (!binding_slot_semantic_equal(slot, &empty_slot)) {
                return false;
            }
            continue;
        }
        if (!address_is_valid(slot->node_address) ||
            slot->generation_high_water == 0U ||
            slot->generation_high_water > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
            !binding_certificate_is_valid(&slot->certificate, realm_id) ||
            slot->certificate.binding.node_address != slot->node_address ||
            slot->certificate.binding.binding_generation !=
                slot->generation_high_water ||
            (slot->active && !snapshot->epoch_valid) ||
            (snapshot->epoch_valid &&
             (slot->certificate.authority_generation >
                  snapshot->epoch.authority_generation ||
              slot->certificate.authority_lease_sequence >
                  snapshot->epoch.lease_sequence ||
              (slot->certificate.authority_generation ==
                   snapshot->epoch.authority_generation &&
               memcmp(slot->certificate.authority_principal.bytes,
                      snapshot->epoch.authority_principal.bytes,
                      sizeof(slot->certificate.authority_principal.bytes)) !=
                   0)))) {
            return false;
        }
        for (right = left + 1U; right < UCN_V6_MAX_BINDING_SLOTS; ++right) {
            if (snapshot->bindings[right].occupied &&
                snapshot->bindings[right].node_address == slot->node_address) {
                return false;
            }
        }
    }
    return true;
}

static void snapshot_from_authority(
    const ucn_v6_identity_authority_t *authority,
    ucn_v6_identity_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = UCN_V6_IDENTITY_SNAPSHOT_MAGIC;
    snapshot->schema = UCN_V6_IDENTITY_SNAPSHOT_SCHEMA;
    snapshot->record_generation = authority->record_generation;
    snapshot->realm_id = authority->realm_id;
    snapshot->epoch_valid = authority->epoch_valid;
    snapshot->epoch = authority->epoch;
    memcpy(snapshot->bindings, authority->bindings,
           sizeof(snapshot->bindings));
    snapshot->groups = authority->groups;
}

static void authority_apply_snapshot(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_identity_snapshot_t *snapshot)
{
    authority->record_generation = snapshot->record_generation;
    authority->realm_id = snapshot->realm_id;
    authority->epoch_valid = snapshot->epoch_valid;
    authority->epoch = snapshot->epoch;
    memcpy(authority->bindings, snapshot->bindings,
           sizeof(authority->bindings));
    authority->groups = snapshot->groups;
}

static ucn_v6_result_t persist_snapshot(
    ucn_v6_identity_authority_t *authority,
    ucn_v6_identity_snapshot_t *candidate)
{
    ucn_v6_identity_snapshot_t loaded;
    ucn_v6_durable_generation_witness_t witness = {0};
    ucn_v6_durable_generation_witness_t pending = {0};
    ucn_v6_durable_generation_witness_t committed = {0};
    uint64_t violations_before;
    ucn_v6_result_t result;

    if (authority->record_generation >=
        UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        authority->faulted = true;
        return UCN_V6_ERR_EXHAUSTED;
    }
    candidate->record_generation = authority->record_generation + 1U;
    violations_before = ucn_v6_callback_gate_violation_count(
        authority->callback_gate);
    if (!identity_snapshot_is_valid(candidate, authority->realm_id, false) ||
        violations_before == UINT64_MAX || !callback_enter(authority)) {
        return UCN_V6_ERR_STATE;
    }

    memset(&witness, 0, sizeof(witness));
    result = authority->store.load_witness(authority->store.context,
                                            &witness);
    if (!callback_scope_is_clean(authority->callback_gate, authority,
                                 violations_before)) {
        result = UCN_V6_ERR_STATE;
    }
    if (result == UCN_V6_OK) {
        if (!witness_is_valid(&witness, false) ||
            witness.committed_generation != authority->record_generation ||
            witness.pending_generation != 0U) {
            result = UCN_V6_ERR_STATE;
        }
    }
    if (result == UCN_V6_OK) {
        pending = witness;
        pending.flags |= UCN_V6_DURABLE_WITNESS_COMMISSIONED;
        pending.pending_generation = candidate->record_generation;
        result = reserve_witness_transition(&authority->store, &witness,
                                            &pending,
                                            authority->callback_gate,
                                            authority, violations_before);
    }
    if (result == UCN_V6_OK) {
        result = authority->store.submit(authority->store.context, candidate);
        if (!callback_scope_is_clean(authority->callback_gate, authority,
                                     violations_before)) {
            result = UCN_V6_ERR_STATE;
        }
    }
    if (result == UCN_V6_OK) {
        memset(&loaded, 0, sizeof(loaded));
        result = authority->store.load(authority->store.context, &loaded);
        if (!callback_scope_is_clean(authority->callback_gate, authority,
                                     violations_before)) {
            result = UCN_V6_ERR_STATE;
        }
    }
    if (result == UCN_V6_OK &&
        (!identity_snapshot_is_valid(&loaded, authority->realm_id, false) ||
         !identity_snapshot_semantic_equal(&loaded, candidate))) {
        result = UCN_V6_ERR_STATE;
    }
    if (result == UCN_V6_OK) {
        committed = pending;
        committed.committed_generation = candidate->record_generation;
        committed.pending_generation = 0U;
        result = reserve_witness_transition(&authority->store, &pending,
                                            &committed,
                                            authority->callback_gate,
                                            authority, violations_before);
    }
    result = callback_exit(authority, violations_before, result);
    if (result != UCN_V6_OK) {
        authority->faulted = true;
        return result;
    }
    authority_apply_snapshot(authority, &loaded);
    return UCN_V6_OK;
}

bool ucn_v6_principal_is_valid(const ucn_v6_principal_t *principal)
{
    return principal != NULL &&
           bytes_are_nontrivial(principal->bytes, sizeof(principal->bytes));
}

bool ucn_v6_binding_key_is_valid(const ucn_v6_binding_key_t *binding)
{
    return binding != NULL && binding->realm_id != 0U &&
           binding->realm_id != UINT32_MAX &&
           address_is_valid(binding->node_address) &&
           binding->binding_generation != 0U &&
           binding->binding_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

bool ucn_v6_binding_key_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right)
{
    return left != NULL && right != NULL &&
           left->realm_id == right->realm_id &&
           left->node_address == right->node_address &&
           left->binding_generation == right->binding_generation;
}

ucn_v6_result_t ucn_v6_serial_checked_next(uint32_t current, uint32_t *next)
{
    if (next == NULL || current > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (current == UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *next = current + 1U;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_lease_deadline_build(
    uint64_t challenge_started_local_us,
    uint64_t max_remaining_lease_us,
    const ucn_v6_lease_verifier_policy_t *policy,
    uint64_t *local_deadline_us)
{
    uint64_t ppm_product;
    uint64_t clock_margin;
    uint64_t read_margin;
    uint64_t quantization_margin;
    uint64_t safe_duration;
    uint64_t effective_duration;

    if (policy == NULL || local_deadline_us == NULL ||
        max_remaining_lease_us == 0U ||
        policy->local_timer_resolution_us == 0U ||
        !policy->timer_read_uncertainty_known ||
        policy->local_policy_max_lease_us == 0U ||
        policy->local_timer_max_slow_ppm > UINT32_C(1000000)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (policy->local_timer_max_slow_ppm != 0U &&
        max_remaining_lease_us >
            UINT64_MAX / (uint64_t)policy->local_timer_max_slow_ppm) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    ppm_product = max_remaining_lease_us *
                  (uint64_t)policy->local_timer_max_slow_ppm;
    clock_margin = ppm_product / UINT64_C(1000000);
    if (ppm_product % UINT64_C(1000000) != 0U) {
        ++clock_margin;
    }
    if (policy->local_timer_read_uncertainty_us > UINT64_MAX / 2U) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    read_margin = policy->local_timer_read_uncertainty_us * 2U;
    if (policy->local_timer_resolution_us > UINT64_MAX - read_margin) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    quantization_margin = policy->local_timer_resolution_us + read_margin;
    if (clock_margin >= max_remaining_lease_us ||
        quantization_margin >= max_remaining_lease_us - clock_margin) {
        return UCN_V6_ERR_TIMEOUT;
    }
    safe_duration = max_remaining_lease_us - clock_margin -
                    quantization_margin;
    effective_duration = safe_duration < policy->local_policy_max_lease_us ?
                             safe_duration :
                             policy->local_policy_max_lease_us;
    if (effective_duration == 0U ||
        challenge_started_local_us > UINT64_MAX - effective_duration) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *local_deadline_us = challenge_started_local_us + effective_duration;
    if (*local_deadline_us == 0U) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    return UCN_V6_OK;
}

bool ucn_v6_lease_deadline_is_live(uint64_t now_us, uint64_t deadline_us)
{
    return deadline_us != 0U && now_us < deadline_us;
}

ucn_v6_result_t ucn_v6_callback_gate_init(
    ucn_v6_callback_gate_t *gate,
    void *context,
    void (*lock)(void *context),
    void (*unlock)(void *context))
{
    ucn_v6_callback_gate_t initialized;

    if (gate == NULL || lock == NULL || unlock == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    /* Padding has no semantic meaning and may be indeterminate in a valid C
     * object. Check every public field instead of comparing the representation.
     * padding 没有语义且在合法 C 对象中可能是不确定值；因此逐字段检查，
     * 不能比较整个对象表示。 */
    if (gate->context != NULL || gate->lock != NULL ||
        gate->unlock != NULL || gate->active_owner != NULL ||
        gate->violation_count != 0U || gate->initialized || gate->active) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.context = context;
    initialized.lock = lock;
    initialized.unlock = unlock;
    initialized.initialized = true;
    *gate = initialized;
    return UCN_V6_OK;
}

bool ucn_v6_callback_gate_is_active(ucn_v6_callback_gate_t *gate)
{
    bool active;

    if (!callback_gate_is_valid(gate)) {
        return true;
    }
    gate->lock(gate->context);
    active = gate->active;
    gate->unlock(gate->context);
    return active;
}

uint64_t ucn_v6_callback_gate_violation_count(
    ucn_v6_callback_gate_t *gate)
{
    uint64_t count;

    if (!callback_gate_is_valid(gate)) {
        return UINT64_MAX;
    }
    gate->lock(gate->context);
    count = gate->violation_count;
    gate->unlock(gate->context);
    return count;
}

ucn_v6_result_t ucn_v6_callback_gate_try_enter(
    ucn_v6_callback_gate_t *gate,
    const void *owner)
{
    if (!callback_gate_is_valid(gate) || owner == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    gate->lock(gate->context);
    if (gate->active) {
        if (gate->violation_count != UINT64_MAX) {
            ++gate->violation_count;
        }
        gate->unlock(gate->context);
        return UCN_V6_ERR_STATE;
    }
    gate->active = true;
    gate->active_owner = owner;
    gate->unlock(gate->context);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_callback_gate_leave(
    ucn_v6_callback_gate_t *gate,
    const void *owner)
{
    if (!callback_gate_is_valid(gate) || owner == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    gate->lock(gate->context);
    if (!gate->active || gate->active_owner != owner) {
        gate->unlock(gate->context);
        return UCN_V6_ERR_STATE;
    }
    gate->active = false;
    gate->active_owner = NULL;
    gate->unlock(gate->context);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    uint32_t realm_id,
    const ucn_v6_identity_authority_verifier_ops_t *verifier,
    const ucn_v6_identity_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_identity_authority_t **authority_out)
{
    ucn_v6_identity_authority_t initialized;
    ucn_v6_identity_snapshot_t loaded = {0};
    ucn_v6_durable_generation_witness_t witness = {0};
    ucn_v6_durable_generation_witness_t pending = {0};
    ucn_v6_durable_generation_witness_t committed = {0};
    ucn_v6_result_t witness_result;
    ucn_v6_result_t load_result;
    ucn_v6_result_t result;
    uint64_t violations_before;

    if (authority_out == NULL || realm_id == 0U || realm_id == UINT32_MAX ||
        !authority_verifier_is_valid(verifier) ||
        !identity_store_is_valid(store) ||
        !callback_gate_is_valid(callback_gate)) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(
        (const ucn_v6_feature_manifest_t *)manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(initialized),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    violations_before = ucn_v6_callback_gate_violation_count(callback_gate);
    if (violations_before == UINT64_MAX ||
        ucn_v6_callback_gate_try_enter(callback_gate, storage) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    memset(&loaded, 0, sizeof(loaded));
    memset(&witness, 0, sizeof(witness));
    witness_result = store->load_witness(store->context, &witness);
    if (!callback_scope_is_clean(callback_gate, storage,
                                 violations_before)) {
        result = UCN_V6_ERR_STATE;
        goto callback_done;
    }
    load_result = store->load(store->context, &loaded);
    if (!callback_scope_is_clean(callback_gate, storage,
                                 violations_before)) {
        result = UCN_V6_ERR_STATE;
        goto callback_done;
    }
    if (witness_result == UCN_V6_ERR_NOT_FOUND &&
        load_result == UCN_V6_ERR_NOT_FOUND) {
        witness_make_factory(&witness);
        snapshot_make_factory(&loaded, realm_id, 1U);
        pending = witness;
        pending.flags = UCN_V6_DURABLE_WITNESS_COMMISSIONED;
        pending.pending_generation = 1U;
        result = reserve_witness_transition(
            store, &witness, &pending, callback_gate, storage,
            violations_before);
        if (result == UCN_V6_OK) {
            result = store->submit(store->context, &loaded);
            if (!callback_scope_is_clean(callback_gate, storage,
                                         violations_before)) {
                result = UCN_V6_ERR_STATE;
            }
        }
        if (result == UCN_V6_OK) {
            memset(&loaded, 0, sizeof(loaded));
            result = store->load(store->context, &loaded);
            if (!callback_scope_is_clean(callback_gate, storage,
                                         violations_before)) {
                result = UCN_V6_ERR_STATE;
            }
        }
        if (result == UCN_V6_OK &&
            (!identity_snapshot_is_valid(&loaded, realm_id, false) ||
             loaded.record_generation != 1U)) {
            result = UCN_V6_ERR_STATE;
        }
        if (result == UCN_V6_OK) {
            committed = pending;
            committed.committed_generation = 1U;
            committed.pending_generation = 0U;
            result = reserve_witness_transition(
                store, &pending, &committed, callback_gate, storage,
                violations_before);
        }
        if (result != UCN_V6_OK) {
            goto callback_done;
        }
        witness = committed;
    } else if (witness_result != UCN_V6_OK ||
               !witness_is_valid(&witness, false)) {
        result = UCN_V6_ERR_STATE;
        goto callback_done;
    } else if (witness.pending_generation != 0U) {
        if (load_result == UCN_V6_OK &&
            identity_snapshot_is_valid(&loaded, realm_id, false) &&
            loaded.record_generation == witness.pending_generation) {
            committed = witness;
            committed.committed_generation = witness.pending_generation;
            committed.pending_generation = 0U;
            result = reserve_witness_transition(
                store, &witness, &committed, callback_gate, storage,
                violations_before);
            if (result != UCN_V6_OK) {
                goto callback_done;
            }
            witness = committed;
        } else if (load_result == UCN_V6_OK &&
                   identity_snapshot_is_valid(&loaded, realm_id, false) &&
                   loaded.record_generation == witness.committed_generation) {
            committed = witness;
            committed.pending_generation = 0U;
            result = reserve_witness_transition(
                store, &witness, &committed, callback_gate, storage,
                violations_before);
            if (result != UCN_V6_OK) {
                goto callback_done;
            }
            witness = committed;
        } else if (witness.committed_generation == 0U &&
                   witness.pending_generation == 1U &&
                   load_result == UCN_V6_ERR_NOT_FOUND) {
            /* EN: The first commissioning snapshot may have torn after its
             * independent pending witness became durable.  Re-submit the one
             * canonical generation-1 factory record; never allocate a new
             * generation or clear the witness before the record reloads.
             * 中文：首次 commissioning 可能在独立 pending witness 落盘后、
             * Snapshot 落盘前撕裂。此时只能重交同一个规范 generation-1
             * 工厂记录；在回读成功前不得另分配代际或清除 witness。 */
            snapshot_make_factory(&loaded, realm_id, 1U);
            result = store->submit(store->context, &loaded);
            if (!callback_scope_is_clean(callback_gate, storage,
                                         violations_before)) {
                result = UCN_V6_ERR_STATE;
            }
            if (result == UCN_V6_OK) {
                memset(&loaded, 0, sizeof(loaded));
                result = store->load(store->context, &loaded);
                if (!callback_scope_is_clean(callback_gate, storage,
                                             violations_before)) {
                    result = UCN_V6_ERR_STATE;
                }
            }
            if (result == UCN_V6_OK &&
                (!identity_snapshot_is_valid(&loaded, realm_id, false) ||
                 loaded.record_generation != 1U)) {
                result = UCN_V6_ERR_STATE;
            }
            if (result == UCN_V6_OK) {
                committed = witness;
                committed.committed_generation = 1U;
                committed.pending_generation = 0U;
                result = reserve_witness_transition(
                    store, &witness, &committed, callback_gate, storage,
                    violations_before);
            }
            if (result != UCN_V6_OK) {
                goto callback_done;
            }
            witness = committed;
        } else {
            result = UCN_V6_ERR_STATE;
            goto callback_done;
        }
    } else if (load_result != UCN_V6_OK ||
               !identity_snapshot_is_valid(&loaded, realm_id, false) ||
               loaded.record_generation != witness.committed_generation) {
        result = UCN_V6_ERR_STATE;
        goto callback_done;
    }
    result = UCN_V6_OK;

callback_done:
    result = callback_scope_finish(callback_gate, storage,
                                   violations_before, result);
    if (result != UCN_V6_OK) {
        return result;
    }

    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_V6_IDENTITY_AUTHORITY_MAGIC;
    initialized.schema = UCN_V6_STORAGE_LAYOUT;
    initialized.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    initialized.verifier = *verifier;
    initialized.store = *store;
    initialized.callback_gate = callback_gate;
    initialized.canary = UCN_V6_IDENTITY_AUTHORITY_CANARY;
    authority_apply_snapshot(&initialized, &loaded);
    initialized.local_lease_deadline_us = 0U;
    memcpy(storage, &initialized, sizeof(initialized));
    *authority_out = (ucn_v6_identity_authority_t *)storage;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_install_epoch(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    const ucn_v6_authority_freshness_t *freshness,
    uint64_t challenge_started_local_us,
    const ucn_v6_lease_verifier_policy_t *lease_policy,
    const ucn_v6_authority_proof_t *proof)
{
    ucn_v6_identity_snapshot_t candidate;
    ucn_v6_authority_transition_request_t request;
    uint64_t local_lease_deadline_us = 0U;
    uint64_t expected_lease_sequence = 0U;
    uint32_t expected_generation;
    bool same_epoch = false;
    bool exact_freshness_replay = false;
    ucn_v6_result_t result;

    if (!authority_storage_is_valid(authority) || authority->realm_id == 0U ||
        authority->faulted ||
        !callback_gate_is_valid(authority->callback_gate) ||
        !authority_epoch_is_valid(epoch) || epoch->realm_id != authority->realm_id ||
        !authority_freshness_is_valid(freshness, epoch, false) ||
        memcmp(freshness->verifier_device_principal.bytes,
               epoch->authority_principal.bytes,
               sizeof(epoch->authority_principal.bytes)) != 0 ||
        ucn_v6_lease_deadline_build(challenge_started_local_us,
                                    freshness->max_remaining_lease_us,
                                    lease_policy,
                                    &local_lease_deadline_us) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    if (!authority_proof_is_valid(proof)) {
        return UCN_V6_ERR_SECURITY;
    }

    memset(&request, 0, sizeof(request));
    request.realm_id = authority->realm_id;
    request.committed_epoch_valid = authority->epoch_valid;
    if (authority->epoch_valid) {
        request.committed_epoch = authority->epoch;
    }
    request.proposed_epoch = *epoch;
    request.freshness = *freshness;
    request.challenge_started_local_us = challenge_started_local_us;
    request.lease_policy = *lease_policy;
    request.derived_local_deadline_us = local_lease_deadline_us;

    if (authority->epoch_valid) {
        if (authority_epoch_identity_equal(&authority->epoch, epoch)) {
            same_epoch = true;
            request.kind = UCN_V6_AUTHORITY_TRANSITION_FRESHNESS;
            if (authority->local_lease_deadline_us != 0U &&
                authority->freshness_transaction_id ==
                    freshness->transaction_id &&
                authority->freshness_challenge_nonce ==
                    freshness->challenge_nonce) {
                if (authority->local_lease_deadline_us !=
                        local_lease_deadline_us ||
                    authority->freshness_max_remaining_lease_us !=
                        freshness->max_remaining_lease_us) {
                    return UCN_V6_ERR_REPLAY;
                }
                exact_freshness_replay = true;
            }
            if (!exact_freshness_replay &&
                authority->local_lease_deadline_us != 0U &&
                (freshness->max_remaining_lease_us >
                     authority->freshness_max_remaining_lease_us ||
                 freshness->transaction_id <=
                     authority->freshness_transaction_id ||
                 local_lease_deadline_us >
                     authority->local_lease_deadline_us)) {
                return UCN_V6_ERR_REPLAY;
            }
        } else {
            result = serial64_checked_next(authority->epoch.lease_sequence,
                                           &expected_lease_sequence);
            if (result != UCN_V6_OK) {
                return result;
            }
            if (epoch->lease_sequence != expected_lease_sequence) {
                return UCN_V6_ERR_REPLAY;
            }
            if (authority_epoch_owner_equal(&authority->epoch, epoch)) {
                request.kind = UCN_V6_AUTHORITY_TRANSITION_RENEWAL;
            } else {
                result = ucn_v6_serial_checked_next(
                    authority->epoch.authority_generation,
                    &expected_generation);
                if (result != UCN_V6_OK) {
                    return result;
                }
                if (epoch->authority_generation != expected_generation ||
                    memcmp(authority->epoch.durable_fence_token,
                           epoch->durable_fence_token,
                           sizeof(epoch->durable_fence_token)) == 0) {
                    return UCN_V6_ERR_REPLAY;
                }
                request.kind = UCN_V6_AUTHORITY_TRANSITION_TRANSFER;
            }
        }
    } else if (epoch->authority_generation != 1U ||
               epoch->lease_sequence != 1U) {
        return UCN_V6_ERR_STATE;
    } else {
        request.kind = UCN_V6_AUTHORITY_TRANSITION_INITIAL;
    }

    result = verify_authority_transition(authority, &request, proof);
    if (result != UCN_V6_OK) {
        return result;
    }
    if (same_epoch) {
        if (!exact_freshness_replay) {
            authority->local_lease_deadline_us = local_lease_deadline_us;
            authority->freshness_transaction_id = freshness->transaction_id;
            authority->freshness_challenge_nonce =
                freshness->challenge_nonce;
            authority->freshness_max_remaining_lease_us =
                freshness->max_remaining_lease_us;
        }
        return UCN_V6_OK;
    }

    snapshot_from_authority(authority, &candidate);
    candidate.epoch = *epoch;
    candidate.epoch_valid = true;
    result = persist_snapshot(authority, &candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
    authority->local_lease_deadline_us = local_lease_deadline_us;
    authority->freshness_transaction_id = freshness->transaction_id;
    authority->freshness_challenge_nonce = freshness->challenge_nonce;
    authority->freshness_max_remaining_lease_us =
        freshness->max_remaining_lease_us;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_allocate_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    const ucn_v6_principal_t *device_principal,
    ucn_v6_address_mode_t mode,
    const uint8_t lease_id[16],
    uint64_t lease_duration_us,
    ucn_v6_binding_certificate_t *certificate)
{
    ucn_v6_binding_slot_t *slot;
    size_t slot_index;
    ucn_v6_identity_snapshot_t candidate;
    ucn_v6_binding_slot_t next_slot;
    uint32_t generation;
    ucn_v6_result_t result;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    if (!address_is_valid(node_address) ||
        !ucn_v6_principal_is_valid(device_principal) ||
        !address_mode_is_valid(mode) ||
        !bytes_are_nontrivial(lease_id, 16U) || lease_duration_us == 0U ||
        lease_duration_us > authority->epoch.lease_duration_us ||
        certificate == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_binding_slot(authority, node_address, true);
    if (slot == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    slot_index = (size_t)(slot - authority->bindings);
    if (slot->occupied && slot->active) {
        return UCN_V6_ERR_STATE;
    }
    if (slot->occupied) {
        result = ucn_v6_serial_checked_next(slot->generation_high_water,
                                             &generation);
        if (result != UCN_V6_OK) {
            return result;
        }
    } else {
        generation = 1U;
    }

    memset(&next_slot, 0, sizeof(next_slot));
    next_slot.occupied = true;
    next_slot.active = true;
    next_slot.node_address = node_address;
    next_slot.generation_high_water = generation;
    next_slot.certificate.device_principal = *device_principal;
    next_slot.certificate.authority_principal =
        authority->epoch.authority_principal;
    next_slot.certificate.binding.realm_id = authority->realm_id;
    next_slot.certificate.binding.node_address = node_address;
    next_slot.certificate.binding.binding_generation = generation;
    next_slot.certificate.authority_generation =
        authority->epoch.authority_generation;
    memcpy(next_slot.certificate.lease_id, lease_id,
           sizeof(next_slot.certificate.lease_id));
    next_slot.certificate.lease_duration_us = lease_duration_us;
    next_slot.certificate.authority_lease_sequence =
        authority->epoch.lease_sequence;
    next_slot.certificate.mode = mode;

    snapshot_from_authority(authority, &candidate);
    candidate.bindings[slot_index] = next_slot;
    result = persist_snapshot(authority, &candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
    *certificate = next_slot.certificate;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_reendorse_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    const ucn_v6_binding_key_t *binding,
    const ucn_v6_principal_t *device_principal,
    ucn_v6_address_mode_t mode,
    const uint8_t lease_id[16],
    uint64_t lease_duration_us,
    ucn_v6_binding_certificate_t *certificate)
{
    ucn_v6_binding_slot_t *slot;
    ucn_v6_binding_slot_t next_slot;
    ucn_v6_identity_snapshot_t candidate;
    size_t slot_index;
    bool current_issuer;
    ucn_v6_result_t result;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    if (!ucn_v6_binding_key_is_valid(binding) ||
        binding->realm_id != authority->realm_id ||
        !ucn_v6_principal_is_valid(device_principal) ||
        !address_mode_is_valid(mode) ||
        !bytes_are_nontrivial(lease_id, 16U) || lease_duration_us == 0U ||
        lease_duration_us > authority->epoch.lease_duration_us ||
        certificate == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot = find_binding_slot(authority, binding->node_address, false);
    if (slot == NULL || !slot->active ||
        slot->generation_high_water != binding->binding_generation ||
        !ucn_v6_binding_key_equal(&slot->certificate.binding, binding) ||
        memcmp(slot->certificate.device_principal.bytes,
               device_principal->bytes,
               sizeof(device_principal->bytes)) != 0 ||
        slot->certificate.mode != mode) {
        return UCN_V6_ERR_STATE;
    }
    current_issuer =
        slot->certificate.authority_generation ==
            authority->epoch.authority_generation &&
        slot->certificate.authority_lease_sequence ==
            authority->epoch.lease_sequence &&
        memcmp(slot->certificate.authority_principal.bytes,
               authority->epoch.authority_principal.bytes,
               sizeof(authority->epoch.authority_principal.bytes)) == 0;
    if (current_issuer) {
        if (memcmp(slot->certificate.lease_id, lease_id,
                   sizeof(slot->certificate.lease_id)) == 0 &&
            slot->certificate.lease_duration_us == lease_duration_us) {
            *certificate = slot->certificate;
            return UCN_V6_OK;
        }
        return UCN_V6_ERR_REPLAY;
    }
    if (slot->certificate.authority_generation >
            authority->epoch.authority_generation ||
        (slot->certificate.authority_generation ==
             authority->epoch.authority_generation &&
         slot->certificate.authority_lease_sequence >=
             authority->epoch.lease_sequence)) {
        return UCN_V6_ERR_REPLAY;
    }

    next_slot = *slot;
    next_slot.certificate.authority_principal =
        authority->epoch.authority_principal;
    next_slot.certificate.authority_generation =
        authority->epoch.authority_generation;
    memcpy(next_slot.certificate.lease_id, lease_id,
           sizeof(next_slot.certificate.lease_id));
    next_slot.certificate.lease_duration_us = lease_duration_us;
    next_slot.certificate.authority_lease_sequence =
        authority->epoch.lease_sequence;

    slot_index = (size_t)(slot - authority->bindings);
    snapshot_from_authority(authority, &candidate);
    candidate.bindings[slot_index] = next_slot;
    result = persist_snapshot(authority, &candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
    *certificate = next_slot.certificate;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_retire_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    uint32_t binding_generation)
{
    ucn_v6_binding_slot_t *slot;
    size_t slot_index;
    ucn_v6_identity_snapshot_t candidate;
    ucn_v6_binding_slot_t retired;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    slot = find_binding_slot(authority, node_address, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    slot_index = (size_t)(slot - authority->bindings);
    if (!slot->active || slot->generation_high_water != binding_generation) {
        return UCN_V6_ERR_REPLAY;
    }
    retired = *slot;
    retired.active = false;
    snapshot_from_authority(authority, &candidate);
    candidate.bindings[slot_index] = retired;
    return persist_snapshot(authority, &candidate);
}

ucn_v6_result_t ucn_v6_identity_authority_allocate_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t *group_id)
{
    size_t index;
    ucn_v6_identity_snapshot_t candidate;
    uint32_t next;
    ucn_v6_result_t result;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    if (group_id == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_MAX_ACTIVE_GROUPS; ++index) {
        if (authority->groups.active_group_ids[index] == 0U) {
            break;
        }
    }
    if (index == UCN_V6_MAX_ACTIVE_GROUPS) {
        return UCN_V6_ERR_NO_SPACE;
    }
    result = ucn_v6_serial_checked_next(
        authority->groups.dynamic_group_id_high_water, &next);
    if (result != UCN_V6_OK || !address_is_valid(next)) {
        return result == UCN_V6_OK ? UCN_V6_ERR_EXHAUSTED : result;
    }
    snapshot_from_authority(authority, &candidate);
    candidate.groups.dynamic_group_id_high_water = next;
    candidate.groups.active_group_ids[index] = next;
    result = persist_snapshot(authority, &candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
    *group_id = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_identity_authority_retire_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t group_id)
{
    size_t index;
    ucn_v6_identity_snapshot_t candidate;

    if (!authority_can_write(authority, now_us)) {
        return UCN_V6_ERR_ACCESS;
    }
    if (group_id == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_MAX_ACTIVE_GROUPS; ++index) {
        if (authority->groups.active_group_ids[index] == group_id) {
            snapshot_from_authority(authority, &candidate);
            candidate.groups.active_group_ids[index] = 0U;
            return persist_snapshot(authority, &candidate);
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_identity_authority_copy_view(
    const ucn_v6_identity_authority_t *authority,
    ucn_v6_identity_authority_view_t *view)
{
    ucn_v6_identity_authority_view_t next;
    size_t index;

    if (!authority_storage_is_valid(authority) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&next, 0, sizeof(next));
    next.record_generation = authority->record_generation;
    next.realm_id = authority->realm_id;
    next.epoch = authority->epoch;
    next.local_lease_deadline_us = authority->local_lease_deadline_us;
    next.dynamic_group_id_high_water =
        authority->groups.dynamic_group_id_high_water;
    next.epoch_valid = authority->epoch_valid;
    next.faulted = authority->faulted;
    for (index = 0U; index < UCN_V6_MAX_BINDING_SLOTS; ++index) {
        if (authority->bindings[index].occupied) {
            ++next.occupied_bindings;
        }
    }
    *view = next;
    return UCN_V6_OK;
}
