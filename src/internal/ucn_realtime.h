#ifndef UCN_INTERNAL_REALTIME_H
#define UCN_INTERNAL_REALTIME_H

#include "internal/ucn_owner.h"
#include "ucn/ucn_config.h"

#define UCN_I_REALTIME_MAGIC UINT32_C(0x52544D45)
#define UCN_I_REALTIME_SCHEMA UINT16_C(1)
#define UCN_I_REALTIME_ENVELOPE_VERSION UINT8_C(1)
#define UCN_I_REALTIME_ENVELOPE_BYTES 16U
#define UCN_I_REALTIME_DOMAIN_ID_MAX UINT16_C(0xFFFE)
#define UCN_I_REALTIME_DOMAIN_GENERATION_MAX UINT32_C(0x7FFFFFFF)
#define UCN_I_REALTIME_UNCERTAINTY_UNKNOWN UINT8_C(31)
#define UCN_I_REALTIME_DIGEST_BYTES 16U
#define UCN_I_REALTIME_SAMPLE_WINDOW 5U
#define UCN_I_REALTIME_DOMAIN_RECORD_BYTES 128U
#define UCN_I_REALTIME_DOMAIN_SCHEMA_ID UINT16_C(0x0801)
#define UCN_I_REALTIME_DOMAIN_RECORD_SCHEMA UINT16_C(1)
#define UCN_I_REALTIME_DOMAIN_PERSIST_KIND UINT16_C(0x0801)

enum {
    UCN_I_REALTIME_KNOWN_TIMER = 1U << 0,
    UCN_I_REALTIME_KNOWN_LINK = 1U << 1,
    UCN_I_REALTIME_KNOWN_FILTER = 1U << 2,
    UCN_I_REALTIME_KNOWN_ROUNDING = 1U << 3,
    UCN_I_REALTIME_KNOWN_CAPTURE = 1U << 4,
    UCN_I_REALTIME_KNOWN_ASYMMETRY = 1U << 5,
    UCN_I_REALTIME_KNOWN_ALL = 0x3FU
};

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_I_REALTIME_DOMAIN_COUNT 1U
#define UCN_I_REALTIME_POLICY_COUNT 4U
#define UCN_I_REALTIME_SYNC_COUNT 1U
#define UCN_I_REALTIME_RELEASE_COUNT 2U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_I_REALTIME_DOMAIN_COUNT 2U
#define UCN_I_REALTIME_POLICY_COUNT 12U
#define UCN_I_REALTIME_SYNC_COUNT 4U
#define UCN_I_REALTIME_RELEASE_COUNT 8U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_I_REALTIME_DOMAIN_COUNT 4U
#define UCN_I_REALTIME_POLICY_COUNT 32U
#define UCN_I_REALTIME_SYNC_COUNT 8U
#define UCN_I_REALTIME_RELEASE_COUNT 16U
#else
#error "UCN_PROFILE must select NANO, LITE, or FULL"
#endif

typedef uint8_t ucn_i_realtime_mode_t;
enum {
    UCN_I_REALTIME_NONE = 0,
    UCN_I_REALTIME_LOCAL_STAMP = 1,
    UCN_I_REALTIME_SYNCED_STAMP = 2,
    UCN_I_REALTIME_DEADLINE = 3
};

typedef uint8_t ucn_i_realtime_policy_requirement_t;
enum {
    UCN_I_REALTIME_DISABLED = 0,
    UCN_I_REALTIME_PREFERRED = 1,
    UCN_I_REALTIME_REQUIRED = 2
};

typedef uint8_t ucn_i_realtime_domain_phase_t;
enum {
    UCN_I_REALTIME_GENERATION_LOADING = 1,
    UCN_I_REALTIME_UNSYNCED = 2,
    UCN_I_REALTIME_ACQUIRING = 3,
    UCN_I_REALTIME_LOCKED = 4,
    UCN_I_REALTIME_HOLDOVER = 5,
    UCN_I_REALTIME_FAULT = 6,
    UCN_I_REALTIME_GENERATION_PENDING = 7
};

typedef uint8_t ucn_i_realtime_reject_reason_t;
enum {
    UCN_I_REALTIME_REJECT_NONE = 0,
    UCN_I_REALTIME_REJECT_MALFORMED = 1,
    UCN_I_REALTIME_REJECT_SECURITY = 2,
    UCN_I_REALTIME_REJECT_DOMAIN = 3,
    UCN_I_REALTIME_REJECT_UNCERTAINTY = 4,
    UCN_I_REALTIME_REJECT_FUTURE = 5,
    UCN_I_REALTIME_REJECT_EXPIRED = 6,
    UCN_I_REALTIME_REJECT_HOLDOVER = 7
};

typedef struct ucn_i_realtime_envelope {
    uint64_t capture_time_us;
    uint32_t domain_generation;
    uint16_t clock_domain_id;
    uint8_t mode;
    uint8_t uncertainty_class;
    uint8_t sample_capture_hardware;
    uint8_t domain_time_valid;
    uint8_t source_holdover;
    uint8_t reserved_zero;
} ucn_i_realtime_envelope_t;

typedef struct ucn_i_realtime_uncertainty {
    uint32_t timer_resolution_bound_us;
    uint32_t link_timestamp_capture_bound_us;
    uint32_t filter_residual_bound_us;
    uint32_t arithmetic_rounding_bound_us;
    uint32_t sample_capture_bound_us;
    uint32_t path_asymmetry_bound_us;
    uint8_t known_mask;
    uint8_t reserved_zero[3];
} ucn_i_realtime_uncertainty_t;

typedef struct ucn_i_realtime_config {
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t reserved_zero;
    ucn_i_lock_ops_t state_lock;
} ucn_i_realtime_config_t;

typedef struct ucn_i_realtime_policy {
    uint64_t max_age_us;
    uint32_t max_uncertainty_us;
    uint16_t endpoint;
    uint16_t clock_domain_id;
    uint8_t mode;
    uint8_t requirement;
    uint8_t require_hardware_capture;
    uint8_t allow_remote_holdover;
} ucn_i_realtime_policy_t;

typedef struct ucn_i_realtime_policy_slot {
    ucn_i_realtime_policy_t policy;
    uint8_t occupied;
    uint8_t reserved_zero[7];
} ucn_i_realtime_policy_slot_t;

typedef uint8_t ucn_i_realtime_event_direction_t;
enum {
    UCN_I_REALTIME_EVENT_RX = 1,
    UCN_I_REALTIME_EVENT_TX = 2
};

typedef struct ucn_i_realtime_event_key {
    uint32_t link_generation;
    uint32_t token;
    uint16_t link_id;
    uint8_t direction;
    uint8_t reserved_zero;
} ucn_i_realtime_event_key_t;

typedef struct ucn_i_realtime_path_facts {
    uint64_t route_causal_id;
    uint32_t route_generation;
    uint32_t session_generation;
    uint32_t capability_generation;
    uint32_t forward_link_generation;
    uint32_t reverse_link_generation;
    uint16_t forward_link_id;
    uint16_t reverse_link_id;
    uint8_t capability_digest[UCN_I_REALTIME_DIGEST_BYTES];
    uint8_t authenticated;
    uint8_t frozen;
    uint8_t directional;
    uint8_t asymmetry_known;
    uint32_t max_asymmetry_us;
} ucn_i_realtime_path_facts_t;

typedef struct ucn_i_realtime_domain_config {
    ucn_i_realtime_uncertainty_t base_uncertainty;
    ucn_i_realtime_path_facts_t path;
    uint64_t sync_timeout_us;
    uint64_t max_holdover_us;
    uint32_t domain_generation;
    uint32_t oscillator_uncertainty_ppb;
    uint32_t max_offset_jump_us;
    uint16_t clock_domain_id;
    uint8_t lock_sample_count;
    uint8_t reserved_zero;
} ucn_i_realtime_domain_config_t;

typedef struct ucn_i_realtime_domain_slot {
    ucn_i_realtime_domain_config_t config;
    ucn_i_realtime_domain_config_t pending_config;
    int64_t samples[UCN_I_REALTIME_SAMPLE_WINDOW];
    int64_t offset_us;
    uint64_t last_sample_local_us;
    uint64_t last_output_local_us;
    uint64_t last_output_domain_us;
    uint32_t uncertainty_us;
    uint64_t pending_domain_id;
    uint64_t pending_foundation_transaction_id;
    uint64_t pending_expected_record_generation;
    uint64_t pending_absolute_deadline_us;
    uint16_t pending_persistence_domain_generation;
    uint16_t handle_generation;
    uint8_t sample_count;
    uint8_t sample_cursor;
    uint8_t consecutive_samples;
    uint8_t phase;
    uint8_t occupied;
    uint8_t has_output_high_water;
    uint8_t persistence_bound;
    uint8_t reserved_zero;
    ucn_handle_t persistence_handle;
    uint8_t pending_body_digest[UCN_I_REALTIME_DIGEST_BYTES];
    uint8_t current_body_digest[UCN_I_REALTIME_DIGEST_BYTES];
} ucn_i_realtime_domain_slot_t;

typedef struct ucn_i_realtime_durability {
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t expected_record_generation;
    uint64_t absolute_deadline_us;
    ucn_handle_t volatile_continuation;
    uint16_t persistence_domain_generation;
    uint16_t schema_id;
    uint16_t schema_version;
} ucn_i_realtime_durability_t;

typedef struct ucn_i_realtime_requirement {
    ucn_i_realtime_durability_t durability;
    uint8_t body[UCN_I_REALTIME_DOMAIN_RECORD_BYTES];
    uint8_t canonical_body_digest[UCN_I_REALTIME_DIGEST_BYTES];
    uint8_t expected_body_digest[UCN_I_REALTIME_DIGEST_BYTES];
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint16_t caller_owner_instance;
    uint16_t operation_kind;
} ucn_i_realtime_requirement_t;

typedef struct ucn_i_realtime_proof {
    ucn_handle_t persistence_handle;
    uint64_t domain_id;
    uint64_t foundation_transaction_id;
    uint64_t record_generation;
    uint64_t witness_generation;
    uint32_t runtime_instance;
    uint32_t body_bytes;
    uint16_t persistence_domain_generation;
    uint16_t persistence_owner_instance;
    uint16_t caller_owner_instance;
    uint16_t schema_id;
    uint16_t schema_version;
    uint16_t operation_kind;
    uint16_t reserved_zero;
    uint8_t body_digest[UCN_I_REALTIME_DIGEST_BYTES];
} ucn_i_realtime_proof_t;

typedef struct ucn_i_realtime_sync_sample {
    ucn_i_realtime_path_facts_t path;
    int64_t offset_us;
    uint64_t sample_local_us;
    uint32_t uncertainty_us;
    uint16_t clock_domain_id;
    uint8_t valid_sync_sample;
    uint8_t reserved_zero;
} ucn_i_realtime_sync_sample_t;

typedef struct ucn_i_realtime_clock_view {
    uint64_t domain_time_us;
    uint64_t holdover_age_us;
    uint32_t domain_generation;
    uint32_t uncertainty_us;
    uint16_t clock_domain_id;
    uint8_t phase;
    uint8_t available;
    uint8_t holdover;
    uint8_t reserved_zero[3];
} ucn_i_realtime_clock_view_t;

typedef struct ucn_i_realtime_sync_announce {
    ucn_i_realtime_path_facts_t path;
    ucn_i_realtime_event_key_t t2_member_rx;
    uint64_t t2_member_rx_us;
    uint64_t absolute_deadline_us;
    uint32_t domain_generation;
    uint32_t sync_sequence;
    uint32_t t2_uncertainty_us;
    uint16_t clock_domain_id;
    uint16_t reserved_zero;
} ucn_i_realtime_sync_announce_t;

typedef struct ucn_i_realtime_sync_response {
    ucn_i_realtime_path_facts_t path;
    uint64_t t1_master_tx_us;
    uint64_t t4_master_rx_us;
    uint32_t domain_generation;
    uint32_t sync_sequence;
    uint32_t t1_uncertainty_us;
    uint32_t t4_uncertainty_us;
    uint16_t clock_domain_id;
    uint16_t reserved_zero;
} ucn_i_realtime_sync_response_t;

typedef struct ucn_i_realtime_sync_slot {
    ucn_i_realtime_path_facts_t path;
    ucn_i_realtime_event_key_t t2;
    ucn_i_realtime_event_key_t t3;
    uint64_t t2_us;
    uint64_t t3_us;
    uint64_t deadline_us;
    uint32_t domain_generation;
    uint32_t sync_sequence;
    uint32_t t2_uncertainty_us;
    uint32_t t3_uncertainty_us;
    uint16_t clock_domain_id;
    uint16_t handle_generation;
    uint8_t occupied;
    uint8_t t3_bound;
    uint8_t reserved_zero[2];
} ucn_i_realtime_sync_slot_t;

typedef struct ucn_i_realtime_release_slot {
    ucn_i_realtime_event_key_t key;
    uint16_t generation;
    uint8_t occupied;
    uint8_t reserved_zero;
} ucn_i_realtime_release_slot_t;

typedef struct ucn_i_realtime_release_view {
    ucn_i_realtime_event_key_t key;
    uint16_t slot;
    uint16_t generation;
} ucn_i_realtime_release_view_t;

typedef struct ucn_i_realtime_prepared {
    ucn_i_realtime_envelope_t envelope;
    uint8_t metadata_present;
    uint8_t fell_back;
    uint8_t reserved_zero[6];
} ucn_i_realtime_prepared_t;

typedef struct ucn_i_realtime_receive_facts {
    ucn_i_realtime_envelope_t envelope;
    uint64_t local_now_us;
    uint16_t endpoint;
    uint8_t metadata_present;
    uint8_t e2e_authenticated;
    uint8_t source_acl_authorized;
    uint8_t reserved_zero;
} ucn_i_realtime_receive_facts_t;

typedef struct ucn_i_realtime_receive_view {
    uint64_t age_upper_us;
    uint32_t combined_uncertainty_us;
    uint8_t accepted;
    uint8_t reason;
    uint8_t reserved_zero[2];
} ucn_i_realtime_receive_view_t;

typedef struct ucn_i_realtime_owner {
    uint32_t magic;
    uint32_t runtime_instance;
    uint16_t schema;
    uint16_t owner_instance;
    ucn_i_lock_ops_t state_lock;
    ucn_i_realtime_policy_slot_t policies[UCN_I_REALTIME_POLICY_COUNT];
    ucn_i_realtime_domain_slot_t domains[UCN_I_REALTIME_DOMAIN_COUNT];
    ucn_i_realtime_sync_slot_t sync[UCN_I_REALTIME_SYNC_COUNT];
    ucn_i_realtime_release_slot_t releases[UCN_I_REALTIME_RELEASE_COUNT];
    uint16_t release_cursor;
    uint16_t next_sync_generation;
} ucn_i_realtime_owner_t;

bool ucn_i_realtime_envelope_valid(
    const ucn_i_realtime_envelope_t *envelope);
ucn_result_t ucn_i_realtime_envelope_encode(
    const ucn_i_realtime_envelope_t *envelope,
    uint8_t output[UCN_I_REALTIME_ENVELOPE_BYTES]);
ucn_result_t ucn_i_realtime_envelope_decode(
    const uint8_t *input, size_t input_length,
    ucn_i_realtime_envelope_t *envelope_out);
ucn_result_t ucn_i_realtime_uncertainty_class_encode(
    bool known, uint64_t upper_bound_us, uint8_t *class_out);
ucn_result_t ucn_i_realtime_uncertainty_class_decode(
    uint8_t uncertainty_class, bool *known_out,
    uint32_t *upper_bound_us_out);
ucn_result_t ucn_i_realtime_uncertainty_aggregate(
    const ucn_i_realtime_uncertainty_t *components,
    uint32_t *upper_bound_us_out);

ucn_result_t ucn_i_realtime_owner_init(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_config_t *config);
ucn_result_t ucn_i_realtime_owner_destroy(ucn_i_realtime_owner_t *owner);
ucn_result_t ucn_i_realtime_local_stamp(
    uint64_t capture_time_us, bool hardware_capture,
    ucn_i_realtime_envelope_t *envelope_out);
ucn_result_t ucn_i_realtime_domain_record_encode(
    const ucn_i_realtime_domain_config_t *config,
    uint8_t body_out[UCN_I_REALTIME_DOMAIN_RECORD_BYTES]);
ucn_result_t ucn_i_realtime_domain_prepare(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_domain_config_t *config,
    const ucn_i_realtime_durability_t *durability,
    ucn_handle_t *domain_out,
    ucn_i_realtime_requirement_t *requirement_out);
ucn_result_t ucn_i_realtime_domain_bind_persistence(
    ucn_i_realtime_owner_t *owner, ucn_handle_t domain,
    ucn_handle_t persistence_handle,
    const uint8_t published_body_digest[UCN_I_REALTIME_DIGEST_BYTES]);
ucn_result_t ucn_i_realtime_domain_accept_proof(
    ucn_i_realtime_owner_t *owner, ucn_handle_t domain,
    const ucn_i_realtime_proof_t *proof, uint64_t now_us);
ucn_result_t ucn_i_realtime_domain_import(
    ucn_i_realtime_owner_t *owner, const uint8_t *body,
    size_t body_bytes, const ucn_i_realtime_durability_t *durability,
    const uint8_t published_body_digest[UCN_I_REALTIME_DIGEST_BYTES],
    ucn_handle_t *domain_out);
ucn_result_t ucn_i_realtime_domain_accept_sample(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_sync_sample_t *sample);
ucn_result_t ucn_i_realtime_domain_get_clock(
    ucn_i_realtime_owner_t *owner, uint16_t clock_domain_id,
    uint64_t local_now_us, ucn_i_realtime_clock_view_t *view_out);
ucn_result_t ucn_i_realtime_step(ucn_i_realtime_owner_t *owner,
                                  uint64_t now_us);
ucn_result_t ucn_i_realtime_sync_begin(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_sync_announce_t *announce,
    ucn_handle_t *sync_out);
ucn_result_t ucn_i_realtime_sync_bind_t3(
    ucn_i_realtime_owner_t *owner, ucn_handle_t sync,
    const ucn_i_realtime_event_key_t *t3_event,
    uint64_t t3_member_tx_us, uint32_t t3_uncertainty_us);
ucn_result_t ucn_i_realtime_sync_complete(
    ucn_i_realtime_owner_t *owner, ucn_handle_t sync,
    const ucn_i_realtime_sync_response_t *response,
    uint64_t member_receive_local_us);
ucn_result_t ucn_i_realtime_release_peek(
    ucn_i_realtime_owner_t *owner,
    ucn_i_realtime_release_view_t *release_out);
ucn_result_t ucn_i_realtime_release_ack(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_release_view_t *release);
ucn_result_t ucn_i_realtime_policy_set(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_policy_t *policy);
ucn_result_t ucn_i_realtime_prepare(
    ucn_i_realtime_owner_t *owner, uint16_t endpoint,
    uint64_t local_capture_us, uint32_t sample_capture_bound_us,
    bool hardware_capture, ucn_i_realtime_prepared_t *prepared_out);
ucn_result_t ucn_i_realtime_admit(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_receive_facts_t *facts,
    ucn_i_realtime_receive_view_t *view_out);

/* Private cross-file helpers; caller holds owner->state_lock. */
ucn_result_t ucn_i_realtime_p_accept_sample_locked(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_sync_sample_t *sample);
ucn_result_t ucn_i_realtime_p_sync_step_locked(
    ucn_i_realtime_owner_t *owner, uint64_t now_us);

#endif
