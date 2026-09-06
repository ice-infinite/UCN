#include "ucn/v6/ports/ucn_v6_freertos.h"
#include "ucn/v6/adapters/ucn_v6_can.h"
#include "ucn/v6/adapters/ucn_v6_uart.h"
#include "ucn/v6/adapters/ucn_v6_usb.h"
#include "ucn/v6/adapters/ucn_v6_wifi.h"
#include "ucn/v6/reference/esp32s3/ucn_v6_esp32s3.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

typedef struct fake_environment {
    bool locked;
    unsigned task_notifications;
    unsigned isr_notifications;
    unsigned wait_calls;
    bool wait_notified;
    uint64_t now_us;
    uint64_t last_wait_us;
} fake_environment_t;

typedef struct fake_driver {
    ucn_v6_adapter_owner_t *owner;
    ucn_v6_driver_event_key_t last_key;
    uint8_t last_frame[UCN_V6_CONFIG_ADAPTER_FRAME_BYTES];
    size_t last_length;
    uint8_t last_priority;
    unsigned submit_calls;
    unsigned cancel_calls;
    unsigned quiesce_calls;
    ucn_v6_result_t submit_result;
    ucn_v6_result_t cancel_result;
    ucn_v6_result_t quiesce_result;
    bool complete_synchronously;
} fake_driver_t;

typedef struct fake_handler {
    ucn_v6_adapter_owner_t *adapter;
    unsigned phase_calls;
    unsigned timer_calls;
    unsigned retired_rx;
    bool publish_deadline;
    uint64_t deadline_us;
    bool backlog_timer_once;
    bool backlog_emitted;
} fake_handler_t;

static ucn_v6_adapter_owner_storage_t adapter_storage;
static ucn_v6_freertos_port_storage_t port_storage;

static ucn_v6_result_t fake_lock_task(void *context)
{
    fake_environment_t *environment = (fake_environment_t *)context;
    if (environment->locked) return UCN_V6_ERR_STATE;
    environment->locked = true;
    return UCN_V6_OK;
}

static void fake_port_lock_task(void *context)
{
    (void)fake_lock_task(context);
}

static bool fake_try_lock_from_isr(void *context)
{
    fake_environment_t *environment = (fake_environment_t *)context;
    if (environment->locked) return false;
    environment->locked = true;
    return true;
}

static void fake_unlock_task(void *context)
{
    fake_environment_t *environment = (fake_environment_t *)context;
    environment->locked = false;
}

static void fake_unlock_from_isr(void *context)
{
    fake_environment_t *environment = (fake_environment_t *)context;
    environment->locked = false;
}

static ucn_v6_result_t fake_post(
    void *context, ucn_v6_owner_event_t event, bool from_isr)
{
    fake_environment_t *environment = (fake_environment_t *)context;
    (void)event;
    if (from_isr) ++environment->isr_notifications;
    else ++environment->task_notifications;
    return UCN_V6_OK;
}

static void fake_notify(void *context, bool from_isr)
{
    fake_environment_t *environment = (fake_environment_t *)context;
    if (from_isr) ++environment->isr_notifications;
    else ++environment->task_notifications;
}

static ucn_v6_result_t fake_wait(
    void *context, uint64_t max_wait_us, bool *notified)
{
    fake_environment_t *environment = (fake_environment_t *)context;
    if (max_wait_us == 0U || notified == NULL) return UCN_V6_ERR_ARGUMENT;
    ++environment->wait_calls;
    environment->last_wait_us = max_wait_us;
    *notified = environment->wait_notified;
    return UCN_V6_OK;
}

static ucn_v6_result_t fake_now(void *context, uint64_t *now_us)
{
    fake_environment_t *environment = (fake_environment_t *)context;
    if (now_us == NULL) return UCN_V6_ERR_ARGUMENT;
    *now_us = environment->now_us;
    return UCN_V6_OK;
}

static bool key_equal(
    const ucn_v6_driver_event_key_t *left,
    const ucn_v6_driver_event_key_t *right)
{
    return left->link_id == right->link_id &&
           left->link_generation == right->link_generation &&
           left->event_token == right->event_token;
}

static ucn_v6_result_t fake_submit(
    void *context,
    const ucn_v6_driver_event_key_t *key,
    const uint8_t *frame,
    size_t frame_length,
    uint8_t hardware_priority,
    bool request_timestamp)
{
    fake_driver_t *driver = (fake_driver_t *)context;
    ucn_v6_driver_timestamp_t timestamp;
    ++driver->submit_calls;
    driver->last_key = *key;
    driver->last_length = frame_length;
    driver->last_priority = hardware_priority;
    memcpy(driver->last_frame, frame, frame_length);
    if (driver->complete_synchronously && driver->submit_result == UCN_V6_OK) {
        memset(&timestamp, 0, sizeof(timestamp));
        if (request_timestamp) {
            timestamp.timestamp_us = 1234U;
            timestamp.uncertainty_us = 2U;
            timestamp.valid = true;
            timestamp.hardware = true;
        }
        return ucn_v6_adapter_publish_tx_completion(
            driver->owner, key, UCN_V6_OK, &timestamp, false);
    }
    return driver->submit_result;
}

static ucn_v6_result_t fake_cancel(
    void *context, const ucn_v6_driver_event_key_t *key)
{
    fake_driver_t *driver = (fake_driver_t *)context;
    ++driver->cancel_calls;
    driver->last_key = *key;
    return driver->cancel_result;
}

static ucn_v6_result_t fake_quiesce(void *context)
{
    fake_driver_t *driver = (fake_driver_t *)context;
    ++driver->quiesce_calls;
    return driver->quiesce_result;
}

static ucn_v6_driver_link_ops_t driver_ops(fake_driver_t *driver)
{
    ucn_v6_driver_link_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_V6_ADAPTER_API_VERSION;
    ops.context = driver;
    ops.submit = fake_submit;
    ops.cancel = fake_cancel;
    ops.quiesce = fake_quiesce;
    return ops;
}

static ucn_v6_result_t init_adapter(
    fake_environment_t *environment,
    ucn_v6_adapter_owner_t **adapter)
{
    ucn_v6_driver_runtime_ops_t runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.context = environment;
    runtime.lock_task = fake_lock_task;
    runtime.try_lock_from_isr = fake_try_lock_from_isr;
    runtime.unlock_task = fake_unlock_task;
    runtime.unlock_from_isr = fake_unlock_from_isr;
    runtime.post_owner_event = fake_post;
    return ucn_v6_adapter_init_in_place(
        adapter_storage.bytes, sizeof(adapter_storage),
        ucn_v6_compiled_manifest(), &runtime, adapter);
}

static int test_reference_profiles_and_rx(void)
{
    fake_environment_t environment;
    fake_driver_t uart;
    fake_driver_t can;
    ucn_v6_adapter_owner_t *adapter = NULL;
    ucn_v6_uart_link_settings_t uart_settings;
    ucn_v6_can_link_settings_t can_settings;
    ucn_v6_esp_now_link_settings_t wifi_settings;
    ucn_v6_usb_link_settings_t usb_settings;
    ucn_v6_driver_link_config_t uart_config;
    ucn_v6_driver_link_config_t can_config;
    ucn_v6_driver_link_config_t auxiliary_config;
    ucn_v6_driver_event_key_t key;
    ucn_v6_driver_rx_view_t view;
    ucn_v6_driver_rx_view_t before_view;
    ucn_v6_driver_timestamp_t timestamp;
    uint8_t frame[40];
    uint8_t output[40];
    size_t index;
    memset(&environment, 0, sizeof(environment));
    memset(&uart, 0, sizeof(uart));
    memset(&can, 0, sizeof(can));
    uart.submit_result = UCN_V6_OK;
    uart.cancel_result = UCN_V6_OK;
    uart.quiesce_result = UCN_V6_OK;
    can = uart;
    CHECK(init_adapter(&environment, &adapter) == UCN_V6_OK);
    uart.owner = adapter;
    can.owner = adapter;

    memset(&uart_settings, 0, sizeof(uart_settings));
    uart_settings.base.link_id = UINT16_MAX;
    uart_settings.base.initial_generation = 1U;
    uart_settings.base.ops = driver_ops(&uart);
    uart_settings.rx_timestamp_hardware = true;
    CHECK(ucn_v6_uart_link_config_init(
              &uart_settings, &uart_config) == UCN_V6_ERR_ARGUMENT);
    uart_settings.base.link_id = 1U;
    CHECK(ucn_v6_uart_link_config_init(
              &uart_settings, &uart_config) == UCN_V6_OK);
    CHECK(uart_config.nominal_bitrate_bps == UINT32_C(3000000));
    CHECK(uart_config.carrier_mtu == UCN_V6_CONFIG_ADAPTER_FRAME_BYTES);
    CHECK(ucn_v6_adapter_register_link(adapter, &uart_config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_register_link(adapter, &uart_config) ==
          UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 1U, 1U, UCN_V6_LINK_READY) == UCN_V6_OK);

    memset(&can_settings, 0, sizeof(can_settings));
    can_settings.base.link_id = 2U;
    can_settings.base.initial_generation = 1U;
    can_settings.base.ops = driver_ops(&can);
    CHECK(ucn_v6_can_link_config_init(
              &can_settings, &can_config) == UCN_V6_OK);
    CHECK(can_config.carrier_mtu == 8U &&
          can_config.hardware_priority_count == 4U);
    CHECK(ucn_v6_adapter_register_link(adapter, &can_config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 2U, 1U, UCN_V6_LINK_READY) == UCN_V6_OK);

    memset(&wifi_settings, 0, sizeof(wifi_settings));
    wifi_settings.base.link_id = 3U;
    wifi_settings.base.initial_generation = 1U;
    wifi_settings.base.ops = driver_ops(&uart);
    CHECK(ucn_v6_esp_now_link_config_init(
              &wifi_settings, &auxiliary_config) == UCN_V6_OK);
    CHECK(auxiliary_config.bearer == UCN_V6_BEARER_ESP_NOW &&
          auxiliary_config.carrier_mtu ==
              (UCN_V6_CONFIG_ADAPTER_FRAME_BYTES < 250U ?
                   UCN_V6_CONFIG_ADAPTER_FRAME_BYTES : 250U));
    memset(&usb_settings, 0, sizeof(usb_settings));
    usb_settings.base.link_id = 4U;
    usb_settings.base.initial_generation = 1U;
    usb_settings.base.ops = driver_ops(&uart);
    CHECK(ucn_v6_usb_link_config_init(
              &usb_settings, &auxiliary_config) == UCN_V6_OK);
    CHECK(auxiliary_config.bearer == UCN_V6_BEARER_USB &&
          auxiliary_config.nominal_bitrate_bps == UINT32_C(12000000));

    for (index = 0U; index < sizeof(frame); ++index) frame[index] = (uint8_t)index;
    memset(&timestamp, 0, sizeof(timestamp));
    timestamp.timestamp_us = 700U;
    timestamp.uncertainty_us = 3U;
    timestamp.valid = true;
    timestamp.hardware = true;
    CHECK(ucn_v6_adapter_publish_rx(
              adapter, 1U, 1U, frame, sizeof(frame), &timestamp,
              true, &key) == UCN_V6_OK);
    CHECK(environment.isr_notifications == 1U);
    memset(output, 0, sizeof(output));
    CHECK(ucn_v6_adapter_peek_rx(
              adapter, output, sizeof(output), &view) == UCN_V6_OK);
    CHECK(memcmp(frame, output, sizeof(frame)) == 0);
    CHECK(key_equal(&key, &view.key));
    CHECK(view.timestamp.timestamp_us == 700U && view.timestamp.hardware);
    memset(&before_view, 0xA5, sizeof(before_view));
    view = before_view;
    CHECK(ucn_v6_adapter_peek_rx(
              adapter, output, sizeof(output) - 1U,
              &view) == UCN_V6_ERR_NO_SPACE);
    CHECK(memcmp(&view, &before_view, sizeof(view)) == 0);
    CHECK(ucn_v6_adapter_retire_rx(adapter, &key) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_retire_rx(adapter, &key) == UCN_V6_ERR_NOT_FOUND);
    CHECK(ucn_v6_adapter_publish_rx(
              adapter, 1U, 2U, frame, sizeof(frame), NULL,
              false, &key) == UCN_V6_ERR_REPLAY);
    return 0;
}

static int test_esp32s3_reference_configuration(void)
{
    fake_driver_t driver;
    ucn_v6_esp32s3_uart_binding_t uart;
    ucn_v6_esp32s3_esp_now_binding_t wifi;
    ucn_v6_driver_link_config_t config;
    memset(&driver, 0, sizeof(driver));
    memset(&uart, 0, sizeof(uart));
    uart.struct_size = sizeof(uart);
    uart.api_version = UCN_V6_ESP32S3_REFERENCE_API_VERSION;
    uart.uart_port = 1U;
    uart.tx_gpio = 19;
    uart.rx_gpio = 20;
    uart.rts_or_de_gpio = UCN_V6_ESP32S3_GPIO_UNUSED;
    uart.rx_dma_bytes = 1024U;
    uart.tx_dma_bytes = 1024U;
    uart.link.base.link_id = 20U;
    uart.link.base.initial_generation = 1U;
    uart.link.base.ops = driver_ops(&driver);
    CHECK(ucn_v6_esp32s3_uart_binding_build(&uart, &config) == UCN_V6_OK);
    CHECK(config.bearer == UCN_V6_BEARER_UART &&
          config.nominal_bitrate_bps == UINT32_C(3000000));
    uart.rx_gpio = 19;
    CHECK(ucn_v6_esp32s3_uart_binding_build(&uart, &config) ==
          UCN_V6_ERR_CONFIG);

    memset(&wifi, 0, sizeof(wifi));
    wifi.struct_size = sizeof(wifi);
    wifi.api_version = UCN_V6_ESP32S3_REFERENCE_API_VERSION;
    wifi.wifi_interface = 0U;
    wifi.channel = 6U;
    wifi.peer_capacity = 8U;
    wifi.link.base.link_id = 21U;
    wifi.link.base.initial_generation = 1U;
    wifi.link.base.ops = driver_ops(&driver);
    CHECK(ucn_v6_esp32s3_esp_now_binding_build(&wifi, &config) == UCN_V6_OK);
    CHECK(config.bearer == UCN_V6_BEARER_ESP_NOW &&
          config.carrier_mtu ==
              (UCN_V6_CONFIG_ADAPTER_FRAME_BYTES < 250U ?
                   UCN_V6_CONFIG_ADAPTER_FRAME_BYTES : 250U));
    wifi.channel = 15U;
    CHECK(ucn_v6_esp32s3_esp_now_binding_build(&wifi, &config) ==
          UCN_V6_ERR_CONFIG);
    return 0;
}

static int test_tx_lifecycle_and_reopen(void)
{
    fake_environment_t environment;
    fake_driver_t driver;
    ucn_v6_adapter_owner_t *adapter = NULL;
    ucn_v6_can_link_settings_t settings;
    ucn_v6_driver_link_config_t config;
    ucn_v6_driver_event_key_t key1;
    ucn_v6_driver_event_key_t key2;
    ucn_v6_driver_event_key_t key3;
    ucn_v6_driver_event_key_t key4;
    ucn_v6_driver_tx_completion_t completion;
    ucn_v6_driver_timestamp_t timestamp;
    uint8_t frame[36];
    uint64_t token = 0U;
    uint64_t retired[2];
    size_t retired_count = 0U;
    uint32_t generation = 0U;
    bool submitted = false;
    memset(&environment, 0, sizeof(environment));
    memset(&driver, 0, sizeof(driver));
    memset(frame, 0x5A, sizeof(frame));
    driver.submit_result = UCN_V6_OK;
    driver.cancel_result = UCN_V6_OK;
    driver.quiesce_result = UCN_V6_OK;
    CHECK(init_adapter(&environment, &adapter) == UCN_V6_OK);
    driver.owner = adapter;
    memset(&settings, 0, sizeof(settings));
    settings.base.link_id = 9U;
    settings.base.initial_generation = 3U;
    settings.base.rx_slot_quota = 2U;
    settings.base.tx_slot_quota = 2U;
    settings.base.ops = driver_ops(&driver);
    settings.can_fd = true;
    settings.data_bitrate_bps = UINT32_C(2000000);
    CHECK(ucn_v6_can_link_config_init(&settings, &config) == UCN_V6_OK);
    config.tx_timestamp_hardware = true;
    CHECK(ucn_v6_adapter_register_link(adapter, &config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 9U, 3U, UCN_V6_LINK_READY) == UCN_V6_OK);

    CHECK(ucn_v6_adapter_enqueue_tx(
              adapter, 9U, 101U, frame, sizeof(frame), UCN_V6_TRAFFIC_Q0,
              true, &key1) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_service_tx(adapter, &submitted) == UCN_V6_OK);
    CHECK(submitted && driver.submit_calls == 1U && driver.last_priority == 3U);
    CHECK(driver.last_length == sizeof(frame) &&
          memcmp(driver.last_frame, frame, sizeof(frame)) == 0);
    memset(&timestamp, 0, sizeof(timestamp));
    timestamp.timestamp_us = 900U;
    timestamp.uncertainty_us = 1U;
    timestamp.valid = true;
    timestamp.hardware = true;
    CHECK(ucn_v6_adapter_publish_tx_completion(
              adapter, &key1, UCN_V6_OK, &timestamp, true) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_publish_tx_completion(
              adapter, &key1, UCN_V6_OK, &timestamp, true) ==
          UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_adapter_peek_tx_completion(adapter, &completion) == UCN_V6_OK);
    CHECK(key_equal(&completion.key, &key1) &&
          completion.buffer_token == 101U && completion.timestamp.hardware);
    CHECK(ucn_v6_adapter_retire_tx_completion(
              adapter, &key1, &token) == UCN_V6_OK);
    CHECK(token == 101U);

    driver.submit_result = UCN_V6_ERR_NO_SPACE;
    CHECK(ucn_v6_adapter_enqueue_tx(
              adapter, 9U, 102U, frame, sizeof(frame), UCN_V6_TRAFFIC_Q3,
              false, &key2) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_service_tx(adapter, &submitted) ==
          UCN_V6_ERR_NO_SPACE);
    CHECK(!submitted);
    driver.submit_result = UCN_V6_OK;
    driver.complete_synchronously = true;
    CHECK(ucn_v6_adapter_service_tx(adapter, &submitted) == UCN_V6_OK);
    CHECK(submitted && driver.last_priority == 0U);
    CHECK(ucn_v6_adapter_peek_tx_completion(adapter, &completion) == UCN_V6_OK);
    CHECK(key_equal(&completion.key, &key2));
    CHECK(ucn_v6_adapter_retire_tx_completion(
              adapter, &key2, &token) == UCN_V6_OK && token == 102U);

    driver.complete_synchronously = false;
    CHECK(ucn_v6_adapter_enqueue_tx(
              adapter, 9U, 103U, frame, sizeof(frame), UCN_V6_TRAFFIC_Q2,
              false, &key3) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_service_tx(adapter, &submitted) == UCN_V6_OK &&
          submitted);
    CHECK(ucn_v6_adapter_cancel_tx(adapter, &key3) == UCN_V6_OK);
    CHECK(driver.cancel_calls == 1U);
    CHECK(ucn_v6_adapter_cancel_tx(adapter, &key3) == UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_adapter_peek_tx_completion(adapter, &completion) == UCN_V6_OK);
    CHECK(completion.result == UCN_V6_ERR_CANCELLED);
    CHECK(ucn_v6_adapter_enqueue_tx(
              adapter, 9U, 104U, frame, sizeof(frame), UCN_V6_TRAFFIC_Q1,
              false, &key4) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_cancel_tx(adapter, &key4) == UCN_V6_OK);
    CHECK(driver.cancel_calls == 1U);

    driver.quiesce_result = UCN_V6_ERR_STATE;
    CHECK(ucn_v6_adapter_reopen_link(
              adapter, 9U, retired, 2U, &retired_count,
              &generation) == UCN_V6_ERR_STATE);
    driver.quiesce_result = UCN_V6_OK;
    CHECK(ucn_v6_adapter_reopen_link(
              adapter, 9U, retired, 0U, &retired_count,
              &generation) == UCN_V6_ERR_NO_SPACE);
    CHECK(ucn_v6_adapter_reopen_link(
              adapter, 9U, retired, 2U, &retired_count,
              &generation) == UCN_V6_OK);
    CHECK(retired_count == 2U && generation == 4U);
    CHECK((retired[0] == 103U && retired[1] == 104U) ||
          (retired[0] == 104U && retired[1] == 103U));
    CHECK(ucn_v6_adapter_publish_tx_completion(
              adapter, &key3, UCN_V6_OK, NULL, true) == UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_adapter_publish_rx(
              adapter, 9U, 4U, frame, sizeof(frame), NULL,
              true, &key4) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 9U, 4U, UCN_V6_LINK_READY) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_publish_rx(
              adapter, 9U, 4U, frame, sizeof(frame), NULL,
              true, &key4) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_retire_rx(adapter, &key4) == UCN_V6_OK);
    return 0;
}

static int test_offline_link_does_not_block_ready_link(void)
{
    fake_environment_t environment;
    fake_driver_t offline_driver;
    fake_driver_t ready_driver;
    ucn_v6_adapter_owner_t *adapter = NULL;
    ucn_v6_uart_link_settings_t settings;
    ucn_v6_driver_link_config_t config;
    ucn_v6_driver_event_key_t offline_key;
    ucn_v6_driver_event_key_t ready_key;
    uint8_t frame[8];
    bool submitted = false;

    memset(&environment, 0, sizeof(environment));
    memset(&offline_driver, 0, sizeof(offline_driver));
    memset(&ready_driver, 0, sizeof(ready_driver));
    memset(frame, 0x3C, sizeof(frame));
    offline_driver.submit_result = UCN_V6_OK;
    offline_driver.cancel_result = UCN_V6_OK;
    offline_driver.quiesce_result = UCN_V6_OK;
    ready_driver = offline_driver;
    CHECK(init_adapter(&environment, &adapter) == UCN_V6_OK);
    offline_driver.owner = adapter;
    ready_driver.owner = adapter;

    memset(&settings, 0, sizeof(settings));
    settings.base.link_id = 30U;
    settings.base.initial_generation = 1U;
    settings.base.ops = driver_ops(&offline_driver);
    CHECK(ucn_v6_uart_link_config_init(&settings, &config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_register_link(adapter, &config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 30U, 1U, UCN_V6_LINK_READY) == UCN_V6_OK);

    settings.base.link_id = 31U;
    settings.base.ops = driver_ops(&ready_driver);
    CHECK(ucn_v6_uart_link_config_init(&settings, &config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_register_link(adapter, &config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 31U, 1U, UCN_V6_LINK_READY) == UCN_V6_OK);

    CHECK(ucn_v6_adapter_enqueue_tx(
              adapter, 30U, 301U, frame, sizeof(frame), UCN_V6_TRAFFIC_Q1,
              false, &offline_key) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_enqueue_tx(
              adapter, 31U, 302U, frame, sizeof(frame), UCN_V6_TRAFFIC_Q1,
              false, &ready_key) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 30U, 1U, UCN_V6_LINK_OFFLINE) == UCN_V6_OK);

    CHECK(ucn_v6_adapter_service_tx(adapter, &submitted) == UCN_V6_OK);
    CHECK(submitted && offline_driver.submit_calls == 0U &&
          ready_driver.submit_calls == 1U &&
          key_equal(&ready_driver.last_key, &ready_key));
    submitted = true;
    CHECK(ucn_v6_adapter_service_tx(adapter, &submitted) == UCN_V6_OK);
    CHECK(!submitted && offline_driver.submit_calls == 0U);
    CHECK(ucn_v6_adapter_cancel_tx(adapter, &offline_key) == UCN_V6_OK);
    return 0;
}

static ucn_v6_result_t port_noop_phase(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    fake_handler_t *handler = (fake_handler_t *)context;
    if (handler == NULL || result == NULL || now_us == 0U || budget == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ++handler->phase_calls;
    memset(result, 0, sizeof(*result));
    return UCN_V6_OK;
}

static ucn_v6_result_t port_timer_phase(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    fake_handler_t *handler = (fake_handler_t *)context;
    ucn_v6_result_t rc = port_noop_phase(
        context, now_us, budget, result);
    if (rc == UCN_V6_OK) {
        ++handler->timer_calls;
        if (handler->publish_deadline) {
            result->has_deadline = true;
            result->next_deadline_us = handler->deadline_us;
        }
        if (handler->backlog_timer_once && !handler->backlog_emitted) {
            handler->backlog_emitted = true;
            result->work_done = 1U;
            result->has_more = true;
        }
    }
    return rc;
}

static ucn_v6_result_t port_rx_phase(
    void *context, uint64_t now_us, uint16_t budget,
    ucn_v6_stack_phase_result_t *phase_result)
{
    fake_handler_t *handler = (fake_handler_t *)context;
    uint8_t frame[64];
    ucn_v6_driver_rx_view_t view;
    ucn_v6_result_t result;
    result = port_noop_phase(context, now_us, budget, phase_result);
    if (result != UCN_V6_OK || handler->adapter == NULL) return result;
    result = ucn_v6_adapter_peek_rx(
        handler->adapter, frame, sizeof(frame), &view);
    if (result == UCN_V6_ERR_NOT_FOUND) return UCN_V6_OK;
    if (result != UCN_V6_OK) return result;
    result = ucn_v6_adapter_retire_rx(handler->adapter, &view.key);
    if (result == UCN_V6_OK) {
        ++handler->retired_rx;
        phase_result->work_done = 1U;
    }
    return result;
}

static ucn_v6_result_t port_invalidate(
    void *context, const ucn_v6_stack_invalidation_t *invalidation)
{
    return context != NULL && invalidation != NULL ?
        UCN_V6_OK : UCN_V6_ERR_ARGUMENT;
}

static ucn_v6_stack_hooks_t port_stack_hooks(fake_handler_t *handler)
{
    ucn_v6_stack_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = handler;
    hooks.rx_ingress = port_rx_phase;
    hooks.tx_completion = port_noop_phase;
    hooks.timer_expiry = port_timer_phase;
    hooks.persistence = port_noop_phase;
    hooks.hop_security = port_noop_phase;
    hooks.e2e_security = port_noop_phase;
    hooks.capability = port_noop_phase;
    hooks.route_authority = port_noop_phase;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    hooks.realtime = port_noop_phase;
    hooks.invalidate_realtime = port_invalidate;
#endif
    hooks.operation = port_noop_phase;
    hooks.endpoint = port_noop_phase;
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    hooks.cluster = port_noop_phase;
    hooks.invalidate_cluster = port_invalidate;
#endif
    hooks.qos_tx = port_noop_phase;
#if UCN_V6_FEATURE_ADAPTER_ENABLED
    hooks.invalidate_adapter = port_invalidate;
#endif
    hooks.invalidate_security = port_invalidate;
    hooks.invalidate_capability = port_invalidate;
    hooks.invalidate_transfer = port_invalidate;
    hooks.invalidate_route = port_invalidate;
    hooks.invalidate_qos = port_invalidate;
    hooks.invalidate_endpoint = port_invalidate;
    return hooks;
}

static ucn_v6_stack_budget_t port_stack_budget(void)
{
    ucn_v6_stack_budget_t budget;
    size_t phase;
    memset(&budget, 0, sizeof(budget));
    for (phase = 0U; phase < UCN_V6_STACK_PHASE_COUNT; ++phase) {
        budget.phase_work[phase] = 1U;
        ++budget.max_total_work;
    }
#if !UCN_V6_FEATURE_REALTIME_ENABLED
    budget.phase_work[UCN_V6_STACK_PHASE_REALTIME] = 0U;
    --budget.max_total_work;
#endif
#if !UCN_V6_FEATURE_CLUSTER_ENABLED
    budget.phase_work[UCN_V6_STACK_PHASE_CLUSTER] = 0U;
    --budget.max_total_work;
#endif
    return budget;
}

static int test_freertos_budget_preflight_is_non_destructive(void)
{
    ucn_v6_freertos_port_storage_t storage;
    ucn_v6_freertos_port_storage_t before;
    fake_environment_t environment;
    fake_handler_t handler;
    ucn_v6_freertos_port_ops_t port_ops;
    ucn_v6_stack_hooks_t stack_hooks;
    ucn_v6_stack_budget_t budget;
    ucn_v6_freertos_port_t *port;

    memset(&environment, 0, sizeof(environment));
    memset(&handler, 0, sizeof(handler));
    memset(&port_ops, 0, sizeof(port_ops));
    port_ops.struct_size = sizeof(port_ops);
    port_ops.api_version = UCN_V6_FREERTOS_PORT_API_VERSION;
    port_ops.context = &environment;
    port_ops.lock_task = fake_port_lock_task;
    port_ops.try_lock_from_isr = fake_try_lock_from_isr;
    port_ops.unlock_task = fake_unlock_task;
    port_ops.unlock_from_isr = fake_unlock_from_isr;
    port_ops.notify_owner_task = fake_notify;
    port_ops.wait_for_notification = fake_wait;
    port_ops.read_monotonic_time_us = fake_now;
    stack_hooks = port_stack_hooks(&handler);

    memset(&storage, 0xA5, sizeof(storage));
    before = storage;
    memset(&budget, 0, sizeof(budget));
    port = (ucn_v6_freertos_port_t *)(void *)storage.bytes;
    CHECK(ucn_v6_freertos_port_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &port_ops, &stack_hooks, &budget, &port) == UCN_V6_ERR_CONFIG);
    CHECK(port == NULL && memcmp(&storage, &before, sizeof(storage)) == 0);

    memset(&storage, 0x5A, sizeof(storage));
    before = storage;
    budget = port_stack_budget();
    --budget.max_total_work;
    port = (ucn_v6_freertos_port_t *)(void *)storage.bytes;
    CHECK(ucn_v6_freertos_port_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &port_ops, &stack_hooks, &budget, &port) == UCN_V6_ERR_CONFIG);
    CHECK(port == NULL && memcmp(&storage, &before, sizeof(storage)) == 0);

#if !UCN_V6_FEATURE_REALTIME_ENABLED
    memset(&storage, 0x3C, sizeof(storage));
    before = storage;
    budget = port_stack_budget();
    budget.phase_work[UCN_V6_STACK_PHASE_REALTIME] = 1U;
    port = (ucn_v6_freertos_port_t *)(void *)storage.bytes;
    CHECK(ucn_v6_freertos_port_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &port_ops, &stack_hooks, &budget, &port) == UCN_V6_ERR_CONFIG);
    CHECK(port == NULL && memcmp(&storage, &before, sizeof(storage)) == 0);
#endif
#if !UCN_V6_FEATURE_CLUSTER_ENABLED
    memset(&storage, 0xC3, sizeof(storage));
    before = storage;
    budget = port_stack_budget();
    budget.phase_work[UCN_V6_STACK_PHASE_CLUSTER] = 1U;
    port = (ucn_v6_freertos_port_t *)(void *)storage.bytes;
    CHECK(ucn_v6_freertos_port_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &port_ops, &stack_hooks, &budget, &port) == UCN_V6_ERR_CONFIG);
    CHECK(port == NULL && memcmp(&storage, &before, sizeof(storage)) == 0);
#endif
    return 0;
}

static int test_freertos_notification_owner(void)
{
    fake_environment_t environment;
    fake_driver_t driver;
    fake_handler_t handler;
    ucn_v6_freertos_port_ops_t port_ops;
    ucn_v6_stack_hooks_t stack_hooks;
    ucn_v6_stack_budget_t stack_budget;
    ucn_v6_stack_run_result_t run_result;
    ucn_v6_freertos_port_t *port = NULL;
    ucn_v6_driver_runtime_ops_t runtime;
    ucn_v6_adapter_owner_t *adapter = NULL;
    ucn_v6_uart_link_settings_t settings;
    ucn_v6_driver_link_config_t config;
    ucn_v6_driver_event_key_t key;
    uint8_t frame[36];
    memset(&environment, 0, sizeof(environment));
    memset(&driver, 0, sizeof(driver));
    memset(&handler, 0, sizeof(handler));
    memset(&port_ops, 0, sizeof(port_ops));
    memset(frame, 0x33, sizeof(frame));
    environment.now_us = 5000U;
    port_ops.struct_size = sizeof(port_ops);
    port_ops.api_version = UCN_V6_FREERTOS_PORT_API_VERSION;
    port_ops.context = &environment;
    port_ops.lock_task = fake_port_lock_task;
    port_ops.try_lock_from_isr = fake_try_lock_from_isr;
    port_ops.unlock_task = fake_unlock_task;
    port_ops.unlock_from_isr = fake_unlock_from_isr;
    port_ops.notify_owner_task = fake_notify;
    port_ops.wait_for_notification = fake_wait;
    port_ops.read_monotonic_time_us = fake_now;
    stack_hooks = port_stack_hooks(&handler);
    stack_budget = port_stack_budget();
    CHECK(ucn_v6_freertos_port_init_in_place(
              port_storage.bytes, sizeof(port_storage),
              ucn_v6_compiled_manifest(), &port_ops, &stack_hooks,
              &stack_budget, &port) == UCN_V6_OK);
    CHECK(ucn_v6_freertos_port_make_adapter_runtime(
              port, &runtime) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_init_in_place(
              adapter_storage.bytes, sizeof(adapter_storage),
              ucn_v6_compiled_manifest(), &runtime, &adapter) == UCN_V6_OK);
    CHECK(ucn_v6_freertos_port_bind_adapter(port, adapter) == UCN_V6_OK);
    handler.adapter = adapter;
    driver.owner = adapter;
    driver.submit_result = UCN_V6_OK;
    driver.cancel_result = UCN_V6_OK;
    driver.quiesce_result = UCN_V6_OK;
    memset(&settings, 0, sizeof(settings));
    settings.base.link_id = 4U;
    settings.base.initial_generation = 1U;
    settings.base.ops = driver_ops(&driver);
    CHECK(ucn_v6_uart_link_config_init(&settings, &config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_register_link(adapter, &config) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_set_link_readiness(
              adapter, 4U, 1U, UCN_V6_LINK_READY) == UCN_V6_OK);
    CHECK(ucn_v6_adapter_publish_rx(
              adapter, 4U, 1U, frame, sizeof(frame), NULL,
              true, &key) == UCN_V6_OK);
    CHECK(environment.isr_notifications == 1U);
    CHECK(ucn_v6_freertos_port_run(port, &run_result) == UCN_V6_OK);
    CHECK(run_result.work_done == 1U && handler.retired_rx == 1U);

    CHECK(ucn_v6_freertos_port_post_timer(port, false) == UCN_V6_OK);
    CHECK(ucn_v6_freertos_port_wait_and_run(
              port, 1000U, &run_result) == UCN_V6_OK);
    CHECK(environment.wait_calls == 0U);

    handler.publish_deadline = true;
    handler.deadline_us = 5500U;
    CHECK(ucn_v6_freertos_port_post_timer(port, false) == UCN_V6_OK);
    CHECK(ucn_v6_freertos_port_run(port, &run_result) == UCN_V6_OK);
    environment.wait_notified = false;
    CHECK(ucn_v6_freertos_port_wait_and_run(
              port, 1000U, &run_result) == UCN_V6_OK);
    CHECK(environment.wait_calls == 1U &&
          environment.last_wait_us == 500U);

    handler.publish_deadline = false;
    handler.backlog_timer_once = true;
    environment.now_us = 5001U;
    CHECK(ucn_v6_freertos_port_post_timer(port, false) == UCN_V6_OK);
    CHECK(ucn_v6_freertos_port_run(port, &run_result) == UCN_V6_OK);
    CHECK(run_result.more_work);
    CHECK(ucn_v6_freertos_port_wait_and_run(
              port, 1000U, &run_result) == UCN_V6_OK);
    CHECK(environment.wait_calls == 1U && !run_result.more_work);

    CHECK(ucn_v6_freertos_port_wait_and_run(
              port, 1000U, &run_result) == UCN_V6_OK);
    CHECK(environment.wait_calls == 2U &&
          environment.last_wait_us == 1000U &&
          handler.timer_calls >= 6U);
    return 0;
}

int main(void)
{
    CHECK(test_reference_profiles_and_rx() == 0);
    CHECK(test_esp32s3_reference_configuration() == 0);
    CHECK(test_tx_lifecycle_and_reopen() == 0);
    CHECK(test_offline_link_does_not_block_ready_link() == 0);
    CHECK(test_freertos_budget_preflight_is_non_destructive() == 0);
    CHECK(test_freertos_notification_owner() == 0);
    printf("ucn v6 adapter and FreeRTOS port tests passed "
           "(adapter_storage=%zu, freertos_storage=%zu)\n",
           sizeof(adapter_storage), sizeof(port_storage));
    return 0;
}
