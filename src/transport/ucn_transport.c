#include "transport/ucn_transport_private.h"

#include "internal/ucn_checked.h"

#include <string.h>

bool ucn_i_transport_p_lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

ucn_result_t ucn_i_transport_p_lock(ucn_i_transport_owner_t *owner)
{
    if (owner == NULL || owner->magic != UCN_I_TRANSPORT_MAGIC ||
        owner->schema != UCN_I_TRANSPORT_SCHEMA ||
        !ucn_i_transport_p_lock_valid(&owner->state_lock)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

void ucn_i_transport_p_unlock(ucn_i_transport_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

bool ucn_i_transport_p_bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;

    if (bytes == NULL) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool binding_valid(const ucn_i_transport_binding_t *binding)
{
    return binding != NULL && binding->address != 0U &&
           binding->generation != 0U &&
           ucn_i_transport_p_bytes_nonzero(binding->principal,
                                           sizeof(binding->principal));
}

static bool binding_equal(const ucn_i_transport_binding_t *left,
                          const ucn_i_transport_binding_t *right)
{
    return left->address == right->address &&
           left->generation == right->generation &&
           memcmp(left->principal, right->principal,
                  sizeof(left->principal)) == 0;
}

bool ucn_i_transport_p_key_valid(
    const ucn_i_transport_reliable_key_t *key)
{
    return key != NULL && binding_valid(&key->source) &&
           binding_valid(&key->destination) && key->realm != 0U &&
           key->origin_sequence != 0U && key->service_id != 0U &&
           key->delivery == UCN_DELIVERY_RELIABLE &&
           key->reserved_zero == 0U;
}

bool ucn_i_transport_p_security_valid(
    const ucn_i_transport_security_facts_t *security)
{
    if (security == NULL || security->origin_security > 2U ||
        security->endpoint_public_unauthenticated > 1U ||
        security->trusted_link_policy > 1U ||
        security->authenticated_replay_candidate > 1U) {
        return false;
    }
    if (security->origin_security == 0U) {
        return security->session_generation == 0U &&
               security->key_generation == 0U &&
               security->link_id != 0U &&
               security->link_generation != 0U &&
               security->policy_generation != 0U &&
               security->endpoint_public_unauthenticated != 0U &&
               security->trusted_link_policy != 0U &&
               security->authenticated_replay_candidate == 0U;
    }
    return security->session_generation != 0U &&
           security->key_generation != 0U && security->link_id == 0U &&
           security->link_generation == 0U &&
           security->policy_generation == 0U &&
           security->endpoint_public_unauthenticated == 0U &&
           security->trusted_link_policy == 0U;
}

bool ucn_i_transport_p_key_equal(
    const ucn_i_transport_reliable_key_t *left,
    const ucn_i_transport_reliable_key_t *right)
{
    return binding_equal(&left->source, &right->source) &&
           binding_equal(&left->destination, &right->destination) &&
           left->realm == right->realm &&
           left->origin_sequence == right->origin_sequence &&
           left->service_id == right->service_id &&
           left->delivery == right->delivery;
}

bool ucn_i_transport_p_setup_equal(
    const ucn_i_transport_transfer_setup_t *left,
    const ucn_i_transport_transfer_setup_t *right)
{
    return memcmp(left->message_digest, right->message_digest, 16U) == 0 &&
           left->operation_id == right->operation_id &&
           left->parent_generation == right->parent_generation &&
           left->transfer_id == right->transfer_id &&
           left->total_length == right->total_length &&
           left->lifetime_ms == right->lifetime_ms &&
           left->parent_id == right->parent_id &&
           left->service_id == right->service_id &&
           left->fragment_count == right->fragment_count &&
           left->fragment_budget == right->fragment_budget &&
           left->result_code == right->result_code &&
           left->parent_kind == right->parent_kind &&
           left->delivery == right->delivery &&
           left->interaction == right->interaction &&
           left->operation_flags == right->operation_flags;
}

ucn_handle_t ucn_i_transport_p_handle(
    const ucn_i_transport_owner_t *owner,
    uint16_t slot,
    uint16_t generation,
    uint8_t kind)
{
    ucn_handle_t handle;

    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = slot;
    handle.generation = generation;
    handle.object_kind = kind;
    return handle;
}

ucn_i_transport_reliable_tx_record_t *ucn_i_transport_p_reliable_tx(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_transport_reliable_tx_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_TRANSPORT_RELIABLE_TX_COUNT ||
        handle.generation == 0U ||
        handle.object_kind != UCN_I_TRANSPORT_RELIABLE_TX_KIND) {
        return NULL;
    }
    record = &owner->reliable_tx[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
}

ucn_i_transport_transfer_tx_record_t *ucn_i_transport_p_transfer_tx(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_transport_transfer_tx_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_TRANSPORT_TRANSFER_TX_COUNT ||
        handle.generation == 0U ||
        handle.object_kind != UCN_I_TRANSPORT_TRANSFER_TX_KIND) {
        return NULL;
    }
    record = &owner->transfer_tx[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
}

ucn_i_transport_transfer_rx_record_t *ucn_i_transport_p_transfer_rx(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_transport_transfer_rx_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_TRANSPORT_TRANSFER_RX_COUNT ||
        handle.generation == 0U ||
        handle.object_kind != UCN_I_TRANSPORT_TRANSFER_RX_KIND) {
        return NULL;
    }
    record = &owner->transfer_rx[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
}

ucn_i_transport_terminal_receipt_record_t *
ucn_i_transport_p_transfer_receipt(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_transport_terminal_receipt_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT ||
        handle.generation == 0U ||
        handle.object_kind != UCN_I_TRANSPORT_TRANSFER_RECEIPT_KIND) {
        return NULL;
    }
    record = &owner->transfer_receipts[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
}

ucn_i_transport_parent_record_t *ucn_i_transport_p_parent(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_transport_parent_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_TRANSPORT_PARENT_COUNT ||
        handle.generation == 0U ||
        handle.object_kind != UCN_I_TRANSPORT_PARENT_KIND) {
        return NULL;
    }
    record = &owner->parents[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
}

static bool object_zero(const void *object, size_t bytes)
{
    const uint8_t *value = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (value[index] != 0U) {
            return false;
        }
    }
    return true;
}

ucn_result_t ucn_i_transport_owner_init(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_config_t *config)
{
    if (owner == NULL || config == NULL || !object_zero(owner, sizeof(*owner)) ||
        config->runtime_instance == 0U || config->owner_instance == 0U ||
        config->reliable_lifetime_us == 0U ||
        config->reliable_retry_us == 0U ||
        config->receipt_lifetime_us == 0U ||
        config->reliable_retry_us >= config->reliable_lifetime_us ||
        config->reliable_max_attempts == 0U || config->reserved_zero != 0U ||
        !ucn_i_transport_p_lock_valid(&config->state_lock) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             config->state_lock.context, 1U)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_I_TRANSPORT_MAGIC;
    owner->runtime_instance = config->runtime_instance;
    owner->reliable_lifetime_us = config->reliable_lifetime_us;
    owner->reliable_retry_us = config->reliable_retry_us;
    owner->receipt_lifetime_us = config->receipt_lifetime_us;
    owner->schema = UCN_I_TRANSPORT_SCHEMA;
    owner->owner_instance = config->owner_instance;
    owner->reliable_max_attempts = config->reliable_max_attempts;
    owner->state_lock = config->state_lock;
    return UCN_OK;
}

ucn_result_t ucn_i_transport_owner_destroy(ucn_i_transport_owner_t *owner)
{
    ucn_result_t result = ucn_i_transport_p_lock(owner);
    size_t index;

    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_TRANSPORT_RELIABLE_TX_COUNT; ++index) {
        if (owner->reliable_tx[index].occupied != 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_TRANSPORT_RELIABLE_RX_COUNT; ++index) {
        if (owner->reliable_rx[index].occupied != 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_TRANSPORT_TRANSFER_TX_COUNT; ++index) {
        if (owner->transfer_tx[index].occupied != 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_TRANSPORT_TRANSFER_RX_COUNT; ++index) {
        if (owner->transfer_rx[index].occupied != 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT;
         ++index) {
        if (owner->transfer_receipts[index].occupied != 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_TRANSPORT_PARENT_COUNT; ++index) {
        if (owner->parents[index].occupied != 0U &&
            owner->parents[index].prepared != 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    /* Durable parent views are configuration state, not outstanding work.
     * Exclusive lifecycle destruction may drop their RAM cache; the durable
     * record remains owned by Persistence and is imported again on restart. */
    ucn_i_transport_p_unlock(owner);
    memset(owner, 0, sizeof(*owner));
    return UCN_OK;
}

static bool security_equal(const ucn_i_transport_security_facts_t *left,
                           const ucn_i_transport_security_facts_t *right)
{
    return left->session_generation == right->session_generation &&
           left->key_generation == right->key_generation &&
           left->link_generation == right->link_generation &&
           left->policy_generation == right->policy_generation &&
           left->link_id == right->link_id &&
           left->origin_security == right->origin_security &&
           left->endpoint_public_unauthenticated ==
               right->endpoint_public_unauthenticated &&
           left->trusted_link_policy == right->trusted_link_policy;
}

static bool ack_facts_match(
    const ucn_i_transport_reliable_tx_record_t *record,
    const ucn_i_transport_ack_facts_t *facts)
{
    return facts != NULL && facts->realm == record->key.realm &&
           facts->reserved_zero[0] == 0U &&
           facts->reserved_zero[1] == 0U &&
           facts->reserved_zero[2] == 0U &&
           facts->reserved_zero[3] == 0U &&
           binding_equal(&facts->source, &record->key.destination) &&
           binding_equal(&facts->destination, &record->key.source) &&
           ucn_i_transport_p_security_valid(&facts->security) &&
           security_equal(&facts->security, &record->security);
}

static ucn_result_t next_generation(uint16_t current, uint16_t *next_out)
{
    if (next_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (current == UINT16_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *next_out = (uint16_t)(current + 1U);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_reliable_begin(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_reliable_key_t *key,
    const ucn_i_transport_security_facts_t *security,
    const uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    const uint8_t payload_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    const uint8_t *sealed,
    uint16_t sealed_bytes,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    uint64_t deadline;
    uint64_t lifetime_us;
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_TRANSPORT_RELIABLE_TX_COUNT;
    bool exhausted_slot = false;

    if (!ucn_i_transport_p_key_valid(key) ||
        !ucn_i_transport_p_security_valid(security) || aad_digest == NULL ||
        payload_digest == NULL || sealed == NULL || sealed_bytes == 0U ||
        sealed_bytes > UCN_I_TRANSPORT_RELIABLE_BYTES || handle_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), key, sizeof(*key)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), security,
                             sizeof(*security)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), aad_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), sealed, sealed_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(security, sizeof(*security), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(aad_digest, 16U, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(payload_digest, 16U, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(sealed, sealed_bytes, handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    lifetime_us = owner->reliable_lifetime_us;
    result = ucn_i_deadline_from_duration_us(now_us, lifetime_us, &deadline);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    for (index = 0U; index < UCN_I_TRANSPORT_RELIABLE_TX_COUNT; ++index) {
        if (owner->reliable_tx[index].occupied == 0U) {
            if (owner->reliable_tx[index].generation == UINT16_MAX) {
                exhausted_slot = true;
            } else if (free_index == UCN_I_TRANSPORT_RELIABLE_TX_COUNT) {
                free_index = index;
            }
        } else if (ucn_i_transport_p_key_equal(&owner->reliable_tx[index].key,
                                                key)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    if (free_index == UCN_I_TRANSPORT_RELIABLE_TX_COUNT) {
        ucn_i_transport_p_unlock(owner);
        return exhausted_slot ? UCN_ERR_EXHAUSTED : UCN_ERR_NO_SPACE;
    }
    {
        ucn_i_transport_reliable_tx_record_t *record =
            &owner->reliable_tx[free_index];
        uint16_t generation;

        if (next_generation(record->generation, &generation) != UCN_OK) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_EXHAUSTED;
        }

        memset(record, 0, sizeof(*record));
        record->key = *key;
        record->security = *security;
        memcpy(record->aad_digest, aad_digest, 16U);
        memcpy(record->payload_digest, payload_digest, 16U);
        memcpy(record->sealed, sealed, sealed_bytes);
        record->deadline_us = deadline;
        record->next_retry_us = now_us;
        record->sealed_bytes = sealed_bytes;
        record->generation = generation;
        record->phase = UCN_I_TRANSPORT_RELIABLE_READY;
        record->occupied = 1U;
        *handle_out = ucn_i_transport_p_handle(
            owner, (uint16_t)free_index, generation,
            UCN_I_TRANSPORT_RELIABLE_TX_KIND);
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_reliable_copy_attempt(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint64_t now_us,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    ucn_i_transport_reliable_tx_record_t *record;
    ucn_result_t result;

    if (output == NULL || output_bytes == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                             output_capacity) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                             sizeof(*output_bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_reliable_tx(owner, handle);
    if (record == NULL || record->phase != UCN_I_TRANSPORT_RELIABLE_READY) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
        record->phase = UCN_I_TRANSPORT_RELIABLE_FAILED;
        record->terminal_result = UCN_ERR_TIMEOUT;
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    if (now_us < record->next_retry_us) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (output_capacity < record->sealed_bytes) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    memcpy(output, record->sealed, record->sealed_bytes);
    *output_bytes = record->sealed_bytes;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_reliable_note_submit(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    bool submitted,
    uint64_t now_us)
{
    ucn_i_transport_reliable_tx_record_t *record;
    uint64_t next_retry;
    ucn_result_t result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_reliable_tx(owner, handle);
    if (record == NULL || record->phase != UCN_I_TRANSPORT_RELIABLE_READY ||
        now_us < record->next_retry_us) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
        record->phase = UCN_I_TRANSPORT_RELIABLE_FAILED;
        record->terminal_result = UCN_ERR_TIMEOUT;
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    result = ucn_i_deadline_from_duration_us(
        now_us, owner->reliable_retry_us, &next_retry);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    record->next_retry_us = next_retry;
    if (submitted) {
        record->attempts = (uint8_t)(record->attempts + 1U);
        record->phase = UCN_I_TRANSPORT_RELIABLE_WAIT_ACK;
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_reliable_accept_ack(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_transport_delivery_ack_t *ack,
    const ucn_i_transport_ack_facts_t *facts,
    uint64_t now_us)
{
    ucn_i_transport_reliable_tx_record_t *record;
    ucn_result_t result;

    if (ack == NULL || facts == NULL || ack->service_id == 0U ||
        ack->origin_sequence == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), ack, sizeof(*ack)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_reliable_tx(owner, handle);
    if (record == NULL ||
        (record->phase != UCN_I_TRANSPORT_RELIABLE_WAIT_ACK &&
         record->phase != UCN_I_TRANSPORT_RELIABLE_READY) ||
        (record->phase == UCN_I_TRANSPORT_RELIABLE_READY &&
         record->attempts == 0U) ||
        record->key.service_id != ack->service_id ||
        record->key.origin_sequence != ack->origin_sequence ||
        !ack_facts_match(record, facts)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
        record->phase = UCN_I_TRANSPORT_RELIABLE_FAILED;
        record->terminal_result = UCN_ERR_TIMEOUT;
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    record->phase = UCN_I_TRANSPORT_RELIABLE_DONE;
    record->terminal_result = ack->status == 0U ? UCN_OK : UCN_ERR_STATE;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_reliable_receive(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_reliable_key_t *key,
    const ucn_i_transport_security_facts_t *security,
    const uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    const uint8_t payload_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    uint8_t ack_status,
    uint8_t receive_credit,
    uint64_t now_us,
    ucn_i_transport_receive_action_t *action_out)
{
    uint64_t expires_at;
    uint64_t receipt_lifetime_us;
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_TRANSPORT_RELIABLE_RX_COUNT;
    bool exhausted_slot = false;
    ucn_i_transport_receive_action_t action;

    if (!ucn_i_transport_p_key_valid(key) ||
        !ucn_i_transport_p_security_valid(security) || aad_digest == NULL ||
        payload_digest == NULL || action_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), key, sizeof(*key)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), security,
                             sizeof(*security)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), aad_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), action_out,
                             sizeof(*action_out)) ||
        ucn_i_ranges_overlap(key, sizeof(*key), action_out,
                             sizeof(*action_out)) ||
        ucn_i_ranges_overlap(security, sizeof(*security), action_out,
                             sizeof(*action_out)) ||
        ucn_i_ranges_overlap(aad_digest, 16U, action_out,
                             sizeof(*action_out)) ||
        ucn_i_ranges_overlap(payload_digest, 16U, action_out,
                             sizeof(*action_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    receipt_lifetime_us = owner->receipt_lifetime_us;
    result = ucn_i_deadline_from_duration_us(now_us, receipt_lifetime_us,
                                             &expires_at);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    for (index = 0U; index < UCN_I_TRANSPORT_RELIABLE_RX_COUNT; ++index) {
        ucn_i_transport_reliable_rx_record_t *record =
            &owner->reliable_rx[index];

        if (record->occupied == 0U) {
            if (record->generation == UINT16_MAX) {
                exhausted_slot = true;
            } else if (free_index == UCN_I_TRANSPORT_RELIABLE_RX_COUNT) {
                free_index = index;
            }
            continue;
        }
        if (ucn_i_deadline_expired_us(now_us, record->expires_at_us)) {
            uint16_t generation = record->generation;
            memset(record, 0, sizeof(*record));
            record->generation = generation;
            if (free_index == UCN_I_TRANSPORT_RELIABLE_RX_COUNT) {
                free_index = index;
            }
            continue;
        }
        if (!ucn_i_transport_p_key_equal(&record->key, key)) {
            continue;
        }
        if (!security_equal(&record->security, security) ||
            memcmp(record->aad_digest, aad_digest, 16U) != 0 ||
            memcmp(record->payload_digest, payload_digest, 16U) != 0) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_REPLAY;
        }
        if (security->origin_security != 0U &&
            security->authenticated_replay_candidate == 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_REPLAY;
        }
        memset(&action, 0, sizeof(action));
        action.ack = record->ack;
        action.exact_duplicate = 1U;
        *action_out = action;
        ucn_i_transport_p_unlock(owner);
        return UCN_OK;
    }
    if (security->authenticated_replay_candidate != 0U) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_REPLAY;
    }
    if (free_index == UCN_I_TRANSPORT_RELIABLE_RX_COUNT) {
        ucn_i_transport_p_unlock(owner);
        return exhausted_slot ? UCN_ERR_EXHAUSTED : UCN_ERR_NO_SPACE;
    }
    {
        ucn_i_transport_reliable_rx_record_t *record =
            &owner->reliable_rx[free_index];
        uint16_t generation;

        if (next_generation(record->generation, &generation) != UCN_OK) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_EXHAUSTED;
        }

        memset(record, 0, sizeof(*record));
        record->key = *key;
        record->security = *security;
        record->security.authenticated_replay_candidate = 0U;
        memcpy(record->aad_digest, aad_digest, 16U);
        memcpy(record->payload_digest, payload_digest, 16U);
        record->ack.service_id = key->service_id;
        record->ack.origin_sequence = key->origin_sequence;
        record->ack.status = ack_status;
        record->ack.receive_credit = receive_credit;
        record->expires_at_us = expires_at;
        record->generation = generation;
        record->occupied = 1U;
        memset(&action, 0, sizeof(action));
        action.ack = record->ack;
        action.deliver_to_application = 1U;
        *action_out = action;
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_reliable_view(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    ucn_i_transport_reliable_view_t *view_out)
{
    ucn_i_transport_reliable_tx_record_t *record;
    ucn_i_transport_reliable_view_t view;
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
    record = ucn_i_transport_p_reliable_tx(owner, handle);
    if (record == NULL) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    memset(&view, 0, sizeof(view));
    view.key = record->key;
    view.deadline_us = record->deadline_us;
    view.next_retry_us = record->next_retry_us;
    view.terminal_result = record->terminal_result;
    view.sealed_bytes = record->sealed_bytes;
    view.attempts = record->attempts;
    view.phase = record->phase;
    *view_out = view;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_reliable_cancel(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_transport_reliable_tx_record_t *record;
    ucn_result_t result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_reliable_tx(owner, handle);
    if (record == NULL || record->phase == UCN_I_TRANSPORT_RELIABLE_DONE ||
        record->phase == UCN_I_TRANSPORT_RELIABLE_FAILED ||
        record->phase == UCN_I_TRANSPORT_RELIABLE_CANCELLED) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->phase = UCN_I_TRANSPORT_RELIABLE_CANCELLED;
    record->terminal_result = UCN_ERR_CANCELLED;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_reliable_retire(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_i_transport_reliable_tx_record_t *record;
    uint16_t generation;
    ucn_result_t result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_reliable_tx(owner, handle);
    if (record == NULL ||
        (record->phase != UCN_I_TRANSPORT_RELIABLE_DONE &&
         record->phase != UCN_I_TRANSPORT_RELIABLE_FAILED &&
         record->phase != UCN_I_TRANSPORT_RELIABLE_CANCELLED)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    generation = record->generation;
    memset(record, 0, sizeof(*record));
    record->generation = generation;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

static bool maintain_reliable_tx(ucn_i_transport_owner_t *owner,
                                 size_t index,
                                 uint64_t now_us)
{
    ucn_i_transport_reliable_tx_record_t *record =
        &owner->reliable_tx[index];

    if (record->occupied == 0U ||
        record->phase == UCN_I_TRANSPORT_RELIABLE_DONE ||
        record->phase == UCN_I_TRANSPORT_RELIABLE_FAILED ||
        record->phase == UCN_I_TRANSPORT_RELIABLE_CANCELLED) {
        return false;
    }
    if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
        record->phase = UCN_I_TRANSPORT_RELIABLE_FAILED;
        record->terminal_result = UCN_ERR_TIMEOUT;
        return true;
    }
    if (record->phase == UCN_I_TRANSPORT_RELIABLE_WAIT_ACK &&
        now_us >= record->next_retry_us) {
        if (record->attempts >= owner->reliable_max_attempts) {
            record->phase = UCN_I_TRANSPORT_RELIABLE_FAILED;
            record->terminal_result = UCN_ERR_EXHAUSTED;
        } else {
            record->phase = UCN_I_TRANSPORT_RELIABLE_READY;
        }
        return true;
    }
    return false;
}

static void retire_transfer_receipt(
    ucn_i_transport_owner_t *owner,
    size_t receipt_slot)
{
    ucn_i_transport_terminal_receipt_record_t *receipt =
        &owner->transfer_receipts[receipt_slot];
    size_t index;

    for (index = 0U; index < UCN_I_TRANSPORT_TRANSFER_RX_COUNT; ++index) {
        ucn_i_transport_transfer_rx_record_t *rx =
            &owner->transfer_rx[index];

        if (rx->occupied != 0U &&
            rx->phase == UCN_I_TRANSPORT_TRANSFER_DELIVERED &&
            rx->receipt_slot == receipt_slot &&
            ucn_i_transport_p_setup_equal(&rx->setup, &receipt->setup)) {
            uint16_t generation = rx->generation;
            memset(rx, 0, sizeof(*rx));
            rx->generation = generation;
        }
    }
    {
        uint16_t generation = receipt->generation;
        memset(receipt, 0, sizeof(*receipt));
        receipt->generation = generation;
    }
}

ucn_result_t ucn_i_transport_maintain(
    ucn_i_transport_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *changed_out)
{
    const size_t total = UCN_I_TRANSPORT_RELIABLE_TX_COUNT +
                         UCN_I_TRANSPORT_RELIABLE_RX_COUNT +
                         UCN_I_TRANSPORT_TRANSFER_TX_COUNT +
                         UCN_I_TRANSPORT_TRANSFER_RX_COUNT +
                         UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT;
    uint16_t inspected = 0U;
    uint16_t changed = 0U;
    ucn_result_t result;

    if (budget == 0U || inspected_out == NULL || changed_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), changed_out,
                             sizeof(*changed_out)) ||
        ucn_i_ranges_overlap(inspected_out, sizeof(*inspected_out), changed_out,
                             sizeof(*changed_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    while (inspected < budget && inspected < total) {
        size_t cursor = owner->maintenance_cursor % total;
        bool did_change = false;

        if (cursor < UCN_I_TRANSPORT_RELIABLE_TX_COUNT) {
            did_change = maintain_reliable_tx(owner, cursor, now_us);
        } else if (cursor < UCN_I_TRANSPORT_RELIABLE_TX_COUNT +
                            UCN_I_TRANSPORT_RELIABLE_RX_COUNT) {
            size_t slot = cursor - UCN_I_TRANSPORT_RELIABLE_TX_COUNT;
            ucn_i_transport_reliable_rx_record_t *record =
                &owner->reliable_rx[slot];
            if (record->occupied != 0U &&
                ucn_i_deadline_expired_us(now_us, record->expires_at_us)) {
                uint16_t generation = record->generation;
                memset(record, 0, sizeof(*record));
                record->generation = generation;
                did_change = true;
            }
        } else if (cursor < UCN_I_TRANSPORT_RELIABLE_TX_COUNT +
                            UCN_I_TRANSPORT_RELIABLE_RX_COUNT +
                            UCN_I_TRANSPORT_TRANSFER_TX_COUNT) {
            size_t slot = cursor - UCN_I_TRANSPORT_RELIABLE_TX_COUNT -
                          UCN_I_TRANSPORT_RELIABLE_RX_COUNT;
            ucn_i_transport_transfer_tx_record_t *record =
                &owner->transfer_tx[slot];
            if (record->occupied != 0U &&
                record->phase != UCN_I_TRANSPORT_TRANSFER_COMPLETE &&
                record->phase != UCN_I_TRANSPORT_TRANSFER_ABORTED &&
                record->phase != UCN_I_TRANSPORT_TRANSFER_FAILED &&
                ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
                record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
                did_change = true;
            }
        } else if (cursor < UCN_I_TRANSPORT_RELIABLE_TX_COUNT +
                            UCN_I_TRANSPORT_RELIABLE_RX_COUNT +
                            UCN_I_TRANSPORT_TRANSFER_TX_COUNT +
                            UCN_I_TRANSPORT_TRANSFER_RX_COUNT) {
            size_t slot = cursor - UCN_I_TRANSPORT_RELIABLE_TX_COUNT -
                          UCN_I_TRANSPORT_RELIABLE_RX_COUNT -
                          UCN_I_TRANSPORT_TRANSFER_TX_COUNT;
            ucn_i_transport_transfer_rx_record_t *record =
                &owner->transfer_rx[slot];
            if (record->occupied != 0U &&
                record->phase != UCN_I_TRANSPORT_TRANSFER_COMPLETE &&
                record->phase != UCN_I_TRANSPORT_TRANSFER_DELIVERED &&
                record->phase != UCN_I_TRANSPORT_TRANSFER_ABORTED &&
                record->phase != UCN_I_TRANSPORT_TRANSFER_FAILED &&
                ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
                record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
                did_change = true;
            }
        } else {
            size_t slot = cursor - UCN_I_TRANSPORT_RELIABLE_TX_COUNT -
                          UCN_I_TRANSPORT_RELIABLE_RX_COUNT -
                          UCN_I_TRANSPORT_TRANSFER_TX_COUNT -
                          UCN_I_TRANSPORT_TRANSFER_RX_COUNT;
            ucn_i_transport_terminal_receipt_record_t *record =
                &owner->transfer_receipts[slot];
            if (record->occupied != 0U && record->terminal != 0U &&
                ucn_i_deadline_expired_us(now_us,
                                          record->expires_at_us)) {
                retire_transfer_receipt(owner, slot);
                did_change = true;
            }
        }
        owner->maintenance_cursor =
            (uint16_t)((owner->maintenance_cursor + 1U) % total);
        ++inspected;
        if (did_change) {
            ++changed;
        }
    }
    *inspected_out = inspected;
    *changed_out = changed;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}
