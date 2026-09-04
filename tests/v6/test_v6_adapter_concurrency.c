#include "ucn/v6/adapters/ucn_v6_uart.h"

#include <pthread.h>
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

typedef struct shared_lock {
    pthread_mutex_t mutex;
} shared_lock_t;

typedef struct producer {
    ucn_v6_adapter_owner_t *adapter;
    uint16_t link_id;
    bool from_isr;
    unsigned accepted;
    unsigned contended;
} producer_t;

static ucn_v6_adapter_owner_storage_t storage;

static ucn_v6_result_t lock_task(void *context)
{
    shared_lock_t *lock = (shared_lock_t *)context;
    return pthread_mutex_lock(&lock->mutex) == 0 ?
        UCN_V6_OK : UCN_V6_ERR_STATE;
}

static bool try_lock_from_isr(void *context)
{
    shared_lock_t *lock = (shared_lock_t *)context;
    return pthread_mutex_trylock(&lock->mutex) == 0;
}

static void unlock_task(void *context)
{
    shared_lock_t *lock = (shared_lock_t *)context;
    (void)pthread_mutex_unlock(&lock->mutex);
}

static void unlock_from_isr(void *context)
{
    unlock_task(context);
}

static ucn_v6_result_t post_event(
    void *context, ucn_v6_owner_event_t event, bool from_isr)
{
    (void)context;
    (void)event;
    (void)from_isr;
    return UCN_V6_OK;
}

static ucn_v6_result_t submit(
    void *context,
    const ucn_v6_driver_event_key_t *key,
    const uint8_t *frame,
    size_t length,
    uint8_t priority,
    bool timestamp)
{
    (void)context;
    (void)key;
    (void)frame;
    (void)length;
    (void)priority;
    (void)timestamp;
    return UCN_V6_OK;
}

static ucn_v6_result_t quiesce(void *context)
{
    (void)context;
    return UCN_V6_OK;
}

static void *produce(void *context)
{
    producer_t *producer = (producer_t *)context;
    uint8_t frame[40];
    unsigned index;
    memset(frame, (int)producer->link_id, sizeof(frame));
    for (index = 0U; index < 16U; ++index) {
        ucn_v6_driver_event_key_t key;
        ucn_v6_result_t result = ucn_v6_adapter_publish_rx(
            producer->adapter, producer->link_id, 1U, frame, sizeof(frame),
            NULL, producer->from_isr, &key);
        if (result == UCN_V6_OK) ++producer->accepted;
        else if (result == UCN_V6_ERR_STATE && producer->from_isr) {
            ++producer->contended;
        }
    }
    return NULL;
}

static int register_uart(ucn_v6_adapter_owner_t *adapter, uint16_t link_id)
{
    ucn_v6_uart_link_settings_t settings;
    ucn_v6_driver_link_config_t config;
    memset(&settings, 0, sizeof(settings));
    settings.base.link_id = link_id;
    settings.base.initial_generation = 1U;
    settings.base.rx_slot_quota = 16U;
    settings.base.tx_slot_quota = 1U;
    settings.base.ops.struct_size = sizeof(settings.base.ops);
    settings.base.ops.api_version = UCN_V6_ADAPTER_API_VERSION;
    settings.base.ops.submit = submit;
    settings.base.ops.quiesce = quiesce;
    if (ucn_v6_uart_link_config_init(&settings, &config) != UCN_V6_OK) return 1;
    if (ucn_v6_adapter_register_link(adapter, &config) != UCN_V6_OK) return 1;
    return ucn_v6_adapter_set_link_readiness(
        adapter, link_id, 1U, UCN_V6_LINK_READY) == UCN_V6_OK ? 0 : 1;
}

int main(void)
{
    shared_lock_t lock;
    ucn_v6_driver_runtime_ops_t runtime;
    ucn_v6_adapter_owner_t *adapter = NULL;
    producer_t task_producer;
    producer_t isr_producer;
    pthread_t task_thread;
    pthread_t isr_thread;
    unsigned retired = 0U;
    CHECK(pthread_mutex_init(&lock.mutex, NULL) == 0);
    memset(&runtime, 0, sizeof(runtime));
    runtime.context = &lock;
    runtime.lock_task = lock_task;
    runtime.try_lock_from_isr = try_lock_from_isr;
    runtime.unlock_task = unlock_task;
    runtime.unlock_from_isr = unlock_from_isr;
    runtime.post_owner_event = post_event;
    CHECK(ucn_v6_adapter_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &runtime, &adapter) == UCN_V6_OK);
    CHECK(register_uart(adapter, 1U) == 0);
    CHECK(register_uart(adapter, 2U) == 0);
    memset(&task_producer, 0, sizeof(task_producer));
    memset(&isr_producer, 0, sizeof(isr_producer));
    task_producer.adapter = adapter;
    task_producer.link_id = 1U;
    isr_producer.adapter = adapter;
    isr_producer.link_id = 2U;
    isr_producer.from_isr = true;
    CHECK(pthread_create(&task_thread, NULL, produce, &task_producer) == 0);
    CHECK(pthread_create(&isr_thread, NULL, produce, &isr_producer) == 0);
    CHECK(pthread_join(task_thread, NULL) == 0);
    CHECK(pthread_join(isr_thread, NULL) == 0);
    for (;;) {
        uint8_t frame[40];
        ucn_v6_driver_rx_view_t view;
        ucn_v6_result_t result = ucn_v6_adapter_peek_rx(
            adapter, frame, sizeof(frame), &view);
        if (result == UCN_V6_ERR_NOT_FOUND) break;
        CHECK(result == UCN_V6_OK);
        CHECK(frame[0] == (uint8_t)view.key.link_id);
        CHECK(ucn_v6_adapter_retire_rx(adapter, &view.key) == UCN_V6_OK);
        ++retired;
    }
    CHECK(retired == task_producer.accepted + isr_producer.accepted);
    CHECK(task_producer.accepted == 16U);
    CHECK(isr_producer.accepted + isr_producer.contended == 16U);
    CHECK(pthread_mutex_destroy(&lock.mutex) == 0);
    puts("ucn v6 adapter concurrency tests passed");
    return 0;
}
