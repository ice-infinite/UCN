#include "internal/ucn_service.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression_) do { if (!(expression_)) return __LINE__; } while (0)

typedef struct test_lock { int held; } test_lock_t;
static test_lock_t lock_state;
static ucn_i_service_owner_t owner;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0) return UCN_ERR_STATE;
    lock->held = 1;
    return UCN_OK;
}
static void lock_leave(void *context) { ((test_lock_t *)context)->held = 0; }

static int configure(void)
{
    ucn_i_service_config_t config;
    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&config, 0, sizeof(config));
    config.receipt_lifetime_us = 1000U;
    config.runtime_instance = 3U;
    config.owner_instance = 4U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &lock_state;
    config.state_lock.enter = lock_enter;
    config.state_lock.leave = lock_leave;
    return ucn_i_service_owner_init(&owner, &config) == UCN_OK ? 0 : __LINE__;
}

static ucn_i_service_qos_item_t item(uint8_t traffic_class, uint16_t slot)
{
    ucn_i_service_qos_item_t value;
    memset(&value, 0, sizeof(value));
    value.sendable.runtime_instance = 9U;
    value.sendable.owner_instance = 10U;
    value.sendable.slot = slot;
    value.sendable.generation = 1U;
    value.sendable.object_kind = UCN_OBJECT_KIND_SEND;
    value.source_quota_key = 1U + slot;
    value.flow_quota_key = 100U + slot;
    value.traffic_class = traffic_class;
    value.control_authorized = traffic_class == 0U ? 1U : 0U;
    return value;
}

static int test_q0_authority_and_latest(void)
{
    ucn_i_service_qos_item_t q0 = item(0U, 1U);
    ucn_i_service_qos_item_t latest_a = item(1U, 2U);
    ucn_i_service_qos_item_t latest_b = item(1U, 3U);
    ucn_handle_t qh;
    ucn_handle_t replaced;
    ucn_i_service_qos_view_t view;

    q0.control_authorized = 0U;
    CHECK(ucn_i_service_qos_enqueue(&owner, &q0, &qh, &replaced) == UCN_ERR_ARGUMENT);
    latest_a.latest = 1U;
    latest_b.latest = 1U;
    memset(latest_a.latest_key, 0xA5, sizeof(latest_a.latest_key));
    memcpy(latest_b.latest_key, latest_a.latest_key, sizeof(latest_b.latest_key));
    latest_b.source_quota_key = latest_a.source_quota_key;
    latest_b.flow_quota_key = latest_a.flow_quota_key;
    CHECK(ucn_i_service_qos_enqueue(&owner, &latest_a, &qh, &replaced) == UCN_OK);
    CHECK(replaced.object_kind == 0U);
    CHECK(ucn_i_service_qos_enqueue(&owner, &latest_b, &qh, &replaced) == UCN_OK);
    CHECK(replaced.slot == latest_a.sendable.slot);
    CHECK(ucn_i_service_qos_view(&owner, qh, &view) == UCN_OK);
    CHECK(view.item.sendable.slot == latest_b.sendable.slot);
    CHECK(ucn_i_service_qos_remove(&owner, qh) == UCN_OK);
    return 0;
}

static int test_weighted_finite_progress(void)
{
    ucn_handle_t queue_handles[4];
    ucn_handle_t replaced;
    ucn_handle_t picked;
    ucn_handle_t sendable;
    uint8_t seen = 0U;
    uint8_t traffic_class;
    unsigned iteration;

    for (traffic_class = 0U; traffic_class < 4U; ++traffic_class) {
        ucn_i_service_qos_item_t value = item(traffic_class,
                                              (uint16_t)(10U + traffic_class));
        CHECK(ucn_i_service_qos_enqueue(&owner, &value,
                                        &queue_handles[traffic_class],
                                        &replaced) == UCN_OK);
    }
    for (iteration = 0U; iteration < 12U; ++iteration) {
        CHECK(ucn_i_service_qos_pick(&owner, 0U, &picked, &sendable) == UCN_OK);
        seen = (uint8_t)(seen | (uint8_t)(1U << (sendable.slot - 10U)));
        CHECK(ucn_i_service_qos_release_pick(&owner, picked) == UCN_OK);
    }
    CHECK(seen == 0x0FU);
    for (traffic_class = 0U; traffic_class < 4U; ++traffic_class) {
        CHECK(ucn_i_service_qos_remove(&owner,
                                       queue_handles[traffic_class]) == UCN_OK);
    }
    return 0;
}

static int test_selected_latest_and_deadline_protection(void)
{
    ucn_i_service_qos_item_t first = item(1U, 30U);
    ucn_i_service_qos_item_t newer = item(1U, 31U);
    ucn_handle_t first_queue;
    ucn_handle_t newer_queue;
    ucn_handle_t replaced;
    ucn_handle_t picked;
    ucn_handle_t sendable;
    ucn_i_service_qos_view_t view;
    uint16_t inspected;
    uint16_t changed;
    ucn_result_t enqueue_result;

    first.latest = 1U;
    first.deadline_us = 10U;
    memset(first.latest_key, 0x3C, sizeof(first.latest_key));
    newer.latest = 1U;
    newer.deadline_us = 20U;
    memcpy(newer.latest_key, first.latest_key, sizeof(newer.latest_key));
    newer.source_quota_key = first.source_quota_key;
    newer.flow_quota_key = first.flow_quota_key;

    CHECK(ucn_i_service_qos_enqueue(&owner, &first, &first_queue,
                                    &replaced) == UCN_OK);
    CHECK(ucn_i_service_qos_pick(&owner, 0U, &picked, &sendable) == UCN_OK);
    CHECK(memcmp(&picked, &first_queue, sizeof(picked)) == 0);
    CHECK(sendable.slot == first.sendable.slot);

    enqueue_result = ucn_i_service_qos_enqueue(&owner, &newer, &newer_queue,
                                                &replaced);
    CHECK(enqueue_result == UCN_OK || enqueue_result == UCN_ERR_NO_SPACE);
    if (enqueue_result == UCN_OK) {
        CHECK(replaced.object_kind == 0U);
    }
    CHECK(ucn_i_service_qos_view(&owner, first_queue, &view) == UCN_OK);
    CHECK(view.selected == 1U);
    CHECK(view.item.sendable.slot == first.sendable.slot);

    CHECK(ucn_i_service_maintain(&owner, 10U, UINT16_MAX,
                                 &inspected, &changed) == UCN_OK);
    CHECK(inspected != 0U);
    CHECK(ucn_i_service_qos_view(&owner, first_queue, &view) == UCN_OK);
    CHECK(view.selected == 1U);
    CHECK(ucn_i_service_qos_release_pick(&owner, first_queue) == UCN_OK);
    CHECK(ucn_i_service_maintain(&owner, 10U, UINT16_MAX,
                                 &inspected, &changed) == UCN_OK);
    CHECK(ucn_i_service_qos_view(&owner, first_queue, &view) ==
          UCN_ERR_NOT_FOUND);
    if (enqueue_result == UCN_OK) {
        CHECK(ucn_i_service_qos_remove(&owner, newer_queue) == UCN_OK);
    }
    return 0;
}

static int test_source_and_flow_quota_fail_closed(void)
{
    ucn_handle_t queues[UCN_I_SERVICE_SOURCE_QUOTA + 1U];
    ucn_handle_t replaced;
    ucn_i_service_qos_view_t view;
    size_t index;

    for (index = 0U; index < UCN_I_SERVICE_SOURCE_QUOTA; ++index) {
        ucn_i_service_qos_item_t value = item(2U, (uint16_t)(40U + index));
        value.source_quota_key = UINT32_C(0x1111);
        value.flow_quota_key = (uint32_t)(UINT32_C(0x2200) + index);
        CHECK(ucn_i_service_qos_enqueue(&owner, &value, &queues[index],
                                        &replaced) == UCN_OK);
    }
    {
        ucn_i_service_qos_item_t overflow =
            item(2U, (uint16_t)(40U + UCN_I_SERVICE_SOURCE_QUOTA));
        ucn_handle_t output_sentinel;
        ucn_handle_t replaced_sentinel;

        overflow.source_quota_key = UINT32_C(0x1111);
        overflow.flow_quota_key = UINT32_C(0x3300);
        memset(&output_sentinel, 0xA5, sizeof(output_sentinel));
        memset(&replaced_sentinel, 0x5A, sizeof(replaced_sentinel));
        queues[UCN_I_SERVICE_SOURCE_QUOTA] = output_sentinel;
        replaced = replaced_sentinel;
        CHECK(ucn_i_service_qos_enqueue(
                  &owner, &overflow,
                  &queues[UCN_I_SERVICE_SOURCE_QUOTA], &replaced) ==
              UCN_ERR_NO_SPACE);
        CHECK(memcmp(&queues[UCN_I_SERVICE_SOURCE_QUOTA], &output_sentinel,
                     sizeof(output_sentinel)) == 0);
        CHECK(memcmp(&replaced, &replaced_sentinel,
                     sizeof(replaced_sentinel)) == 0);
    }
    for (index = 0U; index < UCN_I_SERVICE_SOURCE_QUOTA; ++index) {
        CHECK(ucn_i_service_qos_view(&owner, queues[index], &view) == UCN_OK);
        CHECK(view.item.sendable.slot == (uint16_t)(40U + index));
        CHECK(ucn_i_service_qos_remove(&owner, queues[index]) == UCN_OK);
    }

    for (index = 0U; index < UCN_I_SERVICE_FLOW_QUOTA; ++index) {
        ucn_i_service_qos_item_t value = item(3U, (uint16_t)(60U + index));
        value.source_quota_key = (uint32_t)(UINT32_C(0x4400) + index);
        value.flow_quota_key = UINT32_C(0x5555);
        CHECK(ucn_i_service_qos_enqueue(&owner, &value, &queues[index],
                                        &replaced) == UCN_OK);
    }
    {
        ucn_i_service_qos_item_t overflow =
            item(3U, (uint16_t)(60U + UCN_I_SERVICE_FLOW_QUOTA));
        ucn_handle_t output_sentinel;
        ucn_handle_t replaced_sentinel;

        overflow.source_quota_key = UINT32_C(0x6666);
        overflow.flow_quota_key = UINT32_C(0x5555);
        memset(&output_sentinel, 0xA5, sizeof(output_sentinel));
        memset(&replaced_sentinel, 0x5A, sizeof(replaced_sentinel));
        queues[UCN_I_SERVICE_FLOW_QUOTA] = output_sentinel;
        replaced = replaced_sentinel;
        CHECK(ucn_i_service_qos_enqueue(
                  &owner, &overflow,
                  &queues[UCN_I_SERVICE_FLOW_QUOTA], &replaced) ==
              UCN_ERR_NO_SPACE);
        CHECK(memcmp(&queues[UCN_I_SERVICE_FLOW_QUOTA], &output_sentinel,
                     sizeof(output_sentinel)) == 0);
        CHECK(memcmp(&replaced, &replaced_sentinel,
                     sizeof(replaced_sentinel)) == 0);
    }
    for (index = 0U; index < UCN_I_SERVICE_FLOW_QUOTA; ++index) {
        CHECK(ucn_i_service_qos_view(&owner, queues[index], &view) == UCN_OK);
        CHECK(view.item.sendable.slot == (uint16_t)(60U + index));
        CHECK(ucn_i_service_qos_remove(&owner, queues[index]) == UCN_OK);
    }
    return 0;
}

int main(void)
{
    int result = configure();
    if (result == 0) result = test_q0_authority_and_latest();
    if (result == 0) result = test_weighted_finite_progress();
    if (result == 0) result = test_selected_latest_and_deadline_protection();
    if (result == 0) result = test_source_and_flow_quota_fail_closed();
    if (result == 0 && ucn_i_service_owner_destroy(&owner) != UCN_OK) result = __LINE__;
    if (result != 0) return result;
    puts("qos tests passed");
    return 0;
}
