#ifndef UCN_REALTIME_POLICY_H
#define UCN_REALTIME_POLICY_H

/* Optional Endpoint-level Realtime policy and payload admission.
 * 可选的 Endpoint 级实时策略与 Payload 准入模块。
 *
 * This API is deliberately independent from ucn_node_t and
 * ucn_service_router_t.  A product may evaluate or prepare one payload with
 * caller-owned storage before handing it to the existing Service/Core path.
 * Linking this archive does not install a production RX/TX hook. */

#include "ucn/ucn_realtime.h"
#include "ucn/ucn_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UCN_REALTIME_MAX_ENDPOINT_POLICIES
#define UCN_REALTIME_MAX_ENDPOINT_POLICIES ((size_t)8U)
#endif

typedef char ucn_realtime_policy_capacity_must_be_nonzero[
    UCN_REALTIME_MAX_ENDPOINT_POLICIES > 0U ? 1 : -1];

typedef uint8_t ucn_realtime_requirement_t;
enum {
    UCN_REALTIME_REQUIREMENT_DISABLED = 0U,
    UCN_REALTIME_REQUIREMENT_PREFERRED = 1U,
    UCN_REALTIME_REQUIREMENT_REQUIRED = 2U
};

/* A read-only, point-in-time view supplied by the Time Domain owner.
 * Time Policy never mutates the Domain and never reads a global clock. */
typedef struct ucn_realtime_clock_view {
    bool available;
    bool uncertainty_known;
    bool holdover;
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint64_t domain_time_us;
    uint32_t uncertainty_us;
    uint64_t holdover_age_us;
} ucn_realtime_clock_view_t;

typedef struct ucn_realtime_policy {
    ucn_realtime_mode_t mode;
    ucn_realtime_requirement_t requirement;
    uint16_t clock_domain_id;
    uint64_t max_age_us;
    uint32_t max_uncertainty_us;
    uint64_t max_local_holdover_us;
    bool allow_local_holdover;
    bool allow_preferred_remote_holdover;
    bool require_sample_hardware_capture;
    bool require_e2e_protection;
    bool command_guard_present;
} ucn_realtime_policy_t;

typedef struct ucn_realtime_endpoint_policy {
    ucn_endpoint_t endpoint;
    ucn_realtime_policy_t policy;
    bool occupied;
} ucn_realtime_endpoint_policy_t;

typedef struct ucn_realtime_policy_registry {
    ucn_realtime_endpoint_policy_t
        entries[UCN_REALTIME_MAX_ENDPOINT_POLICIES];
} ucn_realtime_policy_registry_t;

typedef struct ucn_realtime_send_request {
    uint64_t capture_time_us;
    const ucn_realtime_clock_view_t *clock;
    const ucn_service_command_guard_t *command_guard;
    const uint8_t *business_payload;
    size_t business_length;
    uint32_t sample_capture_bound_us;
    bool sample_capture_hardware;
    bool sample_capture_bound_known;
    bool e2e_protected;
} ucn_realtime_send_request_t;

typedef struct ucn_realtime_send_result {
    ucn_realtime_mode_t mode;
    size_t payload_length;
    size_t business_offset;
} ucn_realtime_send_result_t;

typedef struct ucn_realtime_receive_context {
    const ucn_realtime_clock_view_t *clock;
    bool e2e_protected;
    bool source_acl_authorized;
    bool has_last_command_id;
    uint32_t last_command_id;
} ucn_realtime_receive_context_t;

typedef uint8_t ucn_realtime_reject_reason_t;
enum {
    UCN_REALTIME_REJECT_NONE = 0U,
    UCN_REALTIME_REJECT_MALFORMED = 1U,
    UCN_REALTIME_REJECT_MODE = 2U,
    UCN_REALTIME_REJECT_CAPTURE_QUALITY = 3U,
    UCN_REALTIME_REJECT_DOMAIN_UNAVAILABLE = 4U,
    UCN_REALTIME_REJECT_DOMAIN_MISMATCH = 5U,
    UCN_REALTIME_REJECT_LOCAL_HOLDOVER = 6U,
    UCN_REALTIME_REJECT_REMOTE_HOLDOVER = 7U,
    UCN_REALTIME_REJECT_UNCERTAINTY = 8U,
    UCN_REALTIME_REJECT_FUTURE = 9U,
    UCN_REALTIME_REJECT_EXPIRED = 10U,
    UCN_REALTIME_REJECT_GUARD = 11U,
    UCN_REALTIME_REJECT_REPLAY = 12U,
    UCN_REALTIME_REJECT_SECURITY = 13U
};

typedef struct ucn_realtime_receive_view {
    bool accepted;
    bool metadata_present;
    bool age_valid;
    ucn_realtime_mode_t mode;
    ucn_realtime_reject_reason_t reason;
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint64_t capture_time_us;
    uint64_t estimated_age_us;
    uint64_t age_upper_us;
    uint32_t combined_uncertainty_us;
    uint32_t command_id;
    size_t business_offset;
    size_t business_length;
} ucn_realtime_receive_view_t;

/* EN: Validates the canonical Endpoint policy combination.
 * 中文：校验规范的 Endpoint 时间策略字段组合。 */
bool ucn_realtime_policy_is_valid(const ucn_realtime_policy_t *policy);

/* EN: Initializes one fixed-capacity Endpoint policy registry.
 * 中文：初始化一个固定容量的 Endpoint 时间策略表。 */
ucn_result_t ucn_realtime_policy_registry_init(
    ucn_realtime_policy_registry_t *registry);

/* EN: Installs or atomically replaces one static Endpoint policy.
 * 中文：安装或原子替换一个静态 Endpoint 的时间策略。 */
ucn_result_t ucn_realtime_policy_registry_set(
    ucn_realtime_policy_registry_t *registry,
    ucn_endpoint_t endpoint,
    const ucn_realtime_policy_t *policy);

/* EN: Finds one immutable policy view, or NULL when not configured.
 * 中文：查找一个只读策略视图；未配置时返回 NULL。 */
const ucn_realtime_policy_t *ucn_realtime_policy_registry_find(
    const ucn_realtime_policy_registry_t *registry,
    ucn_endpoint_t endpoint);

/* EN: Builds the exact policy-bound payload in caller-owned storage.
 * Failure leaves the complete output and result unchanged.
 * 中文：在调用者存储中构造严格绑定策略的 Payload；失败时完整输出与
 * result 均不写回。 */
ucn_result_t ucn_realtime_payload_prepare(
    const ucn_realtime_policy_t *policy,
    const ucn_realtime_send_request_t *request,
    uint8_t *output,
    size_t output_capacity,
    ucn_realtime_send_result_t *result);

/* EN: Evaluates receive-time or execution-time freshness.  Valid arguments
 * always return UCN_OK and expose an accepted/rejected diagnostic view.
 * 中文：评估接收时或执行时新鲜度；参数合法时总返回 UCN_OK，并通过
 * view 给出接受或拒绝诊断。 */
ucn_result_t ucn_realtime_payload_evaluate(
    const ucn_realtime_policy_t *policy,
    const ucn_realtime_receive_context_t *context,
    const uint8_t *payload,
    size_t payload_length,
    ucn_realtime_receive_view_t *view);

/* EN: Fail-closed execution gate.  Re-evaluates the queued payload and only
 * exposes business bytes on success; every failure leaves all outputs intact.
 * 中文：失败关闭的执行门；重新评估排队 Payload，仅成功时暴露业务区间，
 * 任一失败均保持全部输出不变。 */
ucn_result_t ucn_realtime_execution_admit(
    const ucn_realtime_policy_t *policy,
    const ucn_realtime_receive_context_t *context,
    const uint8_t *payload,
    size_t payload_length,
    ucn_realtime_receive_view_t *view,
    const uint8_t **business_payload,
    size_t *business_length);

#ifdef __cplusplus
}
#endif

#endif
