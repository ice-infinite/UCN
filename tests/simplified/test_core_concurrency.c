#include "ucn/ucn_simplified.h"
#include "internal/ucn_wire.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#define RX_MESSAGES 64U

#define CHECK(condition_) do { \
    if (!(condition_)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition_); \
        return 1; \
    } \
} while (0)

typedef struct shared_fixture {
    pthread_mutex_t state_mutex;
    pthread_mutex_t driver_gate_mutex;
    pthread_mutex_t event_mutex;
    pthread_cond_t event_condition;
    ucn_node_t *node;
    ucn_link_handle_t link;
    ucn_driver_token_t token;
    ucn_callback_scope_t callback_scope;
    ucn_stats_t callback_stats;
    ucn_result_t foreign_unscoped_result;
    ucn_result_t scoped_result;
    uint32_t received;
    uint8_t token_ready;
    uint8_t driver_done;
    uint8_t callback_waiting;
    uint8_t callback_release;
    uint8_t failed;
} shared_fixture_t;

static UCN_DECLARE_STORAGE(storage);

static void set_driver_done(shared_fixture_t *fixture, bool failed);

static ucn_result_t state_lock_enter(void *context)
{
    shared_fixture_t *fixture = (shared_fixture_t *)context;

    return fixture != NULL &&
                   pthread_mutex_lock(&fixture->state_mutex) == 0
               ? UCN_OK
               : UCN_ERR_STATE;
}

static void state_lock_leave(void *context)
{
    shared_fixture_t *fixture = (shared_fixture_t *)context;

    if (fixture != NULL) {
        (void)pthread_mutex_unlock(&fixture->state_mutex);
    }
}

static ucn_result_t driver_gate_enter(void *context)
{
    shared_fixture_t *fixture = (shared_fixture_t *)context;

    return fixture != NULL &&
                   pthread_mutex_trylock(&fixture->driver_gate_mutex) == 0
               ? UCN_OK
               : UCN_ERR_STATE;
}

static void driver_gate_leave(void *context)
{
    shared_fixture_t *fixture = (shared_fixture_t *)context;

    if (fixture != NULL) {
        (void)pthread_mutex_unlock(&fixture->driver_gate_mutex);
    }
}

static ucn_driver_submit_result_t submit(void *context,
                                         ucn_link_handle_t link,
                                         const uint8_t *bytes,
                                         size_t length,
                                         ucn_driver_token_t token)
{
    shared_fixture_t *fixture = (shared_fixture_t *)context;

    if (fixture == NULL || bytes == NULL || length == 0U ||
        link.object_kind != UCN_OBJECT_KIND_LINK ||
        pthread_mutex_lock(&fixture->event_mutex) != 0) {
        return UCN_DRIVER_SUBMIT_UNKNOWN;
    }
    fixture->token = token;
    fixture->token_ready = 1U;
    (void)pthread_cond_broadcast(&fixture->event_condition);
    (void)pthread_mutex_unlock(&fixture->event_mutex);
    return UCN_DRIVER_SUBMITTED;
}

static ucn_result_t cancel(void *context, ucn_driver_token_t token)
{
    (void)context;
    (void)token;
    return UCN_OK;
}

static ucn_endpoint_disposition_t receive(
    void *context,
    const ucn_endpoint_message_t *message)
{
    shared_fixture_t *fixture = (shared_fixture_t *)context;

    if (fixture == NULL || message == NULL || message->payload_bytes != 1U ||
        message->payload[0] != UINT8_C(0x5A)) {
        return UCN_ENDPOINT_DROP;
    }
    if (fixture->received == 0U) {
        if (pthread_mutex_lock(&fixture->event_mutex) != 0) {
            return UCN_ENDPOINT_DROP;
        }
        fixture->callback_scope = message->callback_scope;
        fixture->callback_waiting = 1U;
        (void)pthread_cond_broadcast(&fixture->event_condition);
        while (!fixture->callback_release) {
            if (pthread_cond_wait(&fixture->event_condition,
                                  &fixture->event_mutex) != 0) {
                fixture->failed = 1U;
                (void)pthread_mutex_unlock(&fixture->event_mutex);
                return UCN_ENDPOINT_DROP;
            }
        }
        (void)pthread_mutex_unlock(&fixture->event_mutex);
    }
    ++fixture->received;
    return UCN_ENDPOINT_ACCEPT;
}

static void *foreign_query_thread(void *argument)
{
    shared_fixture_t *fixture = (shared_fixture_t *)argument;
    ucn_callback_scope_t scope;
    ucn_stats_t unscoped_output;
    bool query_failed;

    if (pthread_mutex_lock(&fixture->event_mutex) != 0) {
        set_driver_done(fixture, true);
        return NULL;
    }
    while (!fixture->callback_waiting) {
        if (pthread_cond_wait(&fixture->event_condition,
                              &fixture->event_mutex) != 0) {
            fixture->failed = 1U;
            fixture->callback_release = 1U;
            (void)pthread_cond_broadcast(&fixture->event_condition);
            (void)pthread_mutex_unlock(&fixture->event_mutex);
            return NULL;
        }
    }
    scope = fixture->callback_scope;
    (void)pthread_mutex_unlock(&fixture->event_mutex);

    memset(&unscoped_output, 0xA5, sizeof(unscoped_output));
    fixture->foreign_unscoped_result =
        ucn_get_stats(fixture->node, &unscoped_output);
    fixture->scoped_result =
        ucn_callback_get_stats(fixture->node, scope,
                               &fixture->callback_stats);
    query_failed = fixture->foreign_unscoped_result != UCN_ERR_STATE ||
                   fixture->scoped_result != UCN_OK;

    if (pthread_mutex_lock(&fixture->event_mutex) != 0) {
        set_driver_done(fixture, true);
        return NULL;
    }
    if (query_failed) {
        fixture->failed = 1U;
    }
    fixture->callback_release = 1U;
    (void)pthread_cond_broadcast(&fixture->event_condition);
    (void)pthread_mutex_unlock(&fixture->event_mutex);
    return NULL;
}

static int driver_failed(shared_fixture_t *fixture)
{
    int failed;

    if (pthread_mutex_lock(&fixture->event_mutex) != 0) {
        return 1;
    }
    failed = fixture->failed != 0U;
    (void)pthread_mutex_unlock(&fixture->event_mutex);
    return failed;
}

static void set_driver_done(shared_fixture_t *fixture, bool failed)
{
    if (pthread_mutex_lock(&fixture->event_mutex) == 0) {
        fixture->failed = failed ? 1U : fixture->failed;
        fixture->driver_done = 1U;
        (void)pthread_cond_broadcast(&fixture->event_condition);
        (void)pthread_mutex_unlock(&fixture->event_mutex);
    }
}

static void *driver_thread(void *argument)
{
    shared_fixture_t *fixture = (shared_fixture_t *)argument;
    ucn_i_c1_frame_t frame;
    ucn_rx_meta_t meta;
    ucn_driver_token_t token;
    uint8_t payload = 0x5AU;
    uint8_t encoded[32];
    size_t encoded_bytes;
    uint32_t index;

    if (pthread_mutex_lock(&fixture->event_mutex) != 0) {
        set_driver_done(fixture, true);
        return NULL;
    }
    while (!fixture->token_ready) {
        if (pthread_cond_wait(&fixture->event_condition,
                              &fixture->event_mutex) != 0) {
            fixture->failed = 1U;
            fixture->driver_done = 1U;
            (void)pthread_mutex_unlock(&fixture->event_mutex);
            return NULL;
        }
    }
    token = fixture->token;
    (void)pthread_mutex_unlock(&fixture->event_mutex);

    if (ucn_driver_tx_complete(fixture->node, token, UCN_OK, NULL) != UCN_OK) {
        set_driver_done(fixture, true);
        return NULL;
    }
    memset(&frame, 0, sizeof(frame));
    frame.payload = &payload;
    frame.payload_bytes = 1U;
    frame.source_address = 2U;
    frame.destination_address = 1U;
    frame.origin_sequence = 1U;
    frame.service_id = 77U;
    frame.traffic_class = UCN_TRAFFIC_Q1;
    frame.hop_limit = 1U;
    if (ucn_i_c1_encode(&frame, 1U, encoded, sizeof(encoded),
                        &encoded_bytes) != UCN_OK) {
        set_driver_done(fixture, true);
        return NULL;
    }
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    meta.api_version = UCN_API_VERSION;
    for (index = 0U; index < RX_MESSAGES; ++index) {
        ucn_result_t result;
        frame.origin_sequence = index + 1U;
        if (ucn_i_c1_encode(&frame, 1U, encoded, sizeof(encoded),
                            &encoded_bytes) != UCN_OK) {
            set_driver_done(fixture, true);
            return NULL;
        }
        do {
            result = ucn_driver_rx_publish(
                fixture->node, fixture->link, encoded, encoded_bytes, &meta);
            if (result == UCN_ERR_NO_SPACE) {
                sched_yield();
            }
        } while (result == UCN_ERR_NO_SPACE);
        if (result != UCN_OK) {
            set_driver_done(fixture, true);
            return NULL;
        }
    }
    set_driver_done(fixture, false);
    return NULL;
}

static void *step_thread(void *argument)
{
    shared_fixture_t *fixture = (shared_fixture_t *)argument;
    ucn_step_budget_t budget;
    uint64_t now_us = 1U;
    uint32_t idle_after_done = 0U;

    memset(&budget, 0, sizeof(budget));
    budget.struct_size = sizeof(budget);
    budget.api_version = UCN_API_VERSION;
    budget.max_work = 8U;
    while (idle_after_done < 2U) {
        ucn_step_result_t result;
        bool done;

        if (ucn_step(fixture->node, now_us++, &budget, &result) != UCN_OK) {
            set_driver_done(fixture, true);
            return NULL;
        }
        if (pthread_mutex_lock(&fixture->event_mutex) != 0) {
            set_driver_done(fixture, true);
            return NULL;
        }
        done = fixture->driver_done != 0U;
        (void)pthread_mutex_unlock(&fixture->event_mutex);
        if (done && result.work_done == 0U) {
            ++idle_after_done;
        } else {
            idle_after_done = 0U;
        }
        sched_yield();
    }
    return NULL;
}

int main(void)
{
    shared_fixture_t fixture;
    ucn_static_binding_t bindings[2];
    ucn_link_port_t link_port;
    ucn_ports_t ports;
    ucn_config_t config;
    ucn_endpoint_config_t endpoint;
    ucn_endpoint_handle_t endpoint_handle;
    ucn_static_path_t path;
    ucn_path_handle_t path_handle;
    ucn_target_t target;
    ucn_send_options_t options;
    ucn_send_handle_t send;
    ucn_send_view_t view;
    ucn_stats_t stats;
    pthread_t driver;
    pthread_t owner;
    pthread_t foreign_query;
    uint8_t payload = 0x33U;

    memset(&fixture, 0, sizeof(fixture));
    memset(&storage, 0, sizeof(storage));
    memset(bindings, 0, sizeof(bindings));
    memset(&link_port, 0, sizeof(link_port));
    memset(&ports, 0, sizeof(ports));
    memset(&config, 0, sizeof(config));
    CHECK(pthread_mutex_init(&fixture.state_mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture.driver_gate_mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture.event_mutex, NULL) == 0);
    CHECK(pthread_cond_init(&fixture.event_condition, NULL) == 0);
    bindings[0].struct_size = sizeof(bindings[0]);
    bindings[0].api_version = UCN_API_VERSION;
    bindings[0].address = 1U;
    bindings[0].binding_generation = 10U;
    bindings[0].principal_digest = 11U;
    bindings[1] = bindings[0];
    bindings[1].address = 2U;
    bindings[1].binding_generation = 20U;
    bindings[1].principal_digest = 22U;
    link_port.struct_size = sizeof(link_port);
    link_port.api_version = UCN_API_VERSION;
    link_port.link_instance = 1U;
    link_port.frame_mtu = 64U;
    link_port.context = &fixture;
    link_port.tx.struct_size = sizeof(link_port.tx);
    link_port.tx.api_version = UCN_API_VERSION;
    link_port.tx.submit = submit;
    link_port.tx.cancel = cancel;
    ports.struct_size = sizeof(ports);
    ports.api_version = UCN_API_VERSION;
    ports.links = &link_port;
    ports.link_count = 1U;
    ports.state_lock.struct_size = sizeof(ports.state_lock);
    ports.state_lock.api_version = UCN_API_VERSION;
    ports.state_lock.context = &fixture;
    ports.state_lock.enter = state_lock_enter;
    ports.state_lock.leave = state_lock_leave;
    ports.driver_callback_gate.struct_size =
        sizeof(ports.driver_callback_gate);
    ports.driver_callback_gate.api_version = UCN_API_VERSION;
    ports.driver_callback_gate.context = &fixture;
    ports.driver_callback_gate.enter = driver_gate_enter;
    ports.driver_callback_gate.leave = driver_gate_leave;
    config.struct_size = sizeof(config);
    config.api_version = UCN_API_VERSION;
    config.storage_layout = UCN_STORAGE_LAYOUT;
    config.compiled_manifest_hash = UCN_COMPILED_MANIFEST_HASH;
    config.runtime_instance = 17U;
    config.realm_id = 7U;
    config.local_address = 1U;
    config.local_binding_generation = 10U;
    config.local_principal_digest = 11U;
    config.bindings = bindings;
    config.binding_count = 2U;
    config.address_width = 1U;
    config.trusted_o0_network = 1U;
    CHECK(ucn_init(&storage, sizeof(storage), &config, &ports,
                   &fixture.node) == UCN_OK);
    CHECK(ucn_link_get(fixture.node, 0U, &fixture.link) == UCN_OK);
    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.struct_size = sizeof(endpoint);
    endpoint.api_version = UCN_API_VERSION;
    endpoint.service_id = 77U;
    endpoint.receive = receive;
    endpoint.context = &fixture;
    CHECK(ucn_endpoint_add(fixture.node, &endpoint, &endpoint_handle) == UCN_OK);
    memset(&path, 0, sizeof(path));
    path.struct_size = sizeof(path);
    path.api_version = UCN_API_VERSION;
    path.destination_address = 2U;
    path.destination_binding_generation = 20U;
    path.link_index = 0U;
    path.path_frame_mtu = 64U;
    CHECK(ucn_static_path_add(fixture.node, &path, &path_handle) == UCN_OK);
    CHECK(ucn_start(fixture.node, 0U) == UCN_OK);
    memset(&target, 0, sizeof(target));
    target.struct_size = sizeof(target);
    target.api_version = UCN_API_VERSION;
    target.address = 2U;
    target.binding_generation = 20U;
    target.service_id = 77U;
    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.api_version = UCN_API_VERSION;
    options.traffic_class = UCN_TRAFFIC_Q1;
    options.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    options.interaction_role = UCN_INTERACTION_ONE_WAY;
    options.hop_limit = 1U;
    options.copy_payload = 1U;
    CHECK(ucn_publish(fixture.node, &target, &payload, sizeof(payload),
                      &options, &send) == UCN_OK);
    CHECK(pthread_create(&driver, NULL, driver_thread, &fixture) == 0);
    CHECK(pthread_create(&owner, NULL, step_thread, &fixture) == 0);
    CHECK(pthread_create(&foreign_query, NULL, foreign_query_thread,
                         &fixture) == 0);
    CHECK(pthread_join(driver, NULL) == 0);
    CHECK(pthread_join(owner, NULL) == 0);
    CHECK(pthread_join(foreign_query, NULL) == 0);
    CHECK(!driver_failed(&fixture));
    CHECK(fixture.foreign_unscoped_result == UCN_ERR_STATE);
    CHECK(fixture.scoped_result == UCN_OK);
    CHECK(fixture.callback_stats.rx_published == 1U);
    CHECK(fixture.received == RX_MESSAGES);
    CHECK(ucn_send_query(fixture.node, send, &view) == UCN_OK);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(ucn_send_forget(fixture.node, send) == UCN_OK);
    CHECK(ucn_get_stats(fixture.node, &stats) == UCN_OK);
    CHECK(stats.rx_delivered == RX_MESSAGES);
    CHECK(ucn_stop(fixture.node) == UCN_OK);
    CHECK(ucn_static_path_remove(fixture.node, path_handle) == UCN_OK);
    CHECK(ucn_endpoint_remove(fixture.node, endpoint_handle) == UCN_OK);
    CHECK(ucn_deinit(fixture.node) == UCN_OK);
    CHECK(pthread_cond_destroy(&fixture.event_condition) == 0);
    CHECK(pthread_mutex_destroy(&fixture.event_mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.driver_gate_mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.state_mutex) == 0);
    puts("UCN simplified Core concurrency tests passed");
    return 0;
}
