#include "ucn/v6/ucn_v6_owner.h"

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

typedef struct fake_lock {
    bool locked;
    unsigned task_notifications;
    unsigned isr_notifications;
} fake_lock_t;

typedef struct stack_fixture {
    ucn_v6_stack_owner_t *owner;
    ucn_v6_stack_budget_t budget;
    ucn_v6_stack_phase_t phase_trace[32];
    size_t phase_count;
    unsigned phase_calls[UCN_V6_STACK_PHASE_COUNT];
    unsigned invalidation_trace[16];
    size_t invalidation_count;
    bool emit_capability_invalidation;
    ucn_v6_stack_invalidation_type_t emitted_invalidation_type;
    bool emitted;
    bool make_e2e_backlogged;
    bool make_zero_progress_backlogged;
    bool order_violation;
    bool fail_phase_enabled;
    ucn_v6_stack_phase_t fail_phase;
    ucn_v6_result_t nested_result;
} stack_fixture_t;

static void fake_lock_task(void *context)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    lock->locked = true;
}

static bool fake_try_lock_from_isr(void *context)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    if (lock->locked) return false;
    lock->locked = true;
    return true;
}

static void fake_unlock_task(void *context)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    lock->locked = false;
}

static void fake_unlock_from_isr(void *context)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    lock->locked = false;
}

static void fake_notify(void *context, bool from_isr)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    if (from_isr) {
        ++lock->isr_notifications;
    } else {
        ++lock->task_notifications;
    }
}

static ucn_v6_owner_lock_ops_t make_lock_ops(fake_lock_t *lock)
{
    ucn_v6_owner_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = lock;
    ops.lock_task = fake_lock_task;
    ops.try_lock_from_isr = fake_try_lock_from_isr;
    ops.unlock_task = fake_unlock_task;
    ops.unlock_from_isr = fake_unlock_from_isr;
    ops.notify = fake_notify;
    return ops;
}

static ucn_v6_result_t run_stack_phase(
    stack_fixture_t *fixture,
    ucn_v6_stack_phase_t phase,
    uint64_t now_us,
    uint16_t budget,
    ucn_v6_stack_phase_result_t *result)
{
    (void)now_us;
    if (fixture == NULL || result == NULL || budget == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (fixture->phase_count <
        sizeof(fixture->phase_trace) / sizeof(fixture->phase_trace[0])) {
        fixture->phase_trace[fixture->phase_count] = phase;
    }
    ++fixture->phase_count;
    ++fixture->phase_calls[(size_t)phase];
    if (phase == UCN_V6_STACK_PHASE_RX_INGRESS &&
        fixture->phase_calls[(size_t)phase] == 1U) {
        ucn_v6_stack_run_result_t nested;
        memset(&nested, 0xA5, sizeof(nested));
        fixture->nested_result = ucn_v6_stack_owner_run(
            fixture->owner, now_us, &fixture->budget, &nested);
    }
    if (phase == UCN_V6_STACK_PHASE_ROUTE_AUTHORITY &&
        fixture->emit_capability_invalidation &&
        fixture->invalidation_count == 0U) {
        fixture->order_violation = true;
    }
    if (fixture->fail_phase_enabled && phase == fixture->fail_phase) {
        return UCN_V6_ERR_TIMEOUT;
    }
    result->work_done = 1U;
    if (phase == UCN_V6_STACK_PHASE_E2E_SECURITY &&
        fixture->make_zero_progress_backlogged) {
        result->work_done = 0U;
        result->has_more = true;
        return UCN_V6_OK;
    }
    if (phase == UCN_V6_STACK_PHASE_TIMER_EXPIRY) {
        result->has_deadline = true;
        result->next_deadline_us = 500U;
    }
    if (phase == UCN_V6_STACK_PHASE_E2E_SECURITY &&
        fixture->make_e2e_backlogged) {
        result->has_more = true;
        fixture->make_e2e_backlogged = false;
    }
    if (phase == UCN_V6_STACK_PHASE_CAPABILITY &&
        fixture->emit_capability_invalidation && !fixture->emitted) {
        size_t index;
        result->has_invalidation = true;
        result->invalidation.type = fixture->emitted_invalidation_type;
        result->invalidation.link_id = 3U;
        result->invalidation.link_generation = 4U;
        result->invalidation.session.binding.realm_id = 5U;
        result->invalidation.session.binding.node_address = 6U;
        result->invalidation.session.binding.binding_generation = 7U;
        result->invalidation.session.session_generation = 8U;
        for (index = 0U;
             index < sizeof(result->invalidation.session.principal.bytes);
             ++index) {
            result->invalidation.session.principal.bytes[index] =
                (uint8_t)(0x20U + index);
        }
        if (fixture->emitted_invalidation_type >=
            UCN_V6_STACK_INVALIDATE_CAPABILITY) {
            result->invalidation.capability_generation = 9U;
        }
        if (fixture->emitted_invalidation_type ==
            UCN_V6_STACK_INVALIDATE_PATH) {
            result->invalidation.path_id = 10U;
            result->invalidation.path_generation = 11U;
        }
        fixture->emitted = true;
    }
    return UCN_V6_OK;
}

#define DEFINE_STACK_PHASE(name_, phase_)                                  \
    static ucn_v6_result_t name_(                                           \
        void *context, uint64_t now_us, uint16_t budget,                     \
        ucn_v6_stack_phase_result_t *result)                                 \
    {                                                                        \
        return run_stack_phase((stack_fixture_t *)context, phase_, now_us,   \
                               budget, result);                              \
    }

DEFINE_STACK_PHASE(stack_rx, UCN_V6_STACK_PHASE_RX_INGRESS)
DEFINE_STACK_PHASE(stack_completion, UCN_V6_STACK_PHASE_TX_COMPLETION)
DEFINE_STACK_PHASE(stack_timer, UCN_V6_STACK_PHASE_TIMER_EXPIRY)
DEFINE_STACK_PHASE(stack_persistence, UCN_V6_STACK_PHASE_PERSISTENCE)
DEFINE_STACK_PHASE(stack_hop, UCN_V6_STACK_PHASE_HOP_SECURITY)
DEFINE_STACK_PHASE(stack_e2e, UCN_V6_STACK_PHASE_E2E_SECURITY)
DEFINE_STACK_PHASE(stack_capability, UCN_V6_STACK_PHASE_CAPABILITY)
DEFINE_STACK_PHASE(stack_route, UCN_V6_STACK_PHASE_ROUTE_AUTHORITY)
#if UCN_V6_FEATURE_REALTIME_ENABLED
DEFINE_STACK_PHASE(stack_realtime, UCN_V6_STACK_PHASE_REALTIME)
#endif
DEFINE_STACK_PHASE(stack_operation, UCN_V6_STACK_PHASE_OPERATION)
DEFINE_STACK_PHASE(stack_endpoint, UCN_V6_STACK_PHASE_ENDPOINT)
#if UCN_V6_FEATURE_CLUSTER_ENABLED
DEFINE_STACK_PHASE(stack_cluster, UCN_V6_STACK_PHASE_CLUSTER)
#endif
DEFINE_STACK_PHASE(stack_qos, UCN_V6_STACK_PHASE_QOS_TX)

static ucn_v6_result_t record_invalidation(void *context,
                                           unsigned code)
{
    stack_fixture_t *fixture = (stack_fixture_t *)context;
    if (fixture->invalidation_count <
        sizeof(fixture->invalidation_trace) /
            sizeof(fixture->invalidation_trace[0])) {
        fixture->invalidation_trace[fixture->invalidation_count] = code;
    }
    ++fixture->invalidation_count;
    return UCN_V6_OK;
}

#define DEFINE_INVALIDATION(name_, code_)                                  \
    static ucn_v6_result_t name_(                                           \
        void *context, const ucn_v6_stack_invalidation_t *invalidation)      \
    {                                                                        \
        if (invalidation == NULL) return UCN_V6_ERR_ARGUMENT;                \
        return record_invalidation(context, code_);                          \
    }

#if UCN_V6_FEATURE_ADAPTER_ENABLED
DEFINE_INVALIDATION(invalidate_adapter, 1U)
#endif
DEFINE_INVALIDATION(invalidate_security, 2U)
DEFINE_INVALIDATION(invalidate_capability, 3U)
#if UCN_V6_FEATURE_REALTIME_ENABLED
DEFINE_INVALIDATION(invalidate_realtime, 4U)
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
DEFINE_INVALIDATION(invalidate_cluster, 5U)
#endif
DEFINE_INVALIDATION(invalidate_transfer, 6U)
DEFINE_INVALIDATION(invalidate_route, 7U)
DEFINE_INVALIDATION(invalidate_qos, 8U)
DEFINE_INVALIDATION(invalidate_endpoint, 9U)

static ucn_v6_stack_hooks_t make_stack_hooks(stack_fixture_t *fixture)
{
    ucn_v6_stack_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = fixture;
    hooks.rx_ingress = stack_rx;
    hooks.tx_completion = stack_completion;
    hooks.timer_expiry = stack_timer;
    hooks.persistence = stack_persistence;
    hooks.hop_security = stack_hop;
    hooks.e2e_security = stack_e2e;
    hooks.capability = stack_capability;
    hooks.route_authority = stack_route;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    hooks.realtime = stack_realtime;
    hooks.invalidate_realtime = invalidate_realtime;
#endif
    hooks.operation = stack_operation;
    hooks.endpoint = stack_endpoint;
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    hooks.cluster = stack_cluster;
    hooks.invalidate_cluster = invalidate_cluster;
#endif
    hooks.qos_tx = stack_qos;
#if UCN_V6_FEATURE_ADAPTER_ENABLED
    hooks.invalidate_adapter = invalidate_adapter;
#endif
    hooks.invalidate_security = invalidate_security;
    hooks.invalidate_capability = invalidate_capability;
    hooks.invalidate_transfer = invalidate_transfer;
    hooks.invalidate_route = invalidate_route;
    hooks.invalidate_qos = invalidate_qos;
    hooks.invalidate_endpoint = invalidate_endpoint;
    return hooks;
}

static ucn_v6_stack_budget_t make_stack_budget(void)
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

static int test_stack_owner_preflight_is_non_destructive(void)
{
    ucn_v6_stack_owner_storage_t storage;
    ucn_v6_stack_owner_storage_t before;
    stack_fixture_t fixture;
    fake_lock_t lock;
    ucn_v6_owner_lock_ops_t ops;
    ucn_v6_stack_hooks_t hooks;
    ucn_v6_stack_owner_t *owner = NULL;

    memset(&fixture, 0, sizeof(fixture));
    memset(&lock, 0, sizeof(lock));
    ops = make_lock_ops(&lock);
    hooks = make_stack_hooks(&fixture);
    hooks.hop_security = NULL;
    memset(&storage, 0xA5, sizeof(storage));
    before = storage;
    CHECK(ucn_v6_stack_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &hooks, &owner) == UCN_V6_ERR_CONFIG);
    CHECK(owner == NULL);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);
    return 0;
}

static int test_stack_budget_contract(void)
{
    ucn_v6_stack_budget_t budget = make_stack_budget();
    ucn_v6_stack_budget_t invalid;

    CHECK(ucn_v6_stack_budget_is_valid(
              ucn_v6_compiled_manifest(), &budget));
    memset(&invalid, 0, sizeof(invalid));
    CHECK(!ucn_v6_stack_budget_is_valid(
              ucn_v6_compiled_manifest(), &invalid));
    invalid = budget;
    --invalid.max_total_work;
    CHECK(!ucn_v6_stack_budget_is_valid(
              ucn_v6_compiled_manifest(), &invalid));
    invalid = budget;
    invalid.phase_work[UCN_V6_STACK_PHASE_RX_INGRESS] = 0U;
    CHECK(!ucn_v6_stack_budget_is_valid(
              ucn_v6_compiled_manifest(), &invalid));
#if !UCN_V6_FEATURE_REALTIME_ENABLED
    invalid = budget;
    invalid.phase_work[UCN_V6_STACK_PHASE_REALTIME] = 1U;
    CHECK(!ucn_v6_stack_budget_is_valid(
              ucn_v6_compiled_manifest(), &invalid));
#endif
#if !UCN_V6_FEATURE_CLUSTER_ENABLED
    invalid = budget;
    invalid.phase_work[UCN_V6_STACK_PHASE_CLUSTER] = 1U;
    CHECK(!ucn_v6_stack_budget_is_valid(
              ucn_v6_compiled_manifest(), &invalid));
#endif
    return 0;
}

static int test_stack_owner_event_coalescing_and_corruption(void)
{
    ucn_v6_stack_owner_storage_t storage;
    stack_fixture_t fixture;
    fake_lock_t lock;
    ucn_v6_owner_lock_ops_t ops;
    ucn_v6_stack_hooks_t hooks;
    ucn_v6_stack_owner_t *owner = NULL;
    ucn_v6_stack_run_result_t run_result;
    ucn_v6_stack_owner_view_t view;
    uint32_t all_events_mask =
        (UINT32_C(1) << UCN_V6_OWNER_EVENT_COUNT) - UINT32_C(1);
    unsigned index;

    memset(&fixture, 0, sizeof(fixture));
    memset(&lock, 0, sizeof(lock));
    memset(&storage, 0, sizeof(storage));
    ops = make_lock_ops(&lock);
    hooks = make_stack_hooks(&fixture);
    fixture.budget = make_stack_budget();
    CHECK(ucn_v6_stack_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &hooks, &owner) == UCN_V6_OK);
    fixture.owner = owner;

    for (index = 0U; index < 300U; ++index) {
        CHECK(ucn_v6_stack_owner_post(
                  owner, UCN_V6_OWNER_EVENT_RX,
                  (index & 1U) != 0U) == UCN_V6_OK);
    }
    CHECK(lock.task_notifications + lock.isr_notifications == 1U);
    for (index = (unsigned)UCN_V6_OWNER_EVENT_TX;
         index <= (unsigned)UCN_V6_OWNER_EVENT_PROVIDER; ++index) {
        CHECK(ucn_v6_stack_owner_post(
                  owner, (ucn_v6_owner_event_t)index, false) == UCN_V6_OK);
    }
    CHECK(lock.task_notifications + lock.isr_notifications ==
          UCN_V6_OWNER_EVENT_COUNT);
    CHECK(ucn_v6_stack_owner_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.pending_event_mask == all_events_mask);
    CHECK(ucn_v6_stack_owner_run(
              owner, 1U, &fixture.budget, &run_result) == UCN_V6_OK);
    CHECK(run_result.pending_event_mask == 0U);
    CHECK(fixture.phase_count <= UCN_V6_STACK_PHASE_COUNT);

    storage.bytes[0] ^= UINT8_C(1);
    CHECK(ucn_v6_stack_owner_post(
              owner, UCN_V6_OWNER_EVENT_RX, false) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_stack_owner_copy_view(owner, &view) == UCN_V6_ERR_ARGUMENT);
    return 0;
}

static int test_stack_owner_fixed_order_and_fanout(void)
{
    ucn_v6_stack_owner_storage_t storage;
    stack_fixture_t fixture;
    fake_lock_t lock;
    ucn_v6_owner_lock_ops_t ops;
    ucn_v6_stack_hooks_t hooks;
    ucn_v6_stack_owner_t *owner = NULL;
    ucn_v6_stack_run_result_t run_result;
    ucn_v6_stack_run_result_t sentinel;
    ucn_v6_stack_owner_view_t view;
    ucn_v6_stack_phase_t expected[UCN_V6_STACK_PHASE_COUNT];
    unsigned expected_invalidation[8];
    size_t expected_count = 0U;
    size_t invalidation_count = 0U;
    size_t phase;

    memset(&fixture, 0, sizeof(fixture));
    memset(&lock, 0, sizeof(lock));
    memset(&storage, 0, sizeof(storage));
    ops = make_lock_ops(&lock);
    hooks = make_stack_hooks(&fixture);
    fixture.budget = make_stack_budget();
    fixture.emit_capability_invalidation = true;
    fixture.emitted_invalidation_type = UCN_V6_STACK_INVALIDATE_CAPABILITY;
    fixture.make_e2e_backlogged = true;
    CHECK(ucn_v6_stack_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &hooks, &owner) == UCN_V6_OK);
    fixture.owner = owner;
    CHECK(ucn_v6_stack_owner_post(
              owner, UCN_V6_OWNER_EVENT_RX, true) == UCN_V6_OK);
    CHECK(ucn_v6_stack_owner_run(owner, 100U, &fixture.budget,
                                 &run_result) == UCN_V6_OK);
    for (phase = 0U; phase < UCN_V6_STACK_PHASE_COUNT; ++phase) {
#if !UCN_V6_FEATURE_REALTIME_ENABLED
        if (phase == UCN_V6_STACK_PHASE_REALTIME) continue;
#endif
#if !UCN_V6_FEATURE_CLUSTER_ENABLED
        if (phase == UCN_V6_STACK_PHASE_CLUSTER) continue;
#endif
        expected[expected_count++] = (ucn_v6_stack_phase_t)phase;
    }
    CHECK(fixture.phase_count == expected_count);
    for (phase = 0U; phase < expected_count; ++phase) {
        CHECK(fixture.phase_trace[phase] == expected[phase]);
    }
    expected_invalidation[invalidation_count++] = 3U;
#if UCN_V6_FEATURE_REALTIME_ENABLED
    expected_invalidation[invalidation_count++] = 4U;
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
    expected_invalidation[invalidation_count++] = 5U;
#endif
    expected_invalidation[invalidation_count++] = 6U;
    expected_invalidation[invalidation_count++] = 7U;
    expected_invalidation[invalidation_count++] = 8U;
    expected_invalidation[invalidation_count++] = 9U;
    CHECK(fixture.invalidation_count == invalidation_count);
    for (phase = 0U; phase < invalidation_count; ++phase) {
        CHECK(fixture.invalidation_trace[phase] ==
              expected_invalidation[phase]);
    }
    CHECK(!fixture.order_violation);
    CHECK(fixture.nested_result == UCN_V6_ERR_STATE);
    CHECK(run_result.work_done == expected_count && run_result.more_work &&
          run_result.has_next_deadline &&
          run_result.next_deadline_us == 500U &&
          (run_result.phases_backlogged_mask &
           (UINT32_C(1) << UCN_V6_STACK_PHASE_E2E_SECURITY)) != 0U);
    CHECK(ucn_v6_stack_owner_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.invalidations_applied == 1U && view.rerun_pending &&
          view.has_time && view.last_now_us == 100U && !view.faulted);

    /* A PATH event must still pass through Capability before every dependent.
     * It may originate in Route/Transfer and therefore cannot assume that the
     * Capability phase already removed the parent record. */
    fixture.emitted = false;
    fixture.emitted_invalidation_type = UCN_V6_STACK_INVALIDATE_PATH;
    fixture.invalidation_count = 0U;
    CHECK(ucn_v6_stack_owner_run(owner, 101U, &fixture.budget,
                                 &run_result) == UCN_V6_OK);
    CHECK(fixture.invalidation_count == invalidation_count);
    for (phase = 0U; phase < invalidation_count; ++phase) {
        CHECK(fixture.invalidation_trace[phase] ==
              expected_invalidation[phase]);
    }
    CHECK(ucn_v6_stack_owner_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.invalidations_applied == 2U && !view.faulted);

    memset(&run_result, 0x5A, sizeof(run_result));
    sentinel = run_result;
    CHECK(ucn_v6_stack_owner_run(owner, 99U, &fixture.budget,
                                 &run_result) == UCN_V6_ERR_STATE);
    CHECK(memcmp(&run_result, &sentinel, sizeof(run_result)) == 0);
    return 0;
}

static int test_stack_owner_phase_failure_fences_output(void)
{
    ucn_v6_stack_owner_storage_t storage;
    stack_fixture_t fixture;
    fake_lock_t lock;
    ucn_v6_owner_lock_ops_t ops;
    ucn_v6_stack_hooks_t hooks;
    ucn_v6_stack_owner_t *owner = NULL;
    ucn_v6_stack_run_result_t run_result;
    ucn_v6_stack_owner_view_t view;

    memset(&fixture, 0, sizeof(fixture));
    memset(&lock, 0, sizeof(lock));
    memset(&storage, 0, sizeof(storage));
    ops = make_lock_ops(&lock);
    hooks = make_stack_hooks(&fixture);
    fixture.budget = make_stack_budget();
    fixture.fail_phase_enabled = true;
    fixture.fail_phase = UCN_V6_STACK_PHASE_HOP_SECURITY;
    CHECK(ucn_v6_stack_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &hooks, &owner) == UCN_V6_OK);
    fixture.owner = owner;
    CHECK(ucn_v6_stack_owner_run(owner, 1U, &fixture.budget,
                                 &run_result) == UCN_V6_ERR_TIMEOUT);
    CHECK(fixture.phase_calls[UCN_V6_STACK_PHASE_HOP_SECURITY] == 1U);
    CHECK(fixture.phase_calls[UCN_V6_STACK_PHASE_E2E_SECURITY] == 0U);
    CHECK(fixture.phase_calls[UCN_V6_STACK_PHASE_QOS_TX] == 0U);
    CHECK(ucn_v6_stack_owner_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.faulted && view.last_error == UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_stack_owner_post(
              owner, UCN_V6_OWNER_EVENT_RX, false) == UCN_V6_ERR_STATE);
    return 0;
}

static int test_stack_owner_rejects_zero_progress_backlog(void)
{
    ucn_v6_stack_owner_storage_t storage;
    stack_fixture_t fixture;
    fake_lock_t lock;
    ucn_v6_owner_lock_ops_t ops;
    ucn_v6_stack_hooks_t hooks;
    ucn_v6_stack_owner_t *owner = NULL;
    ucn_v6_stack_run_result_t run_result;
    ucn_v6_stack_owner_view_t view;

    memset(&fixture, 0, sizeof(fixture));
    memset(&lock, 0, sizeof(lock));
    memset(&storage, 0, sizeof(storage));
    ops = make_lock_ops(&lock);
    hooks = make_stack_hooks(&fixture);
    fixture.budget = make_stack_budget();
    fixture.make_zero_progress_backlogged = true;
    CHECK(ucn_v6_stack_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &hooks, &owner) == UCN_V6_OK);
    fixture.owner = owner;
    CHECK(ucn_v6_stack_owner_run(owner, 1U, &fixture.budget,
                                 &run_result) == UCN_V6_ERR_STATE);
    CHECK(fixture.phase_calls[UCN_V6_STACK_PHASE_E2E_SECURITY] == 1U);
    CHECK(fixture.phase_calls[UCN_V6_STACK_PHASE_QOS_TX] == 0U);
    CHECK(ucn_v6_stack_owner_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.faulted && view.last_error == UCN_V6_ERR_STATE &&
          !view.rerun_pending);
    return 0;
}

int main(void)
{
    CHECK(test_stack_owner_preflight_is_non_destructive() == 0);
    CHECK(test_stack_budget_contract() == 0);
    CHECK(test_stack_owner_event_coalescing_and_corruption() == 0);
    CHECK(test_stack_owner_fixed_order_and_fanout() == 0);
    CHECK(test_stack_owner_phase_failure_fences_output() == 0);
    CHECK(test_stack_owner_rejects_zero_progress_backlog() == 0);
    puts("ucn v6 protocol owner tests passed");
    return 0;
}
