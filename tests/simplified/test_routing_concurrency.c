#include "internal/ucn_flow.h"
#include "internal/ucn_route.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ITERATIONS 4000U

typedef struct pthread_lock {
    pthread_mutex_t mutex;
} pthread_lock_t;

typedef struct fixture {
    ucn_i_route_owner_t route;
    ucn_i_flow_owner_t flow;
    pthread_lock_t route_lock;
    pthread_lock_t flow_lock;
} fixture_t;

static fixture_t fixture;

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

static ucn_i_lock_ops_t make_lock(pthread_lock_t *lock)
{
    ucn_i_lock_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = mutex_enter;
    ops.leave = mutex_leave;
    return ops;
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
    ucn_i_route_config_t route_config;
    ucn_i_flow_config_t flow_config;
    ucn_i_lock_ops_t route_lock;
    ucn_i_lock_ops_t flow_lock;

    memset(&fixture, 0, sizeof(fixture));
    if (pthread_mutex_init(&fixture.route_lock.mutex, NULL) != 0 ||
        pthread_mutex_init(&fixture.flow_lock.mutex, NULL) != 0) {
        return __LINE__;
    }
    route_lock = make_lock(&fixture.route_lock);
    flow_lock = make_lock(&fixture.flow_lock);

    memset(&route_config, 0, sizeof(route_config));
    route_config.runtime_instance = 1U;
    route_config.realm = 1U;
    route_config.local_session_generation = 1U;
    route_config.discovery_lifetime_us = 1000U;
    route_config.discovery_retry_us = 100U;
    route_config.reverse_lifetime_us = 1000U;
    route_config.route_lifetime_us = 1000U;
    route_config.first_transaction_id = 1U;
    route_config.local.address = 1U;
    route_config.local.generation = 1U;
    fill_principal(route_config.local.principal, 0x10U);
    route_config.owner_instance = 1U;
    route_config.address_width = 2U;
    route_config.discovery_max_attempts = 3U;
    route_config.maximum_hops = 8U;
    if (ucn_i_route_owner_init(&fixture.route, &route_config,
                               &route_lock) != UCN_OK) {
        return __LINE__;
    }

    memset(&flow_config, 0, sizeof(flow_config));
    flow_config.local = route_config.local;
    flow_config.probe_lifetime_us = 100U;
    flow_config.stage_lifetime_us = 100U;
    flow_config.commit_lifetime_us = 100U;
    flow_config.flow_lifetime_us = 1000U;
    flow_config.receipt_lifetime_us = 100U;
    flow_config.first_transaction_id = 1U;
    flow_config.runtime_instance = 1U;
    flow_config.realm = 1U;
    flow_config.policy_generation = 1U;
    flow_config.first_candidate_id = 1U;
    flow_config.first_route_generation = 1U;
    flow_config.first_flow_generation = 1U;
    flow_config.owner_instance = 2U;
    flow_config.first_context_id = 1U;
    flow_config.first_label = 1U;
    if (ucn_i_flow_owner_init(&fixture.flow, &flow_config,
                              &flow_lock) != UCN_OK) {
        return __LINE__;
    }
    return 0;
}

static void *route_reader(void *unused)
{
    unsigned index;

    (void)unused;
    for (index = 0U; index < ITERATIONS; ++index) {
        uint16_t a;
        uint16_t b;
        uint16_t c;
        uint16_t d;

        if (ucn_i_route_counts(&fixture.route, &a, &b, &c, &d) != UCN_OK) {
            return (void *)(uintptr_t)__LINE__;
        }
    }
    return NULL;
}

static void *route_maintainer(void *unused)
{
    unsigned index;

    (void)unused;
    for (index = 0U; index < ITERATIONS; ++index) {
        uint16_t inspected;
        uint16_t expired;

        if (ucn_i_route_maintain(&fixture.route, 0U, 1U, &inspected,
                                 &expired) != UCN_OK || inspected != 1U) {
            return (void *)(uintptr_t)__LINE__;
        }
    }
    return NULL;
}

static void *flow_reader(void *unused)
{
    unsigned index;

    (void)unused;
    for (index = 0U; index < ITERATIONS; ++index) {
        uint16_t a;
        uint16_t b;
        uint16_t c;
        uint16_t d;

        if (ucn_i_flow_counts(&fixture.flow, &a, &b, &c, &d) != UCN_OK) {
            return (void *)(uintptr_t)__LINE__;
        }
    }
    return NULL;
}

static void *flow_maintainer(void *unused)
{
    unsigned index;

    (void)unused;
    for (index = 0U; index < ITERATIONS; ++index) {
        uint16_t inspected;
        uint16_t retired;

        if (ucn_i_flow_maintain(&fixture.flow, 0U, 1U, &inspected,
                                &retired) != UCN_OK || inspected != 1U) {
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
    if (pthread_create(&threads[0], NULL, route_reader, NULL) != 0 ||
        pthread_create(&threads[1], NULL, route_maintainer, NULL) != 0 ||
        pthread_create(&threads[2], NULL, flow_reader, NULL) != 0 ||
        pthread_create(&threads[3], NULL, flow_maintainer, NULL) != 0) {
        return __LINE__;
    }
    for (index = 0U; index < 4U; ++index) {
        if (pthread_join(threads[index], &thread_result) != 0 ||
            thread_result != NULL) {
            return thread_result == NULL ? __LINE__ :
                   (int)(uintptr_t)thread_result;
        }
    }
    if (ucn_i_route_owner_destroy(&fixture.route) != UCN_OK ||
        ucn_i_flow_owner_destroy(&fixture.flow) != UCN_OK ||
        pthread_mutex_destroy(&fixture.route_lock.mutex) != 0 ||
        pthread_mutex_destroy(&fixture.flow_lock.mutex) != 0) {
        return __LINE__;
    }
    puts("routing concurrency tests passed");
    return 0;
}
