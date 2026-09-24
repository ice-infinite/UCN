#include "service/ucn_service_private.h"

#include "internal/ucn_checked.h"

#include <string.h>

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

static bool lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

ucn_result_t ucn_i_service_p_lock(ucn_i_service_owner_t *owner)
{
    if (owner == NULL || owner->magic != UCN_I_SERVICE_MAGIC ||
        owner->schema != UCN_I_SERVICE_SCHEMA ||
        !lock_valid(&owner->state_lock)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

void ucn_i_service_p_unlock(ucn_i_service_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
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

static bool binding_valid(const ucn_i_service_binding_t *binding)
{
    return binding != NULL && binding->address != 0U &&
           binding->generation != 0U &&
           bytes_nonzero(binding->principal, sizeof(binding->principal));
}

static bool binding_equal(const ucn_i_service_binding_t *left,
                          const ucn_i_service_binding_t *right)
{
    return left->address == right->address &&
           left->generation == right->generation &&
           memcmp(left->principal, right->principal,
                  sizeof(left->principal)) == 0;
}

static bool security_valid(const ucn_i_service_security_facts_t *security)
{
    return security != NULL && security->session_generation != 0U &&
           security->key_generation != 0U &&
           security->policy_generation != 0U &&
           security->origin_security >= 1U &&
           security->origin_security <= 2U &&
           security->acl_authorized == 1U &&
           security->authenticated_replay_candidate <= 1U &&
           security->reserved_zero == 0U;
}

static bool security_equal(const ucn_i_service_security_facts_t *left,
                           const ucn_i_service_security_facts_t *right)
{
    return left->session_generation == right->session_generation &&
           left->key_generation == right->key_generation &&
           left->policy_generation == right->policy_generation &&
           left->origin_security == right->origin_security &&
           left->acl_authorized == right->acl_authorized;
}

bool ucn_i_service_p_key_valid(const ucn_i_service_key_t *key)
{
    return key != NULL && binding_valid(&key->client) &&
           binding_valid(&key->server) && security_valid(&key->security) &&
           key->operation_id != 0U && key->realm != 0U &&
           key->service_id != 0U && key->opcode != 0U;
}

bool ucn_i_service_p_key_equal(const ucn_i_service_key_t *left,
                               const ucn_i_service_key_t *right)
{
    return binding_equal(&left->client, &right->client) &&
           binding_equal(&left->server, &right->server) &&
           security_equal(&left->security, &right->security) &&
           left->operation_id == right->operation_id &&
           left->realm == right->realm &&
           left->service_id == right->service_id &&
           left->opcode == right->opcode;
}

bool ucn_i_service_p_response_key_matches(
    const ucn_i_service_key_t *request,
    const ucn_i_service_key_t *response)
{
    return ucn_i_service_p_key_valid(response) &&
           binding_equal(&request->client, &response->server) &&
           binding_equal(&request->server, &response->client) &&
           security_equal(&request->security, &response->security) &&
           request->operation_id == response->operation_id &&
           request->realm == response->realm &&
           request->service_id == response->service_id &&
           request->opcode == response->opcode;
}

ucn_handle_t ucn_i_service_p_handle(const ucn_i_service_owner_t *owner,
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

ucn_i_service_request_record_t *ucn_i_service_p_request(
    ucn_i_service_owner_t *owner, ucn_handle_t handle)
{
    ucn_i_service_request_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_SERVICE_REQUEST_COUNT ||
        handle.generation == 0U ||
        handle.object_kind != UCN_I_SERVICE_REQUEST_KIND) {
        return NULL;
    }
    record = &owner->requests[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
}

ucn_i_service_receipt_record_t *ucn_i_service_p_receipt(
    ucn_i_service_owner_t *owner, ucn_handle_t handle)
{
    ucn_i_service_receipt_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_SERVICE_RECEIPT_COUNT ||
        handle.generation == 0U ||
        handle.object_kind != UCN_I_SERVICE_RECEIPT_KIND) {
        return NULL;
    }
    record = &owner->receipts[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
}

ucn_i_service_qos_record_t *ucn_i_service_p_qos(
    ucn_i_service_owner_t *owner, ucn_handle_t handle)
{
    ucn_i_service_qos_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_SERVICE_QOS_COUNT || handle.generation == 0U ||
        handle.object_kind != UCN_I_SERVICE_QOS_KIND) {
        return NULL;
    }
    record = &owner->qos[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
}

ucn_i_service_operation_record_t *ucn_i_service_p_operation(
    ucn_i_service_owner_t *owner, ucn_handle_t handle)
{
    ucn_i_service_operation_record_t *record;

    if (handle.reserved_zero != 0U ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_SERVICE_OPERATION_COUNT ||
        handle.generation == 0U ||
        handle.object_kind != UCN_I_SERVICE_OPERATION_KIND) {
        return NULL;
    }
    record = &owner->operations[handle.slot];
    return record->occupied != 0U && record->generation == handle.generation ?
               record : NULL;
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

static void result_freeze(ucn_i_service_result_t *destination,
                          const ucn_i_service_result_t *source)
{
    memset(destination, 0, sizeof(*destination));
    destination->application_result = source->application_result;
    destination->bytes = source->bytes;
    if (source->bytes != 0U) {
        memcpy(destination->payload, source->payload, source->bytes);
    }
}

ucn_result_t ucn_i_service_owner_init(ucn_i_service_owner_t *owner,
                                      const ucn_i_service_config_t *config)
{
    if (owner == NULL || config == NULL || !object_zero(owner, sizeof(*owner)) ||
        config->runtime_instance == 0U || config->owner_instance == 0U ||
        config->receipt_lifetime_us == 0U || config->reserved_zero != 0U ||
        !lock_valid(&config->state_lock) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             config->state_lock.context, 1U)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_I_SERVICE_MAGIC;
    owner->runtime_instance = config->runtime_instance;
    owner->receipt_lifetime_us = config->receipt_lifetime_us;
    owner->next_enqueue_order = 1U;
    owner->operation_id_next = 1U;
    owner->schema = UCN_I_SERVICE_SCHEMA;
    owner->owner_instance = config->owner_instance;
    owner->state_lock = config->state_lock;
    return UCN_OK;
}

ucn_result_t ucn_i_service_owner_destroy(ucn_i_service_owner_t *owner)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    size_t index;

    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_SERVICE_REQUEST_COUNT; ++index) {
        if (owner->requests[index].occupied != 0U) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_SERVICE_RECEIPT_COUNT; ++index) {
        if (owner->receipts[index].occupied != 0U) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_SERVICE_QOS_COUNT; ++index) {
        if (owner->qos[index].occupied != 0U) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_SERVICE_OPERATION_COUNT; ++index) {
        if (owner->operations[index].occupied != 0U) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    if (owner->operation_id_persist_pending != 0U ||
        owner->operation_id_persistence_bound != 0U) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    ucn_i_service_p_unlock(owner);
    memset(owner, 0, sizeof(*owner));
    return UCN_OK;
}

ucn_result_t ucn_i_service_request_begin(ucn_i_service_owner_t *owner,
                                         const ucn_i_service_key_t *key,
                                         uint64_t deadline_us,
                                         ucn_handle_t *handle_out)
{
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_SERVICE_REQUEST_COUNT;
    uint16_t generation;

    if (!ucn_i_service_p_key_valid(key) || handle_out == NULL ||
        deadline_us == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_SERVICE_REQUEST_COUNT; ++index) {
        if (owner->requests[index].occupied != 0U &&
            owner->requests[index].key.operation_id == key->operation_id &&
            owner->requests[index].key.realm == key->realm) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        if (owner->requests[index].occupied == 0U &&
            free_index == UCN_I_SERVICE_REQUEST_COUNT) {
            free_index = index;
        }
    }
    if (free_index == UCN_I_SERVICE_REQUEST_COUNT) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (next_generation(owner->requests[free_index].generation,
                        &generation) != UCN_OK) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    memset(&owner->requests[free_index], 0,
           sizeof(owner->requests[free_index]));
    owner->requests[free_index].key = *key;
    owner->requests[free_index].deadline_us = deadline_us;
    owner->requests[free_index].terminal_result = UCN_ERR_STATE;
    owner->requests[free_index].generation = generation;
    owner->requests[free_index].occupied = 1U;
    owner->requests[free_index].phase = UCN_I_SERVICE_REQUEST_WAIT_SEND;
    *handle_out = ucn_i_service_p_handle(owner, (uint16_t)free_index,
                                         generation,
                                         UCN_I_SERVICE_REQUEST_KIND);
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_request_note_sent(ucn_i_service_owner_t *owner,
                                             ucn_handle_t handle,
                                             uint64_t now_us)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_request_record_t *record;

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_request(owner, handle);
    if (record == NULL || record->phase != UCN_I_SERVICE_REQUEST_WAIT_SEND) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (now_us >= record->deadline_us) {
        record->phase = UCN_I_SERVICE_REQUEST_FAILED;
        record->terminal_result = UCN_ERR_TIMEOUT;
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    record->phase = UCN_I_SERVICE_REQUEST_WAIT_RESULT;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_request_accept_result(
    ucn_i_service_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_service_key_t *response_key,
    const ucn_i_service_result_t *result_value,
    uint64_t now_us)
{
    ucn_result_t result;
    ucn_i_service_request_record_t *record;

    if (response_key == NULL || result_value == NULL ||
        result_value->bytes > UCN_I_SERVICE_RESULT_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_request(owner, handle);
    if (record == NULL || record->phase != UCN_I_SERVICE_REQUEST_WAIT_RESULT ||
        !ucn_i_service_p_response_key_matches(&record->key, response_key)) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (response_key->security.authenticated_replay_candidate != 0U ||
        now_us >= record->deadline_us) {
        if (now_us >= record->deadline_us) {
            record->phase = UCN_I_SERVICE_REQUEST_FAILED;
            record->terminal_result = UCN_ERR_TIMEOUT;
        }
        ucn_i_service_p_unlock(owner);
        return now_us >= record->deadline_us ? UCN_ERR_TIMEOUT : UCN_ERR_REPLAY;
    }
    result_freeze(&record->result, result_value);
    record->terminal_result = result_value->application_result;
    record->phase = UCN_I_SERVICE_REQUEST_COMPLETE;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_request_view(ucn_i_service_owner_t *owner,
                                        ucn_handle_t handle,
                                        ucn_i_service_request_view_t *view_out)
{
    ucn_result_t result;
    ucn_i_service_request_record_t *record;

    if (view_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_request(owner, handle);
    if (record == NULL) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    memset(view_out, 0, sizeof(*view_out));
    view_out->key = record->key;
    view_out->deadline_us = record->deadline_us;
    view_out->terminal_result = record->terminal_result;
    view_out->result_bytes = record->result.bytes;
    view_out->phase = record->phase;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_request_copy_result(
    ucn_i_service_owner_t *owner,
    ucn_handle_t handle,
    ucn_i_service_result_t *result_out)
{
    ucn_result_t result;
    ucn_i_service_request_record_t *record;

    if (result_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_request(owner, handle);
    if (record == NULL || record->phase != UCN_I_SERVICE_REQUEST_COMPLETE) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    *result_out = record->result;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_request_cancel(ucn_i_service_owner_t *owner,
                                          ucn_handle_t handle)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_request_record_t *record;

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_request(owner, handle);
    if (record == NULL || record->phase == UCN_I_SERVICE_REQUEST_COMPLETE ||
        record->phase == UCN_I_SERVICE_REQUEST_FAILED ||
        record->phase == UCN_I_SERVICE_REQUEST_CANCELLED) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->phase = UCN_I_SERVICE_REQUEST_CANCELLED;
    record->terminal_result = UCN_ERR_CANCELLED;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_request_retire(ucn_i_service_owner_t *owner,
                                          ucn_handle_t handle)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_request_record_t *record;

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_request(owner, handle);
    if (record == NULL ||
        (record->phase != UCN_I_SERVICE_REQUEST_COMPLETE &&
         record->phase != UCN_I_SERVICE_REQUEST_FAILED &&
         record->phase != UCN_I_SERVICE_REQUEST_CANCELLED)) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->occupied = 0U;
    record->phase = 0U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_receive_request(
    ucn_i_service_owner_t *owner,
    const ucn_i_service_key_t *key,
    const uint8_t request_digest[UCN_I_SERVICE_DIGEST_BYTES],
    uint64_t now_us,
    ucn_handle_t *receipt_out,
    ucn_i_service_receive_action_t *action_out)
{
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_SERVICE_RECEIPT_COUNT;
    uint16_t generation;

    if (!ucn_i_service_p_key_valid(key) || request_digest == NULL ||
        !bytes_nonzero(request_digest, UCN_I_SERVICE_DIGEST_BYTES) ||
        receipt_out == NULL || action_out == NULL ||
        key->security.authenticated_replay_candidate != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_SERVICE_RECEIPT_COUNT; ++index) {
        ucn_i_service_receipt_record_t *record = &owner->receipts[index];

        if (record->occupied != 0U &&
            (record->terminal == 0U || record->expires_at_us > now_us) &&
            record->key.operation_id == key->operation_id &&
            record->key.realm == key->realm) {
            if (!ucn_i_service_p_key_equal(&record->key, key) ||
                memcmp(record->request_digest, request_digest,
                       UCN_I_SERVICE_DIGEST_BYTES) != 0) {
                ucn_i_service_p_unlock(owner);
                return UCN_ERR_REPLAY;
            }
            if (record->terminal == 0U) {
                ucn_i_service_p_unlock(owner);
                return UCN_ERR_REPLAY;
            }
            *receipt_out = ucn_i_service_p_handle(
                owner, (uint16_t)index, record->generation,
                UCN_I_SERVICE_RECEIPT_KIND);
            *action_out = UCN_I_SERVICE_RECEIVE_REPLAY_RESULT;
            ucn_i_service_p_unlock(owner);
            return UCN_OK;
        }
        if (record->occupied == 0U &&
            free_index == UCN_I_SERVICE_RECEIPT_COUNT) {
            free_index = index;
        }
    }
    if (free_index == UCN_I_SERVICE_RECEIPT_COUNT) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (next_generation(owner->receipts[free_index].generation,
                        &generation) != UCN_OK) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    memset(&owner->receipts[free_index], 0,
           sizeof(owner->receipts[free_index]));
    owner->receipts[free_index].key = *key;
    memcpy(owner->receipts[free_index].request_digest, request_digest,
           UCN_I_SERVICE_DIGEST_BYTES);
    owner->receipts[free_index].expires_at_us = 0U;
    owner->receipts[free_index].generation = generation;
    owner->receipts[free_index].occupied = 1U;
    *receipt_out = ucn_i_service_p_handle(owner, (uint16_t)free_index,
                                          generation,
                                          UCN_I_SERVICE_RECEIPT_KIND);
    *action_out = UCN_I_SERVICE_RECEIVE_INVOKE;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_commit_result(
    ucn_i_service_owner_t *owner,
    ucn_handle_t receipt,
    const ucn_i_service_result_t *result_value,
    uint64_t now_us)
{
    ucn_result_t result;
    ucn_i_service_receipt_record_t *record;

    if (result_value == NULL || result_value->bytes > UCN_I_SERVICE_RESULT_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_receipt(owner, receipt);
    if (record == NULL || record->terminal != 0U) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (UINT64_MAX - now_us < owner->receipt_lifetime_us) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    result_freeze(&record->result, result_value);
    record->expires_at_us = now_us + owner->receipt_lifetime_us;
    record->terminal = 1U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_receipt_copy_result(
    ucn_i_service_owner_t *owner,
    ucn_handle_t receipt,
    ucn_i_service_result_t *result_out)
{
    ucn_result_t result;
    ucn_i_service_receipt_record_t *record;

    if (result_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_receipt(owner, receipt);
    if (record == NULL || record->terminal == 0U) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    *result_out = record->result;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_maintain(ucn_i_service_owner_t *owner,
                                    uint64_t now_us,
                                    uint16_t budget,
                                    uint16_t *inspected_out,
                                    uint16_t *changed_out)
{
    ucn_result_t result;
    uint16_t inspected = 0U;
    uint16_t changed = 0U;
    const uint16_t total = (uint16_t)(UCN_I_SERVICE_REQUEST_COUNT +
                                     UCN_I_SERVICE_RECEIPT_COUNT +
                                     UCN_I_SERVICE_QOS_COUNT);
    uint16_t limit;

    if (budget == 0U || inspected_out == NULL || changed_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    limit = budget < total ? budget : total;
    while (inspected < limit) {
        uint16_t cursor = owner->maintain_cursor;
        owner->maintain_cursor = (uint16_t)((cursor + 1U) % total);
        ++inspected;
        if (cursor < UCN_I_SERVICE_REQUEST_COUNT) {
            ucn_i_service_request_record_t *record = &owner->requests[cursor];
            if (record->occupied != 0U &&
                (record->phase == UCN_I_SERVICE_REQUEST_WAIT_SEND ||
                 record->phase == UCN_I_SERVICE_REQUEST_WAIT_RESULT) &&
                now_us >= record->deadline_us) {
                record->phase = UCN_I_SERVICE_REQUEST_FAILED;
                record->terminal_result = UCN_ERR_TIMEOUT;
                ++changed;
            }
        } else if (cursor < UCN_I_SERVICE_REQUEST_COUNT +
                              UCN_I_SERVICE_RECEIPT_COUNT) {
            size_t slot = (size_t)(cursor - UCN_I_SERVICE_REQUEST_COUNT);
            ucn_i_service_receipt_record_t *record = &owner->receipts[slot];
            if (record->occupied != 0U && record->terminal != 0U &&
                now_us >= record->expires_at_us) {
                record->occupied = 0U;
                record->terminal = 0U;
                ++changed;
            }
        } else {
            size_t slot = (size_t)(cursor - UCN_I_SERVICE_REQUEST_COUNT -
                                   UCN_I_SERVICE_RECEIPT_COUNT);
            ucn_i_service_qos_record_t *record = &owner->qos[slot];
            if (record->occupied != 0U && record->selected == 0U &&
                record->submitted == 0U && record->item.deadline_us != 0U &&
                now_us >= record->item.deadline_us) {
                record->occupied = 0U;
                record->selected = 0U;
                ++changed;
            }
        }
    }
    *inspected_out = inspected;
    *changed_out = changed;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}
