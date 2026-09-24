#include "internal/ucn_transport.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ITERATIONS 4000U

typedef struct pthread_lock {
    pthread_mutex_t mutex;
} pthread_lock_t;

static ucn_i_transport_owner_t owner;
static pthread_lock_t owner_lock;
static ucn_handle_t reliable_handle;

static ucn_result_t mutex_enter(void *context)
{
    pthread_lock_t *lock = context;

    return lock != NULL && pthread_mutex_lock(&lock->mutex) == 0 ?
               UCN_OK : UCN_ERR_STATE;
}

static void mutex_leave(void *context)
{
    pthread_lock_t *lock = context;

    if (lock != NULL) {
        (void)pthread_mutex_unlock(&lock->mutex);
    }
}

static void fill_principal(uint8_t principal[16], uint8_t seed)
{
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        principal[index] = (uint8_t)(seed + index);
    }
}

static int configure(void)
{
    static const uint8_t sealed[] = {1U, 2U, 3U, 4U};
    ucn_i_transport_config_t config;
    ucn_i_transport_reliable_key_t key;
    ucn_i_transport_security_facts_t security;
    uint8_t aad[16];
    uint8_t payload[16];

    memset(&owner, 0, sizeof(owner));
    memset(&owner_lock, 0, sizeof(owner_lock));
    if (pthread_mutex_init(&owner_lock.mutex, NULL) != 0) {
        return __LINE__;
    }
    memset(&config, 0, sizeof(config));
    config.reliable_lifetime_us = 1000000U;
    config.reliable_retry_us = 1000U;
    config.receipt_lifetime_us = 1000U;
    config.runtime_instance = 1U;
    config.owner_instance = 2U;
    config.reliable_max_attempts = 3U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &owner_lock;
    config.state_lock.enter = mutex_enter;
    config.state_lock.leave = mutex_leave;
    if (ucn_i_transport_owner_init(&owner, &config) != UCN_OK) {
        return __LINE__;
    }
    memset(&key, 0, sizeof(key));
    key.source.address = 1U;
    key.source.generation = 1U;
    fill_principal(key.source.principal, 0x10U);
    key.destination.address = 2U;
    key.destination.generation = 1U;
    fill_principal(key.destination.principal, 0x30U);
    key.realm = 1U;
    key.origin_sequence = 1U;
    key.service_id = 1U;
    key.delivery = UCN_DELIVERY_RELIABLE;
    memset(&security, 0, sizeof(security));
    security.link_generation = 1U;
    security.policy_generation = 1U;
    security.link_id = 1U;
    security.endpoint_public_unauthenticated = 1U;
    security.trusted_link_policy = 1U;
    memset(aad, 0xA1, sizeof(aad));
    memset(payload, 0xB2, sizeof(payload));
    return ucn_i_transport_reliable_begin(
               &owner, &key, &security, aad, payload, sealed,
               sizeof(sealed), 0U, &reliable_handle) == UCN_OK ? 0 : __LINE__;
}

static void *reader(void *unused)
{
    unsigned index;

    (void)unused;
    for (index = 0U; index < ITERATIONS; ++index) {
        ucn_i_transport_reliable_view_t view;

        if (ucn_i_transport_reliable_view(
                &owner, reliable_handle, &view) != UCN_OK ||
            view.key.origin_sequence != 1U) {
            return (void *)(uintptr_t)__LINE__;
        }
    }
    return NULL;
}

static void *maintainer(void *unused)
{
    unsigned index;

    (void)unused;
    for (index = 0U; index < ITERATIONS; ++index) {
        uint16_t inspected;
        uint16_t changed;

        if (ucn_i_transport_maintain(
                &owner, 0U, 1U, &inspected, &changed) != UCN_OK ||
            inspected != 1U || changed != 0U) {
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
    int configured = configure();

    if (configured != 0) {
        return configured;
    }
    if (pthread_create(&threads[0], NULL, reader, NULL) != 0 ||
        pthread_create(&threads[1], NULL, maintainer, NULL) != 0 ||
        pthread_create(&threads[2], NULL, reader, NULL) != 0 ||
        pthread_create(&threads[3], NULL, maintainer, NULL) != 0) {
        return __LINE__;
    }
    for (index = 0U; index < 4U; ++index) {
        if (pthread_join(threads[index], &thread_result) != 0 ||
            thread_result != NULL) {
            return thread_result == NULL ? __LINE__ :
                   (int)(uintptr_t)thread_result;
        }
    }
    if (ucn_i_transport_reliable_cancel(&owner, reliable_handle) != UCN_OK ||
        ucn_i_transport_reliable_retire(&owner, reliable_handle) != UCN_OK ||
        ucn_i_transport_owner_destroy(&owner) != UCN_OK ||
        pthread_mutex_destroy(&owner_lock.mutex) != 0) {
        return __LINE__;
    }
    puts("transport concurrency tests passed");
    return 0;
}
