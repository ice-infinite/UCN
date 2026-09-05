#include "../internal/ucn_v6_message_private.h"

#include "ucn/v6/ucn_v6_config.h"

#include <string.h>

typedef char ucn_v6_operation_allocator_storage_size_check[
    sizeof(ucn_v6_operation_id_allocator_t) <=
            UCN_V6_OPERATION_ID_ALLOCATOR_STORAGE_BYTES ? 1 : -1];
typedef char ucn_v6_operation_journal_storage_size_check[
    sizeof(ucn_v6_operation_journal_t) <=
            UCN_V6_OPERATION_JOURNAL_STORAGE_BYTES ? 1 : -1];

static bool allocator_storage_is_valid(
    const ucn_v6_operation_id_allocator_t *allocator)
{
    return allocator != NULL && allocator->initialized &&
           allocator->magic == UCN_V6_OPERATION_ALLOCATOR_MAGIC &&
           allocator->schema == UCN_V6_STORAGE_LAYOUT &&
           allocator->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           allocator->canary == UCN_V6_OPERATION_ALLOCATOR_CANARY;
}

static bool journal_storage_is_valid(
    const ucn_v6_operation_journal_t *journal)
{
    return journal != NULL && journal->initialized &&
           journal->magic == UCN_V6_OPERATION_JOURNAL_OBJECT_MAGIC &&
           journal->schema == UCN_V6_STORAGE_LAYOUT &&
           journal->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           journal->canary == UCN_V6_OPERATION_JOURNAL_CANARY;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool digest_is_valid(const uint8_t *digest)
{
    size_t index;
    bool any_nonzero = false;
    bool any_not_ff = false;

    if (digest == NULL) {
        return false;
    }
    for (index = 0U; index < UCN_V6_OPERATION_DIGEST_BYTES; ++index) {
        any_nonzero = any_nonzero || digest[index] != 0U;
        any_not_ff = any_not_ff || digest[index] != UINT8_MAX;
    }
    return any_nonzero && any_not_ff;
}

static bool endpoint_id_is_valid(uint16_t endpoint_id)
{
    return endpoint_id != 0U && endpoint_id != UINT16_MAX;
}

static bool execution_contract_is_valid(
    ucn_v6_endpoint_execution_contract_t contract)
{
    return contract == UCN_V6_ENDPOINT_NON_RETRYABLE ||
           contract == UCN_V6_ENDPOINT_IDEMPOTENT_REPLAYABLE ||
           contract == UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE;
}

static bool operation_key_is_valid(const ucn_v6_operation_key_t *key)
{
    return key != NULL &&
           ucn_v6_principal_is_valid(&key->initiator_principal) &&
           key->operation_id != 0U &&
           key->operation_id <= UCN_V6_SERIAL64_ROTATION_THRESHOLD;
}

static bool operation_key_equal(
    const ucn_v6_operation_key_t *left,
    const ucn_v6_operation_key_t *right)
{
    return left->operation_id == right->operation_id &&
           memcmp(left->initiator_principal.bytes,
                  right->initiator_principal.bytes,
                  sizeof(left->initiator_principal.bytes)) == 0;
}

static bool principal_equal(
    const ucn_v6_principal_t *left,
    const ucn_v6_principal_t *right)
{
    return memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool slot_is_canonical_empty(const ucn_v6_operation_slot_t *slot)
{
    return !slot->occupied &&
           bytes_are_zero(slot->key.initiator_principal.bytes,
                          sizeof(slot->key.initiator_principal.bytes)) &&
           slot->key.operation_id == 0U && slot->endpoint_id == 0U &&
           slot->execution_contract == 0 &&
           bytes_are_zero(slot->request_digest,
                          sizeof(slot->request_digest)) &&
           slot->phase == UCN_V6_OPERATION_PHASE_INVALID &&
           slot->result_code == 0 && slot->result_length == 0U &&
           bytes_are_zero(slot->result, sizeof(slot->result)) &&
           bytes_are_zero(slot->result_digest,
                          sizeof(slot->result_digest));
}

static bool slot_is_valid(const ucn_v6_operation_slot_t *slot)
{
    bool terminal;

    if (!slot->occupied) {
        return slot_is_canonical_empty(slot);
    }
    if (!operation_key_is_valid(&slot->key) ||
        !endpoint_id_is_valid(slot->endpoint_id) ||
        slot->execution_contract != UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE ||
        !digest_is_valid(slot->request_digest) ||
        slot->phase < UCN_V6_OPERATION_PHASE_PREPARED ||
        slot->phase > UCN_V6_OPERATION_PHASE_TOMBSTONED ||
        slot->result_length > UCN_V6_OPERATION_RESULT_MAX_BYTES) {
        return false;
    }
    terminal = slot->phase == UCN_V6_OPERATION_PHASE_COMMITTED_RESULT ||
               slot->phase == UCN_V6_OPERATION_PHASE_TOMBSTONED;
    if (!terminal) {
        return slot->result_code == 0 && slot->result_length == 0U &&
               bytes_are_zero(slot->result, sizeof(slot->result)) &&
               bytes_are_zero(slot->result_digest,
                              sizeof(slot->result_digest));
    }
    if (!digest_is_valid(slot->result_digest)) {
        return false;
    }
    if (slot->phase == UCN_V6_OPERATION_PHASE_TOMBSTONED) {
        return slot->result_length == 0U &&
               bytes_are_zero(slot->result, sizeof(slot->result));
    }
    return bytes_are_zero(slot->result + slot->result_length,
                          sizeof(slot->result) - slot->result_length);
}

static bool high_water_is_canonical_empty(
    const ucn_v6_operation_high_water_t *high_water)
{
    return !high_water->occupied &&
           bytes_are_zero(high_water->initiator_principal.bytes,
                          sizeof(high_water->initiator_principal.bytes)) &&
           high_water->retired_through_operation_id == 0U;
}

static bool high_water_is_valid(
    const ucn_v6_operation_high_water_t *high_water)
{
    if (!high_water->occupied) {
        return high_water_is_canonical_empty(high_water);
    }
    return ucn_v6_principal_is_valid(&high_water->initiator_principal) &&
           high_water->retired_through_operation_id != 0U &&
           high_water->retired_through_operation_id <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD;
}

static bool snapshot_is_valid(
    const ucn_v6_operation_journal_snapshot_t *snapshot,
    bool allow_factory_generation)
{
    size_t left;
    size_t right;

    if (snapshot == NULL ||
        snapshot->magic != UCN_V6_OPERATION_JOURNAL_MAGIC ||
        snapshot->schema != UCN_V6_OPERATION_JOURNAL_SCHEMA ||
        snapshot->slot_count != UCN_V6_OPERATION_JOURNAL_SLOTS ||
        snapshot->snapshot_generation >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        (!allow_factory_generation && snapshot->snapshot_generation == 0U)) {
        return false;
    }
    for (left = 0U; left < UCN_V6_OPERATION_JOURNAL_SLOTS; ++left) {
        if (!slot_is_valid(&snapshot->slots[left])) {
            return false;
        }
        if (snapshot->slots[left].occupied) {
            for (right = left + 1U;
                 right < UCN_V6_OPERATION_JOURNAL_SLOTS; ++right) {
                if (snapshot->slots[right].occupied &&
                    operation_key_equal(&snapshot->slots[left].key,
                                        &snapshot->slots[right].key)) {
                    return false;
                }
            }
        }
    }
    for (left = 0U; left < UCN_V6_OPERATION_HIGH_WATER_SLOTS; ++left) {
        if (!high_water_is_valid(&snapshot->high_waters[left])) {
            return false;
        }
        if (snapshot->high_waters[left].occupied) {
            for (right = left + 1U;
                 right < UCN_V6_OPERATION_HIGH_WATER_SLOTS; ++right) {
                if (snapshot->high_waters[right].occupied &&
                    principal_equal(
                        &snapshot->high_waters[left].initiator_principal,
                        &snapshot->high_waters[right].initiator_principal)) {
                    return false;
                }
            }
        }
    }
    for (left = 0U; left < UCN_V6_OPERATION_JOURNAL_SLOTS; ++left) {
        if (snapshot->slots[left].occupied) {
            for (right = 0U;
                 right < UCN_V6_OPERATION_HIGH_WATER_SLOTS; ++right) {
                if (snapshot->high_waters[right].occupied &&
                    principal_equal(
                        &snapshot->slots[left].key.initiator_principal,
                        &snapshot->high_waters[right].initiator_principal) &&
                    snapshot->slots[left].key.operation_id <=
                        snapshot->high_waters[right]
                            .retired_through_operation_id) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool slot_equal(
    const ucn_v6_operation_slot_t *left,
    const ucn_v6_operation_slot_t *right)
{
    return left->occupied == right->occupied &&
           left->key.operation_id == right->key.operation_id &&
           principal_equal(&left->key.initiator_principal,
                           &right->key.initiator_principal) &&
           left->endpoint_id == right->endpoint_id &&
           left->execution_contract == right->execution_contract &&
           memcmp(left->request_digest, right->request_digest,
                  sizeof(left->request_digest)) == 0 &&
           left->phase == right->phase &&
           left->result_code == right->result_code &&
           left->result_length == right->result_length &&
           memcmp(left->result, right->result, sizeof(left->result)) == 0 &&
           memcmp(left->result_digest, right->result_digest,
                  sizeof(left->result_digest)) == 0;
}

static bool snapshot_equal(
    const ucn_v6_operation_journal_snapshot_t *left,
    const ucn_v6_operation_journal_snapshot_t *right)
{
    size_t index;

    if (left->magic != right->magic || left->schema != right->schema ||
        left->slot_count != right->slot_count ||
        left->snapshot_generation != right->snapshot_generation) {
        return false;
    }
    for (index = 0U; index < UCN_V6_OPERATION_JOURNAL_SLOTS; ++index) {
        if (!slot_equal(&left->slots[index], &right->slots[index])) {
            return false;
        }
    }
    for (index = 0U; index < UCN_V6_OPERATION_HIGH_WATER_SLOTS; ++index) {
        const ucn_v6_operation_high_water_t *a = &left->high_waters[index];
        const ucn_v6_operation_high_water_t *b = &right->high_waters[index];
        if (a->occupied != b->occupied ||
            !principal_equal(&a->initiator_principal,
                             &b->initiator_principal) ||
            a->retired_through_operation_id !=
                b->retired_through_operation_id) {
            return false;
        }
    }
    return true;
}

static void witness_make_factory(ucn_v6_message_witness_t *witness)
{
    memset(witness, 0, sizeof(*witness));
    witness->magic = UCN_V6_MESSAGE_WITNESS_MAGIC;
    witness->schema = UCN_V6_MESSAGE_WITNESS_SCHEMA;
}

static bool witness_is_valid(const ucn_v6_message_witness_t *witness,
                             bool allow_factory)
{
    const uint16_t known_flags =
        UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED |
        UCN_V6_MESSAGE_WITNESS_ALLOCATOR_COMMISSIONED;
    bool journal_commissioned;
    bool allocator_commissioned;

    if (witness == NULL || witness->magic != UCN_V6_MESSAGE_WITNESS_MAGIC ||
        witness->schema != UCN_V6_MESSAGE_WITNESS_SCHEMA ||
        (witness->flags & (uint16_t)~known_flags) != 0U ||
        witness->witness_generation > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        witness->journal_committed_generation >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        witness->journal_pending_generation >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        witness->operation_id_high_water >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return false;
    }
    if (witness->witness_generation == 0U) {
        return allow_factory && witness->flags == 0U &&
               witness->journal_committed_generation == 0U &&
               witness->journal_pending_generation == 0U &&
               witness->operation_id_high_water == 0U;
    }
    journal_commissioned =
        (witness->flags & UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED) != 0U;
    allocator_commissioned =
        (witness->flags & UCN_V6_MESSAGE_WITNESS_ALLOCATOR_COMMISSIONED) != 0U;
    if ((journal_commissioned &&
         witness->journal_committed_generation == 0U &&
         witness->journal_pending_generation == 0U) ||
        (!journal_commissioned &&
         (witness->journal_committed_generation != 0U ||
          witness->journal_pending_generation != 0U)) ||
        (!allocator_commissioned && witness->operation_id_high_water != 0U)) {
        return false;
    }
    if (allocator_commissioned && witness->operation_id_high_water == 0U) {
        return false;
    }
    if (witness->journal_pending_generation != 0U &&
        (witness->journal_committed_generation ==
             UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
         witness->journal_pending_generation !=
             witness->journal_committed_generation + 1U)) {
        return false;
    }
    return true;
}

static bool witness_equal(const ucn_v6_message_witness_t *left,
                          const ucn_v6_message_witness_t *right)
{
    return left->magic == right->magic && left->schema == right->schema &&
           left->flags == right->flags &&
           left->witness_generation == right->witness_generation &&
           left->journal_committed_generation ==
               right->journal_committed_generation &&
           left->journal_pending_generation ==
               right->journal_pending_generation &&
           left->operation_id_high_water == right->operation_id_high_water;
}

ucn_v6_result_t ucn_v6_message_witness_transition_validate(
    const ucn_v6_message_witness_t *previous,
    const ucn_v6_message_witness_t *next)
{
    bool allocator_changed;
    bool journal_changed;
    bool journal_transition = false;

    if (!witness_is_valid(previous, true) ||
        !witness_is_valid(next, false) ||
        previous->witness_generation ==
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        next->witness_generation != previous->witness_generation + 1U ||
        next->operation_id_high_water <
            previous->operation_id_high_water) {
        return UCN_V6_ERR_STATE;
    }
    allocator_changed = next->operation_id_high_water !=
                        previous->operation_id_high_water;
    journal_changed = next->journal_committed_generation !=
                          previous->journal_committed_generation ||
                      next->journal_pending_generation !=
                          previous->journal_pending_generation ||
                      ((next->flags ^ previous->flags) &
                       UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED) != 0U;
    if ((previous->flags &
         UCN_V6_MESSAGE_WITNESS_ALLOCATOR_COMMISSIONED) != 0U &&
        (next->flags &
         UCN_V6_MESSAGE_WITNESS_ALLOCATOR_COMMISSIONED) == 0U) {
        return UCN_V6_ERR_STATE;
    }
    if (allocator_changed &&
        (next->flags &
         UCN_V6_MESSAGE_WITNESS_ALLOCATOR_COMMISSIONED) == 0U) {
        return UCN_V6_ERR_STATE;
    }
    if (!journal_changed) {
        journal_transition = true;
    } else if (previous->journal_pending_generation == 0U &&
               next->journal_committed_generation ==
                   previous->journal_committed_generation &&
               next->journal_pending_generation ==
                   previous->journal_committed_generation + 1U &&
               (next->flags &
                UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED) != 0U) {
        journal_transition = true;
    } else if (previous->journal_pending_generation != 0U &&
               next->journal_pending_generation == 0U &&
               (next->journal_committed_generation ==
                    previous->journal_committed_generation ||
                next->journal_committed_generation ==
                    previous->journal_pending_generation)) {
        if (next->journal_committed_generation == 0U) {
            journal_transition =
                (next->flags &
                 UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED) == 0U;
        } else {
            journal_transition =
                (next->flags &
                 UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED) != 0U;
        }
    }
    if (!journal_transition || (allocator_changed && journal_changed)) {
        return UCN_V6_ERR_STATE;
    }
    return UCN_V6_OK;
}

static void snapshot_make_factory(
    ucn_v6_operation_journal_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = UCN_V6_OPERATION_JOURNAL_MAGIC;
    snapshot->schema = UCN_V6_OPERATION_JOURNAL_SCHEMA;
    snapshot->slot_count = (uint16_t)UCN_V6_OPERATION_JOURNAL_SLOTS;
}

static bool store_is_valid(const ucn_v6_message_store_ops_t *store)
{
    return store != NULL && store->load_witness != NULL &&
           store->reserve_witness != NULL && store->load_journal != NULL &&
           store->submit_journal != NULL;
}

static ucn_v6_result_t serial64_checked_next(
    uint64_t current,
    uint64_t *next)
{
    if (next == NULL || current > UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (current == UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    *next = current + 1U;
    return UCN_V6_OK;
}

static ucn_v6_result_t load_under_gate(
    const ucn_v6_message_store_ops_t *store,
    ucn_v6_operation_journal_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    return store->load_journal(store->context, snapshot);
}

static ucn_v6_result_t load_witness_under_gate(
    const ucn_v6_message_store_ops_t *store,
    ucn_v6_message_witness_t *witness)
{
    memset(witness, 0, sizeof(*witness));
    return store->load_witness(store->context, witness);
}

static ucn_v6_result_t reserve_witness_under_gate(
    const ucn_v6_message_store_ops_t *store,
    const ucn_v6_message_witness_t *previous,
    ucn_v6_message_witness_t *candidate,
    ucn_v6_message_witness_t *verified)
{
    uint64_t next_generation;
    ucn_v6_result_t result;

    if (previous == NULL || candidate == NULL || verified == NULL ||
        candidate->witness_generation != previous->witness_generation) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = serial64_checked_next(previous->witness_generation,
                                   &next_generation);
    if (result != UCN_V6_OK) {
        return result;
    }
    candidate->witness_generation = next_generation;
    if (ucn_v6_message_witness_transition_validate(previous, candidate) !=
        UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    result = store->reserve_witness(store->context, candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = load_witness_under_gate(store, verified);
    if (result != UCN_V6_OK || !witness_is_valid(verified, false) ||
        !witness_equal(candidate, verified)) {
        return UCN_V6_ERR_STATE;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t submit_and_verify_under_gate(
    const ucn_v6_message_store_ops_t *store,
    const ucn_v6_operation_journal_snapshot_t *candidate,
    ucn_v6_operation_journal_snapshot_t *verified)
{
    ucn_v6_result_t result =
        store->submit_journal(store->context, candidate);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = load_under_gate(store, verified);
    if (result != UCN_V6_OK ||
        !snapshot_is_valid(verified, false) ||
        !snapshot_equal(candidate, verified)) {
        return UCN_V6_ERR_STATE;
    }
    return UCN_V6_OK;
}

static bool journal_is_ready(ucn_v6_operation_journal_t *journal)
{
    return journal_storage_is_valid(journal) && !journal->faulted &&
           snapshot_is_valid(&journal->committed, true) &&
           !ucn_v6_callback_gate_is_active(journal->callback_gate);
}

static ucn_v6_operation_slot_t *find_slot(
    ucn_v6_operation_journal_snapshot_t *snapshot,
    const ucn_v6_operation_key_t *key,
    bool allow_empty)
{
    ucn_v6_operation_slot_t *empty = NULL;
    size_t index;

    for (index = 0U; index < UCN_V6_OPERATION_JOURNAL_SLOTS; ++index) {
        if (snapshot->slots[index].occupied &&
            operation_key_equal(&snapshot->slots[index].key, key)) {
            return &snapshot->slots[index];
        }
        if (!snapshot->slots[index].occupied && empty == NULL) {
            empty = &snapshot->slots[index];
        }
    }
    return allow_empty ? empty : NULL;
}

static const ucn_v6_operation_high_water_t *find_high_water_const(
    const ucn_v6_operation_journal_snapshot_t *snapshot,
    const ucn_v6_principal_t *principal)
{
    size_t index;
    for (index = 0U; index < UCN_V6_OPERATION_HIGH_WATER_SLOTS; ++index) {
        if (snapshot->high_waters[index].occupied &&
            principal_equal(&snapshot->high_waters[index].initiator_principal,
                            principal)) {
            return &snapshot->high_waters[index];
        }
    }
    return NULL;
}

static ucn_v6_operation_high_water_t *find_high_water(
    ucn_v6_operation_journal_snapshot_t *snapshot,
    const ucn_v6_principal_t *principal,
    bool allow_empty)
{
    ucn_v6_operation_high_water_t *empty = NULL;
    size_t index;
    for (index = 0U; index < UCN_V6_OPERATION_HIGH_WATER_SLOTS; ++index) {
        if (snapshot->high_waters[index].occupied &&
            principal_equal(&snapshot->high_waters[index].initiator_principal,
                            principal)) {
            return &snapshot->high_waters[index];
        }
        if (!snapshot->high_waters[index].occupied && empty == NULL) {
            empty = &snapshot->high_waters[index];
        }
    }
    return allow_empty ? empty : NULL;
}

static ucn_v6_result_t persist_candidate(
    ucn_v6_operation_journal_t *journal,
    ucn_v6_operation_journal_snapshot_t *candidate)
{
    ucn_v6_operation_journal_snapshot_t verified = {0};
    ucn_v6_message_witness_t current = {0};
    ucn_v6_message_witness_t pending = {0};
    ucn_v6_message_witness_t pending_verified = {0};
    ucn_v6_message_witness_t committed_witness = {0};
    ucn_v6_message_witness_t committed_verified = {0};
    uint64_t next_generation;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    result = serial64_checked_next(journal->committed.snapshot_generation,
                                   &next_generation);
    if (result != UCN_V6_OK) {
        journal->faulted = true;
        return result;
    }
    candidate->snapshot_generation = next_generation;
    if (!snapshot_is_valid(candidate, false)) {
        return UCN_V6_ERR_STATE;
    }
    result = ucn_v6_callback_gate_try_enter(journal->callback_gate, journal);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = load_witness_under_gate(&journal->store, &current);
    if (result == UCN_V6_ERR_NOT_FOUND &&
        journal->witness.witness_generation == 0U) {
        witness_make_factory(&current);
        result = UCN_V6_OK;
    }
    if (result != UCN_V6_OK || !witness_is_valid(&current, true) ||
        current.journal_committed_generation !=
            journal->committed.snapshot_generation ||
        current.journal_pending_generation != 0U) {
        result = UCN_V6_ERR_STATE;
    }
    if (result == UCN_V6_OK) {
        pending = current;
        pending.flags |= UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED;
        pending.journal_pending_generation = next_generation;
        result = reserve_witness_under_gate(&journal->store, &current,
                                            &pending,
                                            &pending_verified);
    }
    if (result == UCN_V6_OK) {
        result = submit_and_verify_under_gate(&journal->store, candidate,
                                              &verified);
    }
    if (result == UCN_V6_OK) {
        committed_witness = pending_verified;
        committed_witness.journal_committed_generation = next_generation;
        committed_witness.journal_pending_generation = 0U;
        result = reserve_witness_under_gate(&journal->store,
                                            &pending_verified,
                                            &committed_witness,
                                            &committed_verified);
    }
    leave_result = ucn_v6_callback_gate_leave(journal->callback_gate, journal);
    if (result != UCN_V6_OK || leave_result != UCN_V6_OK) {
        journal->faulted = true;
        return result != UCN_V6_OK ? result : leave_result;
    }
    journal->witness = committed_verified;
    journal->committed = verified;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_message_validate(
    const ucn_v6_message_descriptor_t *message,
    const ucn_v6_endpoint_contract_t *endpoint)
{
    uint32_t traffic;
    uint32_t delivery;
    uint32_t role;
    bool one_way;

    if (message == NULL || endpoint == NULL ||
        !endpoint_id_is_valid(endpoint->endpoint_id) ||
        endpoint->traffic_class_mask == 0U ||
        (endpoint->traffic_class_mask & 0xF0U) != 0U ||
        endpoint->delivery_guarantee_mask == 0U ||
        (endpoint->delivery_guarantee_mask & 0xF8U) != 0U ||
        endpoint->interaction_role_mask == 0U ||
        (endpoint->interaction_role_mask & 0xF0U) != 0U ||
        endpoint->max_payload_bytes == 0U ||
        endpoint->max_result_bytes > UCN_V6_OPERATION_RESULT_MAX_BYTES ||
        !execution_contract_is_valid(endpoint->execution_contract) ||
        message->destination_endpoint != endpoint->endpoint_id ||
        !endpoint_id_is_valid(message->source_endpoint)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    traffic = (uint32_t)message->traffic_class;
    delivery = (uint32_t)message->delivery_guarantee;
    role = (uint32_t)message->interaction_role;
    if (traffic > UCN_V6_TRAFFIC_Q3 ||
        delivery > UCN_V6_DELIVERY_RELIABLE ||
        role > UCN_V6_INTERACTION_ERROR ||
        (endpoint->traffic_class_mask & (uint8_t)(1U << traffic)) == 0U ||
        (endpoint->delivery_guarantee_mask &
         (uint8_t)(1U << delivery)) == 0U ||
        (endpoint->interaction_role_mask & (uint8_t)(1U << role)) == 0U ||
        message->payload_length > endpoint->max_payload_bytes) {
        return UCN_V6_ERR_ACCESS;
    }
    one_way = message->interaction_role == UCN_V6_INTERACTION_ONE_WAY;
    if ((one_way && message->operation_id != 0U) ||
        (!one_way && (message->operation_id == 0U ||
                      message->operation_id >
                          UCN_V6_SERIAL64_ROTATION_THRESHOLD))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if ((message->interaction_role == UCN_V6_INTERACTION_RESULT ||
         message->interaction_role == UCN_V6_INTERACTION_ERROR) &&
        message->payload_length > endpoint->max_result_bytes) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (endpoint->execution_contract == UCN_V6_ENDPOINT_NON_RETRYABLE &&
        message->delivery_guarantee == UCN_V6_DELIVERY_RELIABLE) {
        return UCN_V6_ERR_ACCESS;
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_operation_id_allocator_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_message_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    uint32_t reservation_block_size,
    ucn_v6_operation_id_allocator_t **allocator_out)
{
    ucn_v6_operation_id_allocator_t initialized;
    ucn_v6_operation_id_allocator_t *allocator;
    ucn_v6_message_witness_t witness;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    if (allocator_out == NULL || !store_is_valid(store) ||
        callback_gate == NULL || reservation_block_size == 0U) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(
        (const ucn_v6_feature_manifest_t *)manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(*allocator),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    allocator = (ucn_v6_operation_id_allocator_t *)storage;
    if (ucn_v6_callback_gate_is_active(callback_gate)) {
        return UCN_V6_ERR_STATE;
    }
    result = ucn_v6_callback_gate_try_enter(callback_gate, allocator);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = load_witness_under_gate(store, &witness);
    if (result == UCN_V6_ERR_NOT_FOUND) {
        witness_make_factory(&witness);
        result = UCN_V6_OK;
    } else if (result != UCN_V6_OK ||
               !witness_is_valid(&witness, false)) {
        result = UCN_V6_ERR_STATE;
    }
    leave_result = ucn_v6_callback_gate_leave(callback_gate, allocator);
    if (result != UCN_V6_OK || leave_result != UCN_V6_OK) {
        return result != UCN_V6_OK ? result : leave_result;
    }
    if (witness.operation_id_high_water ==
        UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_V6_OPERATION_ALLOCATOR_MAGIC;
    initialized.schema = UCN_V6_STORAGE_LAYOUT;
    initialized.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    initialized.store = *store;
    initialized.callback_gate = callback_gate;
    initialized.witness = witness;
    initialized.next_id = witness.operation_id_high_water + 1U;
    initialized.reserved_through = witness.operation_id_high_water;
    initialized.reservation_block_size = reservation_block_size;
    initialized.initialized = true;
    initialized.canary = UCN_V6_OPERATION_ALLOCATOR_CANARY;
    *allocator = initialized;
    *allocator_out = allocator;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_operation_id_take(
    ucn_v6_operation_id_allocator_t *allocator,
    uint64_t *operation_id)
{
    uint64_t new_high_water;
    ucn_v6_message_witness_t current = {0};
    ucn_v6_message_witness_t candidate = {0};
    ucn_v6_message_witness_t verified = {0};
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    if (!allocator_storage_is_valid(allocator) || operation_id == NULL ||
        allocator->faulted ||
        ucn_v6_callback_gate_is_active(allocator->callback_gate)) {
        return UCN_V6_ERR_STATE;
    }
    if (allocator->next_id > allocator->reserved_through) {
        if (allocator->reserved_through >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD -
                allocator->reservation_block_size) {
            allocator->faulted = true;
            return UCN_V6_ERR_EXHAUSTED;
        }
        new_high_water = allocator->reserved_through +
                         allocator->reservation_block_size;
        result = ucn_v6_callback_gate_try_enter(allocator->callback_gate,
                                                allocator);
        if (result != UCN_V6_OK) {
            return result;
        }
        result = load_witness_under_gate(&allocator->store, &current);
        if (result == UCN_V6_ERR_NOT_FOUND &&
            allocator->witness.witness_generation == 0U) {
            witness_make_factory(&current);
            result = UCN_V6_OK;
        }
        if (result != UCN_V6_OK || !witness_is_valid(&current, true) ||
            current.operation_id_high_water !=
                allocator->reserved_through) {
            result = UCN_V6_ERR_STATE;
        } else {
            candidate = current;
            candidate.flags |=
                UCN_V6_MESSAGE_WITNESS_ALLOCATOR_COMMISSIONED;
            candidate.operation_id_high_water = new_high_water;
            result = reserve_witness_under_gate(&allocator->store,
                                                &current,
                                                &candidate, &verified);
        }
        leave_result = ucn_v6_callback_gate_leave(allocator->callback_gate,
                                                  allocator);
        if (result != UCN_V6_OK || leave_result != UCN_V6_OK) {
            allocator->faulted = true;
            return result != UCN_V6_OK ? result : leave_result;
        }
        allocator->witness = verified;
        allocator->reserved_through = new_high_water;
    }
    if (allocator->next_id == 0U ||
        allocator->next_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        allocator->faulted = true;
        return UCN_V6_ERR_EXHAUSTED;
    }
    *operation_id = allocator->next_id;
    ++allocator->next_id;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_operation_id_allocator_copy_view(
    const ucn_v6_operation_id_allocator_t *allocator,
    ucn_v6_operation_id_allocator_view_t *view)
{
    if (!allocator_storage_is_valid(allocator) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    view->next_id = allocator->next_id;
    view->reserved_through = allocator->reserved_through;
    view->witness_generation = allocator->witness.witness_generation;
    view->reservation_block_size = allocator->reservation_block_size;
    view->faulted = allocator->faulted;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_operation_journal_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_message_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_operation_journal_t **journal_out)
{
    ucn_v6_operation_journal_t initialized = {0};
    ucn_v6_operation_journal_t *journal;
    ucn_v6_operation_journal_snapshot_t loaded = {0};
    ucn_v6_operation_journal_snapshot_t migrated = {0};
    ucn_v6_operation_journal_snapshot_t verified = {0};
    ucn_v6_message_witness_t witness = {0};
    ucn_v6_message_witness_t witness_candidate = {0};
    ucn_v6_message_witness_t witness_verified = {0};
    uint64_t next_generation;
    size_t index;
    bool changed = false;
    bool journal_found = false;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    if (journal_out == NULL || !store_is_valid(store) ||
        callback_gate == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(
        (const ucn_v6_feature_manifest_t *)manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(*journal),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    journal = (ucn_v6_operation_journal_t *)storage;
    if (ucn_v6_callback_gate_is_active(callback_gate)) {
        return UCN_V6_ERR_STATE;
    }
    result = ucn_v6_callback_gate_try_enter(callback_gate, journal);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = load_witness_under_gate(store, &witness);
    if (result == UCN_V6_ERR_NOT_FOUND) {
        witness_make_factory(&witness);
        result = UCN_V6_OK;
    } else if (result != UCN_V6_OK ||
               !witness_is_valid(&witness, false)) {
        result = UCN_V6_ERR_STATE;
    }
    if (result == UCN_V6_OK) {
        result = load_under_gate(store, &loaded);
        if (result == UCN_V6_ERR_NOT_FOUND) {
            snapshot_make_factory(&loaded);
            result = UCN_V6_OK;
        } else if (result == UCN_V6_OK &&
                   snapshot_is_valid(&loaded, false)) {
            journal_found = true;
        } else {
            result = UCN_V6_ERR_STATE;
        }
    }
    if (result == UCN_V6_OK &&
        (witness.flags & UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED) == 0U) {
        if (journal_found || loaded.snapshot_generation != 0U) {
            result = UCN_V6_ERR_STATE;
        }
    } else if (result == UCN_V6_OK &&
               witness.journal_pending_generation != 0U) {
        if ((journal_found && loaded.snapshot_generation ==
                                  witness.journal_pending_generation)) {
            witness_candidate = witness;
            witness_candidate.journal_committed_generation =
                witness.journal_pending_generation;
            witness_candidate.journal_pending_generation = 0U;
        } else if ((!journal_found &&
                    witness.journal_committed_generation == 0U) ||
                   (journal_found && loaded.snapshot_generation ==
                                         witness.journal_committed_generation)) {
            witness_candidate = witness;
            witness_candidate.journal_pending_generation = 0U;
            if (witness_candidate.journal_committed_generation == 0U) {
                witness_candidate.flags &= (uint16_t)
                    ~UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED;
            }
        } else {
            result = UCN_V6_ERR_STATE;
        }
        if (result == UCN_V6_OK) {
            result = reserve_witness_under_gate(store, &witness,
                                                &witness_candidate,
                                                &witness_verified);
            if (result == UCN_V6_OK) {
                witness = witness_verified;
            }
        }
    } else if (result == UCN_V6_OK &&
               (!journal_found || loaded.snapshot_generation !=
                                     witness.journal_committed_generation)) {
        result = UCN_V6_ERR_STATE;
    }
    if (result == UCN_V6_OK) {
        migrated = loaded;
        for (index = 0U; index < UCN_V6_OPERATION_JOURNAL_SLOTS; ++index) {
            if (migrated.slots[index].occupied &&
                migrated.slots[index].phase ==
                    UCN_V6_OPERATION_PHASE_EXECUTING) {
                migrated.slots[index].phase =
                    UCN_V6_OPERATION_PHASE_IN_DOUBT;
                changed = true;
            }
        }
        if (changed) {
            result = serial64_checked_next(loaded.snapshot_generation,
                                           &next_generation);
            if (result == UCN_V6_OK) {
                migrated.snapshot_generation = next_generation;
                witness_candidate = witness;
                witness_candidate.flags |=
                    UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED;
                witness_candidate.journal_pending_generation =
                    next_generation;
                result = reserve_witness_under_gate(store, &witness,
                                                    &witness_candidate,
                                                    &witness_verified);
            }
            if (result == UCN_V6_OK) {
                witness = witness_verified;
                result = submit_and_verify_under_gate(store, &migrated,
                                                       &verified);
            }
            if (result == UCN_V6_OK) {
                witness_candidate = witness;
                witness_candidate.journal_committed_generation =
                    next_generation;
                witness_candidate.journal_pending_generation = 0U;
                result = reserve_witness_under_gate(store, &witness,
                                                    &witness_candidate,
                                                    &witness_verified);
                if (result == UCN_V6_OK) {
                    loaded = verified;
                    witness = witness_verified;
                }
            }
        }
    }
    leave_result = ucn_v6_callback_gate_leave(callback_gate, journal);
    if (result != UCN_V6_OK || leave_result != UCN_V6_OK) {
        return result != UCN_V6_OK ? result : leave_result;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_V6_OPERATION_JOURNAL_OBJECT_MAGIC;
    initialized.schema = UCN_V6_STORAGE_LAYOUT;
    initialized.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    initialized.committed = loaded;
    initialized.witness = witness;
    initialized.store = *store;
    initialized.callback_gate = callback_gate;
    initialized.initialized = true;
    initialized.canary = UCN_V6_OPERATION_JOURNAL_CANARY;
    *journal = initialized;
    *journal_out = journal;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_operation_prepare(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    uint16_t endpoint_id,
    ucn_v6_endpoint_execution_contract_t execution_contract,
    const uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES],
    ucn_v6_operation_admission_t *admission)
{
    ucn_v6_operation_journal_snapshot_t candidate;
    ucn_v6_operation_slot_t *slot;
    const ucn_v6_operation_high_water_t *high_water;
    ucn_v6_operation_admission_t duplicate;
    ucn_v6_result_t result;

    if (journal == NULL || !operation_key_is_valid(key) ||
        !endpoint_id_is_valid(endpoint_id) ||
        execution_contract != UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE ||
        !digest_is_valid(request_digest) || admission == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!journal_is_ready(journal)) {
        return UCN_V6_ERR_STATE;
    }
    high_water = find_high_water_const(&journal->committed,
                                       &key->initiator_principal);
    if (high_water != NULL &&
        key->operation_id <= high_water->retired_through_operation_id) {
        return UCN_V6_ERR_REPLAY;
    }
    candidate = journal->committed;
    slot = find_slot(&candidate, key, true);
    if (slot == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (slot->occupied) {
        if (slot->endpoint_id != endpoint_id ||
            slot->execution_contract != execution_contract ||
            memcmp(slot->request_digest, request_digest,
                   sizeof(slot->request_digest)) != 0) {
            return UCN_V6_ERR_REPLAY;
        }
        switch (slot->phase) {
        case UCN_V6_OPERATION_PHASE_PREPARED:
            duplicate = UCN_V6_OPERATION_ADMISSION_PREPARED;
            break;
        case UCN_V6_OPERATION_PHASE_EXECUTING:
            duplicate = UCN_V6_OPERATION_ADMISSION_EXECUTING;
            break;
        case UCN_V6_OPERATION_PHASE_COMMITTED_RESULT:
            duplicate = UCN_V6_OPERATION_ADMISSION_RESULT_REPLAY;
            break;
        case UCN_V6_OPERATION_PHASE_IN_DOUBT:
            duplicate = UCN_V6_OPERATION_ADMISSION_IN_DOUBT;
            break;
        case UCN_V6_OPERATION_PHASE_TOMBSTONED:
            duplicate = UCN_V6_OPERATION_ADMISSION_TOMBSTONED;
            break;
        default:
            return UCN_V6_ERR_STATE;
        }
        *admission = duplicate;
        return UCN_V6_OK;
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->key = *key;
    slot->endpoint_id = endpoint_id;
    slot->execution_contract = execution_contract;
    memcpy(slot->request_digest, request_digest,
           sizeof(slot->request_digest));
    slot->phase = UCN_V6_OPERATION_PHASE_PREPARED;
    result = persist_candidate(journal, &candidate);
    if (result == UCN_V6_OK) {
        *admission = UCN_V6_OPERATION_ADMISSION_NEW;
    }
    return result;
}

ucn_v6_result_t ucn_v6_operation_mark_executing(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key)
{
    ucn_v6_operation_journal_snapshot_t candidate;
    ucn_v6_operation_slot_t *slot;

    if (journal == NULL || !operation_key_is_valid(key)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!journal_is_ready(journal)) {
        return UCN_V6_ERR_STATE;
    }
    candidate = journal->committed;
    slot = find_slot(&candidate, key, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (slot->phase != UCN_V6_OPERATION_PHASE_PREPARED) {
        return UCN_V6_ERR_STATE;
    }
    slot->phase = UCN_V6_OPERATION_PHASE_EXECUTING;
    return persist_candidate(journal, &candidate);
}

static ucn_v6_result_t slot_set_committed_result(
    ucn_v6_operation_slot_t *slot,
    int32_t result_code,
    const uint8_t *result,
    uint16_t result_length,
    const uint8_t *result_digest)
{
    if (result_length > UCN_V6_OPERATION_RESULT_MAX_BYTES ||
        (result_length != 0U && result == NULL) ||
        !digest_is_valid(result_digest)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slot->phase = UCN_V6_OPERATION_PHASE_COMMITTED_RESULT;
    slot->result_code = result_code;
    slot->result_length = result_length;
    memset(slot->result, 0, sizeof(slot->result));
    if (result_length != 0U) {
        memcpy(slot->result, result, result_length);
    }
    memcpy(slot->result_digest, result_digest,
           sizeof(slot->result_digest));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_operation_commit_result(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    int32_t result_code,
    const uint8_t *result,
    uint16_t result_length,
    const uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES])
{
    ucn_v6_operation_journal_snapshot_t candidate;
    ucn_v6_operation_slot_t *slot;
    ucn_v6_result_t result_set;

    if (journal == NULL || !operation_key_is_valid(key) ||
        result_length > UCN_V6_OPERATION_RESULT_MAX_BYTES ||
        (result_length != 0U && result == NULL) ||
        !digest_is_valid(result_digest)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!journal_is_ready(journal)) {
        return UCN_V6_ERR_STATE;
    }
    candidate = journal->committed;
    slot = find_slot(&candidate, key, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (slot->phase == UCN_V6_OPERATION_PHASE_COMMITTED_RESULT) {
        if (slot->result_code == result_code &&
            slot->result_length == result_length &&
            (result_length == 0U ||
             (result != NULL &&
              memcmp(slot->result, result, result_length) == 0)) &&
            memcmp(slot->result_digest, result_digest,
                   sizeof(slot->result_digest)) == 0) {
            return UCN_V6_OK;
        }
        return UCN_V6_ERR_REPLAY;
    }
    if (slot->phase != UCN_V6_OPERATION_PHASE_EXECUTING) {
        return UCN_V6_ERR_STATE;
    }
    result_set = slot_set_committed_result(slot, result_code, result,
                                           result_length, result_digest);
    return result_set == UCN_V6_OK ?
               persist_candidate(journal, &candidate) : result_set;
}

ucn_v6_result_t ucn_v6_operation_resolve_in_doubt(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    bool authenticated,
    bool result_known,
    int32_t result_code,
    const uint8_t *result,
    uint16_t result_length,
    const uint8_t terminal_digest[UCN_V6_OPERATION_DIGEST_BYTES])
{
    ucn_v6_operation_journal_snapshot_t candidate;
    ucn_v6_operation_slot_t *slot;
    ucn_v6_result_t result_set;

    if (journal == NULL || !operation_key_is_valid(key) ||
        !digest_is_valid(terminal_digest)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!journal_is_ready(journal)) {
        return UCN_V6_ERR_STATE;
    }
    if (!authenticated) {
        return UCN_V6_ERR_ACCESS;
    }
    candidate = journal->committed;
    slot = find_slot(&candidate, key, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (slot->phase != UCN_V6_OPERATION_PHASE_IN_DOUBT) {
        return UCN_V6_ERR_STATE;
    }
    if (result_known) {
        result_set = slot_set_committed_result(slot, result_code, result,
                                               result_length,
                                               terminal_digest);
        if (result_set != UCN_V6_OK) {
            return result_set;
        }
    } else {
        if (result_length != 0U || result != NULL) {
            return UCN_V6_ERR_ARGUMENT;
        }
        slot->phase = UCN_V6_OPERATION_PHASE_TOMBSTONED;
        slot->result_code = result_code;
        memcpy(slot->result_digest, terminal_digest,
               sizeof(slot->result_digest));
    }
    return persist_candidate(journal, &candidate);
}

ucn_v6_result_t ucn_v6_operation_abort_prepared(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    int32_t result_code,
    const uint8_t terminal_digest[UCN_V6_OPERATION_DIGEST_BYTES])
{
    ucn_v6_operation_journal_snapshot_t candidate;
    ucn_v6_operation_slot_t *slot;

    if (journal == NULL || !operation_key_is_valid(key) ||
        !digest_is_valid(terminal_digest)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!journal_is_ready(journal)) {
        return UCN_V6_ERR_STATE;
    }
    candidate = journal->committed;
    slot = find_slot(&candidate, key, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (slot->phase != UCN_V6_OPERATION_PHASE_PREPARED) {
        return UCN_V6_ERR_STATE;
    }
    slot->phase = UCN_V6_OPERATION_PHASE_TOMBSTONED;
    slot->result_code = result_code;
    memcpy(slot->result_digest, terminal_digest,
           sizeof(slot->result_digest));
    return persist_candidate(journal, &candidate);
}

ucn_v6_result_t ucn_v6_operation_tombstone_result(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    bool authenticated_result_ack,
    bool minimum_retention_elapsed)
{
    ucn_v6_operation_journal_snapshot_t candidate;
    ucn_v6_operation_slot_t *slot;

    if (journal == NULL || !operation_key_is_valid(key)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!journal_is_ready(journal)) {
        return UCN_V6_ERR_STATE;
    }
    if (!authenticated_result_ack || !minimum_retention_elapsed) {
        return UCN_V6_ERR_ACCESS;
    }
    candidate = journal->committed;
    slot = find_slot(&candidate, key, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (slot->phase != UCN_V6_OPERATION_PHASE_COMMITTED_RESULT) {
        return UCN_V6_ERR_STATE;
    }
    slot->phase = UCN_V6_OPERATION_PHASE_TOMBSTONED;
    slot->result_length = 0U;
    memset(slot->result, 0, sizeof(slot->result));
    return persist_candidate(journal, &candidate);
}

ucn_v6_result_t ucn_v6_operation_reclaim_tombstone(
    ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    uint64_t durable_initiator_high_water,
    bool maximum_replay_lifetime_elapsed)
{
    ucn_v6_operation_journal_snapshot_t candidate;
    ucn_v6_operation_slot_t *slot;
    ucn_v6_operation_high_water_t *high_water;

    if (journal == NULL || !operation_key_is_valid(key)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!journal_is_ready(journal)) {
        return UCN_V6_ERR_STATE;
    }
    if (!maximum_replay_lifetime_elapsed ||
        durable_initiator_high_water <= key->operation_id ||
        durable_initiator_high_water >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ACCESS;
    }
    candidate = journal->committed;
    slot = find_slot(&candidate, key, false);
    if (slot == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (slot->phase != UCN_V6_OPERATION_PHASE_TOMBSTONED) {
        return UCN_V6_ERR_STATE;
    }
    high_water = find_high_water(&candidate, &key->initiator_principal, true);
    if (high_water == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (!high_water->occupied) {
        memset(high_water, 0, sizeof(*high_water));
        high_water->occupied = true;
        high_water->initiator_principal = key->initiator_principal;
    }
    if (key->operation_id > high_water->retired_through_operation_id) {
        high_water->retired_through_operation_id = key->operation_id;
    }
    memset(slot, 0, sizeof(*slot));
    return persist_candidate(journal, &candidate);
}

ucn_v6_result_t ucn_v6_operation_copy_slot(
    const ucn_v6_operation_journal_t *journal,
    const ucn_v6_operation_key_t *key,
    ucn_v6_operation_slot_t *slot)
{
    size_t index;

    if (!journal_storage_is_valid(journal) || slot == NULL ||
        journal->faulted || !operation_key_is_valid(key) ||
        !snapshot_is_valid(&journal->committed, true)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_OPERATION_JOURNAL_SLOTS; ++index) {
        if (journal->committed.slots[index].occupied &&
            operation_key_equal(&journal->committed.slots[index].key, key)) {
            *slot = journal->committed.slots[index];
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_operation_journal_copy_view(
    const ucn_v6_operation_journal_t *journal,
    ucn_v6_operation_journal_view_t *view)
{
    ucn_v6_operation_journal_view_t next;
    size_t index;

    if (!journal_storage_is_valid(journal) || view == NULL ||
        !snapshot_is_valid(&journal->committed, true)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&next, 0, sizeof(next));
    next.committed_generation = journal->committed.snapshot_generation;
    next.witness_generation = journal->witness.witness_generation;
    next.pending_generation = journal->witness.journal_pending_generation;
    next.faulted = journal->faulted;
    for (index = 0U; index < UCN_V6_OPERATION_JOURNAL_SLOTS; ++index) {
        if (journal->committed.slots[index].occupied) {
            ++next.occupied_slots;
        }
    }
    *view = next;
    return UCN_V6_OK;
}
