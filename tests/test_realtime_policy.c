#include "ucn/ucn_realtime_policy.h"

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

/* EN: Returns the canonical REQUIRED Deadline policy used by tests.
 * 中文：返回测试使用的规范 REQUIRED Deadline 策略。 */
static ucn_realtime_policy_t deadline_policy(void)
{
    ucn_realtime_policy_t policy;

    (void)memset(&policy, 0, sizeof(policy));
    policy.mode = UCN_REALTIME_MODE_DEADLINE;
    policy.requirement = UCN_REALTIME_REQUIREMENT_REQUIRED;
    policy.clock_domain_id = 10U;
    policy.max_age_us = UINT64_C(5000);
    policy.max_uncertainty_us = 100U;
    policy.require_sample_hardware_capture = true;
    policy.require_e2e_protection = true;
    policy.command_guard_present = true;
    return policy;
}

/* EN: Returns one valid point-in-time shared clock view.
 * 中文：返回一个合法的共享时钟即时视图。 */
static ucn_realtime_clock_view_t clock_view(uint64_t now_us,
                                            uint32_t uncertainty_us)
{
    ucn_realtime_clock_view_t clock;

    (void)memset(&clock, 0, sizeof(clock));
    clock.available = true;
    clock.uncertainty_known = true;
    clock.clock_domain_id = 10U;
    clock.domain_generation = 7U;
    clock.domain_time_us = now_us;
    clock.uncertainty_us = uncertainty_us;
    return clock;
}

/* EN: Builds a guarded Deadline payload for receive-gate tests.
 * 中文：为接收门禁测试构造带 Guard 的 Deadline Payload。 */
static bool build_deadline_payload(uint64_t capture_us,
                                   uint32_t source_uncertainty_us,
                                   uint8_t output[64],
                                   size_t *output_length)
{
    const uint8_t business[] = {0x11U, 0x22U, 0x33U, 0x44U};
    ucn_realtime_policy_t policy = deadline_policy();
    ucn_realtime_clock_view_t source =
        clock_view(capture_us, source_uncertainty_us);
    ucn_service_command_guard_t guard;
    ucn_realtime_send_request_t request;
    ucn_realtime_send_result_t result;

    (void)memset(&guard, 0, sizeof(guard));
    guard.command_id = 9U;
    guard.issued_at_ms = (uint32_t)(capture_us / UINT64_C(1000));
    guard.valid_for_ms = 5U;
    guard.result_endpoint = 0x81U;

    (void)memset(&request, 0, sizeof(request));
    request.capture_time_us = capture_us;
    request.clock = &source;
    request.command_guard = &guard;
    request.business_payload = business;
    request.business_length = sizeof(business);
    request.sample_capture_bound_us = 1U;
    request.sample_capture_hardware = true;
    request.sample_capture_bound_known = true;
    request.e2e_protected = true;
    if (ucn_realtime_payload_prepare(&policy, &request, output, 64U,
                                     &result) != UCN_OK) {
        return false;
    }
    *output_length = result.payload_length;
    return result.business_offset == 28U && result.payload_length == 32U;
}

/* EN: Verifies canonical policy forms and per-Endpoint coexistence.
 * 中文：验证规范策略组合以及同节点多 Endpoint 共存。 */
static bool test_policy_registry_and_mixed_modes(void)
{
    ucn_realtime_policy_registry_t registry;
    ucn_realtime_policy_t none;
    ucn_realtime_policy_t local;
    ucn_realtime_policy_t synced;
    ucn_realtime_policy_t deadline = deadline_policy();
    const ucn_realtime_policy_t *found;

    (void)memset(&none, 0, sizeof(none));
    (void)memset(&local, 0, sizeof(local));
    local.mode = UCN_REALTIME_MODE_LOCAL_STAMP;
    local.requirement = UCN_REALTIME_REQUIREMENT_PREFERRED;
    (void)memset(&synced, 0, sizeof(synced));
    synced.mode = UCN_REALTIME_MODE_SYNCED_STAMP;
    synced.requirement = UCN_REALTIME_REQUIREMENT_REQUIRED;
    synced.clock_domain_id = 10U;
    synced.max_uncertainty_us = 100U;
    synced.require_e2e_protection = true;

    TEST_ASSERT(ucn_realtime_policy_is_valid(&none));
    TEST_ASSERT(ucn_realtime_policy_is_valid(&local));
    TEST_ASSERT(ucn_realtime_policy_is_valid(&synced));
    TEST_ASSERT(ucn_realtime_policy_is_valid(&deadline));
    synced.max_uncertainty_us = 0U;
    TEST_ASSERT(!ucn_realtime_policy_is_valid(&synced));
    TEST_ASSERT(ucn_realtime_policy_registry_init(&registry) == UCN_OK);
    TEST_ASSERT(ucn_realtime_policy_registry_set(&registry, 0x80U, &none) ==
                UCN_OK);
    TEST_ASSERT(ucn_realtime_policy_registry_set(&registry, 0x81U, &local) ==
                UCN_OK);
    synced.max_uncertainty_us = 100U;
    TEST_ASSERT(ucn_realtime_policy_registry_set(&registry, 0x82U, &synced) ==
                UCN_OK);
    TEST_ASSERT(ucn_realtime_policy_registry_set(&registry, 0x83U,
                                                 &deadline) == UCN_OK);
    found = ucn_realtime_policy_registry_find(&registry, 0x83U);
    TEST_ASSERT(found != NULL && found->mode == UCN_REALTIME_MODE_DEADLINE);
    TEST_ASSERT(ucn_realtime_policy_registry_find(&registry, 0x90U) == NULL);
    return true;
}

/* EN: Verifies exact prefix ordering and complete no-write failure.
 * 中文：验证精确前缀顺序与完整的失败不写回。 */
static bool test_payload_prepare_and_no_downgrade(void)
{
    static const uint8_t expected_guard[UCN_SERVICE_COMMAND_GUARD_BYTES] = {
        0x00U, 0x00U, 0x00U, 0x09U,
        0x00U, 0x00U, 0x03U, 0xE8U,
        0x00U, 0x05U, 0x81U, 0x00U
    };
    uint8_t output[64];
    uint8_t before[64];
    size_t length;
    ucn_realtime_policy_t policy = deadline_policy();
    ucn_realtime_clock_view_t source = clock_view(UINT64_C(1000000), 40U);
    ucn_service_command_guard_t guard;
    ucn_realtime_send_request_t request;
    ucn_realtime_send_result_t result;
    ucn_realtime_send_result_t result_before;
    ucn_realtime_envelope_t envelope;
    const uint8_t business[] = {1U, 2U, 3U, 4U};
#if UCN_FEATURE_SERVICE
    uint8_t service_guard[UCN_SERVICE_COMMAND_GUARD_BYTES];
#endif

    TEST_ASSERT(build_deadline_payload(UINT64_C(1000000), 40U,
                                       output, &length));
    TEST_ASSERT(length == 32U);
    TEST_ASSERT(output[0] == 0x13U);
    TEST_ASSERT(ucn_realtime_envelope_decode(
                    output, UCN_REALTIME_ENVELOPE_WIRE_BYTES,
                    &envelope) == UCN_OK);
    TEST_ASSERT(envelope.uncertainty_class == 6U);
    TEST_ASSERT(memcmp(&output[16], expected_guard,
                       sizeof(expected_guard)) == 0);
    TEST_ASSERT(output[28] == 0x11U && output[31] == 0x44U);

    (void)memset(&guard, 0, sizeof(guard));
    guard.command_id = 9U;
    guard.issued_at_ms = 1000U;
    guard.valid_for_ms = 5U;
    guard.result_endpoint = 0x81U;
#if UCN_FEATURE_SERVICE
    TEST_ASSERT(ucn_service_command_guard_encode(&guard, service_guard) ==
                UCN_OK);
    TEST_ASSERT(memcmp(service_guard, expected_guard,
                       sizeof(expected_guard)) == 0);
    TEST_ASSERT(memcmp(&output[16], service_guard,
                       sizeof(service_guard)) == 0);
#endif
    (void)memset(&request, 0, sizeof(request));
    request.capture_time_us = UINT64_C(1000000);
    request.clock = &source;
    request.command_guard = &guard;
    request.business_payload = business;
    request.business_length = sizeof(business);
    request.sample_capture_bound_us = 1U;
    request.sample_capture_hardware = false;
    request.sample_capture_bound_known = true;
    request.e2e_protected = true;
    (void)memset(output, 0xA5, sizeof(output));
    (void)memcpy(before, output, sizeof(output));
    (void)memset(&result, 0x5A, sizeof(result));
    result_before = result;
    TEST_ASSERT(ucn_realtime_payload_prepare(&policy, &request, output,
                                             sizeof(output), &result) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(memcmp(output, before, sizeof(output)) == 0);
    TEST_ASSERT(memcmp(&result, &result_before, sizeof(result)) == 0);

    request.sample_capture_hardware = true;
    request.sample_capture_bound_known = false;
    TEST_ASSERT(ucn_realtime_payload_prepare(&policy, &request, output,
                                             sizeof(output), &result) ==
                UCN_ERR_STATE);
    TEST_ASSERT(memcmp(output, before, sizeof(output)) == 0);
    request.sample_capture_bound_known = true;
    request.sample_capture_bound_us = 0U;
    TEST_ASSERT(ucn_realtime_payload_prepare(&policy, &request, output,
                                             sizeof(output), &result) ==
                UCN_ERR_STATE);
    TEST_ASSERT(memcmp(output, before, sizeof(output)) == 0);
    request.sample_capture_bound_us = 1U;
    source.uncertainty_us = 63U;
    request.sample_capture_bound_us = 2U;
    TEST_ASSERT(ucn_realtime_payload_prepare(&policy, &request, output,
                                             sizeof(output), &result) ==
                UCN_ERR_STATE);
    TEST_ASSERT(memcmp(output, before, sizeof(output)) == 0);
    source.uncertainty_us = 40U;
    request.sample_capture_bound_us = 1U;
    source.available = false;
    TEST_ASSERT(ucn_realtime_payload_prepare(&policy, &request, output,
                                             sizeof(output), &result) ==
                UCN_ERR_STATE);
    TEST_ASSERT(memcmp(output, before, sizeof(output)) == 0);

    source.available = true;
    source.uncertainty_known = false;
    TEST_ASSERT(ucn_realtime_payload_prepare(&policy, &request, output,
                                             sizeof(output), &result) ==
                UCN_ERR_STATE);
    TEST_ASSERT(memcmp(output, before, sizeof(output)) == 0);
    source.uncertainty_known = true;
    request.e2e_protected = false;
    TEST_ASSERT(ucn_realtime_payload_prepare(&policy, &request, output,
                                             sizeof(output), &result) ==
                UCN_ERR_SECURITY);
    TEST_ASSERT(memcmp(output, before, sizeof(output)) == 0);
    return true;
}

/* EN: Verifies combined uncertainty, future skew, and half-open Deadline.
 * 中文：验证组合误差、未来偏斜和半开 Deadline。 */
static bool test_receive_math_boundaries(void)
{
    uint8_t payload[64];
    size_t length;
    ucn_realtime_policy_t policy = deadline_policy();
    ucn_realtime_clock_view_t receiver = clock_view(UINT64_C(1004200), 36U);
    ucn_realtime_receive_context_t context;
    ucn_realtime_receive_view_t view;

    TEST_ASSERT(build_deadline_payload(UINT64_C(1000000), 40U,
                                       payload, &length));
    (void)memset(&context, 0, sizeof(context));
    context.clock = &receiver;
    context.e2e_protected = true;
    context.source_acl_authorized = true;
    TEST_ASSERT(ucn_realtime_payload_evaluate(&policy, &context, payload,
                                              length, &view) == UCN_OK);
    TEST_ASSERT(view.accepted && view.age_valid &&
                view.combined_uncertainty_us == 100U &&
                view.estimated_age_us == 4200U &&
                view.age_upper_us == 4300U && view.business_offset == 28U);

    context.source_acl_authorized = false;
    TEST_ASSERT(ucn_realtime_payload_evaluate(&policy, &context, payload,
                                              length, &view) == UCN_OK);
    TEST_ASSERT(!view.accepted &&
                view.reason == UCN_REALTIME_REJECT_SECURITY);
    context.source_acl_authorized = true;

    receiver.uncertainty_us = 37U;
    TEST_ASSERT(ucn_realtime_payload_evaluate(&policy, &context, payload,
                                              length, &view) == UCN_OK);
    TEST_ASSERT(!view.accepted &&
                view.reason == UCN_REALTIME_REJECT_UNCERTAINTY);

    receiver.uncertainty_us = 36U;
    receiver.domain_time_us = UINT64_C(995000);
    TEST_ASSERT(ucn_realtime_payload_evaluate(&policy, &context, payload,
                                              length, &view) == UCN_OK);
    TEST_ASSERT(!view.accepted && view.reason == UCN_REALTIME_REJECT_FUTURE);

    receiver.domain_time_us = UINT64_C(1004900);
    TEST_ASSERT(ucn_realtime_payload_evaluate(&policy, &context, payload,
                                              length, &view) == UCN_OK);
    TEST_ASSERT(!view.accepted && view.age_upper_us == 5000U &&
                view.reason == UCN_REALTIME_REJECT_EXPIRED);
    return true;
}

/* EN: Verifies local/remote HOLDOVER and the second execution gate.
 * 中文：验证本地/远端 HOLDOVER 以及第二道执行门禁。 */
static bool test_holdover_and_execution_gate(void)
{
    uint8_t payload[64];
    size_t length;
    ucn_realtime_policy_t policy = deadline_policy();
    ucn_realtime_clock_view_t receiver = clock_view(UINT64_C(1001000), 10U);
    ucn_realtime_receive_context_t context;
    ucn_realtime_receive_view_t output_view;
    ucn_realtime_receive_view_t before_view;
    const uint8_t *business = (const uint8_t *)(uintptr_t)0x1234U;
    const uint8_t *before_business = business;
    size_t business_length = 0xA5A5U;
    size_t before_length = business_length;

    TEST_ASSERT(build_deadline_payload(UINT64_C(1000000), 10U,
                                       payload, &length));
    (void)memset(&context, 0, sizeof(context));
    context.clock = &receiver;
    context.e2e_protected = true;
    context.source_acl_authorized = true;
    TEST_ASSERT(ucn_realtime_execution_admit(&policy, &context, payload,
                                             length, &output_view, &business,
                                             &business_length) == UCN_OK);
    TEST_ASSERT(business == &payload[28] && business_length == 4U);

    receiver.domain_time_us = UINT64_C(1004974);
    (void)memset(&output_view, 0x5A, sizeof(output_view));
    before_view = output_view;
    business = before_business;
    business_length = before_length;
    TEST_ASSERT(ucn_realtime_execution_admit(&policy, &context, payload,
                                             length, &output_view, &business,
                                             &business_length) == UCN_ERR_TTL);
    TEST_ASSERT(memcmp(&output_view, &before_view, sizeof(output_view)) == 0);
    TEST_ASSERT(business == before_business && business_length == before_length);

    policy.allow_local_holdover = true;
    policy.max_local_holdover_us = 1000U;
    receiver.domain_time_us = UINT64_C(1001000);
    receiver.holdover = true;
    receiver.holdover_age_us = 1000U;
    TEST_ASSERT(ucn_realtime_payload_evaluate(&policy, &context, payload,
                                              length, &output_view) == UCN_OK);
    TEST_ASSERT(!output_view.accepted &&
                output_view.reason == UCN_REALTIME_REJECT_LOCAL_HOLDOVER);
    return true;
}

/* EN: Runs all RT-02 focused tests.
 * 中文：运行全部 RT-02 定向测试。 */
int main(void)
{
    if (!test_policy_registry_and_mixed_modes() ||
        !test_payload_prepare_and_no_downgrade() ||
        !test_receive_math_boundaries() ||
        !test_holdover_and_execution_gate()) {
        return 1;
    }
    (void)puts("ucn realtime policy tests passed");
    return 0;
}
