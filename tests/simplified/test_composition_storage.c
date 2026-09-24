#include "internal/ucn_composition.h"

#include <stdio.h>
#include <string.h>

#define TEST_STORAGE_CAPACITY 320000U

typedef union aligned_storage {
    uint64_t alignment;
    uint8_t bytes[TEST_STORAGE_CAPACITY];
} aligned_storage_t;

typedef struct test_lock {
    uint32_t enters;
    uint32_t leaves;
    uint8_t held;
} test_lock_t;

static int failures;

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

#if UCN_V6S_FEATURE_ADAPTER_ENABLED

static aligned_storage_t primary_storage;
static aligned_storage_t secondary_storage;

static ucn_result_t test_lock_enter(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;

    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    ++lock->enters;
    return UCN_OK;
}

static void test_lock_leave(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;

    if (lock != NULL && lock->held != 0U) {
        lock->held = 0U;
        ++lock->leaves;
    }
}

static ucn_i_lock_ops_t lock_ops(test_lock_t *lock)
{
    ucn_i_lock_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.struct_size = (uint16_t)sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = test_lock_enter;
    ops.leave = test_lock_leave;
    return ops;
}

static ucn_i_composition_request_t request_make(
    ucn_i_composition_module_mask_t modules,
    ucn_i_composition_capability_mask_t capabilities)
{
    ucn_i_composition_request_t request;

    memset(&request, 0, sizeof(request));
    request.struct_size = (uint16_t)sizeof(request);
    request.schema = UCN_I_COMPOSITION_SCHEMA;
    request.profile = UCN_PROFILE;
    request.available_module_mask = modules;
    request.capability_mask = capabilities;
    return request;
}

static ucn_i_composition_prepare_config_t config_make(test_lock_t *lock,
                                                       uint32_t runtime,
                                                       uint16_t owner_base)
{
    ucn_i_composition_prepare_config_t config;

    memset(&config, 0, sizeof(config));
    config.struct_size = (uint16_t)sizeof(config);
    config.schema = UCN_I_COMPOSITION_RUNTIME_SCHEMA;
    config.runtime_instance = runtime;
    config.owner_instance_base = owner_base;
    config.coordinator_lock = lock_ops(lock);
    return config;
}

static bool bytes_are_zero(const void *object, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void test_minimal_prepare_destroy(void)
{
    const ucn_i_composition_request_t request = request_make(
        UCN_I_COMPOSE_FOUNDATION, UCN_I_CAP_STATIC_C1);
    ucn_i_composition_plan_t plan;
    ucn_i_composition_runtime_t *runtime = NULL;
    ucn_i_composition_owner_view_t view;
    test_lock_t lock;
    ucn_i_composition_prepare_config_t config;
    size_t required = SIZE_MAX;

    memset(&lock, 0, sizeof(lock));
    memset(&primary_storage, 0, sizeof(primary_storage));
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    CHECK(required >= sizeof(ucn_i_composition_runtime_t));
    CHECK(required < TEST_STORAGE_CAPACITY);
    config = config_make(&lock, 41U, 100U);
    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required, &plan,
                                    &config, &runtime) == UCN_OK);
    CHECK(runtime == (ucn_i_composition_runtime_t *)primary_storage.bytes);
    CHECK(ucn_i_composition_runtime_valid(runtime));
    CHECK(runtime->registry_count == 2U);
    CHECK(runtime->storage_bytes == required);

    memset(&view, 0xA5, sizeof(view));
    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_FOUNDATION, &view) == UCN_OK);
    CHECK(view.ref.runtime_instance == 41U);
    CHECK(view.ref.owner_instance == 100U);
    CHECK(view.ref.generation == 1U);
    CHECK(view.state == UCN_I_COMPOSITION_OWNER_REGISTERED);
    CHECK(view.storage_bytes == 0U);

    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR, &view) == UCN_OK);
    CHECK(view.ref.owner_instance == 113U);
    CHECK(view.state == UCN_I_COMPOSITION_OWNER_ACTIVE);
    CHECK(view.storage_bytes == sizeof(ucn_i_coordinator_t));
    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_ROUTE, &view) ==
          UCN_ERR_NOT_FOUND);

    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
    CHECK(lock.enters == 2U && lock.leaves == 2U && lock.held == 0U);
    CHECK(bytes_are_zero(primary_storage.bytes, required));
    CHECK(!ucn_i_composition_runtime_valid(runtime));
}

static void test_prepare_failure_atomicity(void)
{
    const ucn_i_composition_request_t request = request_make(
        UCN_I_COMPOSE_FOUNDATION, UCN_I_CAP_STATIC_C1);
    ucn_i_composition_plan_t plan;
    ucn_i_composition_runtime_t *runtime;
    ucn_i_composition_runtime_t *sentinel =
        (ucn_i_composition_runtime_t *)(uintptr_t)0x1234U;
    ucn_i_composition_prepare_config_t config;
    test_lock_t lock;
    size_t required;

    memset(&lock, 0, sizeof(lock));
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    config = config_make(&lock, 51U, 200U);

    memset(&primary_storage, 0, sizeof(primary_storage));
    runtime = sentinel;
    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required - 1U,
                                    &plan, &config, &runtime) ==
          UCN_ERR_ARGUMENT);
    CHECK(runtime == sentinel);
    CHECK(bytes_are_zero(primary_storage.bytes, required));

    runtime = sentinel;
    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required + 1U,
                                    &plan, &config, &runtime) ==
          UCN_ERR_ARGUMENT);
    CHECK(runtime == sentinel);
    CHECK(bytes_are_zero(primary_storage.bytes, required + 1U));

    runtime = sentinel;
    CHECK(ucn_i_composition_prepare(&primary_storage.bytes[1], required,
                                    &plan, &config, &runtime) ==
          UCN_ERR_ARGUMENT);
    CHECK(runtime == sentinel);
    CHECK(bytes_are_zero(primary_storage.bytes, required + 1U));

    primary_storage.bytes[required - 1U] = 1U;
    runtime = sentinel;
    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required, &plan,
                                    &config, &runtime) == UCN_ERR_ARGUMENT);
    CHECK(runtime == sentinel);
    CHECK(primary_storage.bytes[required - 1U] == 1U);
    primary_storage.bytes[required - 1U] = 0U;

    config.runtime_instance = 0U;
    runtime = sentinel;
    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required, &plan,
                                    &config, &runtime) == UCN_ERR_ARGUMENT);
    CHECK(runtime == sentinel);
    CHECK(bytes_are_zero(primary_storage.bytes, required));
    config.runtime_instance = 51U;

    CHECK(ucn_i_composition_prepare(
              primary_storage.bytes, required, &plan, &config,
              (ucn_i_composition_runtime_t **)(void *)&config) ==
          UCN_ERR_ARGUMENT);
    CHECK(bytes_are_zero(primary_storage.bytes, required));

    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required, &plan,
                                    &config, &runtime) == UCN_OK);
    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required, &plan,
                                    &config, &runtime) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_composition_runtime_valid(runtime));
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
}

static void test_registry_tamper_and_reserved_storage(void)
{
    const ucn_i_composition_request_t request = request_make(
        UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_ROUTE,
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_AUTO_ROUTE);
    ucn_i_composition_plan_t plan;
    ucn_i_composition_runtime_t *runtime = NULL;
    ucn_i_composition_owner_view_t route;
    ucn_i_composition_owner_view_t sentinel;
    ucn_i_composition_owner_view_t before;
    ucn_i_composition_prepare_config_t config;
    test_lock_t lock;
    size_t required;

    memset(&lock, 0, sizeof(lock));
    memset(&primary_storage, 0, sizeof(primary_storage));
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    config = config_make(&lock, 61U, 300U);
    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required, &plan,
                                    &config, &runtime) == UCN_OK);
    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_ROUTE, &route) == UCN_OK);
    CHECK(route.state == UCN_I_COMPOSITION_OWNER_RESERVED);
    CHECK((route.storage_offset % UCN_I_COMPOSITION_STORAGE_ALIGNMENT) == 0U);
    CHECK(bytes_are_zero(&primary_storage.bytes[route.storage_offset],
                         route.storage_bytes));

    runtime->registry[2].ref.generation = 2U;
    CHECK(!ucn_i_composition_runtime_valid(runtime));
    memset(&sentinel, 0xA5, sizeof(sentinel));
    before = sentinel;
    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_ROUTE, &sentinel) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
    runtime->registry[2].ref.generation = 1U;
    CHECK(ucn_i_composition_runtime_valid(runtime));

    primary_storage.bytes[route.storage_offset] = 1U;
    CHECK(!ucn_i_composition_runtime_valid(runtime));
    primary_storage.bytes[route.storage_offset] = 0U;
    CHECK(ucn_i_composition_runtime_valid(runtime));
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
}

static void test_typed_adapter_and_pre_activation_gate(void)
{
    const ucn_i_composition_request_t request = request_make(
        UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_ROUTE,
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_AUTO_ROUTE);
    ucn_i_composition_plan_t plan;
    ucn_i_composition_runtime_t *runtime = NULL;
    ucn_i_composition_owner_view_t foundation;
    ucn_i_composition_typed_adapter_t adapter;
    ucn_i_composition_typed_adapter_t sentinel;
    ucn_i_composition_typed_adapter_t before;
    ucn_i_dependency_requirement_t requirement;
    ucn_handle_t handle;
    ucn_handle_t handle_before;
    ucn_i_composition_prepare_config_t config;
    test_lock_t lock;
    uint8_t exact = 0x5AU;
    size_t required;

    memset(&lock, 0, sizeof(lock));
    memset(&secondary_storage, 0, sizeof(secondary_storage));
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    config = config_make(&lock, 71U, 400U);
    CHECK(ucn_i_composition_prepare(secondary_storage.bytes, required, &plan,
                                    &config, &runtime) == UCN_OK);
    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_FOUNDATION, &foundation) ==
          UCN_OK);
    CHECK(ucn_i_composition_typed_adapter_build(
              runtime, &foundation.ref, UCN_I_DEP_SOFT_ROUTE, &adapter) ==
          UCN_OK);
    CHECK(adapter.requester.module_id ==
          UCN_I_COMPOSITION_MODULE_FOUNDATION);
    CHECK(adapter.target.module_id == UCN_I_COMPOSITION_MODULE_ROUTE);
    CHECK(adapter.target.owner_instance == 406U);

    memset(&sentinel, 0xA5, sizeof(sentinel));
    before = sentinel;
    CHECK(ucn_i_composition_typed_adapter_build(
              runtime, &foundation.ref, UCN_I_DEP_GROUP, &sentinel) ==
          UCN_ERR_UNSUPPORTED);
    CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
    foundation.ref.generation = 2U;
    CHECK(ucn_i_composition_typed_adapter_build(
              runtime, &foundation.ref, UCN_I_DEP_SOFT_ROUTE, &sentinel) ==
          UCN_ERR_STATE);
    CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
    foundation.ref.generation = 1U;

    memset(&requirement, 0, sizeof(requirement));
    CHECK(ucn_i_dependency_requirement_build(
              ucn_i_requirement_digest_default, UCN_I_DEP_SOFT_ROUTE, 71U,
              foundation.ref.owner_instance, 1000U, UINT64_C(9), &exact, 1U,
              &requirement) == UCN_OK);
    memset(&handle, 0xA5, sizeof(handle));
    handle_before = handle;
    CHECK(ucn_i_composition_route_requirement(
              runtime, &adapter, &requirement, 1U, &handle) == UCN_ERR_STATE);
    CHECK(memcmp(&handle, &handle_before, sizeof(handle)) == 0);

    adapter.composition_digest ^= UINT64_C(1);
    CHECK(ucn_i_composition_route_requirement(
              runtime, &adapter, &requirement, 1U, &handle) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&handle, &handle_before, sizeof(handle)) == 0);
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
}

static void test_full_registry_and_dependency_mapping(void)
{
    static const uint8_t expected_targets[UCN_I_DEPENDENCY_KIND_COUNT] = {
        UCN_I_COMPOSITION_MODULE_IDENTITY,
        UCN_I_COMPOSITION_MODULE_SECURITY,
        UCN_I_COMPOSITION_MODULE_CAPABILITY,
        UCN_I_COMPOSITION_MODULE_ROUTE,
        UCN_I_COMPOSITION_MODULE_FLOW,
        UCN_I_COMPOSITION_MODULE_TRANSPORT,
        UCN_I_COMPOSITION_MODULE_GROUP,
        UCN_I_COMPOSITION_MODULE_REALTIME,
        UCN_I_COMPOSITION_MODULE_PERSISTENCE
    };
    const ucn_i_composition_request_t request = request_make(
        UCN_I_COMPOSITION_COMPILED_MODULE_MASK,
        UCN_I_COMPOSE_KNOWN_CAPABILITIES);
    ucn_i_composition_plan_t plan;
    ucn_i_composition_runtime_t *runtime = NULL;
    ucn_i_composition_owner_view_t coordinator;
    ucn_i_composition_typed_adapter_t adapter;
    ucn_i_composition_prepare_config_t config;
    test_lock_t lock;
    size_t required;
    uint8_t kind;

    if (ucn_i_composition_plan_build(&request, &plan) != UCN_OK) {
        return;
    }
    memset(&lock, 0, sizeof(lock));
    memset(&primary_storage, 0, sizeof(primary_storage));
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    CHECK(required < TEST_STORAGE_CAPACITY);
    config = config_make(&lock, 81U, 500U);
    CHECK(ucn_i_composition_prepare(primary_storage.bytes, required, &plan,
                                    &config, &runtime) == UCN_OK);
    CHECK(runtime->registry_count == UCN_I_COMPOSITION_REGISTRY_CAPACITY);
    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR, &coordinator) ==
          UCN_OK);
    for (kind = UCN_I_DEP_IDENTITY_BINDING;
         kind <= UCN_I_DEP_PERSISTENCE; ++kind) {
        CHECK(ucn_i_composition_typed_adapter_build(
                  runtime, &coordinator.ref, kind, &adapter) == UCN_OK);
        CHECK(adapter.target.module_id == expected_targets[kind - 1U]);
        CHECK(adapter.target.runtime_instance == 81U);
        CHECK(adapter.target.generation == 1U);
    }
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
    printf("composition storage profile=%u bytes=%lu registry=%u\n",
           (unsigned)UCN_PROFILE, (unsigned long)required,
           (unsigned)UCN_I_COMPOSITION_REGISTRY_CAPACITY);
}

#endif

int main(void)
{
#if UCN_V6S_FEATURE_ADAPTER_ENABLED
    test_minimal_prepare_destroy();
    test_prepare_failure_atomicity();
    test_registry_tamper_and_reserved_storage();
    test_typed_adapter_and_pre_activation_gate();
    test_full_registry_and_dependency_mapping();
#else
    printf("composition storage feature-off bounded\n");
#endif
    if (failures != 0) {
        fprintf(stderr, "composition storage failures=%d\n", failures);
        return 1;
    }
    printf("composition storage tests passed\n");
    return 0;
}
