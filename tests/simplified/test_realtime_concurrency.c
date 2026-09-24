#include "internal/ucn_realtime.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define ITERATIONS 2000U

typedef struct shared_lock { pthread_mutex_t mutex; } shared_lock_t;
typedef struct worker { uint16_t endpoint; int result; } worker_t;

static shared_lock_t lock_state;
static ucn_i_realtime_owner_t owner;

static ucn_result_t lock_enter(void *context)
{
    shared_lock_t *lock = context;
    return pthread_mutex_lock(&lock->mutex) == 0 ? UCN_OK : UCN_ERR_STATE;
}

static void lock_leave(void *context)
{
    (void)pthread_mutex_unlock(&((shared_lock_t *)context)->mutex);
}

static void *run_worker(void *context)
{
    worker_t *worker = context;
    uint32_t iteration;
    for (iteration = 0U; iteration < ITERATIONS; ++iteration) {
        ucn_i_realtime_policy_t policy;
        ucn_i_realtime_prepared_t prepared;
        memset(&policy, 0, sizeof(policy));
        policy.endpoint = worker->endpoint;
        policy.mode = UCN_I_REALTIME_LOCAL_STAMP;
        policy.requirement = UCN_I_REALTIME_REQUIRED;
        if (ucn_i_realtime_policy_set(&owner, &policy) != UCN_OK ||
            ucn_i_realtime_prepare(&owner, worker->endpoint,
                                   iteration + 1U, 0U, false,
                                   &prepared) != UCN_OK ||
            prepared.metadata_present == 0U ||
            prepared.envelope.capture_time_us != iteration + 1U) {
            worker->result = 1;
            return NULL;
        }
    }
    return NULL;
}

int main(void)
{
    ucn_i_realtime_config_t config;
    worker_t left = {1U, 0};
    worker_t right = {2U, 0};
    pthread_t left_thread;
    pthread_t right_thread;

    memset(&owner, 0, sizeof(owner));
    memset(&config, 0, sizeof(config));
    if (pthread_mutex_init(&lock_state.mutex, NULL) != 0) return 1;
    config.runtime_instance = 1U;
    config.owner_instance = 8U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &lock_state;
    config.state_lock.enter = lock_enter;
    config.state_lock.leave = lock_leave;
    if (ucn_i_realtime_owner_init(&owner, &config) != UCN_OK) return 2;
    if (pthread_create(&left_thread, NULL, run_worker, &left) != 0 ||
        pthread_create(&right_thread, NULL, run_worker, &right) != 0) return 3;
    (void)pthread_join(left_thread, NULL);
    (void)pthread_join(right_thread, NULL);
    if (left.result != 0 || right.result != 0) return 4;
    if (ucn_i_realtime_owner_destroy(&owner) != UCN_OK) return 5;
    (void)pthread_mutex_destroy(&lock_state.mutex);
    printf("realtime_concurrency_iterations=%u\n", (unsigned)ITERATIONS);
    return 0;
}
