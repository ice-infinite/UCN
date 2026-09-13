#include "ucn/ucn_simplified.h"
#include "internal/ucn_runtime.h"

#include <stdio.h>
#include <string.h>

typedef struct fake_link {
    ucn_node_t *peer;
    ucn_link_handle_t peer_link;
    ucn_driver_token_t last_token;
    ucn_driver_submit_result_t submit_result;
    uint32_t not_submitted_remaining;
    uint32_t submit_calls;
    uint32_t cancel_calls;
    uint32_t sent_by_class[UCN_TRAFFIC_CLASS_COUNT];
    uint8_t deliver_on_submit;
    uint8_t last_frame[UCN_ADAPTER_FRAME_BYTES];
    size_t last_frame_bytes;
    uint8_t driver_gate_active;
} fake_link_t;

typedef struct receive_fixture {
    ucn_node_t *node;
    ucn_endpoint_handle_t endpoint;
    ucn_result_t reentry_result;
    ucn_result_t stats_query_result;
    ucn_result_t send_query_result;
    ucn_result_t link_query_result;
    ucn_result_t ordinary_stats_query_result;
    ucn_result_t ordinary_send_query_result;
    ucn_result_t ordinary_link_query_result;
    ucn_result_t forged_scope_query_result;
    ucn_send_handle_t query_send;
    ucn_send_view_t callback_send_view;
    ucn_stats_t callback_stats;
    ucn_link_handle_t callback_link;
    ucn_callback_scope_t callback_scope;
    uint32_t calls;
    uint8_t payload[32];
    size_t payload_bytes;
    uint32_t source;
    uint32_t source_binding;
    uint32_t marker_calls[3];
    uint8_t exercise_read_queries;
} receive_fixture_t;

typedef struct completion_fixture {
    ucn_node_t *node;
    ucn_target_t target;
    ucn_send_options_t options;
    ucn_send_handle_t handle;
    ucn_send_view_t view;
    ucn_result_t reentry_result;
    uint32_t calls;
    uint8_t byte;
} completion_fixture_t;

static UCN_DECLARE_STORAGE(storage_a);
static UCN_DECLARE_STORAGE(storage_b);
static UCN_DECLARE_STORAGE(storage_bad);
static UCN_DECLARE_STORAGE(storage_snapshot);
static int failures;

#define CHECK(condition_)                                                     \
    do {                                                                      \
        if (!(condition_)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition_);    \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static bool bytes_are_value(const void *object, size_t bytes, uint8_t value)
{
    const uint8_t *cursor = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (cursor[index] != value) {
            return false;
        }
    }
    return true;
}

static ucn_result_t lock_enter(void *context)
{
    (void)context;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    (void)context;
}

static ucn_result_t driver_gate_enter(void *context)
{
    fake_link_t *fake = (fake_link_t *)context;

    if (fake == NULL || fake->driver_gate_active) {
        return UCN_ERR_STATE;
    }
    fake->driver_gate_active = 1U;
    return UCN_OK;
}

static void driver_gate_leave(void *context)
{
    fake_link_t *fake = (fake_link_t *)context;

    if (fake != NULL) {
        fake->driver_gate_active = 0U;
    }
}

static ucn_driver_submit_result_t fake_submit(void *context,
                                               ucn_link_handle_t link,
                                               const uint8_t *bytes,
                                               size_t length,
                                               ucn_driver_token_t token)
{
    fake_link_t *fake = (fake_link_t *)context;
    ucn_rx_meta_t meta;

    CHECK(link.object_kind == UCN_OBJECT_KIND_LINK);
    CHECK(length <= sizeof(fake->last_frame));
    memcpy(fake->last_frame, bytes, length);
    fake->last_frame_bytes = length;
    fake->last_token = token;
    ++fake->submit_calls;
    if (length >= 2U) {
        ++fake->sent_by_class[bytes[1] >> 6U];
    }
    if (fake->deliver_on_submit && fake->peer != NULL) {
        memset(&meta, 0, sizeof(meta));
        meta.struct_size = sizeof(meta);
        meta.api_version = UCN_API_VERSION;
        meta.timestamp_us = 12345U;
        CHECK(ucn_driver_rx_publish(fake->peer, fake->peer_link, bytes, length,
                                    &meta) == UCN_OK);
    }
    if (fake->not_submitted_remaining != 0U) {
        --fake->not_submitted_remaining;
        return UCN_DRIVER_NOT_SUBMITTED;
    }
    return fake->submit_result;
}

static ucn_result_t fake_cancel(void *context, ucn_driver_token_t token)
{
    fake_link_t *fake = (fake_link_t *)context;

    (void)token;
    ++fake->cancel_calls;
    return UCN_OK;
}

static void make_binding(ucn_static_binding_t *binding,
                         uint32_t address,
                         uint32_t generation,
                         uint64_t principal)
{
    memset(binding, 0, sizeof(*binding));
    binding->struct_size = sizeof(*binding);
    binding->api_version = UCN_API_VERSION;
    binding->address = address;
    binding->binding_generation = generation;
    binding->principal_digest = principal;
}

static ucn_result_t make_node(ucn_storage_t *storage,
                              uint32_t runtime,
                              uint32_t local_address,
                              fake_link_t *fake,
                              ucn_node_t **node_out)
{
    ucn_static_binding_t bindings[2];
    ucn_link_port_t link;
    ucn_ports_t ports;
    ucn_config_t config;

    make_binding(&bindings[0], 1U, 10U, UINT64_C(0x1111));
    make_binding(&bindings[1], 2U, 20U, UINT64_C(0x2222));
    memset(&link, 0, sizeof(link));
    link.struct_size = sizeof(link);
    link.api_version = UCN_API_VERSION;
    link.link_instance = 1U;
    link.frame_mtu = 64U;
    link.context = fake;
    link.tx.struct_size = sizeof(link.tx);
    link.tx.api_version = UCN_API_VERSION;
    link.tx.submit = fake_submit;
    link.tx.cancel = fake_cancel;
    memset(&ports, 0, sizeof(ports));
    ports.struct_size = sizeof(ports);
    ports.api_version = UCN_API_VERSION;
    ports.links = &link;
    ports.link_count = 1U;
    ports.state_lock.struct_size = sizeof(ports.state_lock);
    ports.state_lock.api_version = UCN_API_VERSION;
    ports.state_lock.enter = lock_enter;
    ports.state_lock.leave = lock_leave;
    ports.driver_callback_gate.struct_size =
        sizeof(ports.driver_callback_gate);
    ports.driver_callback_gate.api_version = UCN_API_VERSION;
    ports.driver_callback_gate.context = fake;
    ports.driver_callback_gate.enter = driver_gate_enter;
    ports.driver_callback_gate.leave = driver_gate_leave;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.api_version = UCN_API_VERSION;
    config.storage_layout = UCN_STORAGE_LAYOUT;
    config.compiled_manifest_hash = UCN_COMPILED_MANIFEST_HASH;
    config.runtime_instance = runtime;
    config.realm_id = 7U;
    config.local_address = local_address;
    config.local_binding_generation =
        local_address == 1U ? bindings[0].binding_generation
                            : bindings[1].binding_generation;
    config.local_principal_digest =
        local_address == 1U ? bindings[0].principal_digest
                            : bindings[1].principal_digest;
    config.bindings = bindings;
    config.binding_count = 2U;
    config.address_width = 1U;
    config.trusted_o0_network = 1U;
    return ucn_init(storage, sizeof(*storage), &config, &ports, node_out);
}

static ucn_endpoint_disposition_t receive_message(
    void *context,
    const ucn_endpoint_message_t *message)
{
    receive_fixture_t *fixture = (receive_fixture_t *)context;

    ++fixture->calls;
    fixture->source = message->source_address;
    fixture->source_binding = message->source_binding_generation;
    fixture->payload_bytes = message->payload_bytes;
    if (message->payload_bytes <= sizeof(fixture->payload)) {
        memcpy(fixture->payload, message->payload, message->payload_bytes);
    }
    if (message->payload_bytes != 0U && message->payload[0] < 3U) {
        ++fixture->marker_calls[message->payload[0]];
    }
    if (fixture->exercise_read_queries) {
        ucn_callback_scope_t forged = message->callback_scope;

        fixture->callback_scope = message->callback_scope;
        fixture->ordinary_stats_query_result =
            ucn_get_stats(fixture->node, &fixture->callback_stats);
        fixture->ordinary_send_query_result =
            ucn_send_query(fixture->node, fixture->query_send,
                           &fixture->callback_send_view);
        fixture->ordinary_link_query_result =
            ucn_link_get(fixture->node, 0U, &fixture->callback_link);
        fixture->stats_query_result =
            ucn_callback_get_stats(fixture->node, message->callback_scope,
                                   &fixture->callback_stats);
        fixture->send_query_result =
            ucn_callback_send_query(fixture->node, message->callback_scope,
                                    fixture->query_send,
                                    &fixture->callback_send_view);
        fixture->link_query_result =
            ucn_callback_link_get(fixture->node, message->callback_scope, 0U,
                                  &fixture->callback_link);
        ++forged.nonce;
        fixture->forged_scope_query_result =
            ucn_callback_get_stats(fixture->node, forged,
                                   &fixture->callback_stats);
    }
    fixture->reentry_result =
        ucn_endpoint_remove(fixture->node, fixture->endpoint);
    return UCN_ENDPOINT_ACCEPT;
}

static void completion(void *context,
                       ucn_send_handle_t handle,
                       const ucn_send_view_t *view)
{
    completion_fixture_t *fixture = (completion_fixture_t *)context;
    ucn_send_handle_t unexpected;

    ++fixture->calls;
    fixture->handle = handle;
    fixture->view = *view;
    memset(&unexpected, 0xA5, sizeof(unexpected));
    fixture->reentry_result =
        ucn_publish(fixture->node, &fixture->target, &fixture->byte, 1U,
                    &fixture->options, &unexpected);
}

static ucn_endpoint_config_t endpoint_config(receive_fixture_t *fixture)
{
    ucn_endpoint_config_t config;

    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.api_version = UCN_API_VERSION;
    config.service_id = 0x1234U;
    config.receive = receive_message;
    config.context = fixture;
    return config;
}

static ucn_static_path_t path_config(void)
{
    ucn_static_path_t path;

    memset(&path, 0, sizeof(path));
    path.struct_size = sizeof(path);
    path.api_version = UCN_API_VERSION;
    path.destination_address = 2U;
    path.destination_binding_generation = 20U;
    path.link_index = 0U;
    path.path_frame_mtu = 64U;
    return path;
}

static ucn_target_t target_config(void)
{
    ucn_target_t target;

    memset(&target, 0, sizeof(target));
    target.struct_size = sizeof(target);
    target.api_version = UCN_API_VERSION;
    target.address = 2U;
    target.binding_generation = 20U;
    target.service_id = 0x1234U;
    return target;
}

static ucn_send_options_t send_options(uint8_t traffic_class)
{
    ucn_send_options_t options;

    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.api_version = UCN_API_VERSION;
    options.traffic_class = traffic_class;
    options.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    options.interaction_role = UCN_INTERACTION_ONE_WAY;
    options.hop_limit = 3U;
    options.copy_payload = 1U;
    return options;
}

static ucn_step_budget_t step_budget(uint16_t work)
{
    ucn_step_budget_t budget;

    memset(&budget, 0, sizeof(budget));
    budget.struct_size = sizeof(budget);
    budget.api_version = UCN_API_VERSION;
    budget.max_work = work;
    return budget;
}

static size_t make_rx_frame(uint32_t sequence,
                            uint8_t marker,
                            uint8_t output[UCN_ADAPTER_FRAME_BYTES])
{
    ucn_i_c1_frame_t frame;
    size_t output_bytes = 0U;

    memset(&frame, 0, sizeof(frame));
    frame.payload = &marker;
    frame.payload_bytes = 1U;
    frame.source_address = 1U;
    frame.destination_address = 2U;
    frame.origin_sequence = sequence;
    frame.service_id = 0x1234U;
    frame.traffic_class = UCN_TRAFFIC_Q1;
    frame.hop_limit = 8U;
    CHECK(ucn_i_c1_encode(&frame, 1U, output, UCN_ADAPTER_FRAME_BYTES,
                          &output_bytes) == UCN_OK);
    return output_bytes;
}

static void test_invalid_init_is_atomic(void)
{
    ucn_static_binding_t binding;
    ucn_link_port_t link;
    ucn_ports_t ports;
    ucn_config_t config;
    ucn_node_t *output = (ucn_node_t *)(uintptr_t)0x1234U;
    ucn_config_t config_before;
    size_t index;
    fake_link_t gate_context;

    memset(&storage_bad, 0, sizeof(storage_bad));
    memset(&gate_context, 0, sizeof(gate_context));
    make_binding(&binding, 1U, 1U, 1U);
    memset(&link, 0, sizeof(link));
    link.struct_size = sizeof(link);
    link.api_version = UCN_API_VERSION;
    link.link_instance = 1U;
    link.frame_mtu = 64U;
    link.tx.struct_size = sizeof(link.tx);
    link.tx.api_version = UCN_API_VERSION;
    link.tx.submit = fake_submit;
    memset(&ports, 0, sizeof(ports));
    ports.struct_size = sizeof(ports);
    ports.api_version = UCN_API_VERSION;
    ports.links = &link;
    ports.link_count = 1U;
    ports.state_lock.struct_size = sizeof(ports.state_lock);
    ports.state_lock.api_version = UCN_API_VERSION;
    ports.state_lock.enter = lock_enter;
    ports.state_lock.leave = lock_leave;
    ports.driver_callback_gate.struct_size =
        sizeof(ports.driver_callback_gate);
    ports.driver_callback_gate.api_version = UCN_API_VERSION;
    ports.driver_callback_gate.context = &gate_context;
    ports.driver_callback_gate.enter = driver_gate_enter;
    ports.driver_callback_gate.leave = driver_gate_leave;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.api_version = UCN_API_VERSION;
    config.storage_layout = UCN_STORAGE_LAYOUT;
    config.compiled_manifest_hash = UCN_COMPILED_MANIFEST_HASH;
    config.runtime_instance = 1U;
    config.realm_id = 1U;
    config.local_address = 1U;
    config.local_binding_generation = 1U;
    config.local_principal_digest = 1U;
    config.bindings = &binding;
    config.binding_count = 1U;
    config.address_width = 1U;
    config.trusted_o0_network = 0U;
    CHECK(ucn_init(&storage_bad, sizeof(storage_bad), &config, &ports,
                   &output) == UCN_ERR_CONFIG);
    CHECK(output == (ucn_node_t *)(uintptr_t)0x1234U);
    for (index = 0U; index < sizeof(storage_bad.bytes); ++index) {
        CHECK(storage_bad.bytes[index] == 0U);
    }
    config.trusted_o0_network = 1U;
    output = (ucn_node_t *)(uintptr_t)0x1234U;
    config.storage_layout = (uint16_t)(UCN_STORAGE_LAYOUT + 1U);
    CHECK(ucn_init(&storage_bad, sizeof(storage_bad), &config, &ports,
                   &output) == UCN_ERR_CONFIG);
    CHECK(output == (ucn_node_t *)(uintptr_t)0x1234U);
    for (index = 0U; index < sizeof(storage_bad.bytes); ++index) {
        CHECK(storage_bad.bytes[index] == 0U);
    }
    config.storage_layout = UCN_STORAGE_LAYOUT;
    config.compiled_manifest_hash ^= UINT64_C(1);
    CHECK(ucn_init(&storage_bad, sizeof(storage_bad), &config, &ports,
                   &output) == UCN_ERR_CONFIG);
    CHECK(output == (ucn_node_t *)(uintptr_t)0x1234U);
    for (index = 0U; index < sizeof(storage_bad.bytes); ++index) {
        CHECK(storage_bad.bytes[index] == 0U);
    }
    config.compiled_manifest_hash = UCN_COMPILED_MANIFEST_HASH;
    config_before = config;
    CHECK(ucn_init(&storage_bad, sizeof(storage_bad), &config, &ports,
                   (ucn_node_t **)(void *)&config) == UCN_ERR_CONFIG);
    CHECK(memcmp(&config, &config_before, sizeof(config)) == 0);
    for (index = 0U; index < sizeof(storage_bad.bytes); ++index) {
        CHECK(storage_bad.bytes[index] == 0U);
    }

    output = (ucn_node_t *)(uintptr_t)0x1234U;
    ports.state_lock.context = &output;
    CHECK(ucn_init(&storage_bad, sizeof(storage_bad), &config, &ports,
                   &output) == UCN_ERR_CONFIG);
    CHECK(output == (ucn_node_t *)(uintptr_t)0x1234U);
    ports.state_lock.context = NULL;

    ports.driver_callback_gate.context = &output;
    CHECK(ucn_init(&storage_bad, sizeof(storage_bad), &config, &ports,
                   &output) == UCN_ERR_CONFIG);
    CHECK(output == (ucn_node_t *)(uintptr_t)0x1234U);
    ports.driver_callback_gate.context = &gate_context;

    link.context = &output;
    CHECK(ucn_init(&storage_bad, sizeof(storage_bad), &config, &ports,
                   &output) == UCN_ERR_CONFIG);
    CHECK(output == (ucn_node_t *)(uintptr_t)0x1234U);
    for (index = 0U; index < sizeof(storage_bad.bytes); ++index) {
        CHECK(storage_bad.bytes[index] == 0U);
    }
}

static void test_driver_facts_cannot_alias_node_storage(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_link_handle_t link_handle;
    ucn_rx_meta_t rx_meta;
    uint8_t external_byte = 0x5AU;
    const uint8_t *node_byte;
    const ucn_rx_meta_t *node_rx_meta;
    const ucn_tx_meta_t *node_tx_meta;
    const ucn_link_event_meta_t *node_link_meta;

    memset(&storage_bad, 0, sizeof(storage_bad));
    memset(&storage_snapshot, 0, sizeof(storage_snapshot));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_bad, 303U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_link_get(node, 0U, &link_handle) == UCN_OK);
    CHECK(ucn_start(node, 1U) == UCN_OK);

    memset(&rx_meta, 0, sizeof(rx_meta));
    rx_meta.struct_size = sizeof(rx_meta);
    rx_meta.api_version = UCN_API_VERSION;
    node_byte = ((const uint8_t *)(const void *)node) +
                offsetof(struct ucn_node, stats);
    node_rx_meta = (const ucn_rx_meta_t *)(const void *)node_byte;
    node_tx_meta = (const ucn_tx_meta_t *)(const void *)node_byte;
    node_link_meta = (const ucn_link_event_meta_t *)(const void *)node_byte;
    memcpy(&storage_snapshot, &storage_bad, sizeof(storage_bad));

    CHECK(ucn_driver_rx_publish(node, link_handle, node_byte, 1U,
                                &rx_meta) == UCN_ERR_ARGUMENT);
    CHECK(ucn_driver_rx_publish(node, link_handle, &external_byte, 1U,
                                node_rx_meta) == UCN_ERR_ARGUMENT);
    CHECK(ucn_driver_tx_complete(node, (ucn_driver_token_t){0}, UCN_OK,
                                 node_tx_meta) == UCN_ERR_ARGUMENT);
    CHECK(ucn_driver_link_event(node, link_handle, UCN_DRIVER_LINK_DOWN,
                                node_link_meta) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&storage_bad, &storage_snapshot, sizeof(storage_bad)) == 0);

    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_initialized_node_uses_stop_fence_before_deinit(void)
{
    fake_link_t link;
    receive_fixture_t receiver;
    ucn_node_t *node = NULL;
    ucn_endpoint_config_t endpoint;
    ucn_endpoint_handle_t endpoint_out;
    ucn_endpoint_handle_t endpoint_before;
    ucn_static_path_t path;
    ucn_path_handle_t path_out;
    ucn_path_handle_t path_before;

    memset(&storage_bad, 0, sizeof(storage_bad));
    memset(&link, 0, sizeof(link));
    memset(&receiver, 0, sizeof(receiver));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_bad, 304U, 1U, &link, &node) == UCN_OK);
    memset(&endpoint_out, 0xA5, sizeof(endpoint_out));
    endpoint_before = endpoint_out;
    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.struct_size = sizeof(endpoint);
    endpoint.api_version = UCN_API_VERSION;
    endpoint.service_id = 0x1234U;
    endpoint.receive = receive_message;
    endpoint.context = &endpoint_out;
    CHECK(ucn_endpoint_add(node, &endpoint, &endpoint_out) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&endpoint_out, &endpoint_before, sizeof(endpoint_out)) == 0);
    CHECK(ucn_deinit(node) == UCN_ERR_STATE);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(node->lifecycle == UCN_LIFECYCLE_QUIESCENT);
    endpoint = endpoint_config(&receiver);
    memset(&endpoint_out, 0xA5, sizeof(endpoint_out));
    endpoint_before = endpoint_out;
    CHECK(ucn_endpoint_add(node, &endpoint, &endpoint_out) == UCN_ERR_STATE);
    CHECK(memcmp(&endpoint_out, &endpoint_before, sizeof(endpoint_out)) == 0);
    path = path_config();
    memset(&path_out, 0xA5, sizeof(path_out));
    path_before = path_out;
    CHECK(ucn_static_path_add(node, &path, &path_out) == UCN_ERR_STATE);
    CHECK(memcmp(&path_out, &path_before, sizeof(path_out)) == 0);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_static_end_to_end(void)
{
    fake_link_t link_a;
    fake_link_t link_b;
    receive_fixture_t receiver;
    completion_fixture_t completed;
    ucn_node_t *node_a = NULL;
    ucn_node_t *node_b = NULL;
    ucn_link_handle_t link_a_handle;
    ucn_link_handle_t link_b_handle;
    ucn_link_handle_t refreshed_link;
    ucn_endpoint_config_t endpoint;
    ucn_static_path_t path;
    ucn_target_t target;
    ucn_send_options_t options;
    ucn_send_handle_t send_handle;
    ucn_path_handle_t path_handle;
    ucn_send_view_t view;
    ucn_step_budget_t budget = step_budget(8U);
    ucn_step_result_t step;
    ucn_link_event_meta_t reopen;
    ucn_rx_meta_t rx_meta;
    ucn_stats_t stats;
    uint8_t payload[4] = {9U, 8U, 7U, 6U};
    uint8_t bad_frame[UCN_ADAPTER_FRAME_BYTES];

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&storage_b, 0, sizeof(storage_b));
    memset(&link_a, 0, sizeof(link_a));
    memset(&link_b, 0, sizeof(link_b));
    memset(&receiver, 0, sizeof(receiver));
    memset(&completed, 0, sizeof(completed));
    link_a.submit_result = UCN_DRIVER_COMPLETE;
    link_a.deliver_on_submit = 1U;
    link_b.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 101U, 1U, &link_a, &node_a) == UCN_OK);
    CHECK(make_node(&storage_b, 202U, 2U, &link_b, &node_b) == UCN_OK);
    CHECK(ucn_link_get(node_a, 0U, &link_a_handle) == UCN_OK);
    CHECK(ucn_link_get(node_b, 0U, &link_b_handle) == UCN_OK);
    link_a.peer = node_b;
    link_a.peer_link = link_b_handle;
    receiver.node = node_b;
    endpoint = endpoint_config(&receiver);
    CHECK(ucn_endpoint_add(node_b, &endpoint, &receiver.endpoint) == UCN_OK);
    CHECK(ucn_start(node_a, 1U) == UCN_OK);
    CHECK(ucn_start(node_b, 1U) == UCN_OK);

    target = target_config();
    completed.node = node_a;
    completed.target = target;
    completed.byte = 0x5AU;
    completed.options = send_options(UCN_TRAFFIC_Q2);
    completed.options.completion = completion;
    completed.options.completion_context = &completed;
    options = completed.options;
    memset(&send_handle, 0xA5, sizeof(send_handle));
    CHECK(ucn_publish(node_a, &target, payload, sizeof(payload), &options,
                      &send_handle) == UCN_ERR_NOT_FOUND);
    CHECK(send_handle.runtime_instance == UINT32_C(0xA5A5A5A5));
    options.delivery_guarantee = UCN_DELIVERY_RELIABLE;
    CHECK(ucn_publish(node_a, &target, payload, sizeof(payload), &options,
                      &send_handle) == UCN_ERR_UNSUPPORTED);
    CHECK(send_handle.runtime_instance == UINT32_C(0xA5A5A5A5));
    options = completed.options;
    path = path_config();
    CHECK(ucn_static_path_add(node_a, &path, &path_handle) == UCN_OK);
    CHECK(ucn_publish(node_a, &target, payload, sizeof(payload), &options,
                      &send_handle) == UCN_OK);
    CHECK(ucn_send_query(node_a, send_handle, &view) == UCN_OK);
    CHECK(view.origin_sequence == 1U);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_PENDING);
    CHECK(ucn_static_path_remove(node_a, path_handle) == UCN_ERR_STATE);
    CHECK(ucn_step(node_a, 10U, &budget, &step) == UCN_OK);
    CHECK(completed.calls == 1U);
    CHECK(completed.reentry_result == UCN_ERR_STATE);
    CHECK(memcmp(&completed.handle, &send_handle, sizeof(send_handle)) == 0);
    CHECK(completed.view.terminal_result == UCN_OK);
    CHECK(ucn_send_query(node_a, send_handle, &view) == UCN_OK);
    CHECK(view.buffer_released == 1U);
    CHECK(view.callback_delivered == 1U);
    CHECK(ucn_step(node_b, 10U, &budget, &step) == UCN_OK);
    CHECK(receiver.calls == 1U);
    CHECK(receiver.reentry_result == UCN_ERR_STATE);
    CHECK(receiver.source == 1U);
    CHECK(receiver.source_binding == 10U);
    CHECK(receiver.payload_bytes == sizeof(payload));
    CHECK(memcmp(receiver.payload, payload, sizeof(payload)) == 0);
    CHECK(ucn_send_forget(node_a, send_handle) == UCN_OK);
    CHECK(ucn_send_query(node_a, send_handle, &view) == UCN_ERR_NOT_FOUND);

    completed.calls = 0U;
    memset(&completed.handle, 0, sizeof(completed.handle));
    link_a.deliver_on_submit = 0U;
    CHECK(ucn_publish(node_a, &target, payload, sizeof(payload),
                      &completed.options, NULL) == UCN_OK);
    CHECK(ucn_step(node_a, 10U, &budget, &step) == UCN_OK);
    link_a.deliver_on_submit = 1U;
    CHECK(completed.calls == 1U);
    CHECK(completed.handle.object_kind == UCN_OBJECT_KIND_SEND);
    CHECK(ucn_send_query(node_a, completed.handle, &view) == UCN_OK);
    CHECK(view.callback_delivered == 1U);
    CHECK(ucn_send_forget(node_a, completed.handle) == UCN_OK);

    memset(&rx_meta, 0, sizeof(rx_meta));
    rx_meta.struct_size = sizeof(rx_meta);
    rx_meta.api_version = UCN_API_VERSION;
    memcpy(bad_frame, link_a.last_frame, link_a.last_frame_bytes);
    bad_frame[0] = 0x51U;
    CHECK(ucn_driver_rx_publish(node_b, link_b_handle, bad_frame,
                                link_a.last_frame_bytes, &rx_meta) == UCN_OK);
    CHECK(ucn_step(node_b, 11U, &budget, &step) == UCN_OK);
    memcpy(bad_frame, link_a.last_frame, link_a.last_frame_bytes);
    bad_frame[5] = 0x22U;
    bad_frame[6] = 0x22U;
    CHECK(ucn_driver_rx_publish(node_b, link_b_handle, bad_frame,
                                link_a.last_frame_bytes, &rx_meta) == UCN_OK);
    CHECK(ucn_step(node_b, 12U, &budget, &step) == UCN_OK);
    memcpy(bad_frame, link_a.last_frame, link_a.last_frame_bytes);
    bad_frame[3] = 3U;
    CHECK(ucn_driver_rx_publish(node_b, link_b_handle, bad_frame,
                                link_a.last_frame_bytes, &rx_meta) == UCN_OK);
    CHECK(ucn_step(node_b, 13U, &budget, &step) == UCN_OK);
    CHECK(receiver.calls == 1U);
    CHECK(ucn_get_stats(node_b, &stats) == UCN_OK);
    CHECK(stats.malformed == 1U);
    CHECK(stats.rx_dropped == 3U);

    options = send_options(UCN_TRAFFIC_Q3);
    options.absolute_deadline_us = 100U;
    CHECK(ucn_publish(node_a, &target, payload, sizeof(payload), &options,
                      &send_handle) == UCN_OK);
    CHECK(ucn_send_query(node_a, send_handle, &view) == UCN_OK);
    CHECK(view.origin_sequence == 3U);
    CHECK(ucn_step(node_a, 100U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node_a, send_handle, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_TIMEOUT);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_CANCELLED);
    CHECK(ucn_send_forget(node_a, send_handle) == UCN_OK);

    memset(&reopen, 0, sizeof(reopen));
    reopen.struct_size = sizeof(reopen);
    reopen.api_version = UCN_API_VERSION;
    reopen.new_link_instance = 2U;
    CHECK(ucn_driver_link_event(node_a, link_a_handle,
                                UCN_DRIVER_LINK_REOPENED, &reopen) == UCN_OK);
    CHECK(ucn_link_get(node_a, 0U, &refreshed_link) == UCN_OK);
    CHECK(refreshed_link.generation != link_a_handle.generation);
    options = send_options(UCN_TRAFFIC_Q1);
    options.pinned_path = path_handle;
    memset(&send_handle, 0xA5, sizeof(send_handle));
    CHECK(ucn_publish(node_a, &target, payload, sizeof(payload), &options,
                      &send_handle) == UCN_ERR_NOT_FOUND);
    CHECK(send_handle.runtime_instance == UINT32_C(0xA5A5A5A5));
    CHECK(node_a->last_origin_sequence == 3U);
    CHECK(ucn_driver_link_event(node_a, link_a_handle, UCN_DRIVER_LINK_DOWN,
                                NULL) == UCN_ERR_NOT_FOUND);

    CHECK(ucn_static_path_remove(node_a, path_handle) == UCN_OK);
    CHECK(ucn_driver_rx_publish(node_b, link_b_handle, link_a.last_frame,
                                link_a.last_frame_bytes, &rx_meta) == UCN_OK);
    reopen.new_link_instance = 2U;
    CHECK(ucn_driver_link_event(node_b, link_b_handle,
                                UCN_DRIVER_LINK_REOPENED, &reopen) == UCN_OK);
    CHECK(ucn_step(node_b, 100U, &budget, &step) == UCN_OK);
    CHECK(receiver.calls == 1U);
    CHECK(ucn_get_stats(node_b, &stats) == UCN_OK);
    CHECK(stats.rx_dropped == 4U);
    CHECK(ucn_link_get(node_b, 0U, &link_b_handle) == UCN_OK);
    CHECK(ucn_driver_rx_publish(node_b, link_b_handle, link_a.last_frame,
                                link_a.last_frame_bytes, &rx_meta) == UCN_OK);
    CHECK(ucn_stop(node_b) == UCN_OK);
    CHECK(ucn_step(node_b, 101U, &budget, &step) == UCN_OK);
    CHECK(receiver.calls == 1U);
    CHECK(step.lifecycle == UCN_LIFECYCLE_QUIESCENT);
    CHECK(ucn_stop(node_a) == UCN_OK);
    CHECK(ucn_deinit(node_a) == UCN_OK);
    CHECK(ucn_deinit(node_b) == UCN_OK);
}

static void test_endpoint_callback_allows_snapshot_queries_only(void)
{
    fake_link_t link_a;
    fake_link_t link_b;
    receive_fixture_t receiver;
    ucn_node_t *node_a = NULL;
    ucn_node_t *node_b = NULL;
    ucn_link_handle_t link_b_handle;
    ucn_endpoint_config_t endpoint;
    ucn_static_path_t path_a = path_config();
    ucn_static_path_t path_b = path_config();
    ucn_path_handle_t path_a_handle;
    ucn_path_handle_t path_b_handle;
    ucn_target_t target_a = target_config();
    ucn_target_t target_b = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t send_a;
    ucn_send_handle_t send_b;
    ucn_send_view_t send_view;
    ucn_step_budget_t one = step_budget(1U);
    ucn_step_budget_t all = step_budget(8U);
    ucn_step_result_t step;
    uint8_t byte = 0x7AU;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&storage_b, 0, sizeof(storage_b));
    memset(&link_a, 0, sizeof(link_a));
    memset(&link_b, 0, sizeof(link_b));
    memset(&receiver, 0, sizeof(receiver));
    link_a.submit_result = UCN_DRIVER_COMPLETE;
    link_a.deliver_on_submit = 1U;
    link_b.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 701U, 1U, &link_a, &node_a) == UCN_OK);
    CHECK(make_node(&storage_b, 702U, 2U, &link_b, &node_b) == UCN_OK);
    CHECK(ucn_link_get(node_b, 0U, &link_b_handle) == UCN_OK);
    link_a.peer = node_b;
    link_a.peer_link = link_b_handle;
    receiver.node = node_b;
    receiver.exercise_read_queries = 1U;
    endpoint = endpoint_config(&receiver);
    CHECK(ucn_endpoint_add(node_b, &endpoint, &receiver.endpoint) == UCN_OK);
    path_b.destination_address = 1U;
    path_b.destination_binding_generation = 10U;
    target_b.address = 1U;
    target_b.binding_generation = 10U;
    CHECK(ucn_static_path_add(node_a, &path_a, &path_a_handle) == UCN_OK);
    CHECK(ucn_static_path_add(node_b, &path_b, &path_b_handle) == UCN_OK);
    CHECK(ucn_start(node_a, 0U) == UCN_OK);
    CHECK(ucn_start(node_b, 0U) == UCN_OK);
    CHECK(ucn_publish(node_b, &target_b, &byte, 1U, &options, &send_b) ==
          UCN_OK);
    receiver.query_send = send_b;
    CHECK(ucn_publish(node_a, &target_a, &byte, 1U, &options, &send_a) ==
          UCN_OK);
    CHECK(ucn_step(node_a, 1U, &all, &step) == UCN_OK);
    CHECK(ucn_step(node_b, 1U, &one, &step) == UCN_OK);
    CHECK(receiver.calls == 1U);
    CHECK(receiver.stats_query_result == UCN_OK);
    CHECK(receiver.callback_stats.rx_published == 1U);
    CHECK(receiver.send_query_result == UCN_OK);
    CHECK(receiver.callback_send_view.link_outcome ==
          UCN_LINK_OUTCOME_PENDING);
    CHECK(receiver.link_query_result == UCN_OK);
    CHECK(memcmp(&receiver.callback_link, &link_b_handle,
                 sizeof(link_b_handle)) == 0);
    CHECK(receiver.ordinary_stats_query_result == UCN_ERR_STATE);
    CHECK(receiver.ordinary_send_query_result == UCN_ERR_STATE);
    CHECK(receiver.ordinary_link_query_result == UCN_ERR_STATE);
    CHECK(receiver.forged_scope_query_result == UCN_ERR_STATE);
    memset(&receiver.callback_stats, 0xA5, sizeof(receiver.callback_stats));
    CHECK(ucn_callback_get_stats(node_b, receiver.callback_scope,
                                 &receiver.callback_stats) == UCN_ERR_STATE);
    CHECK(bytes_are_value(&receiver.callback_stats,
                          sizeof(receiver.callback_stats), 0xA5U));
    CHECK(receiver.reentry_result == UCN_ERR_STATE);
    CHECK(ucn_step(node_b, 2U, &all, &step) == UCN_OK);
    CHECK(ucn_send_query(node_b, send_b, &send_view) == UCN_OK);
    CHECK(send_view.terminal_result == UCN_OK);
    CHECK(ucn_send_forget(node_b, send_b) == UCN_OK);
    CHECK(ucn_send_forget(node_a, send_a) == UCN_OK);
    CHECK(ucn_endpoint_remove(node_b, receiver.endpoint) == UCN_OK);
    CHECK(ucn_static_path_remove(node_a, path_a_handle) == UCN_OK);
    CHECK(ucn_static_path_remove(node_b, path_b_handle) == UCN_OK);
    CHECK(ucn_stop(node_a) == UCN_OK);
    CHECK(ucn_stop(node_b) == UCN_OK);
    CHECK(ucn_deinit(node_a) == UCN_OK);
    CHECK(ucn_deinit(node_b) == UCN_OK);
}

static void test_all_traffic_classes_get_service(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options;
    ucn_step_budget_t budget = step_budget(8U);
    ucn_step_result_t step;
    ucn_stats_t stats;
    uint8_t byte = 1U;
    uint8_t traffic_class;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 303U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    for (traffic_class = 0U; traffic_class < UCN_TRAFFIC_CLASS_COUNT;
         ++traffic_class) {
        options = send_options(traffic_class);
        CHECK(ucn_publish(node, &target, &byte, 1U, &options, NULL) == UCN_OK);
    }
    CHECK(ucn_step(node, 1U, &budget, &step) == UCN_OK);
    CHECK(step.work_done == UCN_TRAFFIC_CLASS_COUNT);
    for (traffic_class = 0U; traffic_class < UCN_TRAFFIC_CLASS_COUNT;
         ++traffic_class) {
        CHECK(link.sent_by_class[traffic_class] == 1U);
    }
    CHECK(ucn_get_stats(node, &stats) == UCN_OK);
    CHECK(stats.tx_admitted == UCN_TRAFFIC_CLASS_COUNT);
    CHECK(stats.tx_completed == UCN_TRAFFIC_CLASS_COUNT);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_queue_partition_and_hot_q0_fairness(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t q0 = send_options(UCN_TRAFFIC_Q0);
    ucn_send_options_t q1 = send_options(UCN_TRAFFIC_Q1);
    ucn_send_options_t q3 = send_options(UCN_TRAFFIC_Q3);
    ucn_step_budget_t drain = step_budget(UCN_TX_SLOT_COUNT);
    ucn_step_budget_t one = step_budget(1U);
    ucn_step_result_t step;
    uint32_t sequence_before;
    uint16_t index;
    uint8_t byte = 0x61U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 304U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);

    for (index = 0U; index < UCN_Q0_DEPTH; ++index) {
        CHECK(ucn_publish(node, &target, &byte, 1U, &q0, NULL) == UCN_OK);
    }
    sequence_before = node->last_origin_sequence;
    CHECK(ucn_publish(node, &target, &byte, 1U, &q0, NULL) ==
          UCN_ERR_NO_SPACE);
    CHECK(node->last_origin_sequence == sequence_before);
    CHECK(ucn_publish(node, &target, &byte, 1U, &q1, NULL) == UCN_OK);
    CHECK(node->last_origin_sequence == sequence_before + 1U);
    CHECK(ucn_step(node, 1U, &drain, &step) == UCN_OK);
    CHECK(link.sent_by_class[UCN_TRAFFIC_Q0] == UCN_Q0_DEPTH);
    CHECK(link.sent_by_class[UCN_TRAFFIC_Q1] == 1U);

    memset(link.sent_by_class, 0, sizeof(link.sent_by_class));
    CHECK(ucn_publish(node, &target, &byte, 1U, &q3, NULL) == UCN_OK);
    for (index = 0U; index < 12U &&
                     link.sent_by_class[UCN_TRAFFIC_Q3] == 0U;
         ++index) {
        CHECK(ucn_publish(node, &target, &byte, 1U, &q0, NULL) == UCN_OK);
        CHECK(ucn_step(node, (uint64_t)(2U + index), &one, &step) == UCN_OK);
    }
    CHECK(link.sent_by_class[UCN_TRAFFIC_Q3] == 1U);
    CHECK(index <= 7U);
    CHECK(ucn_step(node, 20U, &drain, &step) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_in_doubt_requires_link_generation_fence(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_link_handle_t old_link;
    ucn_link_event_meta_t reopen;
    ucn_step_budget_t budget = step_budget(4U);
    ucn_step_result_t step;
    uint8_t byte = 2U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_SUBMIT_UNKNOWN;
    options.absolute_deadline_us = 2U;
    CHECK(make_node(&storage_a, 404U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_link_get(node, 0U, &old_link) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &send) == UCN_OK);
    CHECK(ucn_step(node, 1U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_IN_DOUBT);
    CHECK(view.buffer_released == 0U);
    CHECK(ucn_step(node, 2U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_IN_DOUBT);
    CHECK(view.buffer_released == 0U);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_step(node, 3U, &budget, &step) == UCN_OK);
    CHECK(step.lifecycle == UCN_LIFECYCLE_STOPPING);
    CHECK(ucn_deinit(node) == UCN_ERR_STATE);
    memset(&reopen, 0, sizeof(reopen));
    reopen.struct_size = sizeof(reopen);
    reopen.api_version = UCN_API_VERSION;
    reopen.new_link_instance = 2U;
    CHECK(ucn_driver_link_event(node, old_link, UCN_DRIVER_LINK_REOPENED,
                                &reopen) == UCN_OK);
    CHECK(ucn_step(node, 4U, &budget, &step) == UCN_OK);
    CHECK(step.lifecycle == UCN_LIFECYCLE_STOPPING);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_IN_DOUBT);
    CHECK(view.buffer_released == 1U);
    CHECK(ucn_send_forget(node, send) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_step(node, 4U, &budget, &step) == UCN_OK);
    CHECK(step.lifecycle == UCN_LIFECYCLE_QUIESCENT);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_tracked_capacity_is_atomic(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q2);
    ucn_send_handle_t handles[UCN_REQUEST_COUNT];
    ucn_send_handle_t extra;
    ucn_send_view_t view;
    ucn_step_budget_t budget = step_budget(UCN_REQUEST_COUNT);
    ucn_step_result_t step;
    uint16_t index;
    uint8_t byte = 3U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 505U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    for (index = 0U; index < UCN_REQUEST_COUNT; ++index) {
        options.traffic_class = (uint8_t)(index % UCN_TRAFFIC_CLASS_COUNT);
        CHECK(ucn_publish(node, &target, &byte, 1U, &options,
                          &handles[index]) == UCN_OK);
    }
    memset(&extra, 0xA5, sizeof(extra));
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &extra) ==
          UCN_ERR_NO_SPACE);
    CHECK(extra.runtime_instance == UINT32_C(0xA5A5A5A5));
    CHECK(ucn_step(node, 1U, &budget, &step) == UCN_OK);
    CHECK(step.work_done == UCN_REQUEST_COUNT);
    for (index = 0U; index < UCN_REQUEST_COUNT; ++index) {
        CHECK(ucn_send_query(node, handles[index], &view) == UCN_OK);
        CHECK(view.origin_sequence == (uint32_t)index + 1U);
        CHECK(view.terminal_result == UCN_OK);
        CHECK(ucn_send_forget(node, handles[index]) == UCN_OK);
    }
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &extra) == UCN_OK);
    CHECK(ucn_send_query(node, extra, &view) == UCN_OK);
    CHECK(view.origin_sequence == (uint32_t)UCN_REQUEST_COUNT + 1U);
    CHECK(ucn_step(node, 2U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, extra, &view) == UCN_OK);
    CHECK(view.callback_delivered == 0U);
    CHECK(ucn_send_forget(node, extra) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_submitted_deadline_reports_timeout(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q0);
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_step_budget_t budget = step_budget(4U);
    ucn_step_result_t step;
    uint8_t byte = 4U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_SUBMITTED;
    options.absolute_deadline_us = 100U;
    CHECK(make_node(&storage_a, 606U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &send) == UCN_OK);
    CHECK(ucn_step(node, 99U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_SUBMITTED);
    CHECK(view.buffer_released == 0U);
    CHECK(ucn_step(node, 100U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_TIMEOUT);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_CANCELLED);
    CHECK(view.buffer_released == 1U);
    CHECK(ucn_send_forget(node, send) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_temporary_driver_backpressure_retries_same_request(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_step_budget_t one = step_budget(1U);
    ucn_step_result_t step;
    uint32_t original_sequence;
    uint8_t byte = 0xB1U;
    uint8_t attempt;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    link.not_submitted_remaining = 3U;
    options.absolute_deadline_us = 20U;
    CHECK(make_node(&storage_a, 608U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &send) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    original_sequence = view.origin_sequence;

    for (attempt = 0U; attempt < 3U; ++attempt) {
        CHECK(ucn_step(node, (uint64_t)(attempt + 1U), &one, &step) == UCN_OK);
        CHECK(link.submit_calls == (uint32_t)attempt + 1U);
        CHECK(ucn_send_query(node, send, &view) == UCN_OK);
        CHECK(view.origin_sequence == original_sequence);
        CHECK(view.link_outcome == UCN_LINK_OUTCOME_PENDING);
        CHECK(view.buffer_released == 0U);
    }
    CHECK(ucn_step(node, 4U, &one, &step) == UCN_OK);
    CHECK(link.submit_calls == 4U);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.origin_sequence == original_sequence);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_COMPLETE);
    CHECK(view.buffer_released == 1U);
    CHECK(ucn_send_forget(node, send) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_temporary_driver_backpressure_expires_at_deadline(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_step_budget_t one = step_budget(1U);
    ucn_step_result_t step;
    uint8_t byte = 0xB2U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_NOT_SUBMITTED;
    options.absolute_deadline_us = 3U;
    CHECK(make_node(&storage_a, 609U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &send) == UCN_OK);
    CHECK(ucn_step(node, 1U, &one, &step) == UCN_OK);
    CHECK(ucn_step(node, 2U, &one, &step) == UCN_OK);
    CHECK(link.submit_calls == 2U);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_PENDING);
    CHECK(ucn_step(node, 3U, &one, &step) == UCN_OK);
    CHECK(link.submit_calls == 2U);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_TIMEOUT);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_CANCELLED);
    CHECK(view.buffer_released == 1U);
    CHECK(ucn_send_forget(node, send) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_expired_deadline_is_rejected_before_admission(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q0);
    ucn_send_options_t aliased_options;
    ucn_send_handle_t send;
    ucn_send_handle_t before;
    ucn_stats_t stats;
    uint8_t byte = 5U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    options.absolute_deadline_us = 100U;
    memset(&send, 0xA5, sizeof(send));
    before = send;
    CHECK(make_node(&storage_a, 607U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 100U) == UCN_OK);
    aliased_options = send_options(UCN_TRAFFIC_Q0);
    aliased_options.completion = completion;
    aliased_options.completion_context = &send;
    CHECK(ucn_publish(node, &target, &byte, 1U, &aliased_options, &send) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&send, &before, sizeof(send)) == 0);
    CHECK(node->last_origin_sequence == 0U);
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &send) ==
          UCN_ERR_TIMEOUT);
    CHECK(memcmp(&send, &before, sizeof(send)) == 0);
    CHECK(node->last_origin_sequence == 0U);
    CHECK(link.submit_calls == 0U);
    CHECK(ucn_get_stats(node, &stats) == UCN_OK);
    CHECK(stats.tx_admitted == 0U);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_completion_latched_before_deadline_beats_late_timer_scan(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q0);
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_step_budget_t one = step_budget(1U);
    ucn_step_result_t step;
    uint8_t byte = 7U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_SUBMITTED;
    options.absolute_deadline_us = 100U;
    CHECK(make_node(&storage_a, 611U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &send) == UCN_OK);
    CHECK(ucn_step(node, 1U, &one, &step) == UCN_OK);
    CHECK(ucn_driver_tx_complete(node, link.last_token, UCN_OK, NULL) ==
          UCN_OK);
    node->work_cursor = 1U;
    CHECK(ucn_step(node, 100U, &one, &step) == UCN_OK);
    CHECK(ucn_step(node, 100U, &one, &step) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_COMPLETE);
    CHECK(ucn_send_forget(node, send) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_driver_gate_contention_keeps_cancel_retryable(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path;
    ucn_path_handle_t path_handle;
    ucn_target_t target;
    ucn_send_options_t options;
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_step_budget_t budget = step_budget(8U);
    ucn_step_result_t step;
    uint16_t tx_slot;
    uint8_t payload = 0x91U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_SUBMITTED;
    CHECK(make_node(&storage_a, 404U, 1U, &link, &node) == UCN_OK);
    path = path_config();
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 1U) == UCN_OK);
    target = target_config();
    options = send_options(UCN_TRAFFIC_Q1);

    CHECK(ucn_publish(node, &target, &payload, 1U, &options, &send) ==
          UCN_OK);
    link.driver_gate_active = 1U;
    CHECK(ucn_step(node, 2U, &budget, &step) == UCN_OK);
    CHECK(link.submit_calls == 0U);
    CHECK(step.work_done == 0U);
    CHECK(step.more_work == 1U);
    CHECK(node->lifecycle == UCN_LIFECYCLE_RUNNING);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_PENDING);
    link.driver_gate_active = 0U;
    CHECK(ucn_step(node, 3U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_SUBMITTED);
    link.driver_gate_active = 1U;
    CHECK(ucn_send_cancel(node, send) == UCN_OK);
    CHECK(link.cancel_calls == 0U);
    CHECK(ucn_step(node, 4U, &budget, &step) == UCN_OK);
    CHECK(step.work_done == 0U);
    CHECK(step.more_work == 1U);
    CHECK(link.cancel_calls == 0U);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.link_outcome == UCN_LINK_OUTCOME_SUBMITTED);
    link.driver_gate_active = 0U;
    CHECK(ucn_send_cancel(node, send) == UCN_OK);
    CHECK(ucn_step(node, 4U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_CANCELLED);
    CHECK(ucn_send_forget(node, send) == UCN_OK);

    CHECK(ucn_publish(node, &target, &payload, 1U, &options, &send) ==
          UCN_OK);
    CHECK(ucn_send_cancel(node, send) == UCN_OK);
    CHECK(link.submit_calls == 1U);
    CHECK(link.cancel_calls == 1U);
    CHECK(ucn_step(node, 5U, &budget, &step) == UCN_OK);
    CHECK(link.submit_calls == 1U);
    CHECK(link.cancel_calls == 1U);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_CANCELLED);
    CHECK(ucn_send_forget(node, send) == UCN_OK);

    CHECK(ucn_publish(node, &target, &payload, 1U, &options, &send) ==
          UCN_OK);
    CHECK(ucn_step(node, 5U, &budget, &step) == UCN_OK);
    CHECK(link.cancel_calls == 1U);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(node->lifecycle == UCN_LIFECYCLE_STOPPING);
    CHECK(link.cancel_calls == 1U);
    link.driver_gate_active = 1U;
    tx_slot = node->requests[send.slot - 1U].tx_slot;
    CHECK(tx_slot < UCN_TX_SLOT_COUNT);
    CHECK(node->tx_slots[tx_slot].cancel_requested == 0U);
    CHECK(ucn_step(node, 6U, &budget, &step) == UCN_OK);
    CHECK(step.work_done == 0U);
    CHECK(step.more_work == 1U);
    CHECK(step.lifecycle == UCN_LIFECYCLE_STOPPING);
    link.driver_gate_active = 0U;
    CHECK(ucn_step(node, 7U, &budget, &step) == UCN_OK);
    CHECK(step.lifecycle == UCN_LIFECYCLE_STOPPING);
    CHECK(link.cancel_calls == 2U);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_CANCELLED);
    CHECK(ucn_send_forget(node, send) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_step(node, 7U, &budget, &step) == UCN_OK);
    CHECK(step.lifecycle == UCN_LIFECYCLE_QUIESCENT);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_rx_hot_slot_cannot_starve_older_frame(void)
{
    fake_link_t link_a;
    fake_link_t link_b;
    receive_fixture_t receiver;
    ucn_node_t *node_a = NULL;
    ucn_node_t *node_b = NULL;
    ucn_link_handle_t link_b_handle;
    ucn_endpoint_config_t endpoint;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_step_budget_t two = step_budget(2U);
    ucn_step_budget_t one = step_budget(1U);
    ucn_step_result_t step;
    uint8_t first = 1U;
    uint8_t older = 2U;
    uint8_t hot = 0U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&storage_b, 0, sizeof(storage_b));
    memset(&link_a, 0, sizeof(link_a));
    memset(&link_b, 0, sizeof(link_b));
    memset(&receiver, 0, sizeof(receiver));
    link_a.submit_result = UCN_DRIVER_COMPLETE;
    link_a.deliver_on_submit = 1U;
    link_b.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 707U, 1U, &link_a, &node_a) == UCN_OK);
    CHECK(make_node(&storage_b, 808U, 2U, &link_b, &node_b) == UCN_OK);
    CHECK(ucn_link_get(node_b, 0U, &link_b_handle) == UCN_OK);
    link_a.peer = node_b;
    link_a.peer_link = link_b_handle;
    receiver.node = node_b;
    endpoint = endpoint_config(&receiver);
    CHECK(ucn_endpoint_add(node_b, &endpoint, &receiver.endpoint) == UCN_OK);
    CHECK(ucn_static_path_add(node_a, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node_a, 0U) == UCN_OK);
    CHECK(ucn_start(node_b, 0U) == UCN_OK);

    CHECK(ucn_publish(node_a, &target, &first, 1U, &options, NULL) == UCN_OK);
    CHECK(ucn_publish(node_a, &target, &older, 1U, &options, NULL) == UCN_OK);
    CHECK(ucn_step(node_a, 1U, &two, &step) == UCN_OK);
    CHECK(ucn_step(node_b, 1U, &one, &step) == UCN_OK);
    CHECK(receiver.marker_calls[1] == 1U);
    CHECK(receiver.marker_calls[2] == 0U);

    CHECK(ucn_publish(node_a, &target, &hot, 1U, &options, NULL) == UCN_OK);
    CHECK(ucn_step(node_a, 2U, &one, &step) == UCN_OK);
    CHECK(ucn_step(node_b, 2U, &one, &step) == UCN_OK);
    CHECK(receiver.marker_calls[2] == 1U);
    CHECK(receiver.marker_calls[0] == 0U);

    CHECK(ucn_step(node_b, 3U, &one, &step) == UCN_OK);
    CHECK(receiver.marker_calls[0] == 1U);
    CHECK(ucn_static_path_remove(node_a, path_handle) == UCN_OK);
    CHECK(ucn_stop(node_a) == UCN_OK);
    CHECK(ucn_stop(node_b) == UCN_OK);
    CHECK(ucn_deinit(node_a) == UCN_OK);
    CHECK(ucn_deinit(node_b) == UCN_OK);
}

static void test_rx_flood_cannot_starve_completion_or_cancel_cleanup(void)
{
    fake_link_t link;
    receive_fixture_t receiver;
    ucn_node_t *node = NULL;
    ucn_link_handle_t link_handle;
    ucn_endpoint_config_t endpoint;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t completed_send;
    ucn_send_handle_t cancelled_send;
    ucn_send_view_t view;
    ucn_rx_meta_t meta;
    ucn_step_budget_t one = step_budget(1U);
    ucn_step_result_t step;
    uint8_t frame[UCN_ADAPTER_FRAME_BYTES];
    uint8_t payload = 0x39U;
    size_t frame_bytes;
    uint32_t sequence = 1U;
    uint32_t prior_rx_calls;
    uint8_t iteration;

    memset(&storage_b, 0, sizeof(storage_b));
    memset(&link, 0, sizeof(link));
    memset(&receiver, 0, sizeof(receiver));
    link.submit_result = UCN_DRIVER_SUBMITTED;
    CHECK(make_node(&storage_b, 809U, 2U, &link, &node) == UCN_OK);
    CHECK(ucn_link_get(node, 0U, &link_handle) == UCN_OK);
    receiver.node = node;
    endpoint = endpoint_config(&receiver);
    CHECK(ucn_endpoint_add(node, &endpoint, &receiver.endpoint) == UCN_OK);
    path.destination_address = 1U;
    path.destination_binding_generation = 10U;
    target.address = 1U;
    target.binding_generation = 10U;
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);

    CHECK(ucn_publish(node, &target, &payload, 1U, &options,
                      &completed_send) == UCN_OK);
    CHECK(ucn_step(node, 1U, &one, &step) == UCN_OK);
    CHECK(link.submit_calls == 1U);
    CHECK(ucn_driver_tx_complete(node, link.last_token, UCN_OK, NULL) ==
          UCN_OK);
    CHECK(ucn_publish(node, &target, &payload, 1U, &options,
                      &cancelled_send) == UCN_OK);
    CHECK(ucn_send_cancel(node, cancelled_send) == UCN_OK);

    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    meta.api_version = UCN_API_VERSION;
    frame_bytes = make_rx_frame(sequence++, 0U, frame);
    CHECK(ucn_driver_rx_publish(node, link_handle, frame, frame_bytes,
                                &meta) == UCN_OK);
    node->work_cursor = 2U;
    for (iteration = 0U; iteration < 4U; ++iteration) {
        prior_rx_calls = receiver.calls;
        CHECK(ucn_step(node, (uint64_t)(2U + iteration), &one, &step) ==
              UCN_OK);
        if (receiver.calls != prior_rx_calls) {
            frame_bytes = make_rx_frame(sequence++, 0U, frame);
            CHECK(ucn_driver_rx_publish(node, link_handle, frame, frame_bytes,
                                        &meta) == UCN_OK);
        }
    }
    CHECK(receiver.calls >= 1U);
    CHECK(link.submit_calls == 1U);
    CHECK(link.cancel_calls == 0U);
    CHECK(ucn_send_query(node, completed_send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(ucn_send_query(node, cancelled_send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_ERR_CANCELLED);
    CHECK(ucn_send_forget(node, completed_send) == UCN_OK);
    CHECK(ucn_send_forget(node, cancelled_send) == UCN_OK);

    CHECK(ucn_stop(node) == UCN_OK);
    while (node->lifecycle == UCN_LIFECYCLE_STOPPING) {
        CHECK(ucn_step(node, 7U, &one, &step) == UCN_OK);
    }
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_endpoint_remove(node, receiver.endpoint) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_exhausted_handles_do_not_hide_reusable_capacity(void)
{
    fake_link_t link;
    receive_fixture_t receiver;
    ucn_node_t *node = NULL;
    ucn_endpoint_config_t endpoint;
    ucn_endpoint_handle_t endpoint_handle;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_step_budget_t budget = step_budget(2U);
    ucn_step_result_t step;
    uint8_t byte = 6U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    memset(&receiver, 0, sizeof(receiver));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 909U, 1U, &link, &node) == UCN_OK);

    node->endpoints[0].generation = UINT16_MAX;
    node->endpoint_allocate_cursor = 0U;
    endpoint = endpoint_config(&receiver);
    CHECK(ucn_endpoint_add(node, &endpoint, &endpoint_handle) == UCN_OK);
    CHECK(endpoint_handle.slot == 2U);

    node->paths[0].generation = UINT16_MAX;
    node->path_allocate_cursor = 0U;
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(path_handle.slot == 2U);

    node->requests[0].generation = UINT16_MAX;
    node->receipts[0].generation = UINT16_MAX;
    node->attempts[0].generation = UINT16_MAX;
    node->buffers[0].generation = UINT16_MAX;
    node->request_allocate_cursor = 0U;
    node->receipt_allocate_cursor = 0U;
    node->attempt_allocate_cursor = 0U;
    node->buffer_allocate_cursor = 0U;
    CHECK(ucn_start(node, 0U) == UCN_OK);
    CHECK(ucn_publish(node, &target, &byte, sizeof(byte), &options, &send) ==
          UCN_OK);
    CHECK(send.slot == 2U);
    CHECK(ucn_step(node, 1U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(ucn_send_forget(node, send) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_endpoint_remove(node, endpoint_handle) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_monotonic_time_high_water_is_enforced(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_step_budget_t budget = step_budget(1U);
    ucn_step_result_t step;
    ucn_step_result_t sentinel;
    union {
        ucn_step_budget_t budget;
        ucn_step_result_t result;
    } alias;
    unsigned char alias_before[sizeof(alias)];

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 1001U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_start(node, 100U) == UCN_OK);
    memset(&alias, 0, sizeof(alias));
    alias.budget = step_budget(1U);
    memcpy(alias_before, &alias, sizeof(alias));
    CHECK(ucn_step(node, 100U, &alias.budget, &alias.result) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&alias, alias_before, sizeof(alias)) == 0);
    memset(&step, 0xA5, sizeof(step));
    sentinel = step;
    CHECK(ucn_step(node, 99U, &budget, &step) == UCN_ERR_STATE);
    CHECK(memcmp(&step, &sentinel, sizeof(step)) == 0);
    CHECK(ucn_step(node, 100U, &budget, &step) == UCN_OK);
    CHECK(ucn_step(node, 101U, &budget, &step) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_start(node, 100U) == UCN_ERR_STATE);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_handle_generation_and_kind_are_exact(void)
{
    fake_link_t link;
    receive_fixture_t receiver;
    ucn_node_t *node = NULL;
    ucn_endpoint_config_t endpoint;
    ucn_endpoint_handle_t endpoint_first;
    ucn_endpoint_handle_t endpoint_second;
    ucn_endpoint_handle_t wrong_kind;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_first;
    ucn_path_handle_t path_second;
    uint16_t endpoint_slot;
    uint16_t path_slot;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    memset(&receiver, 0, sizeof(receiver));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 910U, 1U, &link, &node) == UCN_OK);

    endpoint = endpoint_config(&receiver);
    CHECK(ucn_endpoint_add(node, &endpoint, &endpoint_first) == UCN_OK);
    endpoint_slot = (uint16_t)(endpoint_first.slot - 1U);
    CHECK(ucn_endpoint_remove(node, endpoint_first) == UCN_OK);
    node->endpoint_allocate_cursor = endpoint_slot;
    CHECK(ucn_endpoint_add(node, &endpoint, &endpoint_second) == UCN_OK);
    CHECK(endpoint_second.slot == endpoint_first.slot);
    CHECK(endpoint_second.generation != endpoint_first.generation);
    CHECK(ucn_endpoint_remove(node, endpoint_first) == UCN_ERR_NOT_FOUND);
    wrong_kind = endpoint_second;
    wrong_kind.object_kind = UCN_OBJECT_KIND_PATH;
    CHECK(ucn_endpoint_remove(node, wrong_kind) == UCN_ERR_NOT_FOUND);

    CHECK(ucn_static_path_add(node, &path, &path_first) == UCN_OK);
    path_slot = (uint16_t)(path_first.slot - 1U);
    CHECK(ucn_static_path_remove(node, path_first) == UCN_OK);
    node->path_allocate_cursor = path_slot;
    CHECK(ucn_static_path_add(node, &path, &path_second) == UCN_OK);
    CHECK(path_second.slot == path_first.slot);
    CHECK(path_second.generation != path_first.generation);
    CHECK(ucn_static_path_remove(node, path_first) == UCN_ERR_NOT_FOUND);

    CHECK(ucn_static_path_remove(node, path_second) == UCN_OK);
    CHECK(ucn_endpoint_remove(node, endpoint_second) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_static_path_requires_ready_link(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_link_handle_t link_handle;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t output;
    ucn_path_handle_t before;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 911U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_link_get(node, 0U, &link_handle) == UCN_OK);
    CHECK(ucn_driver_link_event(node, link_handle, UCN_DRIVER_LINK_DOWN,
                                NULL) == UCN_OK);
    memset(&output, 0xA5, sizeof(output));
    before = output;
    CHECK(ucn_static_path_add(node, &path, &output) == UCN_ERR_STATE);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    CHECK(ucn_driver_link_event(node, link_handle, UCN_DRIVER_LINK_UP,
                                NULL) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &output) == UCN_OK);
    CHECK(ucn_static_path_remove(node, output) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_request_generation_exhaustion_faults_owner(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t output;
    ucn_send_handle_t before;
    uint16_t index;
    uint8_t byte = 1U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 912U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    for (index = 0U; index < UCN_REQUEST_COUNT; ++index) {
        node->requests[index].generation = UINT16_MAX;
    }
    memset(&output, 0xA5, sizeof(output));
    before = output;
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &output) ==
          UCN_ERR_EXHAUSTED);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    CHECK(node->last_origin_sequence == 0U);
    CHECK(node->paths[path_handle.slot - 1U].references == 0U);
    CHECK(node->lifecycle == UCN_LIFECYCLE_FAULT);
    CHECK(link.submit_calls == 0U);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_origin_sequence_exhaustion_is_irreversible(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_static_path_t path = path_config();
    ucn_path_handle_t path_handle;
    ucn_target_t target = target_config();
    ucn_send_options_t options = send_options(UCN_TRAFFIC_Q1);
    ucn_send_handle_t first;
    ucn_send_handle_t output;
    ucn_send_handle_t before;
    ucn_send_view_t view;
    ucn_step_budget_t budget = step_budget(2U);
    ucn_step_result_t step;
    uint8_t byte = 2U;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 913U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_static_path_add(node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    node->last_origin_sequence = UINT32_MAX - 1U;
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &first) == UCN_OK);
    CHECK(ucn_send_query(node, first, &view) == UCN_OK);
    CHECK(view.origin_sequence == UINT32_MAX);
    CHECK(ucn_step(node, 1U, &budget, &step) == UCN_OK);
    CHECK(ucn_send_query(node, first, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_OK);

    memset(&output, 0xA5, sizeof(output));
    before = output;
    CHECK(ucn_publish(node, &target, &byte, 1U, &options, &output) ==
          UCN_ERR_EXHAUSTED);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    CHECK(node->last_origin_sequence == UINT32_MAX);
    CHECK(node->lifecycle == UCN_LIFECYCLE_FAULT);
    CHECK(ucn_send_forget(node, first) == UCN_OK);
    CHECK(ucn_static_path_remove(node, path_handle) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_adapter_fault_is_observed_by_core(void)
{
    fake_link_t link;
    ucn_node_t *node = NULL;
    ucn_link_handle_t link_handle;
    ucn_link_event_meta_t reopen;

    memset(&storage_a, 0, sizeof(storage_a));
    memset(&link, 0, sizeof(link));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_a, 914U, 1U, &link, &node) == UCN_OK);
    CHECK(ucn_link_get(node, 0U, &link_handle) == UCN_OK);
    node->adapter.links[0].instance_generation = UINT32_MAX;
    memset(&reopen, 0, sizeof(reopen));
    reopen.struct_size = sizeof(reopen);
    reopen.api_version = UCN_API_VERSION;
    reopen.new_link_instance = UINT32_MAX;
    CHECK(ucn_driver_link_event(node, link_handle,
                                UCN_DRIVER_LINK_REOPENED, &reopen) ==
          UCN_ERR_EXHAUSTED);
    CHECK(node->lifecycle == UCN_LIFECYCLE_INITIALIZED);
    CHECK(ucn_start(node, 0U) == UCN_ERR_STATE);
    CHECK(node->lifecycle == UCN_LIFECYCLE_FAULT);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

static void test_trusted_o0_replay_window(void)
{
    static const uint32_t sequences[] = {100U, 99U, 100U, 99U,
                                         36U, 165U, 164U, 164U};
    static const uint32_t delivered_after[] = {1U, 2U, 2U, 2U,
                                               2U, 3U, 4U, 4U};
    fake_link_t link;
    receive_fixture_t receiver;
    ucn_node_t *node = NULL;
    ucn_link_handle_t link_handle;
    ucn_endpoint_config_t endpoint;
    ucn_rx_meta_t meta;
    ucn_step_budget_t budget = step_budget(1U);
    ucn_step_result_t step;
    ucn_stats_t stats;
    uint8_t frame[UCN_ADAPTER_FRAME_BYTES];
    size_t frame_bytes;
    size_t index;

    memset(&storage_b, 0, sizeof(storage_b));
    memset(&link, 0, sizeof(link));
    memset(&receiver, 0, sizeof(receiver));
    link.submit_result = UCN_DRIVER_COMPLETE;
    CHECK(make_node(&storage_b, 915U, 2U, &link, &node) == UCN_OK);
    CHECK(ucn_link_get(node, 0U, &link_handle) == UCN_OK);
    receiver.node = node;
    endpoint = endpoint_config(&receiver);
    CHECK(ucn_endpoint_add(node, &endpoint, &receiver.endpoint) == UCN_OK);
    CHECK(ucn_start(node, 0U) == UCN_OK);
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    meta.api_version = UCN_API_VERSION;

    for (index = 0U; index < sizeof(sequences) / sizeof(sequences[0]);
         ++index) {
        frame_bytes = make_rx_frame(sequences[index],
                                    (uint8_t)(index % 3U), frame);
        meta.timestamp_us = (uint64_t)(index + 1U);
        CHECK(ucn_driver_rx_publish(node, link_handle, frame, frame_bytes,
                                    &meta) == UCN_OK);
        CHECK(ucn_step(node, (uint64_t)(index + 1U), &budget, &step) == UCN_OK);
        CHECK(receiver.calls == delivered_after[index]);
    }
    CHECK(ucn_get_stats(node, &stats) == UCN_OK);
    CHECK(stats.rx_published == 8U);
    CHECK(stats.rx_delivered == 4U);
    CHECK(stats.rx_dropped == 4U);
    CHECK(ucn_endpoint_remove(node, receiver.endpoint) == UCN_OK);
    CHECK(ucn_stop(node) == UCN_OK);
    CHECK(ucn_deinit(node) == UCN_OK);
}

int main(void)
{
    test_invalid_init_is_atomic();
    test_driver_facts_cannot_alias_node_storage();
    test_initialized_node_uses_stop_fence_before_deinit();
    test_static_end_to_end();
    test_endpoint_callback_allows_snapshot_queries_only();
    test_all_traffic_classes_get_service();
    test_queue_partition_and_hot_q0_fairness();
    test_in_doubt_requires_link_generation_fence();
    test_tracked_capacity_is_atomic();
    test_submitted_deadline_reports_timeout();
    test_temporary_driver_backpressure_retries_same_request();
    test_temporary_driver_backpressure_expires_at_deadline();
    test_expired_deadline_is_rejected_before_admission();
    test_completion_latched_before_deadline_beats_late_timer_scan();
    test_driver_gate_contention_keeps_cancel_retryable();
    test_rx_hot_slot_cannot_starve_older_frame();
    test_rx_flood_cannot_starve_completion_or_cancel_cleanup();
    test_exhausted_handles_do_not_hide_reusable_capacity();
    test_monotonic_time_high_water_is_enforced();
    test_handle_generation_and_kind_are_exact();
    test_static_path_requires_ready_link();
    test_request_generation_exhaustion_faults_owner();
    test_origin_sequence_exhaustion_is_irreversible();
    test_adapter_fault_is_observed_by_core();
    test_trusted_o0_replay_window();
    if (failures != 0) {
        printf("core static failures=%d\n", failures);
        return 1;
    }
    printf("core static tests passed\n");
    return 0;
}
