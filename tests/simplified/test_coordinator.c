#include "internal/ucn_coordinator.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition_) do { \
    if (!(condition_)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition_); \
        return 1; \
    } \
} while (0)

typedef struct fixture fixture_t;

typedef struct test_lock {
    bool locked;
} test_lock_t;

typedef struct test_owner {
    uint32_t calls;
    uint32_t retire_calls;
    uint16_t next_slot;
    ucn_result_t forced_result;
    ucn_result_t forced_retire_result;
    ucn_result_t ensure_reentry_result;
    bool return_invalid_handle;
    bool reuse_previous_handle;
    bool try_ensure_reentry;
    fixture_t *fixture;
} test_owner_t;

typedef struct test_sink {
    uint32_t calls;
    ucn_result_t forced_result;
    ucn_result_t reentry_result;
    ucn_i_dependency_event_t last_event;
    bool try_reentry;
    fixture_t *fixture;
} test_sink_t;

struct fixture {
    test_lock_t lock;
    ucn_i_lock_ops_t lock_ops;
    ucn_i_coordinator_t coordinator;
    test_owner_t owner;
    test_sink_t sink;
};

static ucn_result_t test_lock_enter(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    if (lock == NULL || lock->locked) {
        return UCN_ERR_STATE;
    }
    lock->locked = true;
    return UCN_OK;
}

static void test_lock_leave(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    if (lock != NULL) {
        lock->locked = false;
    }
}

static uint64_t constant_digest(const uint8_t *bytes, size_t length)
{
    (void)bytes;
    (void)length;
    return UINT64_C(0x1122334455667788);
}

static ucn_result_t owner_ensure(
    void *context,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    test_owner_t *owner = (test_owner_t *)context;
    ucn_handle_t handle;

    (void)now_us;
    if (owner == NULL || requirement == NULL || handle_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++owner->calls;
    if (owner->try_ensure_reentry) {
        ucn_handle_t reentry_handle = {0};
        owner->ensure_reentry_result = ucn_i_coordinator_route_requirement(
            &owner->fixture->coordinator, requirement, now_us,
            &reentry_handle);
    }
    if (owner->forced_result != UCN_OK) {
        return owner->forced_result;
    }
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = requirement->runtime_instance;
    handle.owner_instance = 77U;
    handle.slot = owner->reuse_previous_handle ?
        (uint16_t)(owner->next_slot - 1U) : owner->next_slot++;
    handle.generation = 1U;
    handle.object_kind = UCN_OBJECT_KIND_ENDPOINT;
    if (owner->return_invalid_handle) {
        handle.runtime_instance = requirement->runtime_instance + 1U;
    }
    *handle_out = handle;
    return UCN_OK;
}

static ucn_result_t owner_retire(void *context, const ucn_handle_t *handle)
{
    test_owner_t *owner = (test_owner_t *)context;
    if (owner == NULL || handle == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++owner->retire_calls;
    return owner->forced_retire_result;
}

static ucn_result_t event_sink(
    void *context,
    uint16_t requester_owner_instance,
    const ucn_i_dependency_requirement_t *requirement,
    const ucn_i_dependency_event_t *event)
{
    test_sink_t *sink = (test_sink_t *)context;
    ucn_handle_t handle = {0};
    ucn_i_dependency_requirement_t reentry_requirement;

    if (sink == NULL || requester_owner_instance == 0U ||
        requirement == NULL || event == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++sink->calls;
    sink->last_event = *event;
    if (sink->try_reentry) {
        reentry_requirement = *requirement;
        sink->reentry_result = ucn_i_coordinator_route_requirement(
            &sink->fixture->coordinator, &reentry_requirement,
            reentry_requirement.absolute_deadline_us - 1U, &handle);
    }
    return sink->forced_result;
}

static ucn_i_owner_binding_t make_identity_binding(test_owner_t *owner)
{
    ucn_i_owner_binding_t binding;
    memset(&binding, 0, sizeof(binding));
    binding.context = owner;
    binding.ensure = owner_ensure;
    binding.retire = owner_retire;
    binding.owner_instance = 77U;
    binding.slot_limit = 64U;
    binding.owner_id = UCN_I_OWNER_IDENTITY;
    binding.object_kind = UCN_OBJECT_KIND_ENDPOINT;
    return binding;
}

static int fixture_init(fixture_t *fixture,
                        ucn_i_requirement_digest_fn digest)
{
    ucn_i_owner_binding_t binding;
    memset(fixture, 0, sizeof(*fixture));
    fixture->lock_ops.struct_size = (uint16_t)sizeof(fixture->lock_ops);
    fixture->lock_ops.api_version = UCN_I_LOCK_OPS_VERSION;
    fixture->lock_ops.context = &fixture->lock;
    fixture->lock_ops.enter = test_lock_enter;
    fixture->lock_ops.leave = test_lock_leave;
    fixture->owner.fixture = fixture;
    fixture->sink.fixture = fixture;
    CHECK(ucn_i_coordinator_init(&fixture->coordinator, 9U, 10U,
          &fixture->lock_ops, digest, event_sink, &fixture->sink) == UCN_OK);
    binding = make_identity_binding(&fixture->owner);
    CHECK(ucn_i_coordinator_bind_owner(&fixture->coordinator, &binding) ==
          UCN_OK);
    return 0;
}

static ucn_i_dependency_requirement_t make_requirement(
    ucn_i_requirement_digest_fn digest, ucn_i_dependency_kind_t kind,
    uint8_t marker,
    uint64_t deadline_us)
{
    ucn_i_dependency_requirement_t requirement;
    uint8_t exact[3] = {marker, (uint8_t)(marker + 1U),
                        (uint8_t)(marker + 2U)};
    memset(&requirement, 0, sizeof(requirement));
    (void)ucn_i_dependency_requirement_build(
        digest, kind, 9U, 20U, deadline_us,
        UINT64_C(0x1234), exact, (uint8_t)sizeof(exact), &requirement);
    return requirement;
}

static ucn_i_dependency_event_t make_event(
    const ucn_i_dependency_requirement_t *requirement,
    const ucn_handle_t *handle)
{
    ucn_i_dependency_event_t event;
    memset(&event, 0, sizeof(event));
    event.requirement_digest = requirement->requirement_digest;
    event.runtime_instance = requirement->runtime_instance;
    event.result = UCN_OK;
    event.dependency_handle = *handle;
    event.owner_instance = handle->owner_instance;
    event.dependency_kind = requirement->kind;
    event.outcome = UCN_I_DEPENDENCY_READY;
    return event;
}

static int test_exact_duplicate_collision_and_event(void)
{
    fixture_t fixture;
    ucn_i_dependency_requirement_t first;
    ucn_i_dependency_requirement_t collision;
    ucn_i_coordinator_t before;
    ucn_handle_t handle;
    ucn_handle_t duplicate;
    ucn_handle_t sentinel = {
        UINT32_C(0xA5A5A5A5), UINT16_C(0xA5A5), UINT16_C(0xA5A5),
        UINT16_C(0xA5A5), UINT8_C(0xA5), UINT8_C(0xA5)};
    ucn_i_dependency_event_t event;

    CHECK(fixture_init(&fixture, constant_digest) == 0);
    first = make_requirement(constant_digest, UCN_I_DEP_IDENTITY_BINDING,
                             1U, 1000U);
    collision = make_requirement(constant_digest, UCN_I_DEP_IDENTITY_BINDING,
                                 2U, 1000U);
    CHECK(first.requirement_digest == collision.requirement_digest);
    CHECK(!ucn_i_dependency_requirement_equal(&first, &collision));
    fixture.owner.try_ensure_reentry = true;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &first, 100U, &handle) == UCN_OK);
    CHECK(fixture.owner.calls == 1U);
    CHECK(fixture.owner.ensure_reentry_result == UCN_ERR_STATE);
    duplicate = sentinel;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &first, 101U, &duplicate) == UCN_OK);
    CHECK(memcmp(&duplicate, &handle, sizeof(handle)) == 0);
    CHECK(fixture.owner.calls == 1U);
    before = fixture.coordinator;
    duplicate = sentinel;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &collision, 101U, &duplicate) ==
          UCN_ERR_STATE);
    CHECK(memcmp(&fixture.coordinator, &before, sizeof(before)) == 0);
    CHECK(memcmp(&duplicate, &sentinel, sizeof(duplicate)) == 0);
    CHECK(fixture.owner.calls == 1U);

    event = make_event(&first, &handle);
    event.dependency_handle.generation = 2U;
    before = fixture.coordinator;
    CHECK(ucn_i_coordinator_route_event(
          &fixture.coordinator, &event, 200U) == UCN_ERR_NOT_FOUND);
    CHECK(memcmp(&fixture.coordinator, &before, sizeof(before)) == 0);
    event = make_event(&first, &handle);
    fixture.sink.forced_result = UCN_ERR_STATE;
    before = fixture.coordinator;
    CHECK(ucn_i_coordinator_route_event(
          &fixture.coordinator, &event, 200U) == UCN_ERR_STATE);
    CHECK(fixture.sink.calls == 1U);
    CHECK(memcmp(&fixture.coordinator, &before, sizeof(before)) == 0);
    fixture.sink.forced_result = UCN_OK;
    fixture.sink.try_reentry = true;
    CHECK(ucn_i_coordinator_route_event(
          &fixture.coordinator, &event, 200U) == UCN_OK);
    CHECK(fixture.sink.calls == 2U);
    CHECK(fixture.owner.retire_calls == 1U);
    CHECK(fixture.sink.reentry_result == UCN_ERR_STATE);
    CHECK(ucn_i_coordinator_route_event(
          &fixture.coordinator, &event, 200U) == UCN_ERR_NOT_FOUND);
    CHECK(ucn_i_coordinator_destroy(&fixture.coordinator) == UCN_OK);
    return 0;
}

static int test_terminal_delivery_and_retire_retry(void)
{
    fixture_t fixture;
    ucn_i_dependency_requirement_t requirement;
    ucn_i_dependency_event_t event;
    ucn_i_dependency_event_t mismatched;
    ucn_i_coordinator_t before_retry;
    ucn_i_coordinator_slot_t empty_slot;
    ucn_handle_t handle;

    memset(&empty_slot, 0, sizeof(empty_slot));
    CHECK(fixture_init(&fixture, ucn_i_requirement_digest_default) == 0);
    requirement = make_requirement(ucn_i_requirement_digest_default,
                                   UCN_I_DEP_IDENTITY_BINDING, 70U, 300U);
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &requirement, 100U, &handle) == UCN_OK);
    event = make_event(&requirement, &handle);
    CHECK(ucn_i_coordinator_route_event(
          &fixture.coordinator, &event, 300U) == UCN_OK);
    CHECK(fixture.sink.calls == 1U);
    CHECK(fixture.sink.last_event.outcome == UCN_I_DEPENDENCY_FAILED);
    CHECK(fixture.sink.last_event.result == UCN_ERR_TIMEOUT);
    CHECK(fixture.owner.retire_calls == 1U);
    CHECK(fixture.coordinator.slots[0].valid == 0U);

    requirement = make_requirement(ucn_i_requirement_digest_default,
                                   UCN_I_DEP_IDENTITY_BINDING, 71U, 500U);
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &requirement, 400U, &handle) == UCN_OK);
    event = make_event(&requirement, &handle);
    event.result = UCN_ERR_TIMEOUT;
    event.outcome = UCN_I_DEPENDENCY_FAILED;
    fixture.owner.forced_retire_result = UCN_ERR_STATE;
    CHECK(ucn_i_coordinator_route_event(
          &fixture.coordinator, &event, 500U) == UCN_ERR_STATE);
    CHECK(fixture.sink.calls == 2U);
    CHECK(fixture.owner.retire_calls == 2U);
    CHECK(fixture.coordinator.slots[0].valid == 1U);
    CHECK(fixture.coordinator.slots[0].terminal_state == 2U);

    before_retry = fixture.coordinator;
    mismatched = event;
    mismatched.outcome = UCN_I_DEPENDENCY_FENCED;
    mismatched.result = UCN_ERR_ACCESS;
    CHECK(ucn_i_coordinator_route_event(
          &fixture.coordinator, &mismatched, 600U) == UCN_ERR_STATE);
    CHECK(memcmp(&fixture.coordinator, &before_retry,
                 sizeof(before_retry)) == 0);
    CHECK(fixture.sink.calls == 2U);
    CHECK(fixture.owner.retire_calls == 2U);

    fixture.owner.forced_retire_result = UCN_OK;
    CHECK(ucn_i_coordinator_route_event(
          &fixture.coordinator, &event, 400U) == UCN_OK);
    CHECK(fixture.sink.calls == 2U);
    CHECK(fixture.owner.retire_calls == 3U);
    CHECK(fixture.coordinator.slots[0].valid == 0U);
    CHECK(memcmp(&fixture.coordinator.slots[0],
                 &empty_slot,
                 sizeof(fixture.coordinator.slots[0])) == 0);
    CHECK(ucn_i_coordinator_destroy(&fixture.coordinator) == UCN_OK);
    return 0;
}

static int test_deadline_capacity_and_fault(void)
{
    fixture_t fixture;
    ucn_i_dependency_requirement_t requirement;
    ucn_i_dependency_event_t event;
    ucn_handle_t handles[UCN_I_COORDINATOR_PENDING_LIMIT];
    ucn_handle_t sentinel = {
        UINT32_C(0xA5A5A5A5), UINT16_C(0xA5A5), UINT16_C(0xA5A5),
        UINT16_C(0xA5A5), UINT8_C(0xA5), UINT8_C(0xA5)};
    ucn_handle_t output;
    uint32_t index;

    CHECK(fixture_init(&fixture, ucn_i_requirement_digest_default) == 0);
    requirement = make_requirement(ucn_i_requirement_digest_default,
                                   UCN_I_DEP_IDENTITY_BINDING, 1U, 100U);
    output = sentinel;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &requirement, 100U, &output) ==
          UCN_ERR_TIMEOUT);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    CHECK(fixture.owner.calls == 0U);

    requirement = make_requirement(ucn_i_requirement_digest_default,
                                   UCN_I_DEP_SECURITY_SESSION, 2U, 1000U);
    output = sentinel;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &requirement, 100U, &output) ==
          UCN_ERR_UNSUPPORTED);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    CHECK(fixture.owner.calls == 0U);

    requirement = make_requirement(ucn_i_requirement_digest_default,
                                   UCN_I_DEP_IDENTITY_BINDING, 3U, 1000U);
    requirement.exact[requirement.exact_length] = 1U;
    output = sentinel;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &requirement, 100U, &output) ==
          UCN_ERR_STATE);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    CHECK(fixture.owner.calls == 0U);

    for (index = 0U; index < UCN_I_COORDINATOR_PENDING_LIMIT; ++index) {
        requirement = make_requirement(ucn_i_requirement_digest_default,
                                       UCN_I_DEP_IDENTITY_BINDING,
                                       (uint8_t)(10U + index), 1000U);
        CHECK(ucn_i_coordinator_route_requirement(
              &fixture.coordinator, &requirement, 100U, &handles[index]) ==
              UCN_OK);
    }
    requirement = make_requirement(ucn_i_requirement_digest_default,
                                   UCN_I_DEP_IDENTITY_BINDING, 99U, 1000U);
    output = sentinel;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &requirement, 100U, &output) ==
          UCN_ERR_NO_SPACE);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    CHECK(fixture.owner.calls == UCN_I_COORDINATOR_PENDING_LIMIT);
    for (index = 0U; index < UCN_I_COORDINATOR_PENDING_LIMIT; ++index) {
        requirement = make_requirement(ucn_i_requirement_digest_default,
                                       UCN_I_DEP_IDENTITY_BINDING,
                                       (uint8_t)(10U + index), 1000U);
        event = make_event(&requirement, &handles[index]);
        CHECK(ucn_i_coordinator_route_event(
              &fixture.coordinator, &event, 200U) == UCN_OK);
    }
    CHECK(ucn_i_coordinator_destroy(&fixture.coordinator) == UCN_OK);

    CHECK(fixture_init(&fixture, ucn_i_requirement_digest_default) == 0);
    fixture.owner.return_invalid_handle = true;
    requirement = make_requirement(ucn_i_requirement_digest_default,
                                   UCN_I_DEP_IDENTITY_BINDING, 7U, 1000U);
    output = sentinel;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &requirement, 100U, &output) ==
          UCN_ERR_STATE);
    CHECK(fixture.coordinator.faulted == 1U);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    return 0;
}

static int test_owner_cannot_reuse_handle_for_different_requirement(void)
{
    fixture_t fixture;
    ucn_i_dependency_requirement_t first;
    ucn_i_dependency_requirement_t second;
    ucn_handle_t first_handle;
    ucn_handle_t sentinel = {
        UINT32_C(0xA5A5A5A5), UINT16_C(0xA5A5), UINT16_C(0xA5A5),
        UINT16_C(0xA5A5), UINT8_C(0xA5), UINT8_C(0xA5)};
    ucn_handle_t output = sentinel;

    CHECK(fixture_init(&fixture, ucn_i_requirement_digest_default) == 0);
    first = make_requirement(ucn_i_requirement_digest_default,
                             UCN_I_DEP_IDENTITY_BINDING, 80U, 1000U);
    second = make_requirement(ucn_i_requirement_digest_default,
                              UCN_I_DEP_IDENTITY_BINDING, 81U, 1000U);
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &first, 100U, &first_handle) == UCN_OK);
    fixture.owner.reuse_previous_handle = true;
    CHECK(ucn_i_coordinator_route_requirement(
          &fixture.coordinator, &second, 100U, &output) == UCN_ERR_STATE);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    CHECK(fixture.coordinator.faulted == 1U);
    CHECK(fixture.coordinator.slots[0].valid == 1U);
    CHECK(fixture.coordinator.slots[1].valid == 0U);
    return 0;
}

static int test_binding_registry_guards(void)
{
    fixture_t fixture;
    ucn_i_owner_binding_t binding;
    ucn_i_coordinator_t before;

    memset(&fixture, 0, sizeof(fixture));
    fixture.lock_ops.struct_size = (uint16_t)sizeof(fixture.lock_ops);
    fixture.lock_ops.api_version = UCN_I_LOCK_OPS_VERSION;
    fixture.lock_ops.context = &fixture.coordinator;
    fixture.lock_ops.enter = test_lock_enter;
    fixture.lock_ops.leave = test_lock_leave;
    CHECK(ucn_i_coordinator_init(
          &fixture.coordinator, 9U, 10U, &fixture.lock_ops,
          ucn_i_requirement_digest_default, event_sink, NULL) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&fixture.coordinator, &(ucn_i_coordinator_t){0},
                 sizeof(fixture.coordinator)) == 0);

    CHECK(fixture_init(&fixture, ucn_i_requirement_digest_default) == 0);
    before = fixture.coordinator;
    binding = make_identity_binding(&fixture.owner);
    binding.owner_id = UCN_I_OWNER_SECURITY;
    binding.object_kind = UINT8_MAX;
    CHECK(ucn_i_coordinator_bind_owner(&fixture.coordinator, &binding) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&fixture.coordinator, &before, sizeof(before)) == 0);

    binding = make_identity_binding(&fixture.owner);
    binding.owner_id = UCN_I_OWNER_SECURITY;
    binding.context = &fixture.coordinator;
    CHECK(ucn_i_coordinator_bind_owner(&fixture.coordinator, &binding) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&fixture.coordinator, &before, sizeof(before)) == 0);

    binding = make_identity_binding(&fixture.owner);
    CHECK(ucn_i_coordinator_bind_owner(&fixture.coordinator, &binding) ==
          UCN_ERR_STATE);
    CHECK(memcmp(&fixture.coordinator, &before, sizeof(before)) == 0);
    CHECK(ucn_i_coordinator_destroy(&fixture.coordinator) == UCN_OK);
    return 0;
}

int main(void)
{
    CHECK(test_exact_duplicate_collision_and_event() == 0);
    CHECK(test_deadline_capacity_and_fault() == 0);
    CHECK(test_terminal_delivery_and_retire_retry() == 0);
    CHECK(test_owner_cannot_reuse_handle_for_different_requirement() == 0);
    CHECK(test_binding_registry_guards() == 0);
    puts("UCN simplified Coordinator tests passed");
    return 0;
}
