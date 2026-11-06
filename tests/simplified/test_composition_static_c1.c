#include "internal/ucn_composition.h"
#include "internal/ucn_runtime.h"
#include "internal/ucn_wire.h"

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

typedef struct fake_link {
    ucn_i_composition_runtime_t *local;
    ucn_i_composition_runtime_t *peer;
    ucn_link_handle_t peer_link;
    ucn_driver_token_t last_token;
    ucn_driver_submit_result_t submit_result;
    uint32_t not_submitted_remaining;
    uint32_t submit_calls;
    uint32_t cancel_calls;
    uint8_t deliver_on_submit;
    uint8_t complete_during_submit;
    uint8_t driver_gate_active;
    uint8_t last_frame[UCN_ADAPTER_FRAME_BYTES];
    size_t last_frame_bytes;
} fake_link_t;

typedef struct foundation_fixture {
    ucn_storage_t storage;
    ucn_static_binding_t bindings[2];
    ucn_link_port_t link;
    ucn_ports_t ports;
    ucn_config_t config;
    test_lock_t state_lock;
    fake_link_t driver;
} foundation_fixture_t;

typedef struct receive_fixture {
    ucn_i_composition_runtime_t *runtime;
    ucn_endpoint_handle_t endpoint;
    ucn_result_t callback_stats_result;
    ucn_result_t ordinary_stats_result;
    ucn_result_t mutation_result;
    ucn_stats_t callback_stats;
    uint32_t calls;
    uint8_t payload[16];
    size_t payload_bytes;
} receive_fixture_t;

static test_composition_storage_t composition_a_storage;
static test_composition_storage_t composition_b_storage;
static foundation_fixture_t foundation_a;
static foundation_fixture_t foundation_b;
static int failures;

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static bool bytes_are_zero(const void *object, size_t bytes)
{
    const uint8_t *cursor = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (cursor[index] != 0U) {
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

static ucn_result_t driver_gate_enter(void *context)
{
    fake_link_t *driver = context;

    if (driver == NULL || driver->driver_gate_active != 0U) {
        return UCN_ERR_STATE;
    }
    driver->driver_gate_active = 1U;
    return UCN_OK;
}

static void driver_gate_leave(void *context)
{
    fake_link_t *driver = context;

    if (driver != NULL) {
        driver->driver_gate_active = 0U;
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

static ucn_driver_submit_result_t fake_submit(
    void *context,
    ucn_link_handle_t link,
    const uint8_t *bytes,
    size_t length,
    ucn_driver_token_t token)
{
    fake_link_t *driver = context;

    CHECK(driver != NULL);
    CHECK(link.object_kind == UCN_OBJECT_KIND_LINK);
    CHECK(length <= sizeof(driver->last_frame));
    if (driver == NULL || length > sizeof(driver->last_frame)) {
        return UCN_ERR_STATE;
    }
    memcpy(driver->last_frame, bytes, length);
    driver->last_frame_bytes = length;
    driver->last_token = token;
    ++driver->submit_calls;
    if (driver->not_submitted_remaining != 0U) {
        --driver->not_submitted_remaining;
        return UCN_DRIVER_NOT_SUBMITTED;
    }
    if (driver->deliver_on_submit != 0U && driver->peer != NULL) {
        ucn_rx_meta_t meta;

        memset(&meta, 0, sizeof(meta));
        meta.struct_size = (uint16_t)sizeof(meta);
        meta.api_version = UCN_API_VERSION;
        meta.timestamp_us = UINT64_C(50);
        CHECK(ucn_i_composition_driver_rx_publish(
                  driver->peer, driver->peer_link, bytes, length,
                  &meta) == UCN_OK);
    }
    if (driver->complete_during_submit != 0U) {
        CHECK(ucn_i_composition_driver_tx_complete(
                  driver->local, token, UCN_OK, NULL) == UCN_OK);
        return UCN_DRIVER_SUBMITTED;
    }
    return driver->submit_result;
}

static ucn_result_t fake_cancel(void *context, ucn_driver_token_t token)
{
    fake_link_t *driver = context;

    (void)token;
    if (driver == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++driver->cancel_calls;
    return UCN_OK;
}

static void make_binding(ucn_static_binding_t *binding,
                         uint32_t address,
                         uint32_t generation,
                         uint64_t principal)
{
    memset(binding, 0, sizeof(*binding));
    binding->struct_size = (uint16_t)sizeof(*binding);
    binding->api_version = UCN_API_VERSION;
    binding->address = address;
    binding->binding_generation = generation;
    binding->principal_digest = principal;
}

static void foundation_fixture_init(foundation_fixture_t *fixture,
                                    uint32_t runtime_instance,
                                    uint32_t local_address)
{
    memset(fixture, 0, sizeof(*fixture));
    make_binding(&fixture->bindings[0], 1U, 10U,
                 UINT64_C(0x1111111111111111));
    make_binding(&fixture->bindings[1], 2U, 20U,
                 UINT64_C(0x2222222222222222));

    fixture->link.struct_size = (uint16_t)sizeof(fixture->link);
    fixture->link.api_version = UCN_API_VERSION;
    fixture->link.link_instance = 1U;
    fixture->link.frame_mtu = 64U;
    fixture->link.context = &fixture->driver;
    fixture->link.tx.struct_size = (uint16_t)sizeof(fixture->link.tx);
    fixture->link.tx.api_version = UCN_API_VERSION;
    fixture->link.tx.submit = fake_submit;
    fixture->link.tx.cancel = fake_cancel;

    fixture->ports.struct_size = (uint16_t)sizeof(fixture->ports);
    fixture->ports.api_version = UCN_API_VERSION;
    fixture->ports.links = &fixture->link;
    fixture->ports.link_count = 1U;
    fixture->ports.state_lock = public_lock(&fixture->state_lock);
    fixture->ports.driver_callback_gate.struct_size =
        (uint16_t)sizeof(fixture->ports.driver_callback_gate);
    fixture->ports.driver_callback_gate.api_version = UCN_API_VERSION;
    fixture->ports.driver_callback_gate.context = &fixture->driver;
    fixture->ports.driver_callback_gate.enter = driver_gate_enter;
    fixture->ports.driver_callback_gate.leave = driver_gate_leave;

    fixture->config.struct_size = (uint16_t)sizeof(fixture->config);
    fixture->config.api_version = UCN_API_VERSION;
    fixture->config.storage_layout = UCN_STORAGE_LAYOUT;
    fixture->config.compiled_manifest_hash = UCN_COMPILED_MANIFEST_HASH;
    fixture->config.runtime_instance = runtime_instance;
    fixture->config.realm_id = 7U;
    fixture->config.local_address = local_address;
    fixture->config.local_binding_generation =
        local_address == 1U ? 10U : 20U;
    fixture->config.local_principal_digest =
        local_address == 1U ? UINT64_C(0x1111111111111111)
                            : UINT64_C(0x2222222222222222);
    fixture->config.bindings = fixture->bindings;
    fixture->config.binding_count = 2U;
    fixture->config.address_width = 1U;
    fixture->config.trusted_o0_network = 1U;
}

static ucn_i_composition_runtime_t *composition_start(
    test_composition_storage_t *storage,
    foundation_fixture_t *foundation,
    test_lock_t *composition_lock,
    uint32_t runtime_instance,
    uint16_t owner_base)
{
    ucn_i_composition_request_t request;
    ucn_i_composition_plan_t plan;
    ucn_i_composition_prepare_config_t prepare;
    ucn_i_composition_start_config_t start;
    ucn_i_composition_lifecycle_view_t view;
    ucn_i_composition_runtime_t *runtime = NULL;
    size_t required = 0U;

    memset(storage, 0, sizeof(*storage));
    memset(composition_lock, 0, sizeof(*composition_lock));
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint16_t)sizeof(request);
    request.schema = UCN_I_COMPOSITION_SCHEMA;
    request.profile = UCN_PROFILE;
    request.available_module_mask = UCN_I_COMPOSE_FOUNDATION;
    request.capability_mask = UCN_I_CAP_STATIC_C1;
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(plan.selected_module_mask == UCN_I_COMPOSE_FOUNDATION);
    CHECK(plan.init_count == 0U);
    CHECK(plan.durable_module_mask == 0U);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    CHECK(required <= sizeof(*storage));

    memset(&prepare, 0, sizeof(prepare));
    prepare.struct_size = (uint16_t)sizeof(prepare);
    prepare.schema = UCN_I_COMPOSITION_RUNTIME_SCHEMA;
    prepare.runtime_instance = runtime_instance;
    prepare.owner_instance_base = owner_base;
    prepare.coordinator_lock = internal_lock(composition_lock);
    CHECK(ucn_i_composition_prepare(storage->bytes, required, &plan,
                                    &prepare, &runtime) == UCN_OK);
    CHECK(runtime != NULL);

    memset(&start, 0, sizeof(start));
    start.struct_size = (uint16_t)sizeof(start);
    start.schema = UCN_I_COMPOSITION_LIFECYCLE_SCHEMA;
    start.foundation_storage = &foundation->storage;
    start.foundation_storage_bytes = sizeof(foundation->storage);
    start.foundation_config = &foundation->config;
    start.foundation_ports = &foundation->ports;
    CHECK(ucn_i_composition_start_begin(runtime, &start) == UCN_OK);
    CHECK(ucn_i_composition_start_step(runtime, 0U, 1U, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_COMPOSITION_RUNNING);
    CHECK(runtime->foundation_node->owner_instance == owner_base);
    CHECK(runtime->foundation_node->runtime_instance == runtime_instance);
    CHECK(ucn_i_composition_runtime_valid(runtime));
    return runtime;
}

static ucn_static_path_t path_to(uint32_t address,
                                 uint32_t binding_generation)
{
    ucn_static_path_t path;

    memset(&path, 0, sizeof(path));
    path.struct_size = (uint16_t)sizeof(path);
    path.api_version = UCN_API_VERSION;
    path.destination_address = address;
    path.destination_binding_generation = binding_generation;
    path.link_index = 0U;
    path.path_frame_mtu = 64U;
    return path;
}

static ucn_target_t target_to(uint32_t address,
                              uint32_t binding_generation)
{
    ucn_target_t target;

    memset(&target, 0, sizeof(target));
    target.struct_size = (uint16_t)sizeof(target);
    target.api_version = UCN_API_VERSION;
    target.address = address;
    target.binding_generation = binding_generation;
    target.service_id = UINT16_C(0x1234);
    return target;
}

static ucn_send_options_t send_options(uint8_t traffic_class)
{
    ucn_send_options_t options;

    memset(&options, 0, sizeof(options));
    options.struct_size = (uint16_t)sizeof(options);
    options.api_version = UCN_API_VERSION;
    options.traffic_class = traffic_class;
    options.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    options.interaction_role = UCN_INTERACTION_ONE_WAY;
    options.hop_limit = 3U;
    options.copy_payload = 1U;
    return options;
}

static ucn_step_budget_t step_budget(uint16_t max_work)
{
    ucn_step_budget_t budget;

    memset(&budget, 0, sizeof(budget));
    budget.struct_size = (uint16_t)sizeof(budget);
    budget.api_version = UCN_API_VERSION;
    budget.max_work = max_work;
    return budget;
}

static ucn_endpoint_disposition_t receive_message(
    void *context,
    const ucn_endpoint_message_t *message)
{
    receive_fixture_t *receiver = context;
    ucn_stats_t ordinary_stats;

    ++receiver->calls;
    receiver->payload_bytes = message->payload_bytes;
    if (message->payload_bytes <= sizeof(receiver->payload)) {
        memcpy(receiver->payload, message->payload,
               message->payload_bytes);
    }
    receiver->ordinary_stats_result =
        ucn_i_composition_get_stats(receiver->runtime, &ordinary_stats);
    receiver->callback_stats_result =
        ucn_i_composition_callback_get_stats(
            receiver->runtime, message->callback_scope,
            &receiver->callback_stats);
    receiver->mutation_result =
        ucn_i_composition_endpoint_remove(receiver->runtime,
                                          receiver->endpoint);
    return UCN_ENDPOINT_ACCEPT;
}

static void stop_and_destroy(ucn_i_composition_runtime_t *runtime,
                             uint64_t now_us)
{
    uint16_t iteration;

    CHECK(ucn_i_composition_stop_begin(runtime) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_i_composition_lifecycle_view_t view;

        CHECK(ucn_i_composition_stop_step(runtime, now_us + iteration,
                                           8U, &view) == UCN_OK);
        if (view.phase == UCN_I_COMPOSITION_QUIESCENT) {
            CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
            return;
        }
    }
    CHECK(false);
}

static void test_static_c1_end_to_end_and_callback_scope(void)
{
    test_lock_t composition_a_lock;
    test_lock_t composition_b_lock;
    ucn_i_composition_runtime_t *runtime_a;
    ucn_i_composition_runtime_t *runtime_b;
    receive_fixture_t receiver;
    ucn_endpoint_config_t endpoint;
    ucn_static_path_t path = path_to(2U, 20U);
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_to(2U, 20U);
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t send;
    ucn_send_view_t send_view;
    ucn_step_budget_t budget = step_budget(8U);
    ucn_step_result_t step;
    ucn_link_handle_t link_a;
    ucn_link_handle_t link_b;
    ucn_i_c1_frame_t decoded;
    size_t header_bytes = 0U;
    const uint8_t payload[] = {0x10U, 0x20U, 0x30U};

    foundation_fixture_init(&foundation_a, 1001U, 1U);
    foundation_fixture_init(&foundation_b, 1002U, 2U);
    runtime_a = composition_start(&composition_a_storage, &foundation_a,
                                  &composition_a_lock, 1001U, 100U);
    runtime_b = composition_start(&composition_b_storage, &foundation_b,
                                  &composition_b_lock, 1002U, 200U);
    CHECK(ucn_i_composition_link_get(runtime_a, 0U, &link_a) == UCN_OK);
    CHECK(ucn_i_composition_link_get(runtime_b, 0U, &link_b) == UCN_OK);
    foundation_a.driver.local = runtime_a;
    foundation_a.driver.peer = runtime_b;
    foundation_a.driver.peer_link = link_b;
    foundation_a.driver.submit_result = UCN_DRIVER_SUBMITTED;
    foundation_a.driver.deliver_on_submit = 1U;
    foundation_a.driver.complete_during_submit = 1U;

    memset(&receiver, 0, sizeof(receiver));
    receiver.runtime = runtime_b;
    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.struct_size = (uint16_t)sizeof(endpoint);
    endpoint.api_version = UCN_API_VERSION;
    endpoint.service_id = UINT16_C(0x1234);
    endpoint.receive = receive_message;
    endpoint.context = &receiver;
    CHECK(ucn_i_composition_endpoint_add(
              runtime_b, &endpoint, &receiver.endpoint) == UCN_OK);
    CHECK(receiver.endpoint.owner_instance == 200U);
    CHECK(ucn_i_composition_static_path_add(
              runtime_a, &path, &path_handle) == UCN_OK);
    CHECK(path_handle.owner_instance == 100U);
    CHECK(ucn_i_composition_publish(runtime_a, &target, payload,
                                    sizeof(payload), &options,
                                    &send) == UCN_OK);
    CHECK(send.owner_instance == 100U);
    CHECK(ucn_i_composition_step(runtime_a, 1U, &budget, &step) == UCN_OK);
    CHECK(foundation_a.driver.submit_calls == 1U);
    CHECK(ucn_i_c1_header_bytes(1U, &header_bytes) == UCN_OK);
    CHECK(foundation_a.driver.last_frame_bytes ==
          header_bytes + sizeof(payload));
    memset(&decoded, 0, sizeof(decoded));
    CHECK(ucn_i_c1_decode(foundation_a.driver.last_frame,
                          foundation_a.driver.last_frame_bytes,
                          1U, &decoded) == UCN_OK);
    CHECK(decoded.payload_bytes == sizeof(payload));
    CHECK(memcmp(decoded.payload, payload, sizeof(payload)) == 0);
    CHECK(ucn_i_composition_step(runtime_b, 1U, &budget, &step) == UCN_OK);
    CHECK(receiver.calls == 1U);
    CHECK(receiver.payload_bytes == sizeof(payload));
    CHECK(memcmp(receiver.payload, payload, sizeof(payload)) == 0);
    CHECK(receiver.ordinary_stats_result == UCN_ERR_STATE);
    CHECK(receiver.callback_stats_result == UCN_OK);
    CHECK(receiver.mutation_result == UCN_ERR_STATE);
    CHECK(receiver.callback_stats.rx_delivered == 0U);
    CHECK(ucn_i_composition_send_query(runtime_a, send,
                                       &send_view) == UCN_OK);
    CHECK(send_view.terminal_result == UCN_OK);
    CHECK(send_view.link_outcome == UCN_LINK_OUTCOME_COMPLETE);
    CHECK(ucn_i_composition_send_forget(runtime_a, send) == UCN_OK);
    CHECK(ucn_i_composition_static_path_remove(
              runtime_a, path_handle) == UCN_OK);
    CHECK(ucn_i_composition_endpoint_remove(
              runtime_b, receiver.endpoint) == UCN_OK);
    CHECK(bytes_are_zero(runtime_a->coordinator.bindings,
                         sizeof(runtime_a->coordinator.bindings)));
    CHECK(bytes_are_zero(runtime_a->coordinator.slots,
                         sizeof(runtime_a->coordinator.slots)));
    CHECK(ucn_i_composition_runtime_valid(runtime_a));
    CHECK(ucn_i_composition_runtime_valid(runtime_b));
    stop_and_destroy(runtime_a, 10U);
    stop_and_destroy(runtime_b, 10U);
}

static void test_backpressure_reuses_frame_and_sequence(void)
{
    test_lock_t composition_lock;
    ucn_i_composition_runtime_t *runtime;
    ucn_static_path_t path = path_to(2U, 20U);
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_to(2U, 20U);
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q2);
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_step_budget_t one = step_budget(1U);
    ucn_step_result_t step;
    uint8_t first_frame[UCN_ADAPTER_FRAME_BYTES];
    size_t first_frame_bytes = 0U;
    uint32_t sequence;
    const uint8_t payload = 0x55U;
    uint8_t attempt;

    foundation_fixture_init(&foundation_a, 1101U, 1U);
    runtime = composition_start(&composition_a_storage, &foundation_a,
                                &composition_lock, 1101U, 300U);
    foundation_a.driver.local = runtime;
    foundation_a.driver.submit_result = UCN_DRIVER_COMPLETE;
    foundation_a.driver.not_submitted_remaining = 2U;
    CHECK(ucn_i_composition_static_path_add(
              runtime, &path, &path_handle) == UCN_OK);
    CHECK(ucn_i_composition_publish(runtime, &target, &payload,
                                    sizeof(payload), &options,
                                    &send) == UCN_OK);
    CHECK(ucn_i_composition_send_query(runtime, send, &view) == UCN_OK);
    sequence = view.origin_sequence;
    for (attempt = 0U; attempt < 2U; ++attempt) {
        CHECK(ucn_i_composition_step(runtime, attempt + 1U, &one,
                                     &step) == UCN_OK);
        CHECK(ucn_i_composition_send_query(runtime, send, &view) == UCN_OK);
        CHECK(view.origin_sequence == sequence);
        CHECK(view.link_outcome == UCN_LINK_OUTCOME_PENDING);
        if (attempt == 0U) {
            first_frame_bytes = foundation_a.driver.last_frame_bytes;
            memcpy(first_frame, foundation_a.driver.last_frame,
                   first_frame_bytes);
        } else {
            CHECK(foundation_a.driver.last_frame_bytes == first_frame_bytes);
            CHECK(memcmp(foundation_a.driver.last_frame, first_frame,
                         first_frame_bytes) == 0);
        }
    }
    CHECK(ucn_i_composition_step(runtime, 3U, &one, &step) == UCN_OK);
    CHECK(foundation_a.driver.submit_calls == 3U);
    CHECK(ucn_i_composition_send_query(runtime, send, &view) == UCN_OK);
    CHECK(view.origin_sequence == sequence);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_COMPLETE);
    CHECK(ucn_i_composition_send_forget(runtime, send) == UCN_OK);
    CHECK(ucn_i_composition_static_path_remove(
              runtime, path_handle) == UCN_OK);
    stop_and_destroy(runtime, 10U);
}

static void test_cancel_and_stop_close_outstanding_attempts(void)
{
    test_lock_t composition_lock;
    ucn_i_composition_runtime_t *runtime;
    ucn_static_path_t path = path_to(2U, 20U);
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_to(2U, 20U);
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q0);
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_step_budget_t budget = step_budget(8U);
    ucn_step_result_t step;
    ucn_i_composition_lifecycle_view_t lifecycle;
    const uint8_t payload = 0x66U;
    uint16_t iteration;

    memset(&lifecycle, 0, sizeof(lifecycle));
    foundation_fixture_init(&foundation_a, 1201U, 1U);
    runtime = composition_start(&composition_a_storage, &foundation_a,
                                &composition_lock, 1201U, 400U);
    foundation_a.driver.local = runtime;
    foundation_a.driver.submit_result = UCN_DRIVER_SUBMITTED;
    CHECK(ucn_i_composition_static_path_add(
              runtime, &path, &path_handle) == UCN_OK);
    CHECK(ucn_i_composition_publish(runtime, &target, &payload,
                                    sizeof(payload), &options,
                                    &send) == UCN_OK);
    CHECK(ucn_i_composition_step(runtime, 1U, &budget, &step) == UCN_OK);
    CHECK(ucn_i_composition_send_cancel(runtime, send) == UCN_OK);
    CHECK(ucn_i_composition_step(runtime, 2U, &budget, &step) == UCN_OK);
    CHECK(foundation_a.driver.cancel_calls == 1U);
    CHECK(ucn_i_composition_send_query(runtime, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_CANCELLED);
    CHECK(ucn_i_composition_send_forget(runtime, send) == UCN_OK);

    CHECK(ucn_i_composition_publish(runtime, &target, &payload,
                                    sizeof(payload), &options,
                                    NULL) == UCN_OK);
    CHECK(ucn_i_composition_step(runtime, 3U, &budget, &step) == UCN_OK);
    CHECK(ucn_i_composition_stop_begin(runtime) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        CHECK(ucn_i_composition_stop_step(runtime, 4U + iteration,
                                           8U, &lifecycle) == UCN_OK);
        if (lifecycle.phase == UCN_I_COMPOSITION_QUIESCENT) {
            break;
        }
    }
    CHECK(lifecycle.phase == UCN_I_COMPOSITION_QUIESCENT);
    CHECK(foundation_a.driver.cancel_calls == 2U);
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);
}

static void test_phase_and_storage_alias_fail_closed(void)
{
    test_lock_t composition_lock;
    ucn_i_composition_request_t request;
    ucn_i_composition_plan_t plan;
    ucn_i_composition_prepare_config_t prepare;
    ucn_i_composition_runtime_t *runtime = NULL;
    ucn_stats_t stats;
    ucn_stats_t stats_before;
    size_t required = 0U;

    memset(&composition_a_storage, 0, sizeof(composition_a_storage));
    memset(&composition_lock, 0, sizeof(composition_lock));
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint16_t)sizeof(request);
    request.schema = UCN_I_COMPOSITION_SCHEMA;
    request.profile = UCN_PROFILE;
    request.available_module_mask = UCN_I_COMPOSE_FOUNDATION;
    request.capability_mask = UCN_I_CAP_STATIC_C1;
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    memset(&prepare, 0, sizeof(prepare));
    prepare.struct_size = (uint16_t)sizeof(prepare);
    prepare.schema = UCN_I_COMPOSITION_RUNTIME_SCHEMA;
    prepare.runtime_instance = 1301U;
    prepare.owner_instance_base = 500U;
    prepare.coordinator_lock = internal_lock(&composition_lock);
    CHECK(ucn_i_composition_prepare(composition_a_storage.bytes,
                                    required, &plan, &prepare,
                                    &runtime) == UCN_OK);
    memset(&stats, 0xA5, sizeof(stats));
    stats_before = stats;
    CHECK(ucn_i_composition_get_stats(runtime, &stats) ==
          UCN_ERR_STATE);
    CHECK(memcmp(&stats, &stats_before, sizeof(stats)) == 0);
    CHECK(ucn_i_composition_destroy(runtime) == UCN_OK);

    memset(&composition_a_storage, 0, sizeof(composition_a_storage));
    prepare.owner_instance_base = UCN_I_ADAPTER_OWNER_INSTANCE;
    runtime = (ucn_i_composition_runtime_t *)(uintptr_t)1U;
    CHECK(ucn_i_composition_prepare(composition_a_storage.bytes,
                                    required, &plan, &prepare,
                                    &runtime) == UCN_ERR_ARGUMENT);
    CHECK(runtime == (ucn_i_composition_runtime_t *)(uintptr_t)1U);
    CHECK(bytes_are_zero(composition_a_storage.bytes, required));

    memset(&composition_a_storage, 0, sizeof(composition_a_storage));
    prepare.owner_instance_base = UCN_I_CORE_OWNER_INSTANCE;
    runtime = (ucn_i_composition_runtime_t *)(uintptr_t)1U;
    CHECK(ucn_i_composition_prepare(composition_a_storage.bytes,
                                    required, &plan, &prepare,
                                    &runtime) == UCN_ERR_ARGUMENT);
    CHECK(runtime == (ucn_i_composition_runtime_t *)(uintptr_t)1U);
    CHECK(bytes_are_zero(composition_a_storage.bytes, required));
}

static void test_o0_feature_off_retires_without_admission_state(void)
{
    test_lock_t composition_lock;
    ucn_i_composition_runtime_t *runtime;
    ucn_i_composition_bootstrap_rx_workspace_t workspace;
    ucn_i_composition_bootstrap_rx_plan_t plan;
    ucn_i_composition_bootstrap_rx_plan_t plan_before;
    ucn_i_c0_frame_t frame;
    ucn_link_handle_t link;
    ucn_rx_meta_t meta;
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    size_t packet_bytes = 0U;

    foundation_fixture_init(&foundation_a, 1401U, 1U);
    runtime = composition_start(&composition_a_storage, &foundation_a,
                                &composition_lock, 1401U, 600U);
    memset(&frame, 0, sizeof(frame));
    frame.transaction_id = 1U;
    frame.realm_id = 7U;
    frame.destination_address = UINT8_MAX;
    frame.opcode = UCN_I_OPCODE_ADDRESS_OFFER;
    frame.traffic_class = UCN_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    frame.interaction_role = UCN_INTERACTION_RESULT;
    frame.payload_kind = UCN_I_PAYLOAD_CONTROL;
    frame.origin_security = UCN_I_ORIGIN_O0;
    frame.hop_limit = 1U;
    CHECK(ucn_i_c0_encode(&frame, 1U, UCN_I_CONTROL_H0,
                           packet, sizeof(packet), &packet_bytes) == UCN_OK);
    CHECK(ucn_i_composition_link_get(runtime, 0U, &link) == UCN_OK);
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = (uint16_t)sizeof(meta);
    meta.api_version = UCN_API_VERSION;
    CHECK(ucn_i_composition_driver_rx_publish(
              runtime, link, packet, packet_bytes, &meta) == UCN_OK);
    memset(&plan, 0xA5, sizeof(plan));
    plan_before = plan;
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              runtime, &workspace, &plan) == UCN_ERR_UNSUPPORTED);
    CHECK(memcmp(&plan, &plan_before, sizeof(plan)) == 0);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              runtime, &workspace, &plan) == UCN_ERR_NOT_FOUND);
    CHECK(bytes_are_zero(runtime->coordinator.bindings,
                         sizeof(runtime->coordinator.bindings)));
    CHECK(bytes_are_zero(runtime->coordinator.slots,
                         sizeof(runtime->coordinator.slots)));
    stop_and_destroy(runtime, 10U);
}

static ucn_endpoint_disposition_t accept_one_message(
    void *context,
    const ucn_endpoint_message_t *message)
{
    uint32_t *calls = context;

    CHECK(calls != NULL);
    CHECK(message != NULL);
    if (calls != NULL) {
        ++*calls;
    }
    return UCN_ENDPOINT_ACCEPT;
}

static void test_c1_step_does_not_claim_pending_c0(void)
{
    test_lock_t composition_lock;
    ucn_i_composition_runtime_t *runtime;
    ucn_i_composition_bootstrap_rx_workspace_t bootstrap_workspace;
    ucn_i_composition_bootstrap_rx_plan_t bootstrap_plan;
    ucn_i_composition_bootstrap_rx_plan_t bootstrap_before;
    ucn_i_c0_frame_t control;
    ucn_i_c1_frame_t data;
    ucn_endpoint_config_t endpoint;
    ucn_endpoint_handle_t endpoint_handle;
    ucn_link_handle_t link;
    ucn_rx_meta_t meta;
    ucn_step_budget_t budget = step_budget(8U);
    ucn_step_result_t step;
    uint8_t control_packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t data_packet[UCN_ADAPTER_FRAME_BYTES];
    size_t control_bytes = 0U;
    size_t data_bytes = 0U;
    uint32_t callback_calls = 0U;
    const uint8_t payload = 0xA7U;

    foundation_fixture_init(&foundation_a, 1501U, 1U);
    runtime = composition_start(&composition_a_storage, &foundation_a,
                                &composition_lock, 1501U, 700U);
    CHECK(runtime->foundation_node->external_ingress_demux == 1U);
    CHECK(ucn_i_composition_link_get(runtime, 0U, &link) == UCN_OK);

    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.struct_size = (uint16_t)sizeof(endpoint);
    endpoint.api_version = UCN_API_VERSION;
    endpoint.service_id = UINT16_C(0x3344);
    endpoint.receive = accept_one_message;
    endpoint.context = &callback_calls;
    CHECK(ucn_i_composition_endpoint_add(
              runtime, &endpoint, &endpoint_handle) == UCN_OK);

    memset(&control, 0, sizeof(control));
    control.transaction_id = 1U;
    control.realm_id = 7U;
    control.destination_address = UINT8_MAX;
    control.opcode = UCN_I_OPCODE_ADDRESS_OFFER;
    control.traffic_class = UCN_TRAFFIC_Q0;
    control.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    control.interaction_role = UCN_INTERACTION_RESULT;
    control.payload_kind = UCN_I_PAYLOAD_CONTROL;
    control.origin_security = UCN_I_ORIGIN_O0;
    control.hop_limit = 1U;
    CHECK(ucn_i_c0_encode(&control, 1U, UCN_I_CONTROL_H0,
                           control_packet, sizeof(control_packet),
                           &control_bytes) == UCN_OK);

    memset(&data, 0, sizeof(data));
    data.payload = &payload;
    data.payload_bytes = sizeof(payload);
    data.source_address = 2U;
    data.destination_address = 1U;
    data.origin_sequence = 1U;
    data.service_id = endpoint.service_id;
    data.traffic_class = UCN_TRAFFIC_Q1;
    data.hop_limit = 1U;
    CHECK(ucn_i_c1_encode(&data, 1U, data_packet,
                           sizeof(data_packet), &data_bytes) == UCN_OK);

    memset(&meta, 0, sizeof(meta));
    meta.struct_size = (uint16_t)sizeof(meta);
    meta.api_version = UCN_API_VERSION;
    CHECK(ucn_i_composition_driver_rx_publish(
              runtime, link, control_packet, control_bytes, &meta) == UCN_OK);
    CHECK(ucn_i_composition_driver_rx_publish(
              runtime, link, data_packet, data_bytes, &meta) == UCN_OK);

    CHECK(ucn_i_composition_step(runtime, 1U, &budget, &step) == UCN_OK);
    CHECK(callback_calls == 1U);
    CHECK(step.more_work == 1U);

    memset(&bootstrap_plan, 0xA5, sizeof(bootstrap_plan));
    bootstrap_before = bootstrap_plan;
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              runtime, &bootstrap_workspace, &bootstrap_plan) ==
          UCN_ERR_UNSUPPORTED);
    CHECK(memcmp(&bootstrap_plan, &bootstrap_before,
                 sizeof(bootstrap_plan)) == 0);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              runtime, &bootstrap_workspace, &bootstrap_plan) ==
          UCN_ERR_NOT_FOUND);
    CHECK(ucn_i_composition_endpoint_remove(
              runtime, endpoint_handle) == UCN_OK);
    stop_and_destroy(runtime, 10U);
}

int main(void)
{
    test_static_c1_end_to_end_and_callback_scope();
    test_backpressure_reuses_frame_and_sequence();
    test_cancel_and_stop_close_outstanding_attempts();
    test_phase_and_storage_alias_fail_closed();
    test_o0_feature_off_retires_without_admission_state();
    test_c1_step_does_not_claim_pending_c0();
    if (failures != 0) {
        fprintf(stderr, "composition static C1 failures=%d\n", failures);
        return 1;
    }
    printf("composition static C1 tests passed\n");
    return 0;
}
