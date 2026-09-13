#include "internal/ucn_coordinator.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition_) do { \
    if (!(condition_)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition_); \
        return 1; \
    } \
} while (0)

typedef struct pthread_lock {
    pthread_mutex_t mutex;
} pthread_lock_t;

typedef struct blocking_owner {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool ensure_entered;
    bool release_ensure;
    uint32_t ensure_calls;
    uint32_t retire_calls;
} blocking_owner_t;

typedef struct route_input {
    ucn_i_coordinator_t *coordinator;
    const ucn_i_dependency_requirement_t *requirement;
    ucn_handle_t handle;
    ucn_result_t result;
} route_input_t;

static ucn_result_t lock_enter(void *context)
{
    pthread_lock_t *lock = (pthread_lock_t *)context;

    return lock != NULL && pthread_mutex_lock(&lock->mutex) == 0 ?
           UCN_OK : UCN_ERR_STATE;
}

static void lock_leave(void *context)
{
    pthread_lock_t *lock = (pthread_lock_t *)context;

    if (lock != NULL) {
        (void)pthread_mutex_unlock(&lock->mutex);
    }
}

static ucn_result_t blocking_ensure(
    void *context,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    blocking_owner_t *owner = (blocking_owner_t *)context;
    ucn_handle_t handle = {0};

    (void)now_us;
    if (owner == NULL || requirement == NULL || handle_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (pthread_mutex_lock(&owner->mutex) != 0) {
        return UCN_ERR_STATE;
    }
    ++owner->ensure_calls;
    owner->ensure_entered = true;
    (void)pthread_cond_broadcast(&owner->condition);
    while (!owner->release_ensure) {
        if (pthread_cond_wait(&owner->condition, &owner->mutex) != 0) {
            (void)pthread_mutex_unlock(&owner->mutex);
            return UCN_ERR_STATE;
        }
    }
    (void)pthread_mutex_unlock(&owner->mutex);

    handle.runtime_instance = requirement->runtime_instance;
    handle.owner_instance = 77U;
    handle.slot = 0U;
    handle.generation = 1U;
    handle.object_kind = UCN_OBJECT_KIND_ENDPOINT;
    *handle_out = handle;
    return UCN_OK;
}

static ucn_result_t blocking_retire(void *context,
                                    const ucn_handle_t *handle)
{
    blocking_owner_t *owner = (blocking_owner_t *)context;

    if (owner == NULL || handle == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++owner->retire_calls;
    return UCN_OK;
}

static ucn_result_t event_sink(
    void *context,
    uint16_t requester_owner_instance,
    const ucn_i_dependency_requirement_t *requirement,
    const ucn_i_dependency_event_t *event)
{
    (void)context;
    return requester_owner_instance != 0U && requirement != NULL &&
           event != NULL ? UCN_OK : UCN_ERR_ARGUMENT;
}

static void *route_thread(void *argument)
{
    route_input_t *input = (route_input_t *)argument;

    input->result = ucn_i_coordinator_route_requirement(
        input->coordinator, input->requirement, 100U, &input->handle);
    return NULL;
}

static ucn_i_dependency_requirement_t make_requirement(uint8_t marker)
{
    ucn_i_dependency_requirement_t requirement;
    uint8_t exact[2] = {marker, (uint8_t)(marker + 1U)};

    memset(&requirement, 0, sizeof(requirement));
    (void)ucn_i_dependency_requirement_build(
        ucn_i_requirement_digest_default, UCN_I_DEP_IDENTITY_BINDING,
        9U, 20U, 1000U, UINT64_C(0x1234), exact,
        (uint8_t)sizeof(exact), &requirement);
    return requirement;
}

int main(void)
{
    pthread_lock_t coordinator_lock;
    blocking_owner_t owner;
    ucn_i_lock_ops_t lock_ops;
    ucn_i_owner_binding_t binding;
    ucn_i_coordinator_t coordinator = {0};
    ucn_i_dependency_requirement_t first = make_requirement(1U);
    ucn_i_dependency_requirement_t second = make_requirement(2U);
    ucn_i_dependency_event_t event;
    route_input_t input = {&coordinator, &first, {0}, UCN_ERR_STATE};
    ucn_handle_t sentinel = {
        UINT32_C(0xA5A5A5A5), UINT16_C(0xA5A5), UINT16_C(0xA5A5),
        UINT16_C(0xA5A5), UINT8_C(0xA5), UINT8_C(0xA5)};
    ucn_handle_t output = sentinel;
    pthread_t thread;

    memset(&owner, 0, sizeof(owner));
    memset(&lock_ops, 0, sizeof(lock_ops));
    memset(&binding, 0, sizeof(binding));
    memset(&event, 0, sizeof(event));
    CHECK(pthread_mutex_init(&coordinator_lock.mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&owner.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&owner.condition, NULL) == 0);
    lock_ops.struct_size = (uint16_t)sizeof(lock_ops);
    lock_ops.api_version = UCN_I_LOCK_OPS_VERSION;
    lock_ops.context = &coordinator_lock;
    lock_ops.enter = lock_enter;
    lock_ops.leave = lock_leave;
    CHECK(ucn_i_coordinator_init(
          &coordinator, 9U, 10U, &lock_ops,
          ucn_i_requirement_digest_default, event_sink, NULL) == UCN_OK);
    binding.context = &owner;
    binding.ensure = blocking_ensure;
    binding.retire = blocking_retire;
    binding.owner_instance = 77U;
    binding.slot_limit = 1U;
    binding.owner_id = UCN_I_OWNER_IDENTITY;
    binding.object_kind = UCN_OBJECT_KIND_ENDPOINT;
    CHECK(ucn_i_coordinator_bind_owner(&coordinator, &binding) == UCN_OK);

    CHECK(pthread_create(&thread, NULL, route_thread, &input) == 0);
    CHECK(pthread_mutex_lock(&owner.mutex) == 0);
    while (!owner.ensure_entered) {
        CHECK(pthread_cond_wait(&owner.condition, &owner.mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&owner.mutex) == 0);

    CHECK(ucn_i_coordinator_route_requirement(
          &coordinator, &second, 100U, &output) == UCN_ERR_STATE);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);

    CHECK(pthread_mutex_lock(&owner.mutex) == 0);
    owner.release_ensure = true;
    CHECK(pthread_cond_broadcast(&owner.condition) == 0);
    CHECK(pthread_mutex_unlock(&owner.mutex) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(input.result == UCN_OK);
    CHECK(owner.ensure_calls == 1U);

    event.requirement_digest = first.requirement_digest;
    event.runtime_instance = first.runtime_instance;
    event.result = UCN_OK;
    event.dependency_handle = input.handle;
    event.owner_instance = input.handle.owner_instance;
    event.dependency_kind = first.kind;
    event.outcome = UCN_I_DEPENDENCY_READY;
    CHECK(ucn_i_coordinator_route_event(&coordinator, &event, 200U) == UCN_OK);
    CHECK(owner.retire_calls == 1U);
    CHECK(ucn_i_coordinator_destroy(&coordinator) == UCN_OK);
    CHECK(pthread_cond_destroy(&owner.condition) == 0);
    CHECK(pthread_mutex_destroy(&owner.mutex) == 0);
    CHECK(pthread_mutex_destroy(&coordinator_lock.mutex) == 0);
    puts("UCN simplified Coordinator concurrency tests passed");
    return 0;
}
