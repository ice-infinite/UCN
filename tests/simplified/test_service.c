#include "internal/ucn_service.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression_) do { if (!(expression_)) return __LINE__; } while (0)

typedef struct test_lock { int held; } test_lock_t;
static test_lock_t lock_state;
static ucn_i_service_owner_t owner;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0) return UCN_ERR_STATE;
    lock->held = 1;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = context;
    if (lock != NULL) lock->held = 0;
}

static void fill_principal(uint8_t value[16], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) value[index] = (uint8_t)(seed + index);
}

static ucn_i_service_key_t request_key(void)
{
    ucn_i_service_key_t key;
    memset(&key, 0, sizeof(key));
    key.client.address = 1U;
    key.client.generation = 11U;
    fill_principal(key.client.principal, 0x10U);
    key.server.address = 2U;
    key.server.generation = 22U;
    fill_principal(key.server.principal, 0x30U);
    key.security.session_generation = 3U;
    key.security.key_generation = 4U;
    key.security.policy_generation = 5U;
    key.security.origin_security = 1U;
    key.security.acl_authorized = 1U;
    key.operation_id = 0x1122334455667788ULL;
    key.realm = 7U;
    key.service_id = 0x1234U;
    key.opcode = 0x55U;
    return key;
}

static ucn_i_service_key_t response_key(void)
{
    ucn_i_service_key_t key = request_key();
    ucn_i_service_binding_t swap = key.client;
    key.client = key.server;
    key.server = swap;
    return key;
}

static int configure(void)
{
    ucn_i_service_config_t config;
    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&config, 0, sizeof(config));
    config.receipt_lifetime_us = 1000U;
    config.runtime_instance = 1U;
    config.owner_instance = 7U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &lock_state;
    config.state_lock.enter = lock_enter;
    config.state_lock.leave = lock_leave;
    return ucn_i_service_owner_init(&owner, &config) == UCN_OK ? 0 : __LINE__;
}

static int test_client_exact_result(void)
{
    ucn_i_service_key_t request = request_key();
    ucn_i_service_key_t response = response_key();
    ucn_i_service_key_t wrong = response;
    ucn_i_service_result_t result;
    ucn_i_service_result_t copied;
    ucn_i_service_request_view_t view;
    ucn_handle_t handle;

    memset(&result, 0xCC, sizeof(result));
    result.application_result = UCN_OK;
    result.bytes = 3U;
    memcpy(result.payload, "yes", 3U);
    CHECK(ucn_i_service_request_begin(&owner, &request, 100U, &handle) == UCN_OK);
    CHECK(ucn_i_service_request_note_sent(&owner, handle, 1U) == UCN_OK);
    wrong.client.generation++;
    CHECK(ucn_i_service_request_accept_result(&owner, handle, &wrong,
                                               &result, 2U) == UCN_ERR_STATE);
    response.security.authenticated_replay_candidate = 1U;
    CHECK(ucn_i_service_request_accept_result(&owner, handle, &response,
                                               &result, 2U) == UCN_ERR_REPLAY);
    response.security.authenticated_replay_candidate = 0U;
    CHECK(ucn_i_service_request_accept_result(&owner, handle, &response,
                                               &result, 2U) == UCN_OK);
    CHECK(ucn_i_service_request_view(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SERVICE_REQUEST_COMPLETE && view.result_bytes == 3U);
    CHECK(ucn_i_service_request_copy_result(&owner, handle, &copied) == UCN_OK);
    CHECK(copied.bytes == 3U && memcmp(copied.payload, "yes", 3U) == 0);
    CHECK(copied.payload[3] == 0U);
    CHECK(ucn_i_service_request_retire(&owner, handle) == UCN_OK);
    return 0;
}

static int test_server_dedup_and_conflict(void)
{
    ucn_i_service_key_t key = request_key();
    ucn_i_service_result_t result;
    ucn_i_service_result_t copied;
    ucn_i_service_receive_action_t action;
    ucn_handle_t receipt;
    ucn_handle_t duplicate;
    uint8_t digest[16];
    uint8_t conflict[16];
    ucn_handle_t duplicate_sentinel;
    ucn_i_service_receive_action_t action_sentinel;
    uint16_t inspected;
    uint16_t changed;

    memset(digest, 0xA5, sizeof(digest));
    memset(conflict, 0x5A, sizeof(conflict));
    memset(&result, 0, sizeof(result));
    result.application_result = UCN_OK;
    result.bytes = 2U;
    memcpy(result.payload, "ok", 2U);
    CHECK(ucn_i_service_receive_request(&owner, &key, digest, 10U,
                                        &receipt, &action) == UCN_OK);
    CHECK(action == UCN_I_SERVICE_RECEIVE_INVOKE);
    memset(&duplicate_sentinel, 0xA5, sizeof(duplicate_sentinel));
    duplicate = duplicate_sentinel;
    action_sentinel = UINT8_C(0xA5);
    action = action_sentinel;
    CHECK(ucn_i_service_receive_request(&owner, &key, digest, 11U,
                                        &duplicate, &action) == UCN_ERR_REPLAY);
    CHECK(memcmp(&duplicate, &duplicate_sentinel, sizeof(duplicate)) == 0);
    CHECK(action == action_sentinel);
    CHECK(ucn_i_service_maintain(&owner, 100000U, UINT16_MAX,
                                 &inspected, &changed) == UCN_OK);
    CHECK(changed == 0U);
    CHECK(ucn_i_service_receive_request(&owner, &key, conflict, 11U,
                                        &duplicate, &action) == UCN_ERR_REPLAY);
    CHECK(ucn_i_service_commit_result(&owner, receipt, &result, 12U) == UCN_OK);
    CHECK(ucn_i_service_receive_request(&owner, &key, digest, 13U,
                                        &duplicate, &action) == UCN_OK);
    CHECK(action == UCN_I_SERVICE_RECEIVE_REPLAY_RESULT);
    CHECK(ucn_i_service_receipt_copy_result(&owner, duplicate, &copied) == UCN_OK);
    CHECK(copied.bytes == 2U && memcmp(copied.payload, "ok", 2U) == 0);
    CHECK(ucn_i_service_maintain(&owner, 1012U, UINT16_MAX,
                                 &inspected, &changed) == UCN_OK);
    CHECK(changed == 1U);
    return 0;
}

static int test_timeout(void)
{
    ucn_i_service_key_t key = request_key();
    ucn_i_service_request_view_t view;
    ucn_handle_t handle;
    uint16_t inspected;
    uint16_t changed;

    key.operation_id++;
    CHECK(ucn_i_service_request_begin(&owner, &key, 50U, &handle) == UCN_OK);
    CHECK(ucn_i_service_maintain(&owner, 50U, UINT16_MAX,
                                 &inspected, &changed) == UCN_OK);
    CHECK(changed == 1U);
    CHECK(ucn_i_service_request_view(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SERVICE_REQUEST_FAILED &&
          view.terminal_result == UCN_ERR_TIMEOUT);
    CHECK(ucn_i_service_request_retire(&owner, handle) == UCN_OK);
    return 0;
}

static int test_bounded_maintenance_rotates(void)
{
    ucn_i_service_key_t first = request_key();
    ucn_i_service_key_t second = request_key();
    ucn_i_service_request_view_t view;
    ucn_handle_t first_handle;
    ucn_handle_t second_handle;
    uint16_t inspected;
    uint16_t changed;

    first.operation_id += 10U;
    second.operation_id += 11U;
    CHECK(ucn_i_service_request_begin(&owner, &first, 1000U,
                                      &first_handle) == UCN_OK);
    CHECK(ucn_i_service_request_begin(&owner, &second, 10U,
                                      &second_handle) == UCN_OK);
    CHECK(ucn_i_service_maintain(&owner, 10U, 1U,
                                 &inspected, &changed) == UCN_OK);
    CHECK(inspected == 1U && changed == 0U);
    CHECK(ucn_i_service_maintain(&owner, 10U, 1U,
                                 &inspected, &changed) == UCN_OK);
    CHECK(inspected == 1U && changed == 1U);
    CHECK(ucn_i_service_request_view(&owner, second_handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SERVICE_REQUEST_FAILED &&
          view.terminal_result == UCN_ERR_TIMEOUT);
    CHECK(ucn_i_service_request_cancel(&owner, first_handle) == UCN_OK);
    CHECK(ucn_i_service_request_retire(&owner, first_handle) == UCN_OK);
    CHECK(ucn_i_service_request_retire(&owner, second_handle) == UCN_OK);
    return 0;
}

int main(void)
{
    int result = configure();
    if (result == 0) result = test_client_exact_result();
    if (result == 0) result = test_server_dedup_and_conflict();
    if (result == 0) result = test_timeout();
    if (result == 0) result = test_bounded_maintenance_rotates();
    if (result == 0 && ucn_i_service_owner_destroy(&owner) != UCN_OK) result = __LINE__;
    if (result != 0) return result;
    puts("service tests passed");
    return 0;
}
