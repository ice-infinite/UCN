#include "ucn/ucn_timed_link.h"

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

typedef struct fake_context {
    ucn_timed_link_t *link;
    ucn_timed_link_t *other_link;
    uint32_t reserve_calls;
    uint32_t submit_calls;
    uint32_t cancel_calls;
    uint32_t quiesce_calls;
    uint32_t task_enter;
    uint32_t task_exit;
    uint32_t isr_enter;
    uint32_t isr_exit;
    ucn_result_t nested_result;
    ucn_result_t nested_other_result;
    ucn_result_t nested_other_rx_result;
    ucn_result_t nested_other_rx_isr_result;
    ucn_result_t nested_init_result;
    uint32_t nested_init_attempts;
    bool try_reentry;
    uint8_t submitted[UCN_MAX_FRAME_BYTES];
    size_t submitted_length;
} fake_context_t;

/* EN: Attempts to overwrite an active Link from foreign Driver code.
 * 中文：尝试从外部 Driver 回调覆盖一个活动 Link。 */
static void probe_nested_init(fake_context_t *fake)
{
    ++fake->nested_init_attempts;
    fake->nested_init_result = ucn_timed_link_init(
        fake->link, fake->link->link_id,
        fake->link->link_instance_generation, fake->link->ops,
        fake->link->context, fake->link->port_ops,
        fake->link->port_context, fake->link->callback_gate);
}

/* EN: Records one Driver token reservation and probes callback reentry.
 * 中文：记录一次驱动 token 保留并探测回调重入。 */
static ucn_result_t fake_reserve(void *context,
                                 const ucn_time_event_key_t *key)
{
    fake_context_t *fake = (fake_context_t *)context;
    ucn_time_event_key_t nested;

    ++fake->reserve_calls;
    if (key == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (fake->try_reentry) {
        fake->nested_result = ucn_timed_link_allocate_event(
            fake->link, UCN_TIME_EVENT_TX, &nested);
        if (fake->other_link != NULL) {
            fake->nested_other_result = ucn_timed_link_allocate_event(
                fake->other_link, UCN_TIME_EVENT_TX, &nested);
            fake->nested_other_rx_result = ucn_timed_link_allocate_event(
                fake->other_link, UCN_TIME_EVENT_RX, &nested);
            fake->nested_other_rx_isr_result =
                ucn_timed_link_allocate_rx_event_from_isr(
                    fake->other_link, &nested);
        }
        probe_nested_init(fake);
    }
    return UCN_OK;
}

/* EN: Saves the inseparable frame/key submission for exact comparison.
 * 中文：保存不可拆分的 frame/key 提交以便精确比较。 */
static ucn_result_t fake_submit(void *context,
                                const ucn_time_event_key_t *key,
                                const uint8_t *frame,
                                size_t length)
{
    fake_context_t *fake = (fake_context_t *)context;

    if (key == NULL || frame == NULL || length > sizeof(fake->submitted)) {
        return UCN_ERR_ARGUMENT;
    }
    ++fake->submit_calls;
    if (fake->try_reentry) {
        probe_nested_init(fake);
    }
    (void)memcpy(fake->submitted, frame, length);
    fake->submitted_length = length;
    return UCN_OK;
}

/* EN: Records Driver cancellation.
 * 中文：记录驱动取消操作。 */
static ucn_result_t fake_cancel(void *context,
                                const ucn_time_event_key_t *key)
{
    fake_context_t *fake = (fake_context_t *)context;

    if (key == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++fake->cancel_calls;
    if (fake->try_reentry) {
        probe_nested_init(fake);
    }
    return UCN_OK;
}

/* EN: Records that callbacks were quiesced before reopen.
 * 中文：记录 reopen 前已经静止回调源。 */
static ucn_result_t fake_quiesce(void *context)
{
    fake_context_t *fake = (fake_context_t *)context;
    ++fake->quiesce_calls;
    if (fake->try_reentry) {
        probe_nested_init(fake);
    }
    return UCN_OK;
}

static void fake_enter_task(void *context)
{
    ++((fake_context_t *)context)->task_enter;
}

static void fake_exit_task(void *context)
{
    ++((fake_context_t *)context)->task_exit;
}

static ucn_port_critical_token_t fake_enter_isr(void *context)
{
    fake_context_t *fake = (fake_context_t *)context;
    ++fake->isr_enter;
    return (ucn_port_critical_token_t)0x1234U;
}

static void fake_exit_isr(void *context, ucn_port_critical_token_t token)
{
    fake_context_t *fake = (fake_context_t *)context;
    if (token == (ucn_port_critical_token_t)0x1234U) {
        ++fake->isr_exit;
    }
}

/* EN: Creates the versioned fake Time Link operations.
 * 中文：创建带版本的 Fake Time Link 操作表。 */
static ucn_time_link_ops_t time_ops(void)
{
    ucn_time_link_ops_t ops;

    (void)memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_TIME_LINK_OPS_API_VERSION;
    ops.reserve_tx_token = fake_reserve;
    ops.submit_timestamped = fake_submit;
    ops.cancel_tx_token = fake_cancel;
    ops.quiesce = fake_quiesce;
    return ops;
}

/* EN: Creates task/ISR critical pairs for queue tests.
 * 中文：为队列测试创建任务/ISR 临界区回调对。 */
static ucn_port_ops_t port_ops(void)
{
    ucn_port_ops_t ops;

    (void)memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_PORT_OPS_API_VERSION;
    ops.enter_critical = fake_enter_task;
    ops.exit_critical = fake_exit_task;
    ops.enter_critical_from_isr = fake_enter_isr;
    ops.exit_critical_from_isr = fake_exit_isr;
    return ops;
}

/* EN: Verifies unique keys, reentry fencing, submit, cancel and reopen.
 * 中文：验证唯一 key、重入围栏、提交、取消和 reopen。 */
static bool test_link_instance_and_driver_contract(void)
{
    fake_context_t fake;
    ucn_time_link_ops_t ops = time_ops();
    ucn_port_ops_t ports = port_ops();
    ucn_timed_link_callback_gate_t callback_gate;
    ucn_timed_link_t link;
    ucn_timed_link_t other_link;
    ucn_time_event_key_t key;
    ucn_time_event_key_t cancel_key;
    ucn_time_event_key_t retire_key;
    ucn_time_event_key_t rx_key;
    uint8_t frame[] = {1U, 2U, 3U, 4U};

    (void)memset(&fake, 0, sizeof(fake));
    TEST_ASSERT(ucn_timed_link_callback_gate_init(
                    &callback_gate, &ports, &fake) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_init(&link, 3U, 1U, &ops, &fake,
                                    &ports, &fake, &callback_gate) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_init(&other_link, 4U, 1U, &ops, &fake,
                                    &ports, &fake, &callback_gate) == UCN_OK);
    fake.link = &link;
    fake.other_link = &other_link;
    fake.try_reentry = true;
    TEST_ASSERT(ucn_timed_link_allocate_event(&link, UCN_TIME_EVENT_TX,
                                              &key) == UCN_OK);
    TEST_ASSERT(key.event_token == 1U && fake.reserve_calls == 1U &&
                fake.nested_result == UCN_ERR_STATE);
    TEST_ASSERT(fake.nested_other_result == UCN_ERR_STATE &&
                fake.nested_other_rx_result == UCN_ERR_STATE &&
                fake.nested_other_rx_isr_result == UCN_ERR_STATE &&
                other_link.next_tx_token == 0U &&
                other_link.next_rx_token == 0U);
    TEST_ASSERT(ucn_timed_link_submit(&link, &key, frame, sizeof(frame)) ==
                UCN_OK);
    TEST_ASSERT(fake.submit_calls == 1U && fake.submitted_length == 4U &&
                memcmp(fake.submitted, frame, sizeof(frame)) == 0);
    TEST_ASSERT(ucn_timed_link_submit(&link, &key, frame, sizeof(frame)) ==
                UCN_ERR_STATE);
    TEST_ASSERT(ucn_timed_link_cancel(&link, &key) == UCN_ERR_STATE &&
                fake.cancel_calls == 0U);
    TEST_ASSERT(ucn_timed_link_complete_event(&link, &key) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_complete_event(&link, &key) == UCN_ERR_STATE);
    TEST_ASSERT(ucn_timed_link_allocate_event(&link, UCN_TIME_EVENT_TX,
                                              &cancel_key) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_cancel(&link, &cancel_key) == UCN_OK &&
                fake.cancel_calls == 1U);
    TEST_ASSERT(ucn_timed_link_cancel(&link, &cancel_key) == UCN_ERR_STATE);
    TEST_ASSERT(ucn_timed_link_allocate_event(&link, UCN_TIME_EVENT_TX,
                                              &retire_key) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_submit(&link, &retire_key, frame,
                                      sizeof(frame)) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_retire_event(&link, &retire_key) == UCN_OK &&
                fake.cancel_calls == 2U);
    TEST_ASSERT(ucn_timed_link_allocate_rx_event_from_isr(&link, &rx_key) ==
                UCN_OK);
    TEST_ASSERT(rx_key.direction == UCN_TIME_EVENT_RX &&
                fake.isr_enter >= 1U && fake.isr_exit >= 1U);
    TEST_ASSERT(ucn_timed_link_complete_event(&link, &rx_key) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_complete_event(&link, &rx_key) ==
                UCN_ERR_STATE);
    TEST_ASSERT(ucn_timed_link_reopen(&link) == UCN_OK);
    TEST_ASSERT(link.link_instance_generation == 2U &&
                fake.quiesce_calls == 1U &&
                !ucn_timed_link_key_is_current(&link, &key));
    TEST_ASSERT(fake.nested_init_attempts >= 4U &&
                fake.nested_init_result == UCN_ERR_STATE);
    TEST_ASSERT(ucn_timed_link_submit(&link, &key, frame, sizeof(frame)) ==
                UCN_ERR_STATE);
    return true;
}

/* EN: Verifies no-wrap failure does not write a caller key.
 * 中文：验证 no-wrap 失败不会写入调用者 key。 */
static bool test_no_wrap(void)
{
    fake_context_t fake;
    ucn_time_link_ops_t ops = time_ops();
    ucn_port_ops_t ports = port_ops();
    ucn_timed_link_callback_gate_t callback_gate;
    ucn_timed_link_t link;
    ucn_time_event_key_t key;
    ucn_time_event_key_t before;

    (void)memset(&fake, 0, sizeof(fake));
    TEST_ASSERT(ucn_timed_link_callback_gate_init(
                    &callback_gate, &ports, &fake) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_init(&link, 1U, 1U, &ops, &fake,
                                    &ports, &fake, &callback_gate) == UCN_OK);
    link.next_tx_token = UCN_TIME_EVENT_SERIAL_MAX;
    (void)memset(&key, 0xA5, sizeof(key));
    before = key;
    TEST_ASSERT(ucn_timed_link_allocate_event(&link, UCN_TIME_EVENT_TX,
                                              &key) == UCN_ERR_EXHAUSTED);
    TEST_ASSERT(memcmp(&key, &before, sizeof(key)) == 0 && link.faulted);
    return true;
}

/* EN: Verifies bounded TX event queue ownership and full behavior.
 * 中文：验证有界 TX 事件队列所有权和满载行为。 */
static bool test_tx_event_queue(void)
{
    fake_context_t fake;
    ucn_port_ops_t ports = port_ops();
    ucn_time_tx_event_queue_t queue;
    ucn_time_tx_timestamp_event_t event;
    ucn_time_tx_timestamp_event_t output;
    size_t index;

    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&queue, 0, sizeof(queue));
    (void)memset(&event, 0, sizeof(event));
    event.key.link_id = 1U;
    event.key.direction = UCN_TIME_EVENT_TX;
    event.key.link_instance_generation = 1U;
    event.key.event_token = 1U;
    event.completion = UCN_OK;
    TEST_ASSERT(ucn_time_tx_event_enqueue(&queue, &event) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_time_tx_event_queue_init(&queue, &ports, &fake) == UCN_OK);
    for (index = 0U; index < UCN_TIME_TX_EVENT_QUEUE_DEPTH; ++index) {
        event.key.event_token = (uint32_t)index + 1U;
        event.timestamp_us = (uint64_t)index + 100U;
        TEST_ASSERT(ucn_time_tx_event_enqueue_from_isr(&queue, &event) ==
                    UCN_OK);
    }
    event.key.event_token++;
    TEST_ASSERT(ucn_time_tx_event_enqueue_from_isr(&queue, &event) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(queue.count == UCN_TIME_TX_EVENT_QUEUE_DEPTH &&
                queue.dropped_full == 1U);
    for (index = 0U; index < UCN_TIME_TX_EVENT_QUEUE_DEPTH; ++index) {
        TEST_ASSERT(ucn_time_tx_event_dequeue(&queue, &output) == UCN_OK);
        TEST_ASSERT(output.key.event_token == (uint32_t)index + 1U &&
                    output.timestamp_us == (uint64_t)index + 100U);
    }
    event.completion = (ucn_result_t)1;
    TEST_ASSERT(ucn_time_tx_event_enqueue(&queue, &event) ==
                UCN_ERR_ARGUMENT);
    event.completion = UCN_OK;
    queue.count = UCN_TIME_TX_EVENT_QUEUE_DEPTH + 1U;
    (void)memset(&output, 0xA5, sizeof(output));
    {
        ucn_time_tx_timestamp_event_t before = output;
        TEST_ASSERT(ucn_time_tx_event_dequeue(&queue, &output) ==
                    UCN_ERR_STATE);
        TEST_ASSERT(memcmp(&output, &before, sizeof(output)) == 0);
    }
    return true;
}

/* EN: Verifies a Timed RX frame/key/timestamp is copied atomically.
 * 中文：验证 Timed RX 的 frame/key/timestamp 被原子复制。 */
static bool test_atomic_timed_rx_queue(void)
{
    fake_context_t fake;
    ucn_port_ops_t ports = port_ops();
    ucn_time_timed_rx_queue_t queue;
    ucn_time_timed_rx_item_t item;
    ucn_time_timed_rx_item_t output;
    ucn_link_t ingress;
    size_t index;

    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&ingress, 0, sizeof(ingress));
    ingress.link_id = 7U;
    (void)memset(&queue, 0, sizeof(queue));
    (void)memset(&item, 0, sizeof(item));
    item.ingress_link = &ingress;
    item.key.link_id = 7U;
    item.key.direction = UCN_TIME_EVENT_RX;
    item.key.link_instance_generation = 2U;
    item.key.event_token = 9U;
    item.timestamp_us = UINT64_C(123456);
    item.quality = 3U;
    item.length = 40U;
    for (index = 0U; index < item.length; ++index) {
        item.data[index] = (uint8_t)(index ^ 0xA5U);
    }
    TEST_ASSERT(ucn_time_timed_rx_enqueue(&queue, &item) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_time_timed_rx_queue_init(&queue, &ports, &fake) == UCN_OK);
    TEST_ASSERT(ucn_time_timed_rx_enqueue_from_isr(&queue, &item) == UCN_OK);
    (void)memset(&output, 0, sizeof(output));
    TEST_ASSERT(ucn_time_timed_rx_dequeue(&queue, &output) == UCN_OK);
    TEST_ASSERT(output.ingress_link == &ingress &&
                output.key.event_token == 9U && output.length == 40U &&
                output.timestamp_us == UINT64_C(123456) &&
                memcmp(output.data, item.data, 40U) == 0);
    item.key.link_id = 6U;
    TEST_ASSERT(ucn_time_timed_rx_enqueue(&queue, &item) ==
                UCN_ERR_ARGUMENT);
    item.key.link_id = 7U;
    queue.tail = UCN_TIME_TIMED_RX_QUEUE_DEPTH;
    (void)memset(&output, 0x5A, sizeof(output));
    {
        ucn_time_timed_rx_item_t before = output;
        TEST_ASSERT(ucn_time_timed_rx_dequeue(&queue, &output) ==
                    UCN_ERR_STATE);
        TEST_ASSERT(memcmp(&output, &before, sizeof(output)) == 0);
    }
    return true;
}

/* EN: Runs all RT-04 focused tests.
 * 中文：运行全部 RT-04 定向测试。 */
int main(void)
{
    if (!test_link_instance_and_driver_contract() || !test_no_wrap() ||
        !test_tx_event_queue() || !test_atomic_timed_rx_queue()) {
        return 1;
    }
    (void)puts("ucn timed link tests passed");
    return 0;
}
