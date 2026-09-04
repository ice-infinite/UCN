/* Optional Endpoint-level Realtime policy and freshness admission.
 * 可选的 Endpoint 级实时策略与新鲜度准入。 */

#include "ucn/ucn_realtime_policy.h"

#include <string.h>

/* EN: Returns true when one mode uses a shared Time Domain.
 * 中文：判断模式是否使用共享时间域。 */
static bool shared_mode(ucn_realtime_mode_t mode)
{
    return mode == UCN_REALTIME_MODE_SYNCED_STAMP ||
           mode == UCN_REALTIME_MODE_DEADLINE;
}

/* EN: Checks a point-in-time clock view against one shared-domain policy.
 * 中文：根据共享时间域策略检查一个时钟快照。 */
static bool clock_is_usable(const ucn_realtime_policy_t *policy,
                            const ucn_realtime_clock_view_t *clock)
{
    if (clock == NULL || !clock->available || !clock->uncertainty_known ||
        clock->clock_domain_id != policy->clock_domain_id ||
        clock->domain_generation == 0U ||
        clock->domain_generation > UCN_REALTIME_DOMAIN_GENERATION_MAX) {
        return false;
    }
    if (clock->holdover &&
        (!policy->allow_local_holdover ||
         policy->max_local_holdover_us == 0U ||
         clock->holdover_age_us >= policy->max_local_holdover_us)) {
        return false;
    }
    return clock->uncertainty_us <= policy->max_uncertainty_us;
}

/* EN: Validates the complete, fixed Endpoint time ABI.
 * 中文：校验完整且固定的 Endpoint 时间 ABI。 */
bool ucn_realtime_policy_is_valid(const ucn_realtime_policy_t *policy)
{
    if (policy == NULL ||
        policy->requirement > UCN_REALTIME_REQUIREMENT_REQUIRED) {
        return false;
    }

    if (policy->mode == UCN_REALTIME_MODE_NONE) {
        return policy->requirement == UCN_REALTIME_REQUIREMENT_DISABLED &&
               policy->clock_domain_id == 0U && policy->max_age_us == 0U &&
               policy->max_uncertainty_us == 0U &&
               policy->max_local_holdover_us == 0U &&
               !policy->allow_local_holdover &&
               !policy->allow_preferred_remote_holdover &&
               !policy->require_sample_hardware_capture &&
               !policy->require_e2e_protection &&
               !policy->command_guard_present;
    }

    if (policy->requirement == UCN_REALTIME_REQUIREMENT_DISABLED) {
        return false;
    }
    if (policy->allow_local_holdover !=
        (policy->max_local_holdover_us != 0U)) {
        return false;
    }
    if (policy->allow_preferred_remote_holdover &&
        policy->requirement != UCN_REALTIME_REQUIREMENT_PREFERRED) {
        return false;
    }

    if (policy->mode == UCN_REALTIME_MODE_LOCAL_STAMP) {
        return policy->clock_domain_id == 0U && policy->max_age_us == 0U &&
               policy->max_uncertainty_us == 0U &&
               !policy->allow_local_holdover &&
               !policy->allow_preferred_remote_holdover &&
               !policy->require_e2e_protection &&
               !policy->command_guard_present;
    }

    if (!shared_mode(policy->mode) || policy->clock_domain_id == 0U ||
        policy->clock_domain_id > UCN_REALTIME_CLOCK_DOMAIN_ID_MAX ||
        policy->max_uncertainty_us == 0U) {
        return false;
    }
    if (policy->requirement == UCN_REALTIME_REQUIREMENT_REQUIRED &&
        !policy->require_e2e_protection) {
        return false;
    }
    if (policy->mode == UCN_REALTIME_MODE_SYNCED_STAMP) {
        return policy->max_age_us == 0U && !policy->command_guard_present;
    }
    return policy->max_age_us != 0U;
}

/* EN: Initializes caller-owned registry storage.
 * 中文：初始化调用者拥有的策略表存储。 */
ucn_result_t ucn_realtime_policy_registry_init(
    ucn_realtime_policy_registry_t *registry)
{
    if (registry == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(registry, 0, sizeof(*registry));
    return UCN_OK;
}

/* EN: Installs or replaces one policy without exposing a partial record.
 * 中文：安装或替换一条策略，且不暴露半写入记录。 */
ucn_result_t ucn_realtime_policy_registry_set(
    ucn_realtime_policy_registry_t *registry,
    ucn_endpoint_t endpoint,
    const ucn_realtime_policy_t *policy)
{
    size_t index;
    size_t free_index = UCN_REALTIME_MAX_ENDPOINT_POLICIES;
    ucn_realtime_endpoint_policy_t replacement;

    if (registry == NULL || !ucn_endpoint_is_static(endpoint) ||
        !ucn_realtime_policy_is_valid(policy)) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_REALTIME_MAX_ENDPOINT_POLICIES; ++index) {
        if (registry->entries[index].occupied &&
            registry->entries[index].endpoint == endpoint) {
            free_index = index;
            break;
        }
        if (!registry->entries[index].occupied &&
            free_index == UCN_REALTIME_MAX_ENDPOINT_POLICIES) {
            free_index = index;
        }
    }
    if (free_index == UCN_REALTIME_MAX_ENDPOINT_POLICIES) {
        return UCN_ERR_NO_SPACE;
    }

    (void)memset(&replacement, 0, sizeof(replacement));
    replacement.endpoint = endpoint;
    replacement.policy = *policy;
    replacement.occupied = true;
    registry->entries[free_index] = replacement;
    return UCN_OK;
}

/* EN: Looks up one configured Endpoint policy.
 * 中文：查找一个已经配置的 Endpoint 时间策略。 */
const ucn_realtime_policy_t *ucn_realtime_policy_registry_find(
    const ucn_realtime_policy_registry_t *registry,
    ucn_endpoint_t endpoint)
{
    size_t index;

    if (registry == NULL || !ucn_endpoint_is_static(endpoint)) {
        return NULL;
    }
    for (index = 0U; index < UCN_REALTIME_MAX_ENDPOINT_POLICIES; ++index) {
        if (registry->entries[index].occupied &&
            registry->entries[index].endpoint == endpoint) {
            return &registry->entries[index].policy;
        }
    }
    return NULL;
}

/* EN: Verifies the Timed Command Guard binding to Domain capture time.
 * 中文：验证 Timed Command Guard 与时间域采样时刻的绑定。 */
static bool guard_matches_capture(const ucn_service_command_guard_t *guard,
                                  uint64_t capture_time_us)
{
    return guard != NULL && guard->command_id != 0U &&
           guard->valid_for_ms != 0U && guard->flags == 0U &&
           ucn_endpoint_is_static(guard->result_endpoint) &&
           guard->issued_at_ms == (uint32_t)(capture_time_us / UINT64_C(1000));
}

/* EN: Encodes the frozen 12-byte Guard without linking Service runtime code.
 * 中文：编码冻结的 12 字节 Guard，且不链接 Service 运行时代码。 */
static ucn_result_t realtime_guard_encode(
    const ucn_service_command_guard_t *guard,
    uint8_t output[UCN_SERVICE_COMMAND_GUARD_BYTES])
{
    if (guard == NULL || output == NULL || guard->command_id == 0U ||
        guard->valid_for_ms == 0U ||
        !ucn_endpoint_is_static(guard->result_endpoint) || guard->flags != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    output[0] = (uint8_t)(guard->command_id >> 24U);
    output[1] = (uint8_t)(guard->command_id >> 16U);
    output[2] = (uint8_t)(guard->command_id >> 8U);
    output[3] = (uint8_t)guard->command_id;
    output[4] = (uint8_t)(guard->issued_at_ms >> 24U);
    output[5] = (uint8_t)(guard->issued_at_ms >> 16U);
    output[6] = (uint8_t)(guard->issued_at_ms >> 8U);
    output[7] = (uint8_t)guard->issued_at_ms;
    output[8] = (uint8_t)(guard->valid_for_ms >> 8U);
    output[9] = (uint8_t)guard->valid_for_ms;
    output[10] = guard->result_endpoint;
    output[11] = guard->flags;
    return UCN_OK;
}

/* EN: Decodes the frozen Guard into a temporary before publishing output.
 * 中文：先把冻结 Guard 解码到临时对象，再发布输出。 */
static ucn_result_t realtime_guard_decode(
    const uint8_t *payload,
    size_t payload_length,
    ucn_service_command_guard_t *guard)
{
    ucn_service_command_guard_t decoded;

    if (payload == NULL || guard == NULL ||
        payload_length < UCN_SERVICE_COMMAND_GUARD_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.command_id = ((uint32_t)payload[0] << 24U) |
                         ((uint32_t)payload[1] << 16U) |
                         ((uint32_t)payload[2] << 8U) | (uint32_t)payload[3];
    decoded.issued_at_ms = ((uint32_t)payload[4] << 24U) |
                           ((uint32_t)payload[5] << 16U) |
                           ((uint32_t)payload[6] << 8U) | (uint32_t)payload[7];
    decoded.valid_for_ms = (uint16_t)(((uint16_t)payload[8] << 8U) |
                                      (uint16_t)payload[9]);
    decoded.result_endpoint = payload[10];
    decoded.flags = payload[11];
    if (decoded.command_id == 0U || decoded.valid_for_ms == 0U ||
        !ucn_endpoint_is_static(decoded.result_endpoint) ||
        decoded.flags != 0U) {
        return UCN_ERR_MALFORMED;
    }
    *guard = decoded;
    return UCN_OK;
}

/* EN: Builds one canonical payload without partially owning caller output.
 * 中文：构造一个规范 Payload，且不会部分占有调用者输出。 */
ucn_result_t ucn_realtime_payload_prepare(
    const ucn_realtime_policy_t *policy,
    const ucn_realtime_send_request_t *request,
    uint8_t *output,
    size_t output_capacity,
    ucn_realtime_send_result_t *result)
{
    uint8_t encoded[UCN_MAX_PAYLOAD_BYTES];
    ucn_realtime_envelope_t envelope;
    ucn_realtime_send_result_t prepared;
    size_t offset = 0U;
    uint8_t uncertainty_class;
    bool encoded_uncertainty_known;
    uint32_t encoded_uncertainty_bound;
    uint64_t sender_uncertainty_bound;
    ucn_result_t status;
    size_t required_prefix;

    if (!ucn_realtime_policy_is_valid(policy) || request == NULL ||
        output == NULL || result == NULL ||
        (request->business_length != 0U && request->business_payload == NULL)) {
        return UCN_ERR_ARGUMENT;
    }
    if (output_capacity > sizeof(encoded)) {
        output_capacity = sizeof(encoded);
    }
    required_prefix = policy->mode == UCN_REALTIME_MODE_NONE ? 0U :
        UCN_REALTIME_ENVELOPE_WIRE_BYTES;
    if (policy->command_guard_present) {
        required_prefix += UCN_SERVICE_COMMAND_GUARD_BYTES;
    }
    if (required_prefix > sizeof(encoded) ||
        request->business_length > sizeof(encoded) - required_prefix ||
        required_prefix + request->business_length > output_capacity) {
        return UCN_ERR_TOO_LARGE;
    }

    (void)memset(encoded, 0, sizeof(encoded));
    (void)memset(&prepared, 0, sizeof(prepared));
    prepared.mode = policy->mode;

    if (policy->mode != UCN_REALTIME_MODE_NONE) {
        (void)memset(&envelope, 0, sizeof(envelope));
        envelope.mode = policy->mode;
        envelope.sample_capture_hardware = request->sample_capture_hardware;
        envelope.capture_time_us = request->capture_time_us;
        if (policy->require_sample_hardware_capture &&
            !request->sample_capture_hardware) {
            return UCN_ERR_ACCESS;
        }
        if (policy->require_e2e_protection && !request->e2e_protected) {
            return UCN_ERR_SECURITY;
        }
        if (policy->mode == UCN_REALTIME_MODE_LOCAL_STAMP) {
            envelope.uncertainty_class =
                UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN;
        } else {
            if (!clock_is_usable(policy, request->clock) ||
                !request->sample_capture_bound_known ||
                request->sample_capture_bound_us == 0U) {
                return UCN_ERR_STATE;
            }
            sender_uncertainty_bound =
                (uint64_t)request->clock->uncertainty_us +
                request->sample_capture_bound_us;
            status = ucn_realtime_uncertainty_class_encode(
                sender_uncertainty_bound <= UINT32_MAX,
                sender_uncertainty_bound, &uncertainty_class);
            if (status != UCN_OK ||
                ucn_realtime_uncertainty_class_decode(
                    uncertainty_class, &encoded_uncertainty_known,
                    &encoded_uncertainty_bound) != UCN_OK ||
                !encoded_uncertainty_known ||
                encoded_uncertainty_bound > policy->max_uncertainty_us) {
                return UCN_ERR_STATE;
            }
            envelope.uncertainty_class = uncertainty_class;
            envelope.domain_time_valid = true;
            envelope.source_holdover = request->clock->holdover;
            envelope.clock_domain_id = request->clock->clock_domain_id;
            envelope.domain_generation = request->clock->domain_generation;
        }
        status = ucn_realtime_envelope_encode(&envelope, encoded);
        if (status != UCN_OK) {
            return status;
        }
        offset = UCN_REALTIME_ENVELOPE_WIRE_BYTES;
    }

    if (policy->command_guard_present) {
        if (!guard_matches_capture(request->command_guard,
                                   request->capture_time_us)) {
            return UCN_ERR_ARGUMENT;
        }
        status = realtime_guard_encode(
            request->command_guard, &encoded[offset]);
        if (status != UCN_OK) {
            return status;
        }
        offset += UCN_SERVICE_COMMAND_GUARD_BYTES;
    } else if (request->command_guard != NULL) {
        return UCN_ERR_ARGUMENT;
    }

    if (request->business_length != 0U) {
        (void)memcpy(&encoded[offset], request->business_payload,
                     request->business_length);
    }
    prepared.business_offset = offset;
    prepared.payload_length = offset + request->business_length;
    (void)memcpy(output, encoded, prepared.payload_length);
    *result = prepared;
    return UCN_OK;
}

/* EN: Maps a diagnostic rejection to the public fail-closed result.
 * 中文：把诊断拒绝原因映射为公开的失败关闭结果。 */
static ucn_result_t reject_result(ucn_realtime_reject_reason_t reason)
{
    switch (reason) {
    case UCN_REALTIME_REJECT_MALFORMED:
    case UCN_REALTIME_REJECT_MODE:
    case UCN_REALTIME_REJECT_GUARD:
        return UCN_ERR_MALFORMED;
    case UCN_REALTIME_REJECT_EXPIRED:
        return UCN_ERR_TTL;
    case UCN_REALTIME_REJECT_REPLAY:
        return UCN_ERR_REPLAY;
    case UCN_REALTIME_REJECT_CAPTURE_QUALITY:
    case UCN_REALTIME_REJECT_REMOTE_HOLDOVER:
        return UCN_ERR_ACCESS;
    case UCN_REALTIME_REJECT_SECURITY:
        return UCN_ERR_SECURITY;
    default:
        return UCN_ERR_STATE;
    }
}

/* EN: Computes combined uncertainty and an upper-bound message age.
 * 中文：计算组合误差和消息年龄上界。 */
static bool compute_age(const ucn_realtime_clock_view_t *clock,
                        uint64_t capture_time_us,
                        uint32_t sender_uncertainty_us,
                        uint64_t *estimated_age_us,
                        uint64_t *age_upper_us,
                        uint32_t *combined_uncertainty_us,
                        ucn_realtime_reject_reason_t *reason)
{
    uint64_t combined;
    uint64_t age;

    combined = (uint64_t)sender_uncertainty_us + clock->uncertainty_us;
    if (combined > UINT32_MAX) {
        *reason = UCN_REALTIME_REJECT_UNCERTAINTY;
        return false;
    }
    if (capture_time_us > clock->domain_time_us) {
        const uint64_t future_delta = capture_time_us - clock->domain_time_us;

        if (future_delta > combined) {
            *reason = UCN_REALTIME_REJECT_FUTURE;
            return false;
        }
        age = 0U;
        *age_upper_us = combined - future_delta;
    } else {
        age = clock->domain_time_us - capture_time_us;
        if (UINT64_MAX - age < combined) {
            *reason = UCN_REALTIME_REJECT_UNCERTAINTY;
            return false;
        }
        *age_upper_us = age + combined;
    }
    *estimated_age_us = age;
    *combined_uncertainty_us = (uint32_t)combined;
    return true;
}

/* EN: Evaluates one payload and always exposes a deterministic diagnostic.
 * 中文：评估一个 Payload，并始终产生确定性的诊断结论。 */
ucn_result_t ucn_realtime_payload_evaluate(
    const ucn_realtime_policy_t *policy,
    const ucn_realtime_receive_context_t *context,
    const uint8_t *payload,
    size_t payload_length,
    ucn_realtime_receive_view_t *view)
{
    ucn_realtime_receive_view_t evaluated;
    ucn_realtime_envelope_t envelope;
    ucn_service_command_guard_t guard;
    bool sender_known;
    uint32_t sender_uncertainty;
    size_t offset = 0U;
    uint64_t effective_max_age;

    if (!ucn_realtime_policy_is_valid(policy) || context == NULL ||
        payload == NULL || view == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&evaluated, 0, sizeof(evaluated));
    evaluated.mode = policy->mode;

    if (policy->mode == UCN_REALTIME_MODE_NONE) {
        evaluated.accepted = true;
        evaluated.business_length = payload_length;
        *view = evaluated;
        return UCN_OK;
    }

    if ((policy->require_e2e_protection && !context->e2e_protected) ||
        (policy->requirement == UCN_REALTIME_REQUIREMENT_REQUIRED &&
         !context->source_acl_authorized)) {
        evaluated.reason = UCN_REALTIME_REJECT_SECURITY;
        *view = evaluated;
        return UCN_OK;
    }

    evaluated.metadata_present = true;
    if (payload_length < UCN_REALTIME_ENVELOPE_WIRE_BYTES ||
        ucn_realtime_envelope_decode(payload,
            UCN_REALTIME_ENVELOPE_WIRE_BYTES, &envelope) != UCN_OK) {
        evaluated.reason = UCN_REALTIME_REJECT_MALFORMED;
        *view = evaluated;
        return UCN_OK;
    }
    evaluated.clock_domain_id = envelope.clock_domain_id;
    evaluated.domain_generation = envelope.domain_generation;
    evaluated.capture_time_us = envelope.capture_time_us;
    if (envelope.mode != policy->mode) {
        evaluated.reason = UCN_REALTIME_REJECT_MODE;
        *view = evaluated;
        return UCN_OK;
    }
    if (policy->require_sample_hardware_capture &&
        !envelope.sample_capture_hardware) {
        evaluated.reason = UCN_REALTIME_REJECT_CAPTURE_QUALITY;
        *view = evaluated;
        return UCN_OK;
    }
    offset = UCN_REALTIME_ENVELOPE_WIRE_BYTES;

    if (shared_mode(policy->mode)) {
        if (context->clock == NULL || !context->clock->available ||
            !context->clock->uncertainty_known) {
            evaluated.reason = UCN_REALTIME_REJECT_DOMAIN_UNAVAILABLE;
            *view = evaluated;
            return UCN_OK;
        }
        if (envelope.clock_domain_id != policy->clock_domain_id ||
            context->clock->clock_domain_id != envelope.clock_domain_id ||
            context->clock->domain_generation != envelope.domain_generation) {
            evaluated.reason = UCN_REALTIME_REJECT_DOMAIN_MISMATCH;
            *view = evaluated;
            return UCN_OK;
        }
        if (context->clock->holdover &&
            (!policy->allow_local_holdover ||
             policy->max_local_holdover_us == 0U ||
             context->clock->holdover_age_us >=
                 policy->max_local_holdover_us)) {
            evaluated.reason = UCN_REALTIME_REJECT_LOCAL_HOLDOVER;
            *view = evaluated;
            return UCN_OK;
        }
        if (envelope.source_holdover &&
            (policy->requirement == UCN_REALTIME_REQUIREMENT_REQUIRED ||
             !policy->allow_preferred_remote_holdover ||
             !context->e2e_protected || !context->source_acl_authorized)) {
            evaluated.reason = UCN_REALTIME_REJECT_REMOTE_HOLDOVER;
            *view = evaluated;
            return UCN_OK;
        }
        if (ucn_realtime_uncertainty_class_decode(
                envelope.uncertainty_class, &sender_known,
                &sender_uncertainty) != UCN_OK || !sender_known ||
            !compute_age(context->clock, envelope.capture_time_us,
                         sender_uncertainty, &evaluated.estimated_age_us,
                         &evaluated.age_upper_us,
                         &evaluated.combined_uncertainty_us,
                         &evaluated.reason)) {
            if (evaluated.reason == UCN_REALTIME_REJECT_NONE) {
                evaluated.reason = UCN_REALTIME_REJECT_UNCERTAINTY;
            }
            *view = evaluated;
            return UCN_OK;
        }
        if (evaluated.combined_uncertainty_us >
            policy->max_uncertainty_us) {
            evaluated.reason = UCN_REALTIME_REJECT_UNCERTAINTY;
            *view = evaluated;
            return UCN_OK;
        }
        evaluated.age_valid = true;
    }

    effective_max_age = policy->max_age_us;
    if (policy->command_guard_present) {
        uint64_t guard_max_age;

        if (payload_length - offset < UCN_SERVICE_COMMAND_GUARD_BYTES ||
            realtime_guard_decode(&payload[offset],
                payload_length - offset, &guard) != UCN_OK ||
            !guard_matches_capture(&guard, envelope.capture_time_us)) {
            evaluated.reason = UCN_REALTIME_REJECT_GUARD;
            *view = evaluated;
            return UCN_OK;
        }
        if (context->has_last_command_id &&
            (int32_t)(guard.command_id - context->last_command_id) <= 0) {
            evaluated.reason = UCN_REALTIME_REJECT_REPLAY;
            *view = evaluated;
            return UCN_OK;
        }
        guard_max_age = (uint64_t)guard.valid_for_ms * UINT64_C(1000);
        if (guard_max_age < effective_max_age) {
            effective_max_age = guard_max_age;
        }
        evaluated.command_id = guard.command_id;
        offset += UCN_SERVICE_COMMAND_GUARD_BYTES;
    }

    if (policy->mode == UCN_REALTIME_MODE_DEADLINE &&
        evaluated.age_upper_us >= effective_max_age) {
        evaluated.reason = UCN_REALTIME_REJECT_EXPIRED;
        *view = evaluated;
        return UCN_OK;
    }
    evaluated.accepted = true;
    evaluated.reason = UCN_REALTIME_REJECT_NONE;
    evaluated.business_offset = offset;
    evaluated.business_length = payload_length - offset;
    *view = evaluated;
    return UCN_OK;
}

/* EN: Applies the second, no-write-on-failure business execution gate.
 * 中文：执行第二道业务门禁，并保证失败时不写回。 */
ucn_result_t ucn_realtime_execution_admit(
    const ucn_realtime_policy_t *policy,
    const ucn_realtime_receive_context_t *context,
    const uint8_t *payload,
    size_t payload_length,
    ucn_realtime_receive_view_t *view,
    const uint8_t **business_payload,
    size_t *business_length)
{
    ucn_realtime_receive_view_t evaluated;
    ucn_result_t status;

    if (view == NULL || business_payload == NULL || business_length == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    status = ucn_realtime_payload_evaluate(policy, context, payload,
                                           payload_length, &evaluated);
    if (status != UCN_OK) {
        return status;
    }
    if (!evaluated.accepted) {
        return reject_result(evaluated.reason);
    }
    *view = evaluated;
    *business_payload = &payload[evaluated.business_offset];
    *business_length = evaluated.business_length;
    return UCN_OK;
}
