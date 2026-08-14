#include <string.h>

#include "test_support.h"
#include "ucn/ports/ucn_event_runtime.h"

typedef struct event_runtime_fake {
    uint32_t now_ms;
    uint32_t task_enters;
    uint32_t task_exits;
    uint32_t isr_enters;
    uint32_t isr_exits;
    ucn_port_critical_token_t next_token;
    ucn_port_critical_token_t last_enter_token;
    ucn_port_critical_token_t last_exit_token;
    uint32_t notifications;
    uint32_t notifications_from_isr;
    uint32_t waits;
    uint32_t last_wait_ms;
    uint32_t yields;
    bool wait_notified;
} event_runtime_fake_t;

typedef struct event_source_fake {
    size_t remaining_work;
    uint32_t calls;
    ucn_event_source_events_t observed_events;
    ucn_event_runtime_t *signal_runtime;
    ucn_event_source_id_t signal_source_id;
    ucn_event_source_events_t signal_events;
    ucn_result_t signal_result;
    bool signal_once;
    bool malformed_result;
    ucn_result_t forced_result;
} event_source_fake_t;

static uint32_t event_now_ms(void *context)
{
    return ((const event_runtime_fake_t *)context)->now_ms;
}

static void event_enter_task(void *context)
{
    ((event_runtime_fake_t *)context)->task_enters++;
}

static void event_exit_task(void *context)
{
    ((event_runtime_fake_t *)context)->task_exits++;
}

static ucn_port_critical_token_t event_enter_isr(void *context)
{
    event_runtime_fake_t *fake = (event_runtime_fake_t *)context;

    fake->isr_enters++;
    fake->next_token++;
    fake->last_enter_token = fake->next_token;
    return fake->last_enter_token;
}

static void event_exit_isr(void *context, ucn_port_critical_token_t token)
{
    event_runtime_fake_t *fake = (event_runtime_fake_t *)context;

    fake->isr_exits++;
    fake->last_exit_token = token;
}

static void event_notify(void *context, bool from_isr)
{
    event_runtime_fake_t *fake = (event_runtime_fake_t *)context;

    fake->notifications++;
    if (from_isr) {
        fake->notifications_from_isr++;
    }
}

static bool event_wait(void *context, uint32_t max_wait_ms)
{
    event_runtime_fake_t *fake = (event_runtime_fake_t *)context;

    fake->waits++;
    fake->last_wait_ms = max_wait_ms;
    return fake->wait_notified;
}

static void event_yield(void *context)
{
    ((event_runtime_fake_t *)context)->yields++;
}

static ucn_result_t event_source_service(
    void *context,
    ucn_event_source_events_t events,
    size_t max_work,
    ucn_event_source_service_result_t *result)
{
    event_source_fake_t *source = (event_source_fake_t *)context;
    size_t work;

    source->calls++;
    source->observed_events |= events;
    if (source->signal_once) {
        source->signal_once = false;
        source->signal_result = ucn_event_runtime_signal_source(
            source->signal_runtime, source->signal_source_id,
            source->signal_events);
    }
    if (source->malformed_result) {
        result->work_done = max_work + 1U;
        result->pending_events = UCN_EVENT_SOURCE_FALLBACK_SCAN;
        return UCN_OK;
    }
    work = source->remaining_work < max_work ? source->remaining_work : max_work;
    source->remaining_work -= work;
    result->work_done = work;
    result->pending_events = source->remaining_work == 0U ? 0U :
        UCN_EVENT_SOURCE_RX_READY;
    return source->forced_result;
}

static const ucn_port_ops_t EVENT_PORT_OPS = {
    .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
    .api_version = UCN_PORT_OPS_API_VERSION,
    .now_ms = event_now_ms,
    .enter_critical = event_enter_task,
    .exit_critical = event_exit_task,
    .enter_critical_from_isr = event_enter_isr,
    .exit_critical_from_isr = event_exit_isr
};

static const ucn_port_ops_t EVENT_PORT_OPS_NO_ISR = {
    .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
    .api_version = UCN_PORT_OPS_API_VERSION,
    .now_ms = event_now_ms,
    .enter_critical = event_enter_task,
    .exit_critical = event_exit_task
};

static const ucn_event_runtime_scheduler_ops_t EVENT_SCHEDULER_OPS = {
    event_notify, event_wait, event_yield
};

static const ucn_event_source_ops_t EVENT_SOURCE_OPS = {
    event_source_service
};

static ucn_result_t event_fixture_init(
    ucn_event_runtime_t *runtime,
    ucn_node_t *node,
    ucn_adapter_rx_queue_t *queue,
    event_runtime_fake_t *fake,
    const ucn_port_ops_t *port_ops,
    const ucn_event_runtime_scheduler_ops_t *scheduler_ops,
    uint8_t max_rounds,
    size_t source_budget)
{
    const ucn_config_t node_config = {
        UINT32_C(0x45564E54), UINT32_C(1), 4U
    };
    ucn_event_runtime_config_t config;
    ucn_result_t result;

    (void)memset(runtime, 0, sizeof(*runtime));
    (void)memset(node, 0, sizeof(*node));
    (void)memset(queue, 0, sizeof(*queue));
    result = ucn_node_init(node, &node_config);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_adapter_rx_queue_init(queue, port_ops, fake);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(&config, 0, sizeof(config));
    config.owner.node = node;
    config.owner.rx_queue = queue;
    config.owner.port_ops = port_ops;
    config.owner.port_context = fake;
    config.owner.max_rx_frames_per_step = 1U;
#if UCN_FEATURE_SERVICE
    config.owner.max_bridge_requests_per_step = 1U;
#endif
    config.scheduler_ops = scheduler_ops;
    config.scheduler_context = fake;
    config.max_drain_rounds = max_rounds;
    config.max_source_work_per_round = source_budget;
    return ucn_event_runtime_init(runtime, &config);
}

static int test_event_runtime_multi_source(void)
{
    ucn_event_runtime_t runtime;
    ucn_node_t node;
    ucn_adapter_rx_queue_t queue;
    event_runtime_fake_t fake;
    event_source_fake_t uart;
    event_source_fake_t can;
    const ucn_event_source_config_t uart_config = {
        &EVENT_SOURCE_OPS, &uart
    };
    const ucn_event_source_config_t can_config = {
        &EVENT_SOURCE_OPS, &can
    };
    ucn_event_runtime_run_result_t run;
    const ucn_event_runtime_stats_t *stats;
    uint32_t uart_calls_before;
    uint32_t can_calls_before;

    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&uart, 0, sizeof(uart));
    (void)memset(&can, 0, sizeof(can));
    uart.remaining_work = 5U;
    can.remaining_work = 1U;
    TEST_ASSERT(event_fixture_init(&runtime, &node, &queue, &fake,
                                   &EVENT_PORT_OPS, &EVENT_SCHEDULER_OPS,
                                   2U, 2U) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_bind_source(&runtime, 0U, &uart_config) ==
                UCN_OK);
    TEST_ASSERT(ucn_event_runtime_bind_source(&runtime, 1U, &can_config) ==
                UCN_OK);
    TEST_ASSERT(ucn_event_runtime_bind_source(&runtime, 0U, &uart_config) ==
                UCN_OK);
    TEST_ASSERT(ucn_event_runtime_bind_source(&runtime, 0U, &can_config) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 0U, UCN_EVENT_SOURCE_RX_READY) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 0U, UCN_EVENT_SOURCE_RX_READY) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_signal_source_from_isr(
                    &runtime, 1U,
                    UCN_EVENT_SOURCE_RX_READY |
                        UCN_EVENT_SOURCE_STATUS_CHANGED) == UCN_OK);
    TEST_ASSERT(fake.notifications == 3U && fake.notifications_from_isr == 1U);
    TEST_ASSERT(ucn_event_runtime_has_pending(&runtime));
    TEST_ASSERT(ucn_event_runtime_run(&runtime, false, &run) == UCN_OK);
    TEST_ASSERT(run.rounds == 2U && run.source_work == 5U &&
                run.work_remaining);
    TEST_ASSERT(uart.calls == 2U && can.calls == 1U &&
                uart.remaining_work == 1U && can.remaining_work == 0U);
    TEST_ASSERT(fake.yields == 1U);
    TEST_ASSERT(ucn_event_runtime_run(&runtime, false, &run) == UCN_OK);
    TEST_ASSERT(run.source_work == 1U && !run.work_remaining &&
                uart.remaining_work == 0U);
    TEST_ASSERT(!ucn_event_runtime_has_pending(&runtime));

    uart_calls_before = uart.calls;
    can_calls_before = can.calls;
    fake.wait_notified = false;
    TEST_ASSERT(ucn_event_runtime_task_cycle(
                    &runtime, UCN_MAX_STEP_INTERVAL_MS + 100U, &run) == UCN_OK);
    TEST_ASSERT(run.fallback_scan && fake.last_wait_ms ==
                UCN_MAX_STEP_INTERVAL_MS);
    TEST_ASSERT(uart.calls == uart_calls_before + 1U &&
                can.calls == can_calls_before + 1U);
    TEST_ASSERT((uart.observed_events & UCN_EVENT_SOURCE_FALLBACK_SCAN) != 0U &&
                (can.observed_events & UCN_EVENT_SOURCE_FALLBACK_SCAN) != 0U);

    fake.wait_notified = true;
    TEST_ASSERT(ucn_event_runtime_task_cycle(&runtime, 1U, &run) == UCN_OK);
    TEST_ASSERT(!run.fallback_scan && run.rounds == 0U);
    stats = ucn_event_runtime_get_stats(&runtime);
    TEST_ASSERT(stats != NULL && stats->source_signals == 3U &&
                stats->source_signals_from_isr == 1U &&
                stats->drain_budget_hits == 1U && stats->fallback_scans == 1U &&
                stats->waits == 2U && stats->wait_timeouts == 1U);
    TEST_ASSERT(fake.task_enters == fake.task_exits &&
                fake.isr_enters == fake.isr_exits &&
                fake.last_enter_token == fake.last_exit_token);
    return 0;
}

static int test_event_runtime_concurrent_signal(void)
{
    ucn_event_runtime_t runtime;
    ucn_node_t node;
    ucn_adapter_rx_queue_t queue;
    event_runtime_fake_t fake;
    event_source_fake_t first;
    event_source_fake_t second;
    ucn_event_source_config_t first_config = { &EVENT_SOURCE_OPS, &first };
    ucn_event_source_config_t second_config = { &EVENT_SOURCE_OPS, &second };
    ucn_event_runtime_run_result_t run;

    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&first, 0, sizeof(first));
    (void)memset(&second, 0, sizeof(second));
    first.remaining_work = 1U;
    second.remaining_work = 1U;
    TEST_ASSERT(event_fixture_init(&runtime, &node, &queue, &fake,
                                   &EVENT_PORT_OPS, &EVENT_SCHEDULER_OPS,
                                   4U, 1U) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_bind_source(&runtime, 0U, &first_config) ==
                UCN_OK);
    TEST_ASSERT(ucn_event_runtime_bind_source(&runtime, 1U, &second_config) ==
                UCN_OK);
    /* Source 0 has already been passed when Source 1 posts this event.  The
     * pending bit must survive and run in the next drain round. */
    second.signal_runtime = &runtime;
    second.signal_source_id = 0U;
    second.signal_events = UCN_EVENT_SOURCE_RX_READY;
    second.signal_once = true;
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 1U, UCN_EVENT_SOURCE_RX_READY) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_run(&runtime, false, &run) == UCN_OK);
    TEST_ASSERT(second.signal_result == UCN_OK && first.calls == 1U &&
                second.calls == 1U && run.source_work == 2U &&
                !run.work_remaining);
    TEST_ASSERT(fake.notifications == 2U);
    return 0;
}

static int test_event_runtime_direct_frame_and_isr_gate(void)
{
    ucn_event_runtime_t runtime;
    ucn_node_t node;
    ucn_adapter_rx_queue_t queue;
    ucn_link_t link;
    event_runtime_fake_t fake;
    const uint8_t malformed[] = { 0U };
    const ucn_event_runtime_stats_t *stats;
    size_t index;

    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&link, 0, sizeof(link));
    TEST_ASSERT(event_fixture_init(&runtime, &node, &queue, &fake,
                                   &EVENT_PORT_OPS, &EVENT_SCHEDULER_OPS,
                                   2U, 1U) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_submit_frame(
                    &runtime, &link, malformed, sizeof(malformed)) == UCN_OK);
#if UCN_ADAPTER_RX_QUEUE_DEPTH >= 2
    TEST_ASSERT(ucn_event_runtime_submit_frame_from_isr(
                    &runtime, &link, malformed, sizeof(malformed)) == UCN_OK);
    for (index = 2U; index < UCN_ADAPTER_RX_QUEUE_DEPTH; ++index) {
        TEST_ASSERT(ucn_event_runtime_submit_frame(
                        &runtime, &link, malformed, sizeof(malformed)) ==
                    UCN_OK);
    }
    TEST_ASSERT(ucn_event_runtime_submit_frame(
                    &runtime, &link, malformed, sizeof(malformed)) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(fake.notifications == UCN_ADAPTER_RX_QUEUE_DEPTH &&
                fake.notifications_from_isr == 1U);
    stats = ucn_event_runtime_get_stats(&runtime);
    TEST_ASSERT(stats->frames_submitted == UCN_ADAPTER_RX_QUEUE_DEPTH &&
                stats->frames_rejected == 1U);
#else
    TEST_ASSERT(ucn_event_runtime_submit_frame_from_isr(
                    &runtime, &link, malformed, sizeof(malformed)) ==
                UCN_ERR_NO_SPACE);
    stats = ucn_event_runtime_get_stats(&runtime);
    TEST_ASSERT(stats->frames_submitted == 1U && stats->frames_rejected == 1U);
#endif

    (void)memset(&fake, 0, sizeof(fake));
    TEST_ASSERT(event_fixture_init(&runtime, &node, &queue, &fake,
                                   &EVENT_PORT_OPS_NO_ISR,
                                   &EVENT_SCHEDULER_OPS, 2U, 1U) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_signal_owner_from_isr(
                    &runtime, UCN_EVENT_OWNER_TIMER) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_event_runtime_submit_frame_from_isr(
                    &runtime, &link, malformed, sizeof(malformed)) ==
                UCN_ERR_CONFIG);
    return 0;
}

static int test_event_runtime_bare_metal_and_failures(void)
{
    ucn_event_runtime_t runtime;
    ucn_node_t node;
    ucn_adapter_rx_queue_t queue;
    event_runtime_fake_t fake;
    event_source_fake_t source;
    event_source_fake_t other;
    const ucn_event_source_config_t source_config = {
        &EVENT_SOURCE_OPS, &source
    };
    const ucn_event_source_config_t other_config = {
        &EVENT_SOURCE_OPS, &other
    };
    ucn_event_runtime_run_result_t run;

    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&source, 0, sizeof(source));
    (void)memset(&other, 0, sizeof(other));
    source.remaining_work = 1U;
    TEST_ASSERT(event_fixture_init(&runtime, &node, &queue, &fake,
                                   &EVENT_PORT_OPS, NULL, 1U, 1U) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_bind_source(&runtime, 0U, &source_config) ==
                UCN_OK);
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 0U, UCN_EVENT_SOURCE_RX_READY) == UCN_OK);
    TEST_ASSERT(fake.notifications == 0U &&
                ucn_event_runtime_has_pending(&runtime));
    TEST_ASSERT(ucn_event_runtime_run(&runtime, false, &run) == UCN_OK);
    TEST_ASSERT(run.source_work == 1U && !run.work_remaining);
    TEST_ASSERT(ucn_event_runtime_task_cycle(&runtime, 100U, &run) == UCN_OK);
    TEST_ASSERT(run.fallback_scan && source.calls >= 2U);

    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, UCN_EVENT_SOURCE_INVALID,
                    UCN_EVENT_SOURCE_RX_READY) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 0U, 0U) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 0U, UCN_EVENT_SOURCE_FALLBACK_SCAN) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_event_runtime_signal_owner(&runtime, 0U) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 1U, UCN_EVENT_SOURCE_RX_READY) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_event_runtime_bind_source(
                    &runtime, UCN_EVENT_SOURCE_INVALID, &other_config) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_event_runtime_bind_source(
                    &runtime,
                    (ucn_event_source_id_t)UCN_EVENT_RUNTIME_MAX_SOURCES,
                    &other_config) == UCN_ERR_ARGUMENT);

    source.malformed_result = true;
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 0U, UCN_EVENT_SOURCE_RX_READY) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_run(&runtime, false, &run) ==
                UCN_ERR_MALFORMED);
    source.malformed_result = false;
    source.forced_result = UCN_ERR_LINK_DOWN;
    TEST_ASSERT(ucn_event_runtime_signal_source(
                    &runtime, 0U, UCN_EVENT_SOURCE_STATUS_CHANGED) == UCN_OK);
    TEST_ASSERT(ucn_event_runtime_run(&runtime, false, &run) ==
                UCN_ERR_LINK_DOWN);
    return 0;
}

int test_event_runtime(void)
{
    ucn_event_runtime_t runtime;
    ucn_event_runtime_config_t invalid;
    ucn_event_runtime_scheduler_ops_t invalid_scheduler = EVENT_SCHEDULER_OPS;

    (void)memset(&runtime, 0, sizeof(runtime));
    (void)memset(&invalid, 0, sizeof(invalid));
    TEST_ASSERT(ucn_event_runtime_init(NULL, &invalid) == UCN_ERR_ARGUMENT);
    invalid.scheduler_ops = &invalid_scheduler;
    invalid_scheduler.wait_owner = NULL;
    TEST_ASSERT(ucn_event_runtime_init(&runtime, &invalid) == UCN_ERR_ARGUMENT);

    TEST_ASSERT(test_event_runtime_multi_source() == 0);
    TEST_ASSERT(test_event_runtime_concurrent_signal() == 0);
    TEST_ASSERT(test_event_runtime_direct_frame_and_isr_gate() == 0);
    TEST_ASSERT(test_event_runtime_bare_metal_and_failures() == 0);
    return 0;
}
