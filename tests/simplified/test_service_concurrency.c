#include "internal/ucn_service.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ITERATIONS 3000U

typedef struct pthread_lock { pthread_mutex_t mutex; } pthread_lock_t;
static pthread_lock_t owner_lock;
static ucn_i_service_owner_t owner;
static ucn_handle_t request_handle;
static ucn_handle_t qos_handle;

static ucn_result_t mutex_enter(void *context)
{
    pthread_lock_t *lock = context;
    return lock != NULL && pthread_mutex_lock(&lock->mutex) == 0 ?
               UCN_OK : UCN_ERR_STATE;
}
static void mutex_leave(void *context)
{
    (void)pthread_mutex_unlock(&((pthread_lock_t *)context)->mutex);
}

static void fill_principal(uint8_t value[16], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) value[index] = (uint8_t)(seed + index);
}

static int configure(void)
{
    ucn_i_service_config_t config;
    ucn_i_service_key_t key;
    ucn_i_service_qos_item_t item;
    ucn_handle_t superseded;

    memset(&owner, 0, sizeof(owner));
    memset(&owner_lock, 0, sizeof(owner_lock));
    if (pthread_mutex_init(&owner_lock.mutex, NULL) != 0) return __LINE__;
    memset(&config, 0, sizeof(config));
    config.receipt_lifetime_us = 1000U;
    config.runtime_instance = 1U;
    config.owner_instance = 2U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &owner_lock;
    config.state_lock.enter = mutex_enter;
    config.state_lock.leave = mutex_leave;
    if (ucn_i_service_owner_init(&owner, &config) != UCN_OK) return __LINE__;
    memset(&key, 0, sizeof(key));
    key.client.address = 1U; key.client.generation = 1U;
    key.server.address = 2U; key.server.generation = 1U;
    fill_principal(key.client.principal, 0x10U);
    fill_principal(key.server.principal, 0x30U);
    key.security.session_generation = 1U;
    key.security.key_generation = 1U;
    key.security.policy_generation = 1U;
    key.security.origin_security = 1U;
    key.security.acl_authorized = 1U;
    key.operation_id = 1U; key.realm = 1U;
    key.service_id = 1U; key.opcode = 1U;
    if (ucn_i_service_request_begin(&owner, &key, 1000000U,
                                    &request_handle) != UCN_OK) return __LINE__;
    memset(&item, 0, sizeof(item));
    item.sendable.runtime_instance = 5U;
    item.sendable.owner_instance = 6U;
    item.sendable.generation = 1U;
    item.sendable.object_kind = UCN_OBJECT_KIND_SEND;
    item.source_quota_key = 1U;
    item.flow_quota_key = 1U;
    item.traffic_class = 2U;
    if (ucn_i_service_qos_enqueue(&owner, &item, &qos_handle,
                                  &superseded) != UCN_OK) return __LINE__;
    return 0;
}

static void *reader(void *unused)
{
    unsigned index;
    (void)unused;
    for (index = 0U; index < ITERATIONS; ++index) {
        ucn_i_service_request_view_t view;
        if (ucn_i_service_request_view(&owner, request_handle, &view) != UCN_OK ||
            view.key.operation_id != 1U) return (void *)(uintptr_t)__LINE__;
    }
    return NULL;
}

static void *scheduler(void *unused)
{
    unsigned index;
    (void)unused;
    for (index = 0U; index < ITERATIONS; ++index) {
        ucn_handle_t picked;
        ucn_handle_t sendable;
        ucn_result_t result = ucn_i_service_qos_pick(
            &owner, 0U, &picked, &sendable);
        if (result == UCN_ERR_NOT_FOUND) {
            sched_yield();
            continue;
        }
        if (result != UCN_OK ||
            ucn_i_service_qos_release_pick(&owner, picked) != UCN_OK) {
            return (void *)(uintptr_t)__LINE__;
        }
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    void *thread_result;
    size_t index;
    int result = configure();
    if (result != 0) return result;
    if (pthread_create(&threads[0], NULL, reader, NULL) != 0 ||
        pthread_create(&threads[1], NULL, scheduler, NULL) != 0 ||
        pthread_create(&threads[2], NULL, reader, NULL) != 0 ||
        pthread_create(&threads[3], NULL, scheduler, NULL) != 0) return __LINE__;
    for (index = 0U; index < 4U; ++index) {
        if (pthread_join(threads[index], &thread_result) != 0 ||
            thread_result != NULL) return thread_result == NULL ?
                __LINE__ : (int)(uintptr_t)thread_result;
    }
    if (ucn_i_service_request_cancel(&owner, request_handle) != UCN_OK ||
        ucn_i_service_request_retire(&owner, request_handle) != UCN_OK ||
        ucn_i_service_qos_remove(&owner, qos_handle) != UCN_OK ||
        ucn_i_service_owner_destroy(&owner) != UCN_OK ||
        pthread_mutex_destroy(&owner_lock.mutex) != 0) return __LINE__;
    puts("service concurrency tests passed");
    return 0;
}
