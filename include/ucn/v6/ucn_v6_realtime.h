#ifndef UCN_V6_REALTIME_H
#define UCN_V6_REALTIME_H

/* Optional v6 Realtime owner.
 *
 * Ordinary Endpoints use mode NONE and carry no realtime bytes.  Timed
 * Endpoints prepend one canonical 16-byte Envelope to their end-to-end
 * payload.  Relays never parse this object; the outer Hop Budget, when
 * present, remains the only hop-visible deadline hint. */

#include "ucn/v6/ucn_v6_capability.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_REALTIME_ENVELOPE_VERSION ((uint8_t)1U)
#define UCN_V6_REALTIME_ENVELOPE_BYTES ((size_t)16U)
#define UCN_V6_TIME_SYNC_SAMPLE_BYTES ((size_t)48U)
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
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
    uint8_t lock_sample_count;
    uint64_t sync_timeout_us;
    uint64_t max_holdover_us;
    uint32_t max_offset_jump_us;
    uint32_t oscillator_uncertainty_ppb;
} ucn_v6_time_domain_config_t;

/* Durable, canonical proposal identity for one Domain generation.  Equality
 * is field-wise; a generation may never be rebound to another Session/Path
 * or timing policy after reset. */
typedef struct ucn_v6_realtime_domain_record {
    ucn_v6_time_domain_config_t config;
} ucn_v6_realtime_domain_record_t;

typedef struct ucn_v6_time_sync_sample {
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint64_t local_sample_us;
    int64_t offset_us;
    ucn_v6_realtime_uncertainty_t uncertainty;
} ucn_v6_time_sync_sample_t;

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

/* The generation store is synchronous and anti-rollback.  Once a record is
 * reserved, the same {master principal, clock domain, generation} may only
 * reload with an exactly equal canonical Domain proposal. */
typedef struct ucn_v6_realtime_generation_store_ops {
    void *context;
    ucn_v6_result_t (*load_domain_record)(
        void *context,
        const ucn_v6_principal_t *master,
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
/* EN: Encodes/decodes the exact authenticated TIME_FOLLOW_UP sample payload.
 * 中文：编码/解码经过认证的 TIME_FOLLOW_UP 精确采样载荷。 */
ucn_v6_result_t ucn_v6_time_sync_sample_encode(
    const ucn_v6_time_sync_sample_t *sample,
    uint8_t output[UCN_V6_TIME_SYNC_SAMPLE_BYTES]);
ucn_v6_result_t ucn_v6_time_sync_sample_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_time_sync_sample_t *sample);

/* EN: Initializes one fixed-capacity Realtime owner.
 * 中文：初始化一个固定容量的实时 Owner。 */
ucn_v6_result_t ucn_v6_realtime_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
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
    const ucn_v6_path_capability_t *fixed_path,
    const ucn_v6_cached_peer_capability_t *master_capability);

/* EN: Admits one authenticated sample; dynamic paths and diagnostic-only
 * capabilities never contribute to LOCKED.
 * 中文：准入一个已认证采样；动态 Path 和仅诊断能力绝不推动 LOCKED。 */
ucn_v6_result_t ucn_v6_realtime_ingest_sample(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_security_open_result_t *opened);
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
ucn_v6_result_t ucn_v6_realtime_copy_view(
    const ucn_v6_realtime_owner_t *owner,
    ucn_v6_realtime_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
