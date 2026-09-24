#include "internal/ucn_composition.h"
#include "internal/ucn_runtime.h"

#if UCN_FEATURE_PERSISTENCE_ENABLED
#include "fake_persistence_provider.h"
#include "internal/ucn_persistence.h"
#endif

#include <stdio.h>
#include <string.h>

#define TEST_COMPOSITION_STORAGE_BYTES 320000U

typedef union test_composition_storage {
    uint64_t alignment;
    uint8_t bytes[TEST_COMPOSITION_STORAGE_BYTES];
} test_composition_storage_t;

typedef struct test_lock {
    uint32_t enters;
    uint32_t leaves;
    uint8_t held;
} test_lock_t;

typedef struct foundation_fixture {
    ucn_storage_t storage;
    ucn_static_binding_t binding;
    ucn_link_port_t link;
    ucn_ports_t ports;
    ucn_config_t config;
    test_lock_t state_lock;
    test_lock_t driver_gate;
} foundation_fixture_t;

static test_composition_storage_t composition_storage;
static test_composition_storage_t composition_snapshot;
static foundation_fixture_t foundation;
static ucn_storage_t foundation_snapshot;
static int failures;

#if UCN_FEATURE_PERSISTENCE_ENABLED
typedef struct persistence_fixture {
    ucn_persist_manifest_entry_t entry;
    ucn_persist_domain_binding_t binding;
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake;
    ucn_persistence_provider_t provider;
    ucn_persistence_config_t config;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace;
    ucn_persist_callback_gate_t *gate;
    test_lock_t owner_lock;
    test_lock_t gate_lock;
    test_lock_t transport_lock;
} persistence_fixture_t;

static persistence_fixture_t persistence;
#endif

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static bool bytes_are_zero(const void *object, size_t size)
{
    const uint8_t *bytes = object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;

    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    ++lock->enters;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = context;

    if (lock != NULL && lock->held != 0U) {
        lock->held = 0U;
        ++lock->leaves;
    }
}

static ucn_i_lock_ops_t internal_lock(test_lock_t *lock)
{
    ucn_i_lock_ops_t value;

    memset(&value, 0, sizeof(value));
    value.struct_size = (uint16_t)sizeof(value);
    value.api_version = UCN_I_LOCK_OPS_VERSION;
    value.context = lock;
    value.enter = lock_enter;
    value.leave = lock_leave;
    return value;
}

static ucn_lock_ops_t public_lock(test_lock_t *lock)
{
    ucn_lock_ops_t value;

    memset(&value, 0, sizeof(value));
    value.struct_size = (uint16_t)sizeof(value);
    value.api_version = UCN_API_VERSION;
    value.context = lock;
    value.enter = lock_enter;
    value.leave = lock_leave;
    return value;
}

static ucn_driver_submit_result_t driver_submit(
    void *context,
    ucn_link_handle_t link,
    const uint8_t *bytes,
    size_t length,
    ucn_driver_token_t token)
{
    (void)context;
    (void)link;
    (void)bytes;
    (void)length;
    (void)token;
    return UCN_DRIVER_COMPLETE;
}

static ucn_result_t driver_cancel(void *context, ucn_driver_token_t token)
{
    (void)context;
    (void)token;
    return UCN_OK;
}

static void foundation_fixture_init(uint32_t runtime_instance)
{
    memset(&foundation, 0, sizeof(foundation));
    foundation.binding.struct_size = (uint16_t)sizeof(foundation.binding);
    foundation.binding.api_version = UCN_API_VERSION;
    foundation.binding.address = 1U;
    foundation.binding.binding_generation = 1U;
    foundation.binding.principal_digest = UINT64_C(0x0102030405060708);

    foundation.link.struct_size = (uint16_t)sizeof(foundation.link);
    foundation.link.api_version = UCN_API_VERSION;
    foundation.link.link_instance = 1U;
    foundation.link.frame_mtu = 64U;
    foundation.link.context = &foundation.link;
    foundation.link.tx.struct_size =
        (uint16_t)sizeof(foundation.link.tx);
    foundation.link.tx.api_version = UCN_API_VERSION;
    foundation.link.tx.submit = driver_submit;
    foundation.link.tx.cancel = driver_cancel;

    foundation.ports.struct_size = (uint16_t)sizeof(foundation.ports);
    foundation.ports.api_version = UCN_API_VERSION;
    foundation.ports.links = &foundation.link;
    foundation.ports.link_count = 1U;
    foundation.ports.state_lock = public_lock(&foundation.state_lock);
    foundation.ports.driver_callback_gate =
        public_lock(&foundation.driver_gate);

    foundation.config.struct_size = (uint16_t)sizeof(foundation.config);
    foundation.config.api_version = UCN_API_VERSION;
    foundation.config.storage_layout = UCN_STORAGE_LAYOUT;
    foundation.config.compiled_manifest_hash = UCN_COMPILED_MANIFEST_HASH;
    foundation.config.runtime_instance = runtime_instance;
    foundation.config.realm_id = 7U;
    foundation.config.local_address = foundation.binding.address;
    foundation.config.local_binding_generation =
        foundation.binding.binding_generation;
    foundation.config.local_principal_digest =
        foundation.binding.principal_digest;
    foundation.config.bindings = &foundation.binding;
    foundation.config.binding_count = 1U;
    foundation.config.address_width = 1U;
    foundation.config.trusted_o0_network = 1U;
}

static ucn_i_composition_request_t composition_request(
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

static ucn_i_composition_prepare_config_t prepare_config(
    test_lock_t *lock,
    uint32_t runtime_instance,
    uint16_t owner_base)
{
    ucn_i_composition_prepare_config_t config;

    memset(&config, 0, sizeof(config));
    config.struct_size = (uint16_t)sizeof(config);
    config.schema = UCN_I_COMPOSITION_RUNTIME_SCHEMA;
    config.runtime_instance = runtime_instance;
    config.owner_instance_base = owner_base;
    config.coordinator_lock = internal_lock(lock);
    return config;
}

static ucn_i_composition_start_config_t start_config(void)
{
    ucn_i_composition_start_config_t config;

    memset(&config, 0, sizeof(config));
    config.struct_size = (uint16_t)sizeof(config);
    config.schema = UCN_I_COMPOSITION_LIFECYCLE_SCHEMA;
    config.foundation_storage = &foundation.storage;
    config.foundation_storage_bytes = sizeof(foundation.storage);
    config.foundation_config = &foundation.config;
    config.foundation_ports = &foundation.ports;
    return config;
}

static ucn_i_composition_runtime_t *prepare_runtime(
    const ucn_i_composition_request_t *request,
    test_lock_t *composition_lock,
    uint32_t runtime_instance,
    uint16_t owner_base,
    size_t *required_out)
{
    ucn_i_composition_plan_t plan;
    ucn_i_composition_prepare_config_t config;
    ucn_i_composition_runtime_t *runtime = NULL;
    size_t required = 0U;

    memset(&composition_storage, 0, sizeof(composition_storage));
    CHECK(ucn_i_composition_plan_build(request, &plan) == UCN_OK);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    CHECK(required <= sizeof(composition_storage));
    config = prepare_config(composition_lock, runtime_instance, owner_base);
    CHECK(ucn_i_composition_prepare(
              composition_storage.bytes, required, &plan, &config,
              &runtime) == UCN_OK);
    if (required_out != NULL) {
        *required_out = required;
    }
    return runtime;
}

static void stop_to_quiescent(ucn_i_composition_runtime_t *runtime)
{
    uint16_t iteration;

    CHECK(ucn_i_composition_stop_begin(runtime) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_i_composition_lifecycle_view_t view;

        CHECK(ucn_i_composition_stop_step(runtime, 1000U + iteration,
                                           4U, &view) == UCN_OK);
        if (view.phase == UCN_I_COMPOSITION_QUIESCENT) {
            return;
        }
    }
    CHECK(false);
}

static void test_minimal_lifecycle_and_callback_fence(void)
{
    const uint32_t runtime_instance = 101U;
    const ucn_i_composition_request_t request = composition_request(
        UCN_I_COMPOSE_FOUNDATION, UCN_I_CAP_STATIC_C1);
    ucn_i_composition_runtime_t *runtime;
    ucn_i_composition_start_config_t start;
    ucn_i_composition_lifecycle_view_t view;
    ucn_i_callback_claim_t callback_claim;
    test_lock_t composition_lock;
    size_t required;
    ucn_result_t start_result;

    memset(&composition_lock, 0, sizeof(composition_lock));
    foundation_fixture_init(runtime_instance);
    runtime = prepare_runtime(&request, &composition_lock,
                              runtime_instance, 100U, &required);
    CHECK(runtime != NULL);
    start = start_config();
    start_result = ucn_i_composition_start_begin(runtime, &start);
    if (start_result != UCN_OK) {
        fprintf(stderr, "minimal start_begin result=%ld\n",
                (long)start_result);
        CHECK(start_result == UCN_OK);
        CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
        return;
    }
    CHECK(runtime->phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED);
    CHECK(runtime->registry[0].state ==
          UCN_I_COMPOSITION_OWNER_INITIALIZED);
    CHECK(ucn_i_composition_start_step(runtime, 10U, 1U, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_COMPOSITION_RUNNING);
    CHECK(view.foundation_ready == 1U);
    CHECK(view.active_owner_count == 2U);

    memset(&callback_claim, 0, sizeof(callback_claim));
    callback_claim.owner_instance = runtime->foundation_node->owner_instance;
    callback_claim.operation_id = 77U;
    callback_claim.operation_generation = 1U;
    callback_claim.operation_kind = 1U;
    CHECK(ucn_i_callback_gate_enter(
              &runtime->foundation_node->callback_gate,
              &callback_claim) == UCN_OK);
    memcpy(composition_snapshot.bytes, composition_storage.bytes, required);
    memcpy(&foundation_snapshot, &foundation.storage,
           sizeof(foundation_snapshot));
    CHECK(ucn_i_composition_stop_begin(runtime) == UCN_ERR_STATE);
    CHECK(memcmp(composition_snapshot.bytes, composition_storage.bytes,
                 required) == 0);
    CHECK(memcmp(&foundation_snapshot, &foundation.storage,
                 sizeof(foundation_snapshot)) == 0);
    CHECK(ucn_i_callback_gate_leave(
              &runtime->foundation_node->callback_gate,
              &callback_claim) == UCN_OK);

    stop_to_quiescent(runtime);
    CHECK(runtime->phase == UCN_I_COMPOSITION_QUIESCENT);
    CHECK(bytes_are_zero(&foundation.storage, sizeof(foundation.storage)));
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
    CHECK(bytes_are_zero(composition_storage.bytes, required));
}

static void route_config_init(ucn_i_route_config_t *config,
                              uint32_t runtime_instance,
                              uint16_t owner_instance)
{
    memset(config, 0, sizeof(*config));
    config->local.address = 1U;
    config->local.generation = 1U;
    memset(config->local.principal, 0x11, sizeof(config->local.principal));
    config->discovery_lifetime_us = 1000U;
    config->discovery_retry_us = 100U;
    config->reverse_lifetime_us = 1000U;
    config->route_lifetime_us = 2000U;
    config->first_transaction_id = 1U;
    config->runtime_instance = runtime_instance;
    config->realm = 7U;
    config->local_session_generation = 1U;
    config->owner_instance = owner_instance;
    config->address_width = 1U;
    config->discovery_max_attempts = 3U;
    config->maximum_hops = 8U;
}

static void test_late_owner_init_failure_rolls_back(void)
{
    const uint32_t runtime_instance = 202U;
    const uint16_t owner_base = 200U;
    const ucn_i_composition_request_t request = composition_request(
        UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_ROUTE,
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_AUTO_ROUTE);
    ucn_i_composition_runtime_t *runtime;
    ucn_i_composition_start_config_t start;
    ucn_i_route_config_t route;
    test_lock_t composition_lock;
    test_lock_t route_lock_state;
    ucn_i_lock_ops_t route_lock;
    size_t required;

    memset(&composition_lock, 0, sizeof(composition_lock));
    memset(&route_lock_state, 0, sizeof(route_lock_state));
    foundation_fixture_init(runtime_instance);
    runtime = prepare_runtime(&request, &composition_lock,
                              runtime_instance, owner_base, &required);
    CHECK(runtime != NULL);
    if (runtime == NULL) {
        return;
    }
    route_config_init(&route, runtime_instance,
                      (uint16_t)(owner_base +
                          UCN_I_COMPOSITION_MODULE_ROUTE));
    route.realm = 0U;
    route_lock = internal_lock(&route_lock_state);
    start = start_config();
    start.route = &route;
    start.route_state_lock = &route_lock;
    memcpy(composition_snapshot.bytes, composition_storage.bytes, required);
    CHECK(ucn_i_composition_start_begin(runtime, &start) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(composition_snapshot.bytes, composition_storage.bytes,
                 required) == 0);
    CHECK(bytes_are_zero(&foundation.storage, sizeof(foundation.storage)));
    CHECK(runtime->phase == UCN_I_COMPOSITION_PREPARED);
    CHECK(runtime->initialized_count == 0U);

    route.realm = 7U;
    route.owner_instance = (uint16_t)(route.owner_instance + 1U);
    CHECK(ucn_i_composition_start_begin(runtime, &start) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(composition_snapshot.bytes, composition_storage.bytes,
                 required) == 0);
    CHECK(bytes_are_zero(&foundation.storage, sizeof(foundation.storage)));
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
}

static void test_adapter_start_contention_is_bounded(void)
{
    const uint32_t runtime_instance = 252U;
    const ucn_i_composition_request_t request = composition_request(
        UCN_I_COMPOSE_FOUNDATION, UCN_I_CAP_STATIC_C1);
    ucn_i_composition_runtime_t *runtime;
    ucn_i_composition_start_config_t start;
    ucn_i_composition_lifecycle_view_t sentinel;
    ucn_i_composition_lifecycle_view_t before;
    test_lock_t composition_lock;
    size_t required;

    memset(&composition_lock, 0, sizeof(composition_lock));
    foundation_fixture_init(runtime_instance);
    runtime = prepare_runtime(&request, &composition_lock,
                              runtime_instance, 250U, &required);
    CHECK(runtime != NULL);
    start = start_config();
    CHECK(ucn_i_composition_start_begin(runtime, &start) == UCN_OK);
    memset(&sentinel, 0xA5, sizeof(sentinel));
    before = sentinel;
    foundation.state_lock.held = 1U;
    CHECK(ucn_i_composition_start_step(runtime, 1U, 1U, &sentinel) ==
          UCN_ERR_STATE);
    foundation.state_lock.held = 0U;
    CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
    CHECK(runtime->phase == UCN_I_COMPOSITION_FAULT);
    CHECK(runtime->registry[0].state ==
          UCN_I_COMPOSITION_OWNER_INITIALIZED);
    CHECK(runtime->foundation_node->lifecycle == UCN_LIFECYCLE_INITIALIZED);

    stop_to_quiescent(runtime);
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
    CHECK(bytes_are_zero(composition_storage.bytes, required));
}

#if UCN_FEATURE_PERSISTENCE_ENABLED
static void persistence_fixture_init(uint32_t runtime_instance,
                                     uint16_t persistence_owner_instance,
                                     uint16_t transport_owner_instance)
{
    uint8_t manifest_digest[16];
    ucn_lock_ops_t owner_lock;
    ucn_lock_ops_t gate_lock;

    memset(&persistence, 0, sizeof(persistence));
    persistence.entry.struct_size =
        (uint16_t)sizeof(persistence.entry);
    persistence.entry.api_version = UCN_PERSIST_API_VERSION;
    persistence.entry.domain.domain_kind =
        UCN_PERSIST_DOMAIN_TRANSPORT_HIGH_WATER;
    persistence.entry.domain.domain_id = 100U;
    persistence.entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    persistence.entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    persistence.entry.schema_id = UCN_I_TRANSPORT_PARENT_SCHEMA_ID;
    persistence.entry.schema_version = UCN_I_TRANSPORT_SCHEMA;
    persistence.entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    persistence.entry.witness_policy =
        UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    persistence.entry.provider_atomicity_class =
        UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;

    persistence.binding.struct_size =
        (uint16_t)sizeof(persistence.binding);
    persistence.binding.api_version = UCN_PERSIST_API_VERSION;
    persistence.binding.domain = persistence.entry.domain;
    persistence.binding.business_owner_instance = transport_owner_instance;
    persistence.binding.domain_generation = 1U;

    persistence.manifest.struct_size =
        (uint16_t)sizeof(persistence.manifest);
    persistence.manifest.api_version = UCN_PERSIST_API_VERSION;
    persistence.manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    persistence.manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    persistence.manifest.composition_feature_bits =
        UCN_COMPILED_FEATURE_MASK;
    persistence.manifest.entries = &persistence.entry;
    persistence.manifest.entry_count = 1U;
    persistence.manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &persistence.manifest, &persistence.digest_workspace,
              manifest_digest) == UCN_OK);
    memcpy(persistence.manifest.expected_digest, manifest_digest,
           sizeof(manifest_digest));

    fake_persist_provider_init(&persistence.fake,
                               &persistence.manifest, 0xFFU);
    fake_persist_provider_make_public(&persistence.fake, 0xFFU,
                                      &persistence.provider);
    owner_lock = public_lock(&persistence.owner_lock);
    gate_lock = public_lock(&persistence.gate_lock);
    CHECK(ucn_persist_callback_gate_init_in_place(
              &persistence.gate_storage,
              sizeof(persistence.gate_storage), &gate_lock,
              &persistence.gate) == UCN_OK);

    persistence.config.struct_size =
        (uint16_t)sizeof(persistence.config);
    persistence.config.api_version = UCN_PERSIST_API_VERSION;
    persistence.config.runtime_instance = runtime_instance;
    persistence.config.owner_instance = persistence_owner_instance;
    persistence.config.required_domain_mask = UINT32_C(1);
    persistence.config.manifest = &persistence.manifest;
    persistence.config.domain_bindings = &persistence.binding;
    persistence.config.domain_binding_count = 1U;
    persistence.config.provider = &persistence.provider;
    persistence.config.state_lock = owner_lock;
    persistence.config.shared_callback_gate = persistence.gate;
    persistence.config.digest_workspace = &persistence.digest_workspace;
}

static void transport_config_init(ucn_i_transport_config_t *config,
                                  uint32_t runtime_instance,
                                  uint16_t owner_instance)
{
    memset(config, 0, sizeof(*config));
    config->reliable_lifetime_us = 1000U;
    config->reliable_retry_us = 100U;
    config->receipt_lifetime_us = 500U;
    config->runtime_instance = runtime_instance;
    config->owner_instance = owner_instance;
    config->reliable_max_attempts = 3U;
    config->state_lock = internal_lock(&persistence.transport_lock);
}

static void test_durable_reload_precedes_owner_publish(void)
{
    const uint32_t runtime_instance = 303U;
    const uint16_t owner_base = 300U;
    const uint16_t persistence_owner = (uint16_t)(
        owner_base + UCN_I_COMPOSITION_MODULE_PERSISTENCE);
    const uint16_t transport_owner = (uint16_t)(
        owner_base + UCN_I_COMPOSITION_MODULE_TRANSPORT);
    const ucn_i_composition_request_t request = composition_request(
        UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
            UCN_I_COMPOSE_TRANSPORT,
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_TRANSFER);
    ucn_i_composition_runtime_t *runtime;
    ucn_i_composition_start_config_t start;
    ucn_i_transport_config_t transport;
    ucn_i_composition_owner_view_t persistence_view;
    ucn_i_composition_owner_view_t transport_view;
    test_lock_t composition_lock;
    uint16_t iteration;
    size_t required;

    memset(&composition_lock, 0, sizeof(composition_lock));
    foundation_fixture_init(runtime_instance);
    persistence_fixture_init(runtime_instance, persistence_owner,
                             transport_owner);
    transport_config_init(&transport, runtime_instance, transport_owner);
    runtime = prepare_runtime(&request, &composition_lock,
                              runtime_instance, owner_base, &required);
    CHECK(runtime != NULL);
    if (runtime == NULL) {
        CHECK(ucn_persist_callback_gate_deinit(persistence.gate) ==
              UCN_OK);
        return;
    }
    start = start_config();
    start.persistence = &persistence.config;
    start.transport = &transport;
    {
        const ucn_result_t result =
            ucn_i_composition_start_begin(runtime, &start);
        if (result != UCN_OK) {
            fprintf(stderr, "durable start_begin result=%ld\n",
                    (long)result);
            CHECK(result == UCN_OK);
            CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
            CHECK(ucn_persist_callback_gate_deinit(persistence.gate) ==
                  UCN_OK);
            return;
        }
    }
    CHECK(runtime->phase == UCN_I_COMPOSITION_RELOADING);
    CHECK(runtime->persistence_ready == 0U);
    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE,
              &persistence_view) == UCN_OK);
    CHECK(ucn_i_composition_owner_view(
              runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT,
              &transport_view) == UCN_OK);
    CHECK(persistence_view.state == UCN_I_COMPOSITION_OWNER_INITIALIZED);
    CHECK(transport_view.state == UCN_I_COMPOSITION_OWNER_INITIALIZED);

    for (iteration = 0U; iteration < 64U; ++iteration) {
        ucn_i_composition_lifecycle_view_t view;

        CHECK(ucn_i_composition_start_step(
                  runtime, (uint64_t)iteration + 1U, 1U, &view) == UCN_OK);
        if (view.phase == UCN_I_COMPOSITION_RELOADING) {
            CHECK(view.persistence_ready == 0U);
            CHECK(view.foundation_ready == 0U);
            CHECK(view.active_owner_count == 1U);
            CHECK(ucn_i_composition_owner_view(
                      runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT,
                      &transport_view) == UCN_OK);
            CHECK(transport_view.state ==
                  UCN_I_COMPOSITION_OWNER_INITIALIZED);
        } else {
            CHECK(view.persistence_ready == 1U);
            break;
        }
    }
    CHECK(iteration < 64U);
    if (runtime->phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED) {
        CHECK(ucn_i_composition_owner_view(
                  runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE,
                  &persistence_view) == UCN_OK);
        CHECK(ucn_i_composition_owner_view(
                  runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT,
                  &transport_view) == UCN_OK);
        CHECK(persistence_view.state == UCN_I_COMPOSITION_OWNER_ACTIVE);
        CHECK(transport_view.state == UCN_I_COMPOSITION_OWNER_ACTIVE);
        CHECK(runtime->registry[0].state ==
              UCN_I_COMPOSITION_OWNER_INITIALIZED);
        {
            ucn_i_composition_lifecycle_view_t view;
            CHECK(ucn_i_composition_start_step(
                      runtime, 100U, 1U, &view) == UCN_OK);
        }
    }
    CHECK(runtime->phase == UCN_I_COMPOSITION_RUNNING);
    CHECK(runtime->persistence_ready == 1U);

    stop_to_quiescent(runtime);
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
    CHECK(bytes_are_zero(composition_storage.bytes, required));
    CHECK(ucn_persist_callback_gate_deinit(persistence.gate) == UCN_OK);
}

static void test_stop_waits_for_recovery_quiescence(void)
{
    const uint32_t runtime_instance = 304U;
    const uint16_t owner_base = 320U;
    const uint16_t persistence_owner = (uint16_t)(
        owner_base + UCN_I_COMPOSITION_MODULE_PERSISTENCE);
    const uint16_t transport_owner = (uint16_t)(
        owner_base + UCN_I_COMPOSITION_MODULE_TRANSPORT);
    const ucn_i_composition_request_t request = composition_request(
        UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
            UCN_I_COMPOSE_TRANSPORT,
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_TRANSFER);
    ucn_i_composition_runtime_t *runtime;
    ucn_i_composition_start_config_t start;
    ucn_i_transport_config_t transport;
    ucn_persistence_owner_t *persistence_runtime = NULL;
    test_lock_t composition_lock;
    uint8_t index;
    uint16_t iteration;
    size_t required;

    memset(&composition_lock, 0, sizeof(composition_lock));
    foundation_fixture_init(runtime_instance);
    persistence_fixture_init(runtime_instance, persistence_owner,
                             transport_owner);
    transport_config_init(&transport, runtime_instance, transport_owner);
    runtime = prepare_runtime(&request, &composition_lock,
                              runtime_instance, owner_base, &required);
    CHECK(runtime != NULL);
    if (runtime == NULL) {
        CHECK(ucn_persist_callback_gate_deinit(persistence.gate) ==
              UCN_OK);
        return;
    }
    start = start_config();
    start.persistence = &persistence.config;
    start.transport = &transport;
    CHECK(ucn_i_composition_start_begin(runtime, &start) == UCN_OK);
    CHECK(runtime->phase == UCN_I_COMPOSITION_RELOADING);
    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].ref.module_id ==
            UCN_I_COMPOSITION_MODULE_PERSISTENCE) {
            persistence_runtime = (ucn_persistence_owner_t *)(
                (uint8_t *)runtime + runtime->registry[index].storage_offset);
            break;
        }
    }
    CHECK(persistence_runtime != NULL);
    if (persistence_runtime == NULL) {
        stop_to_quiescent(runtime);
        CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
        CHECK(ucn_persist_callback_gate_deinit(persistence.gate) ==
              UCN_OK);
        return;
    }
    CHECK(persistence_runtime->owner_phase ==
          UCN_I_PERSIST_OWNER_RECOVERING);
    CHECK(ucn_i_composition_stop_begin(runtime) == UCN_OK);
    {
        const uint8_t initialized_before = runtime->initialized_count;
        ucn_i_composition_lifecycle_view_t view;

        CHECK(ucn_i_composition_stop_step(runtime, 1U, 4U, &view) ==
              UCN_OK);
        if (persistence_runtime->owner_phase ==
            UCN_I_PERSIST_OWNER_RECOVERING) {
            CHECK(runtime->initialized_count == initialized_before);
            CHECK(view.phase == UCN_I_COMPOSITION_STOPPING);
        }
    }
    for (iteration = 0U; iteration < 64U; ++iteration) {
        ucn_i_composition_lifecycle_view_t view;

        CHECK(ucn_i_composition_stop_step(
                  runtime, (uint64_t)iteration + 2U, 4U, &view) == UCN_OK);
        if (view.phase == UCN_I_COMPOSITION_QUIESCENT) {
            break;
        }
    }
    CHECK(iteration < 64U);
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
    CHECK(bytes_are_zero(composition_storage.bytes, required));
    CHECK(ucn_persist_callback_gate_deinit(persistence.gate) == UCN_OK);
}
#endif

int main(void)
{
#if UCN_V6S_FEATURE_ADAPTER_ENABLED
    test_minimal_lifecycle_and_callback_fence();
    test_late_owner_init_failure_rolls_back();
    test_adapter_start_contention_is_bounded();
#if UCN_FEATURE_PERSISTENCE_ENABLED
    test_durable_reload_precedes_owner_publish();
    test_stop_waits_for_recovery_quiescence();
#endif
#endif
    if (failures != 0) {
        fprintf(stderr, "composition lifecycle failures=%d\n", failures);
        return 1;
    }
    printf("composition lifecycle tests passed profile=%u adapter=%u\n",
           (unsigned)UCN_PROFILE,
           (unsigned)UCN_V6S_FEATURE_ADAPTER_ENABLED);
    return 0;
}
