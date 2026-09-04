#include "ucn/ucn_timed_link.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "assertion failed: %s:%d: %s\n",          \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (0)

typedef struct test_lock {
    pthread_mutex_t mutex;
} test_lock_t;

typedef struct blocking_driver {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool callback_entered;
    bool callback_may_return;
} blocking_driver_t;

typedef struct allocation_thread {
    ucn_timed_link_t *link;
    ucn_time_event_key_t key;
    ucn_result_t result;
} allocation_thread_t;

static void enter_task(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    (void)pthread_mutex_lock(&lock->mutex);
}

static void exit_task(void *context)
{
    test_lock_t *lock = (test_lock_t *)context;
    (void)pthread_mutex_unlock(&lock->mutex);
}

static ucn_port_critical_token_t enter_isr(void *context)
{
    enter_task(context);
    return (ucn_port_critical_token_t)1U;
}

static void exit_isr(void *context, ucn_port_critical_token_t token)
{
    (void)token;
    exit_task(context);
}

static ucn_port_ops_t port_ops(void)
{
    ucn_port_ops_t ops;

    (void)memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_PORT_OPS_API_VERSION;
    ops.enter_critical = enter_task;
    ops.exit_critical = exit_task;
    ops.enter_critical_from_isr = enter_isr;
    ops.exit_critical_from_isr = exit_isr;
    return ops;
}

/* EN: Holds one Driver callback open while another thread probes Link B.
 * 中文：保持一个 Driver 回调处于活动状态，同时由另一线程探测 Link B。 */
static ucn_result_t blocking_reserve(
    void *context,
    const ucn_time_event_key_t *key)
{
    blocking_driver_t *driver = (blocking_driver_t *)context;

    if (driver == NULL || key == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)pthread_mutex_lock(&driver->mutex);
    driver->callback_entered = true;
    (void)pthread_cond_broadcast(&driver->condition);
    while (!driver->callback_may_return) {
        (void)pthread_cond_wait(&driver->condition, &driver->mutex);
    }
    (void)pthread_mutex_unlock(&driver->mutex);
    return UCN_OK;
}

static ucn_result_t unused_submit(void *context,
                                  const ucn_time_event_key_t *key,
                                  const uint8_t *frame,
                                  size_t length)
{
    (void)context;
    return key != NULL && frame != NULL && length != 0U ? UCN_OK :
                                                         UCN_ERR_ARGUMENT;
}

static ucn_result_t unused_key(void *context,
                               const ucn_time_event_key_t *key)
{
    (void)context;
    return key != NULL ? UCN_OK : UCN_ERR_ARGUMENT;
}

static ucn_result_t unused_quiesce(void *context)
{
    (void)context;
    return UCN_OK;
}

static void *allocate_link_a(void *context)
{
    allocation_thread_t *thread = (allocation_thread_t *)context;

    thread->result = ucn_timed_link_allocate_event(
        thread->link, UCN_TIME_EVENT_TX, &thread->key);
    return NULL;
}

/* EN: Proves the shared gate synchronizes two Links with different locks and
 * an ISR-context access. This test is intended to run under TSan as well.
 * 中文：证明共享围栏能同步使用不同锁的两个 Link 与 ISR 入口；该测试同时用于
 * TSan。 */
static bool test_cross_link_concurrent_callback_gate(void)
{
    ucn_time_link_ops_t time_ops;
    ucn_port_ops_t ports = port_ops();
    test_lock_t gate_lock;
    test_lock_t link_a_lock;
    test_lock_t link_b_lock;
    blocking_driver_t driver;
    ucn_timed_link_callback_gate_t gate;
    ucn_timed_link_t link_a;
    ucn_timed_link_t link_b;
    allocation_thread_t thread;
    ucn_time_event_key_t rejected;
    ucn_time_event_key_t before;
    pthread_t worker;

    (void)memset(&time_ops, 0, sizeof(time_ops));
    time_ops.struct_size = sizeof(time_ops);
    time_ops.api_version = UCN_TIME_LINK_OPS_API_VERSION;
    time_ops.reserve_tx_token = blocking_reserve;
    time_ops.submit_timestamped = unused_submit;
    time_ops.cancel_tx_token = unused_key;
    time_ops.quiesce = unused_quiesce;
    (void)memset(&driver, 0, sizeof(driver));
    (void)memset(&thread, 0, sizeof(thread));
    TEST_ASSERT(pthread_mutex_init(&gate_lock.mutex, NULL) == 0);
    TEST_ASSERT(pthread_mutex_init(&link_a_lock.mutex, NULL) == 0);
    TEST_ASSERT(pthread_mutex_init(&link_b_lock.mutex, NULL) == 0);
    TEST_ASSERT(pthread_mutex_init(&driver.mutex, NULL) == 0);
    TEST_ASSERT(pthread_cond_init(&driver.condition, NULL) == 0);
    TEST_ASSERT(ucn_timed_link_callback_gate_init(
                    &gate, &ports, &gate_lock) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_init(
                    &link_a, 1U, 1U, &time_ops, &driver,
                    &ports, &link_a_lock, &gate) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_init(
                    &link_b, 2U, 1U, &time_ops, &driver,
                    &ports, &link_b_lock, &gate) == UCN_OK);

    thread.link = &link_a;
    TEST_ASSERT(pthread_create(&worker, NULL, allocate_link_a, &thread) == 0);
    (void)pthread_mutex_lock(&driver.mutex);
    while (!driver.callback_entered) {
        (void)pthread_cond_wait(&driver.condition, &driver.mutex);
    }
    (void)pthread_mutex_unlock(&driver.mutex);

    (void)memset(&rejected, 0xA5, sizeof(rejected));
    before = rejected;
    TEST_ASSERT(ucn_timed_link_allocate_rx_event_from_isr(
                    &link_b, &rejected) == UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&rejected, &before, sizeof(rejected)) == 0 &&
                link_b.next_rx_token == 0U);
    TEST_ASSERT(ucn_timed_link_allocate_event(
                    &link_b, UCN_TIME_EVENT_RX, &rejected) == UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&rejected, &before, sizeof(rejected)) == 0 &&
                link_b.next_rx_token == 0U);

    (void)pthread_mutex_lock(&driver.mutex);
    driver.callback_may_return = true;
    (void)pthread_cond_broadcast(&driver.condition);
    (void)pthread_mutex_unlock(&driver.mutex);
    TEST_ASSERT(pthread_join(worker, NULL) == 0);
    TEST_ASSERT(thread.result == UCN_OK && thread.key.link_id == 1U);

    TEST_ASSERT(pthread_cond_destroy(&driver.condition) == 0);
    TEST_ASSERT(pthread_mutex_destroy(&driver.mutex) == 0);
    TEST_ASSERT(pthread_mutex_destroy(&link_b_lock.mutex) == 0);
    TEST_ASSERT(pthread_mutex_destroy(&link_a_lock.mutex) == 0);
    TEST_ASSERT(pthread_mutex_destroy(&gate_lock.mutex) == 0);
    return true;
}

int main(void)
{
    if (!test_cross_link_concurrent_callback_gate()) {
        return 1;
    }
    (void)puts("ucn timed link concurrency tests passed");
    return 0;
}
