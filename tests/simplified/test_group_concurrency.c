#include "internal/ucn_group.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define ITERATIONS 2000U

typedef struct shared_lock { pthread_mutex_t mutex; } shared_lock_t;
typedef struct worker { ucn_handle_t group; int result; } worker_t;

static shared_lock_t lock_state;
static ucn_i_group_owner_t owner;

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
        ucn_i_group_context_config_t view;
        uint8_t phase = 0U;
        if (ucn_i_group_context_view(&owner, worker->group, &view,
                                     &phase) != UCN_OK ||
            phase != UCN_I_GROUP_ACTIVE || view.group_id != 71U) {
            worker->result = 1;
            return NULL;
        }
    }
    return NULL;
}

int main(void)
{
    ucn_i_group_config_t config;
    ucn_i_group_context_config_t group;
    worker_t left;
    worker_t right;
    pthread_t left_thread;
    pthread_t right_thread;

    memset(&owner, 0, sizeof(owner));
    memset(&config, 0, sizeof(config));
    memset(&group, 0, sizeof(group));
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    if (pthread_mutex_init(&lock_state.mutex, NULL) != 0) return 1;
    config.runtime_instance = 1U;
    config.realm_id = 1U;
    config.owner_instance = 8U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &lock_state;
    config.state_lock.enter = lock_enter;
    config.state_lock.leave = lock_leave;
    if (ucn_i_group_owner_init(&owner, &config) != UCN_OK) return 2;
    group.realm_id = 1U;
    group.group_id = 71U;
    group.group_generation = 1U;
    group.policy_generation = 1U;
    group.member_generation = 1U;
    group.endpoint = 1U;
    group.opcode = 1U;
    group.mode = UCN_I_GROUP_STATIC;
    group.member_count = 1U;
    group.unicast_fanout_limit = 1U;
    group.quorum_weight = 1U;
    group.allowed_scope_mask = UCN_I_GROUP_SCOPE_BIT(
        UCN_I_GROUP_SCOPE_LOCAL_ONLY);
    group.public_static = 1U;
    group.members[0].address = 1U;
    group.members[0].binding_generation = 1U;
    group.members[0].sender_slot = 1U;
    group.members[0].weight = 1U;
    memset(group.members[0].principal, 0x5AU,
           sizeof(group.members[0].principal));
    if (ucn_i_group_install_static(&owner, 0U, &group, true, true,
                                   &left.group) != UCN_OK) return 3;
    right.group = left.group;
    if (pthread_create(&left_thread, NULL, run_worker, &left) != 0 ||
        pthread_create(&right_thread, NULL, run_worker, &right) != 0) return 4;
    (void)pthread_join(left_thread, NULL);
    (void)pthread_join(right_thread, NULL);
    if (left.result != 0 || right.result != 0) return 5;
    if (ucn_i_group_owner_destroy(&owner) != UCN_OK) return 6;
    (void)pthread_mutex_destroy(&lock_state.mutex);
    printf("group_concurrency_iterations=%u\n", (unsigned)ITERATIONS);
    return 0;
}
