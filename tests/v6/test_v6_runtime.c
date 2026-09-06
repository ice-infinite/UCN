#include "ucn/v6/ucn_v6_runtime.h"
#include "ucn/v6/adapters/ucn_v6_uart.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition);                                   \
            return 1;                                                        \
        }                                                                    \
    } while (0)

typedef struct fake_lock {
    bool held;
    unsigned notifications;
} fake_lock_t;

typedef struct fake_driver {
    ucn_v6_adapter_owner_t *adapter;
    ucn_v6_driver_event_key_t submitted_key;
    unsigned submits;
    unsigned quiesces;
} fake_driver_t;

typedef struct fake_app {
    ucn_v6_runtime_owner_t *runtime;
    ucn_v6_security_open_result_t opened;
    ucn_v6_route_path_ref_t reverse_ref;
    unsigned ingress_calls;
    unsigned release_calls;
    unsigned release_failures_remaining;
    uint64_t released_token;
    ucn_v6_result_t released_result;
    bool try_reopen_during_ingress;
    ucn_v6_result_t reopen_during_ingress_result;
    uint32_t reopen_during_ingress_generation;
    bool try_reopen_during_release;
    ucn_v6_result_t reopen_during_release_result;
    uint32_t reopen_during_release_generation;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    bool observe_time_sync;
    ucn_v6_runtime_time_handle_t time_handle;
#endif
} fake_app_t;

static ucn_v6_adapter_owner_storage_t adapter_storage;
static ucn_v6_runtime_owner_storage_t runtime_storage;

static ucn_v6_result_t lock_task(void *context)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    if (lock->held) return UCN_V6_ERR_STATE;
    lock->held = true;
    return UCN_V6_OK;
}

static bool try_lock(void *context)
{
    return lock_task(context) == UCN_V6_OK;
}

static void unlock(void *context)
{
    ((fake_lock_t *)context)->held = false;
}

static ucn_v6_result_t post_event(
    void *context, ucn_v6_owner_event_t event, bool from_isr)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    (void)event;
    (void)from_isr;
    ++lock->notifications;
    return UCN_V6_OK;
}

static ucn_v6_result_t submit_frame(
    void *context, const ucn_v6_driver_event_key_t *key,
    const uint8_t *frame, size_t frame_length,
    uint8_t hardware_priority, bool request_timestamp)
{
    fake_driver_t *driver = (fake_driver_t *)context;
    ucn_v6_driver_timestamp_t timestamp;
    (void)frame;
    (void)frame_length;
    (void)hardware_priority;
    if (!request_timestamp) return UCN_V6_ERR_STATE;
    ++driver->submits;
    driver->submitted_key = *key;
    memset(&timestamp, 0, sizeof(timestamp));
    timestamp.timestamp_us = 140U;
    timestamp.uncertainty_us = 2U;
    timestamp.valid = true;
    timestamp.hardware = true;
    return ucn_v6_adapter_publish_tx_completion(
        driver->adapter, key, UCN_V6_OK, &timestamp, false);
}

static ucn_v6_result_t cancel_frame(
    void *context, const ucn_v6_driver_event_key_t *key)
{
    (void)context;
    (void)key;
    return UCN_V6_OK;
}

static ucn_v6_result_t quiesce(void *context)
{
    ++((fake_driver_t *)context)->quiesces;
    return UCN_V6_OK;
}

static ucn_v6_result_t handle_ingress(
    void *context, ucn_v6_runtime_owner_t *runtime, uint64_t now_us,
    const uint8_t *encoded_frame, size_t encoded_length,
    const ucn_v6_driver_rx_view_t *rx,
    ucn_v6_runtime_ingress_disposition_t *disposition)
{
    fake_app_t *app = (fake_app_t *)context;
    (void)encoded_frame;
#if !UCN_V6_FEATURE_REALTIME_ENABLED
    (void)runtime;
    (void)now_us;
    (void)rx;
#endif
    if (encoded_length != 3U) return UCN_V6_ERR_STATE;
    ++app->ingress_calls;
    if (app->try_reopen_during_ingress) {
        app->reopen_during_ingress_generation = UINT32_C(0xA5A5A5A5);
        app->reopen_during_ingress_result = ucn_v6_runtime_reopen_link(
            runtime, rx->key.link_id,
            &app->reopen_during_ingress_generation);
    }
#if UCN_V6_FEATURE_REALTIME_ENABLED
    if (app->observe_time_sync) {
        if (ucn_v6_runtime_time_observe_sync(
                runtime, &app->opened, rx, &app->reverse_ref, now_us,
                &app->time_handle) != UCN_V6_OK) {
            return UCN_V6_ERR_STATE;
        }
    }
#endif
    *disposition = UCN_V6_RUNTIME_INGRESS_CONSUMED;
    return UCN_V6_OK;
}

static ucn_v6_result_t release_buffer(
    void *context, uint64_t token, ucn_v6_result_t result,
    const ucn_v6_driver_timestamp_t *timestamp)
{
    fake_app_t *app = (fake_app_t *)context;
    ++app->release_calls;
    if (app->try_reopen_during_release) {
        app->reopen_during_release_generation = UINT32_C(0x5A5A5A5A);
        app->reopen_during_release_result = ucn_v6_runtime_reopen_link(
            app->runtime, 1U, &app->reopen_during_release_generation);
    }
    if (app->release_failures_remaining != 0U) {
        --app->release_failures_remaining;
        return UCN_V6_ERR_NO_SPACE;
    }
    if (timestamp == NULL ||
        (result == UCN_V6_OK && !timestamp->hardware) ||
        (result != UCN_V6_OK && result != UCN_V6_ERR_ACCESS)) {
        return UCN_V6_ERR_STATE;
    }
    app->released_token = token;
    app->released_result = result;
    return UCN_V6_OK;
}

static void configure_opened_sync(fake_app_t *app, uint8_t payload[12])
{
#if UCN_V6_FEATURE_REALTIME_ENABLED
    ucn_v6_time_sync_announce_t announce;
    memset(&announce, 0, sizeof(announce));
    announce.clock_domain_id = 7U;
    announce.domain_generation = 8U;
    announce.sync_sequence = 9U;
    (void)ucn_v6_time_sync_announce_encode(&announce, payload);
    memset(&app->opened, 0, sizeof(app->opened));
    app->opened.frame.frame_type = UCN_V6_FRAME_CONTROL;
    app->opened.frame.flags =
        UCN_V6_FLAG_PROTOCOL_CONTEXT | UCN_V6_FLAG_E2E_CONTEXT;
    app->opened.frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TIME_SYNC;
    app->opened.frame.payload = payload;
    app->opened.frame.payload_length = UCN_V6_TIME_SYNC_ANNOUNCE_BYTES;
    app->opened.hop_authenticated = true;
    app->opened.endpoint_authorized = true;
    memset(&app->reverse_ref, 0, sizeof(app->reverse_ref));
    app->reverse_ref.route_generation = 1U;
    app->reverse_ref.path_id = 2U;
    app->reverse_ref.path_generation = 3U;
#else
    (void)app;
    (void)payload;
#endif
}

static int test_standard_runtime_rx_tx_and_timestamp_binding(void)
{
    fake_lock_t lock;
    fake_driver_t driver;
    fake_app_t app;
    ucn_v6_driver_runtime_ops_t adapter_runtime;
    ucn_v6_uart_link_settings_t settings;
    ucn_v6_driver_link_config_t link;
    ucn_v6_adapter_owner_t *adapter = NULL;
    ucn_v6_runtime_owner_t *runtime = NULL;
    ucn_v6_runtime_config_t config;
    ucn_v6_stack_hooks_t hooks;
    ucn_v6_stack_phase_result_t phase;
    ucn_v6_driver_timestamp_t rx_timestamp;
    ucn_v6_driver_event_key_t rx_key;
    ucn_v6_driver_event_key_t tx_key;
    ucn_v6_runtime_view_t view;
    uint8_t announce_payload[12];
    uint8_t rx_frame[3] = {1U, 2U, 3U};
    uint8_t tx_frame[3] = {4U, 5U, 6U};
    uint8_t dummy = 0U;
    uint32_t new_link_generation = 0U;

    memset(&lock, 0, sizeof(lock));
    memset(&driver, 0, sizeof(driver));
    memset(&app, 0, sizeof(app));
    memset(&adapter_runtime, 0, sizeof(adapter_runtime));
    adapter_runtime.context = &lock;
    adapter_runtime.lock_task = lock_task;
    adapter_runtime.try_lock_from_isr = try_lock;
    adapter_runtime.unlock_task = unlock;
    adapter_runtime.unlock_from_isr = unlock;
    adapter_runtime.post_owner_event = post_event;
    CHECK(ucn_v6_adapter_init_in_place(
              adapter_storage.bytes, sizeof(adapter_storage),
              ucn_v6_compiled_manifest(), &adapter_runtime, &adapter) ==
          UCN_V6_OK);
    driver.adapter = adapter;
    memset(&settings, 0, sizeof(settings));
    settings.base.link_id = 1U;
    settings.base.initial_generation = 1U;
    settings.base.rx_slot_quota = 2U;
    settings.base.tx_slot_quota = 2U;
    settings.base.ops.struct_size = sizeof(settings.base.ops);
    settings.base.ops.api_version = UCN_V6_ADAPTER_API_VERSION;
    settings.base.ops.context = &driver;
    settings.base.ops.submit = submit_frame;
    settings.base.ops.cancel = cancel_frame;
    settings.base.ops.quiesce = quiesce;
    settings.rx_timestamp_hardware = true;
    settings.tx_timestamp_hardware = true;
    CHECK(ucn_v6_uart_link_config_init(&settings, &link) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_register_link(adapter, &link) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 1U, 1U, UCN_V6_LINK_READY) == UCN_V6_OK);

    configure_opened_sync(&app, announce_payload);
    app.try_reopen_during_ingress = true;
    memset(&config, 0, sizeof(config));
    config.runtime_instance_generation = 1U;
    config.adapter = adapter;
    config.bootstrap = (ucn_v6_bootstrap_owner_t *)&dummy;
    config.security = (ucn_v6_security_manager_t *)&dummy;
    config.capability = (ucn_v6_capability_owner_t *)&dummy;
    config.route = (ucn_v6_route_owner_t *)&dummy;
    config.metric = (ucn_v6_metric_owner_t *)&dummy;
    config.qos = (ucn_v6_qos_owner_t *)&dummy;
    config.transfer = (ucn_v6_transfer_owner_t *)&dummy;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    config.realtime = (ucn_v6_realtime_owner_t *)&dummy;
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    config.cluster = (ucn_v6_cluster_owner_t *)&dummy;
#endif
    config.app.context = &app;
    config.app.handle_ingress = handle_ingress;
    config.app.release_buffer = release_buffer;
    CHECK(ucn_v6_runtime_init_in_place(
              runtime_storage.bytes, sizeof(runtime_storage),
              ucn_v6_compiled_manifest(), &config, &runtime) == UCN_V6_OK);
    app.runtime = runtime;
    CHECK(ucn_v6_runtime_make_stack_hooks(runtime, &hooks) == UCN_V6_OK);
#if UCN_V6_FEATURE_REALTIME_ENABLED
    {
        ucn_v6_runtime_owner_storage_t before = runtime_storage;
        ucn_v6_route_path_ref_t reference;
        ucn_v6_time_sync_announce_t announce;
        memset(&reference, 0, sizeof(reference));
        memset(&announce, 0, sizeof(announce));
        announce.clock_domain_id = 1U;
        announce.domain_generation = 1U;
        announce.sync_sequence = 1U;
        CHECK(ucn_v6_runtime_time_start_sync(
                  runtime, &reference, &announce, UINT64_C(999), 1U,
                  (ucn_v6_runtime_time_handle_t *)(void *)runtime) ==
              UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(&runtime_storage, &before, sizeof(before)) == 0);
    }
#endif

    memset(&rx_timestamp, 0, sizeof(rx_timestamp));
    rx_timestamp.timestamp_us = 100U;
    rx_timestamp.uncertainty_us = 2U;
    rx_timestamp.valid = true;
    rx_timestamp.hardware = true;
    CHECK(ucn_v6_adapter_publish_rx(adapter, 1U, 1U, rx_frame,
              sizeof(rx_frame), &rx_timestamp, false, &rx_key) == UCN_V6_OK);
    CHECK(hooks.rx_ingress(hooks.context, 110U, 1U, &phase) == UCN_V6_OK);
    CHECK(app.ingress_calls == 1U && phase.work_done == 1U);
    CHECK(app.reopen_during_ingress_result == UCN_V6_ERR_STATE);
    CHECK(app.reopen_during_ingress_generation == UINT32_C(0xA5A5A5A5));
    CHECK(driver.quiesces == 0U);

    CHECK(ucn_v6_adapter_enqueue_tx(adapter, 1U, UINT64_C(77), tx_frame,
              sizeof(tx_frame), UCN_V6_TRAFFIC_Q1, true, &tx_key) ==
          UCN_V6_OK);
    CHECK(hooks.qos_tx(hooks.context, 120U, 1U, &phase) == UCN_V6_OK);
    CHECK(driver.submits == 1U);
    CHECK(hooks.tx_completion(hooks.context, 150U, 1U, &phase) ==
          UCN_V6_OK);
    CHECK(app.release_calls == 0U);
    app.release_failures_remaining = 1U;
    app.try_reopen_during_release = true;
    CHECK(hooks.tx_completion(hooks.context, 151U, 1U, &phase) ==
          UCN_V6_OK);
    CHECK(app.release_calls == 1U && phase.has_deadline);
    CHECK(hooks.tx_completion(hooks.context, 152U, 1U, &phase) ==
          UCN_V6_OK);
    CHECK(app.release_calls == 2U && app.released_token == UINT64_C(77));
    CHECK(app.reopen_during_release_result == UCN_V6_ERR_STATE);
    CHECK(app.reopen_during_release_generation == UINT32_C(0x5A5A5A5A));
    CHECK(driver.quiesces == 0U);
    app.try_reopen_during_release = false;
    CHECK(ucn_v6_runtime_copy_view(runtime, &view) == UCN_V6_OK);
    CHECK(view.rx_consumed == 1U && view.tx_completions == 1U &&
          view.released_buffers == 1U);
#if UCN_V6_FEATURE_REALTIME_ENABLED
    CHECK(view.realtime_exchanges_started == 0U);
    CHECK(view.realtime_tx_timestamps_captured == 0U);
#endif
    CHECK(ucn_v6_adapter_enqueue_tx(adapter, 1U, UINT64_C(88), tx_frame,
              sizeof(tx_frame), UCN_V6_TRAFFIC_Q2, false, &tx_key) ==
          UCN_V6_OK);
    CHECK(ucn_v6_runtime_reopen_link(runtime, 1U, &new_link_generation) ==
          UCN_V6_OK);
    CHECK(new_link_generation == 2U && driver.quiesces == 1U);
    CHECK(hooks.tx_completion(hooks.context, 160U, 1U, &phase) ==
          UCN_V6_OK);
    CHECK(app.released_token == UINT64_C(88) &&
          app.released_result == UCN_V6_ERR_ACCESS);
    CHECK(hooks.hop_security(hooks.context, 161U, 1U, &phase) == UCN_V6_OK);
    CHECK(phase.has_invalidation &&
          phase.invalidation.type == UCN_V6_STACK_INVALIDATE_LINK &&
          phase.invalidation.link_id == 1U &&
          phase.invalidation.link_generation == 1U);
    CHECK(hooks.invalidate_endpoint(hooks.context, &phase.invalidation) ==
          UCN_V6_OK);
    CHECK(hooks.hop_security(hooks.context, 162U, 1U, &phase) == UCN_V6_OK);
    CHECK(!phase.has_invalidation);
    CHECK(ucn_v6_runtime_copy_view(runtime, &view) == UCN_V6_OK);
    CHECK(view.link_reopens == 1U && view.invalidations == 1U &&
          view.released_buffers == 2U);
    return 0;
}

static int test_init_rejects_overlapping_config_without_writing(void)
{
    ucn_v6_runtime_owner_storage_t storage;
    ucn_v6_runtime_owner_storage_t before;
    ucn_v6_runtime_config_t valid;
    ucn_v6_runtime_config_t *inside;
    ucn_v6_runtime_owner_t *runtime = NULL;
    uint8_t dummy = 0U;
    fake_app_t app;
    memset(&storage, 0xA5, sizeof(storage));
    memset(&valid, 0, sizeof(valid));
    valid.runtime_instance_generation = 1U;
    memset(&app, 0, sizeof(app));
    valid.adapter = (ucn_v6_adapter_owner_t *)&dummy;
    valid.bootstrap = (ucn_v6_bootstrap_owner_t *)&dummy;
    valid.security = (ucn_v6_security_manager_t *)&dummy;
    valid.capability = (ucn_v6_capability_owner_t *)&dummy;
    valid.route = (ucn_v6_route_owner_t *)&dummy;
    valid.metric = (ucn_v6_metric_owner_t *)&dummy;
    valid.qos = (ucn_v6_qos_owner_t *)&dummy;
    valid.transfer = (ucn_v6_transfer_owner_t *)&dummy;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    valid.realtime = (ucn_v6_realtime_owner_t *)&dummy;
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    valid.cluster = (ucn_v6_cluster_owner_t *)&dummy;
#endif
    valid.app.context = &app;
    valid.app.handle_ingress = handle_ingress;
    valid.app.release_buffer = release_buffer;
    inside = (ucn_v6_runtime_config_t *)(void *)storage.bytes;
    memcpy(inside, &valid, sizeof(valid));
    before = storage;
    CHECK(ucn_v6_runtime_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              inside, &runtime) == UCN_V6_ERR_CONFIG);
    CHECK(runtime == NULL);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);

    memset(&storage, 0x5A, sizeof(storage));
    valid.app.context = &storage.bytes[sizeof(storage.bytes) / 2U];
    before = storage;
    CHECK(ucn_v6_runtime_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &valid, &runtime) == UCN_V6_ERR_CONFIG);
    CHECK(runtime == NULL);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);

    memset(&storage, 0x3C, sizeof(storage));
    valid.app.context = &app;
    valid.route = (ucn_v6_route_owner_t *)(void *)&storage.bytes[32];
    before = storage;
    CHECK(ucn_v6_runtime_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &valid, &runtime) == UCN_V6_ERR_CONFIG);
    CHECK(runtime == NULL);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);
    return 0;
}

int main(void)
{
    CHECK(test_standard_runtime_rx_tx_and_timestamp_binding() == 0);
    CHECK(test_init_rejects_overlapping_config_without_writing() == 0);
    puts("v6 runtime tests passed");
    return 0;
}
