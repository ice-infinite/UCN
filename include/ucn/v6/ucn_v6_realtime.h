#ifndef UCN_V6_REALTIME_H
#define UCN_V6_REALTIME_H

/* Optional v6 Realtime owner.
 *
 * Ordinary Endpoints use mode NONE and carry no realtime bytes.  Timed
 * Endpoints prepend one canonical 16-byte Envelope to their end-to-end
 * payload.  Relays never parse this object; the outer Hop Budget, when
 * present, remains the only hop-visible deadline hint. */

#include "ucn/v6/ucn_v6_route.h"
#include "ucn/v6/ucn_v6_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_REALTIME_ENVELOPE_VERSION ((uint8_t)1U)
#define UCN_V6_REALTIME_ENVELOPE_BYTES ((size_t)16U)
#define UCN_V6_TIME_SYNC_ANNOUNCE_BYTES ((size_t)12U)
#define UCN_V6_TIME_SYNC_RESPONSE_BYTES ((size_t)40U)
#define UCN_V6_REALTIME_SAMPLE_WINDOW ((size_t)5U)
#define UCN_V6_REALTIME_DOMAIN_ID_MAX UINT16_C(0xFFFE)
#define UCN_V6_REALTIME_UNCERTAINTY_UNKNOWN ((uint8_t)31U)

enum {
    UCN_V6_REALTIME_KNOWN_TIMER = 1U << 0,
    UCN_V6_REALTIME_KNOWN_LINK = 1U << 1,
    UCN_V6_REALTIME_KNOWN_FILTER = 1U << 2,
    UCN_V6_REALTIME_KNOWN_ROUNDING = 1U << 3,
    UCN_V6_REALTIME_KNOWN_CAPTURE = 1U << 4,
    UCN_V6_REALTIME_KNOWN_ASYMMETRY = 1U << 5,
    UCN_V6_REALTIME_KN_ALL = 0x3FU
};

typedef enum ucn_v6_realtime_mode {
    UCN_V6_REALTIME_NONE = 0,
    UCN_V6_REALTIME_LOCAL_STAMP = 1,
    UCN_V6_REALTIME_SYNCED_STAMP = 2,
    UCN_V6_REALTIME_DEADLINE = 3
} ucn_v6_realtime_mode_t;

typedef enum ucn_v6_realtime_requirement {
    UCN_V6_REALTIME_DISABLED = 0,
    UCN_V6_REALTIME_PREFERRED = 1,
    UCN_V6_REALTIME_REQUIRED = 2
} ucn_v6_realtime_requirement_t;

typedef enum ucn_v6_time_domain_phase {
    UCN_V6_TIME_UNSYNCED = 0,
    UCN_V6_TIME_ACQUIRING = 1,
    UCN_V6_TIME_LOCKED = 2,
    UCN_V6_TIME_HOLDOVER = 3,
    UCN_V6_TIME_FAULT = 4
} ucn_v6_time_domain_phase_t;

typedef struct ucn_v6_realtime_envelope {
    ucn_v6_realtime_mode_t mode;
    uint8_t uncertainty_class;
    bool sample_capture_hardware;
    bool domain_time_valid;
    bool source_holdover;
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint64_t capture_time_us;
} ucn_v6_realtime_envelope_t;

typedef struct ucn_v6_realtime_uncertainty {
    uint32_t timer_resolution_bound_us;
    uint32_t link_timestamp_capture_bound_us;
    uint32_t filter_residual_bound_us;
    uint32_t arithmetic_rounding_bound_us;
    uint32_t sample_capture_bound_us;
    uint32_t path_asymmetry_bound_us;
    uint8_t known_mask;
} ucn_v6_realtime_uncertainty_t;

typedef struct ucn_v6_realtime_endpoint_policy {
    uint16_t destination_endpoint;
    ucn_v6_realtime_mode_t mode;
    ucn_v6_realtime_requirement_t requirement;
    uint16_t clock_domain_id;
    uint64_t max_age_us;
    uint32_t max_uncertainty_us;
    uint64_t max_local_holdover_us;
    bool require_hardware_capture;
} ucn_v6_realtime_endpoint_policy_t;

typedef struct ucn_v6_time_domain_config {
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    ucn_v6_principal_t master_principal;
    ucn_v6_binding_key_t master_binding;
    uint32_t master_session_generation;
    uint8_t lock_sample_count;
    uint64_t sync_timeout_us;
    uint64_t max_holdover_us;
    uint32_t max_offset_jump_us;
    uint32_t oscillator_uncertainty_ppb;
    uint32_t timer_resolution_bound_us;
    uint32_t filter_residual_bound_us;
    uint32_t arithmetic_rounding_bound_us;
    uint32_t sample_capture_bound_us;
} ucn_v6_time_domain_config_t;

/* Durable proof fields that are relevant to Realtime admission.  This is not
 * a second Path object: Route Owner remains the only live Path authority.
 * The proof binds the exact destination/local Capability digests and only the
 * capability fields whose change invalidates Time Domain acquisition.
 * 持久化的 Realtime 准入证明字段。它不是第二份 Path 对象；Route Owner
 * 始终是唯一在线 Path 权威。该证明绑定目标/本地 Capability 摘要，并只保存
 * 会使时间域采集失效的能力字段。 */
typedef struct ucn_v6_realtime_path_proof {
    uint32_t destination_capability_generation;
    uint8_t destination_capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint8_t local_parent_capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    uint16_t destination_realtime_mode_bits;
    uint16_t destination_clock_domain_id;
    uint32_t destination_clock_domain_generation;
    uint32_t feature_bits;
    uint16_t timestamp_capability_bits;
    uint32_t timestamp_uncertainty_us;
} ucn_v6_realtime_path_proof_t;

/* Durable, canonical proposal identity for one Domain generation.  Equality
 * is field-wise.  The Master owns Domain generation: a Member may retain that
 * generation only when Security advances the exact Master Session and this
 * complete identity is durably replaced before acquisition. */
typedef struct ucn_v6_realtime_domain_record {
    ucn_v6_time_domain_config_t config;
    ucn_v6_route_path_ref_t route_ref;
    ucn_v6_stack_invalidation_t route_dependency;
    ucn_v6_realtime_path_proof_t path_proof;
} ucn_v6_realtime_domain_record_t;

typedef struct ucn_v6_time_sync_announce {
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint32_t sync_sequence;
} ucn_v6_time_sync_announce_t;

/* EN: Authenticated Master result for one completed four-event exchange.
 * It carries only Master-owned T1/T4. Member-owned T2/T3 never cross Wire.
 * 中文：一次四事件交换的 Master 认证结果；仅携带 Master 拥有的 T1/T4，
 * Member 拥有的 T2/T3 永不上 Wire。 */
typedef struct ucn_v6_time_sync_response {
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint32_t sync_sequence;
    uint64_t t1_master_tx_us;
    uint64_t t4_master_rx_us;
    uint32_t t1_uncertainty_us;
    uint32_t t4_uncertainty_us;
} ucn_v6_time_sync_response_t;

typedef struct ucn_v6_time_local_capture {
    uint16_t link_id;
    uint32_t link_generation;
    uint32_t event_token;
    uint64_t timestamp_us;
    uint32_t uncertainty_us;
    bool hardware;
} ucn_v6_time_local_capture_t;

typedef struct ucn_v6_time_sync_observation {
    uint32_t sync_sequence;
    /* The authenticated Master->Member Path carried TIME_SYNC and must also
     * carry TIME_DELAY_RESPONSE.  The reverse reference below is the distinct
     * Member->Master Path used by TIME_DELAY_REQUEST; the two Path IDs are not
     * interchangeable.
     * 认证的 Master->Member Path 承载 TIME_SYNC，并且也必须承载
     * TIME_DELAY_RESPONSE。下方 reverse 引用是 TIME_DELAY_REQUEST 使用的
     * 独立 Member->Master Path；两个 Path ID 不可混用。 */
    ucn_v6_route_path_ref_t forward_route_ref;
    ucn_v6_route_path_ref_t reverse_route_ref;
    ucn_v6_time_local_capture_t t2_member_rx;
    ucn_v6_time_local_capture_t t3_member_tx;
} ucn_v6_time_sync_observation_t;

typedef struct ucn_v6_realtime_clock_view {
    bool available;
    bool uncertainty_known;
    bool holdover;
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint64_t domain_time_us;
    uint32_t uncertainty_us;
    uint64_t holdover_age_us;
} ucn_v6_realtime_clock_view_t;

typedef struct ucn_v6_realtime_send_result {
    ucn_v6_realtime_mode_t mode;
    size_t payload_length;
    size_t business_offset;
} ucn_v6_realtime_send_result_t;

typedef enum ucn_v6_realtime_reject_reason {
    UCN_V6_REALTIME_REJECT_NONE = 0,
    UCN_V6_REALTIME_REJECT_MALFORMED = 1,
    UCN_V6_REALTIME_REJECT_SECURITY = 2,
    UCN_V6_REALTIME_REJECT_DOMAIN = 3,
    UCN_V6_REALTIME_REJECT_UNCERTAINTY = 4,
    UCN_V6_REALTIME_REJECT_FUTURE = 5,
    UCN_V6_REALTIME_REJECT_EXPIRED = 6,
    UCN_V6_REALTIME_REJECT_HOLDOVER = 7
} ucn_v6_realtime_reject_reason_t;

typedef struct ucn_v6_realtime_receive_view {
    bool accepted;
    bool metadata_present;
    ucn_v6_realtime_reject_reason_t reason;
    ucn_v6_realtime_envelope_t envelope;
    uint64_t age_upper_us;
    uint32_t combined_uncertainty_us;
    size_t business_offset;
    size_t business_length;
} ucn_v6_realtime_receive_view_t;

/* The generation store is synchronous and anti-rollback. clock_domain_id is
 * the sole lookup key; a Master change must advance the Master-owned Domain
 * generation.  A Member reboot may recover that same Domain only through the
 * exact checked-next Security Session plus fresh authenticated Capability and
 * Path proof.  Exact Session/Path replay is rejected.  High-rate local output
 * watermarks are deliberately not written to Flash, so this contract proves
 * freshness across reboot but does not promise a durable local clock-output
 * watermark; products requiring that stronger property need a separate local
 * monotonic-output witness. */
typedef struct ucn_v6_realtime_generation_store_ops {
    void *context;
    ucn_v6_result_t (*load_domain_record)(
        void *context,
        uint16_t clock_domain_id,
        ucn_v6_realtime_domain_record_t *record);
    ucn_v6_result_t (*reserve_domain_record)(
        void *context,
        const ucn_v6_realtime_domain_record_t *record);
} ucn_v6_realtime_generation_store_ops_t;

typedef struct ucn_v6_realtime_owner ucn_v6_realtime_owner_t;
typedef union ucn_v6_realtime_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_REALTIME_OWNER_STORAGE_BYTES];
} ucn_v6_realtime_owner_storage_t;

typedef struct ucn_v6_realtime_view {
    uint16_t endpoint_policies;
    uint16_t domains;
    uint16_t locked_domains;
    uint32_t accepted_samples;
    uint32_t rejected_samples;
    uint32_t rejected_messages;
    bool faulted;
} ucn_v6_realtime_view_t;

/* EN: Strictly encodes/decodes the canonical 16-byte Envelope.
 * 中文：严格编码/解码固定 16 字节实时 Envelope。 */
ucn_v6_result_t ucn_v6_realtime_envelope_encode(
    const ucn_v6_realtime_envelope_t *envelope,
    uint8_t output[UCN_V6_REALTIME_ENVELOPE_BYTES]);
ucn_v6_result_t ucn_v6_realtime_envelope_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_realtime_envelope_t *envelope);
ucn_v6_result_t ucn_v6_realtime_uncertainty_aggregate(
    const ucn_v6_realtime_uncertainty_t *components,
    uint32_t *upper_bound_us);
ucn_v6_result_t ucn_v6_time_sync_announce_encode(
    const ucn_v6_time_sync_announce_t *announce,
    uint8_t output[UCN_V6_TIME_SYNC_ANNOUNCE_BYTES]);
ucn_v6_result_t ucn_v6_time_sync_announce_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_time_sync_announce_t *announce);
/* EN: Encodes/decodes the authenticated Master-owned T1/T4 response. The
 * Member's T2/T3 captures are local-only and cannot be supplied by Wire.
 * 中文：编码/解码由 Master 持有的认证 T1/T4 响应。Member 的 T2/T3 只存在于
 * 本地，Wire 无权提供。 */
ucn_v6_result_t ucn_v6_time_sync_response_encode(
    const ucn_v6_time_sync_response_t *response,
    uint8_t output[UCN_V6_TIME_SYNC_RESPONSE_BYTES]);
ucn_v6_result_t ucn_v6_time_sync_response_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_time_sync_response_t *response);

/* EN: Initializes one fixed-capacity Realtime owner.
 * 中文：初始化一个固定容量的实时 Owner。 */
ucn_v6_result_t ucn_v6_realtime_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_route_owner_t *route_owner,
    const ucn_v6_realtime_generation_store_ops_t *generation_store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_realtime_owner_t **owner);
ucn_v6_result_t ucn_v6_realtime_set_endpoint_policy(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_realtime_endpoint_policy_t *policy);

/* EN: Binds an authenticated master only to an immutable, timestamp-capable
 * Path and durably reserves its observed Domain generation first.
 * 中文：只把认证 Master 绑定到不可变且支持时间戳的 Path，并先持久保留
 * 观察到的 Domain generation。 */
ucn_v6_result_t ucn_v6_realtime_bind_domain(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_time_domain_config_t *config,
    const ucn_v6_route_path_ref_t *fixed_route_ref,
    uint64_t now_us);

/* EN: Ingests one complete T1/T2/T3/T4 exchange. Security supplies the
 * authenticated DELAY_RESPONSE containing T1/T4; the standard Runtime binds
 * T2/T3 to atomic Adapter RX/TX-completion events before calling this API.
 * No remote offset or remote claim of a local sample is accepted.
 * 中文：准入一次完整 T1/T2/T3/T4 交换。Security 提供携带 T1/T4 的认证
 * DELAY_RESPONSE；标准 Runtime 在调用前把 T2/T3 绑定到 Adapter 原子 RX/TX
 * 完成事件。本接口不接受远端直接声明 offset 或本地采样时刻。 */
ucn_v6_result_t ucn_v6_realtime_ingest_exchange(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_security_open_result_t *opened_delay_response,
    const ucn_v6_time_sync_observation_t *observation,
    uint64_t now_us);
ucn_v6_result_t ucn_v6_realtime_get_clock(
    ucn_v6_realtime_owner_t *owner,
    uint16_t clock_domain_id,
    uint64_t local_now_us,
    ucn_v6_realtime_clock_view_t *view);

/* EN: Adds zero bytes for NONE and exactly 16 bytes for timed payloads.
 * Input business storage must not overlap output storage.
 * 中文：NONE 不增加字节；时间载荷严格增加 16 字节。业务输入区不得与输出区
 * 重叠。 */
ucn_v6_result_t ucn_v6_realtime_prepare_payload(
    ucn_v6_realtime_owner_t *owner,
    uint16_t destination_endpoint,
    uint64_t local_capture_us,
    uint32_t sample_capture_bound_us,
    bool hardware_capture,
    const uint8_t *business_payload,
    size_t business_length,
    uint8_t *output,
    size_t output_capacity,
    ucn_v6_realtime_send_result_t *result);

/* EN: Performs the receive-time freshness gate using Security/ACL output.
 * 中文：使用 Security/ACL 结果执行接收时新鲜度门禁。 */
ucn_v6_result_t ucn_v6_realtime_receive_admit(
    ucn_v6_realtime_owner_t *owner,
    uint64_t local_now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_realtime_receive_view_t *view);
/* EN: Repeats the same half-open freshness check immediately before use.
 * 中文：在业务使用前重复同一套半开区间新鲜度校验。 */
ucn_v6_result_t ucn_v6_realtime_execution_admit(
    ucn_v6_realtime_owner_t *owner,
    uint64_t local_now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_realtime_receive_view_t *view,
    const uint8_t **business_payload,
    size_t *business_length);
/* EN: Advances dependency and holdover expiry even while no Endpoint reads the
 * Domain, so diagnostics cannot retain stale LOCKED state.
 * 中文：即使没有 Endpoint 读取 Domain，也推进依赖与保持期过期，防止诊断
 * 长期保留陈旧 LOCKED 状态。 */
ucn_v6_result_t ucn_v6_realtime_step(
    ucn_v6_realtime_owner_t *owner,
    uint64_t now_us);
/* EN: Immediately invalidates a bound Time Domain dependency generation.
 * 中文：立即失效绑定到 Time Domain 的依赖代际。 */
ucn_v6_result_t ucn_v6_realtime_apply_invalidation(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation);
ucn_v6_result_t ucn_v6_realtime_copy_view(
    const ucn_v6_realtime_owner_t *owner,
    ucn_v6_realtime_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
