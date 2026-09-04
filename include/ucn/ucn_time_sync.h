#ifndef UCN_TIME_SYNC_H
#define UCN_TIME_SYNC_H

/* Optional four-message Time Sync v1 semantic protocol.
 * 可选的四报文 Time Sync v1 语义协议。 */

#include "ucn/ucn_time_domain.h"
#include "ucn/ucn_timed_link.h"
#include "ucn/ucn_path.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_TIME_SYNC_ENDPOINT ((ucn_endpoint_t)0xBCU)
#define UCN_TIME_FOLLOW_UP_ENDPOINT ((ucn_endpoint_t)0xBDU)
#define UCN_TIME_DELAY_REQ_ENDPOINT ((ucn_endpoint_t)0xBEU)
#define UCN_TIME_DELAY_RESP_ENDPOINT ((ucn_endpoint_t)0xBFU)
#define UCN_TIME_SYNC_CONTROL_VERSION ((uint8_t)1U)
#define UCN_TIME_SYNC_SEQUENCE_MAX UINT32_C(0x7FFFFFFF)
#define UCN_TIME_SYNC_MAX_PAYLOAD_BYTES ((size_t)31U)

#ifndef UCN_TIME_SYNC_MAX_PEERS
#define UCN_TIME_SYNC_MAX_PEERS ((size_t)4U)
#endif

#define UCN_TIME_SYNC_MASTER_RELEASE_CAPACITY UCN_TIME_SYNC_MAX_PEERS
#define UCN_TIME_SYNC_MEMBER_RELEASE_CAPACITY ((size_t)1U)

typedef char ucn_time_sync_peer_capacity_must_be_nonzero[
    UCN_TIME_SYNC_MAX_PEERS > 0U ? 1 : -1];

typedef struct ucn_time_path_identity {
    ucn_node_id_t owner_node_id;
    ucn_session_id_t owner_session_id;
    ucn_path_id_t path_id;
    ucn_node_id_t destination_node_id;
} ucn_time_path_identity_t;

typedef struct ucn_time_path_contract {
    ucn_time_path_identity_t forward_path;
    ucn_time_path_identity_t reverse_path;
    uint32_t max_asymmetry_us;
    bool installed;
    bool immutable_for_transaction;
    bool ordinary_dynamic_route;
    bool asymmetry_bound_known;
} ucn_time_path_contract_t;

typedef uint8_t ucn_time_path_admission_t;
enum {
    UCN_TIME_PATH_REJECTED = 0U,
    UCN_TIME_PATH_EFFECTIVE_SAMPLE = 1U,
    UCN_TIME_PATH_DIAGNOSTIC_ONLY = 2U,
    UCN_TIME_PATH_FALLBACK_ONLY = 3U
};

typedef struct ucn_wire_time_txn_key {
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint32_t sync_sequence;
    ucn_node_id_t master_node_id;
    ucn_session_id_t master_session_id;
    ucn_node_id_t member_node_id;
    ucn_session_id_t member_session_id;
    ucn_time_path_identity_t forward_path;
    ucn_time_path_identity_t reverse_path;
} ucn_wire_time_txn_key_t;

typedef uint8_t ucn_time_control_role_t;
enum {
    UCN_TIME_CONTROL_SYNC = 1U,
    UCN_TIME_CONTROL_FOLLOW_UP = 2U,
    UCN_TIME_CONTROL_DELAY_REQ = 3U,
    UCN_TIME_CONTROL_DELAY_RESP = 4U
};

typedef struct ucn_time_control_outer {
    ucn_node_id_t source_node_id;
    ucn_session_id_t source_session_id;
    ucn_node_id_t destination_node_id;
    ucn_time_path_identity_t path;
    bool e2e_authenticated;
} ucn_time_control_outer_t;

typedef struct ucn_time_control_message {
    ucn_time_control_role_t role;
    ucn_wire_time_txn_key_t key;
    uint64_t timestamp_us;
    bool authenticated_outer;
} ucn_time_control_message_t;

typedef struct ucn_time_sync_master_config {
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    ucn_node_id_t master_node_id;
    ucn_session_id_t master_session_id;
    uint64_t transaction_timeout_us;
} ucn_time_sync_master_config_t;

typedef struct ucn_time_sync_master_pending {
    ucn_wire_time_txn_key_t key;
    ucn_time_event_key_t t1_key;
    ucn_time_event_key_t t4_key;
    uint64_t deadline_us;
    uint64_t t1_us;
    uint64_t t4_us;
    bool occupied;
    bool t1_complete;
    bool t4_complete;
} ucn_time_sync_master_pending_t;

typedef struct ucn_time_sync_master {
    ucn_time_sync_master_config_t config;
    ucn_time_sync_master_pending_t pending[UCN_TIME_SYNC_MAX_PEERS];
    uint32_t next_sequence;
    uint32_t started;
    uint32_t completed;
    uint32_t timed_out;
    ucn_time_event_key_t
        released_events[UCN_TIME_SYNC_MASTER_RELEASE_CAPACITY];
    size_t released_event_count;
    bool initialized;
} ucn_time_sync_master_t;

typedef struct ucn_time_sync_member_config {
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    ucn_node_id_t master_node_id;
    ucn_session_id_t master_session_id;
    ucn_node_id_t member_node_id;
    ucn_session_id_t member_session_id;
    uint64_t transaction_timeout_us;
    ucn_realtime_uncertainty_components_t uncertainty_components;
} ucn_time_sync_member_config_t;

typedef struct ucn_time_sync_member_pending {
    ucn_wire_time_txn_key_t key;
    ucn_time_event_key_t t2_key;
    ucn_time_event_key_t t3_key;
    uint64_t deadline_us;
    uint64_t t1_us;
    uint64_t t2_us;
    uint64_t t3_us;
    ucn_time_path_admission_t path_admission;
    uint32_t max_asymmetry_us;
    bool occupied;
    bool t1_complete;
    bool t2_complete;
    bool t3_complete;
} ucn_time_sync_member_pending_t;

typedef struct ucn_time_sync_member {
    ucn_time_sync_member_config_t config;
    ucn_time_sync_member_pending_t pending;
    uint32_t last_consumed_sequence;
    uint32_t completed;
    uint32_t diagnostic_completed;
    uint32_t timed_out;
    ucn_time_event_key_t
        released_events[UCN_TIME_SYNC_MEMBER_RELEASE_CAPACITY];
    size_t released_event_count;
    bool initialized;
} ucn_time_sync_member_t;

/* EN: Validates one directional installed Path identity.
 * 中文：校验一条已安装的定向 Path 身份。 */
bool ucn_time_path_identity_is_valid(const ucn_time_path_identity_t *identity);

/* EN: Validates the complete cross-node Time Sync transaction key.
 * 中文：校验完整的跨节点时间同步事务键。 */
bool ucn_wire_time_txn_key_is_valid(const ucn_wire_time_txn_key_t *key);

/* EN: Compares two transaction keys byte-for-semantic-field.
 * 中文：按全部语义字段比较两个时间事务键。 */
bool ucn_wire_time_txn_key_equal(const ucn_wire_time_txn_key_t *left,
                                 const ucn_wire_time_txn_key_t *right);

/* EN: Classifies a Path as effective, diagnostic-only or fallback-only.
 * 中文：把 Path 分类为有效、仅诊断或仅回退。 */
ucn_result_t ucn_time_sync_path_admit(
    const ucn_time_path_contract_t *contract,
    ucn_realtime_requirement_t requirement,
    ucn_time_path_admission_t *admission);

/* EN: Encodes one role-specific control payload (31/19/11/19 bytes).
 * 中文：编码一种角色专属控制载荷（31/19/11/19 字节）。
 *
 * The current direction's
 * authenticated Source/Session/Destination/Path remain in `outer`; only SYNC
 * carries the reverse Path and Member Session needed to establish a key. */
ucn_result_t ucn_time_control_payload_encode(
    const ucn_time_control_message_t *message,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);

/* EN: Decodes the opening SYNC and creates its authenticated transaction key.
 * 中文：解码起始 SYNC，并建立其经过认证的事务键。 */
ucn_result_t ucn_time_control_sync_decode(
    const ucn_time_control_outer_t *outer,
    const uint8_t *payload,
    size_t payload_length,
    ucn_time_control_message_t *message);

/* EN: Decodes a later message against an existing exact transaction key.
 * 中文：依据已有精确事务键解码后续控制消息。 */
ucn_result_t ucn_time_control_existing_decode(
    ucn_time_control_role_t expected_role,
    const ucn_time_control_outer_t *outer,
    const ucn_wire_time_txn_key_t *expected_key,
    const uint8_t *payload,
    size_t payload_length,
    ucn_time_control_message_t *message);

/* EN: Initializes the bounded Master transaction owner.
 * 中文：初始化有界的 Master 事务 Owner。 */
ucn_result_t ucn_time_sync_master_init(
    ucn_time_sync_master_t *master,
    const ucn_time_sync_master_config_t *config);

/* EN: Initializes one Member transaction owner.
 * 中文：初始化一个 Member 事务 Owner。 */
ucn_result_t ucn_time_sync_member_init(
    ucn_time_sync_member_t *member,
    const ucn_time_sync_member_config_t *config);

/* EN: Allocates a Master pending slot and builds SYNC before T1 completion.
 * 中文：分配 Master pending 槽并在 T1 完成前构建 SYNC。 */
ucn_result_t ucn_time_sync_master_begin(
    ucn_time_sync_master_t *master,
    ucn_node_id_t member_node_id,
    ucn_session_id_t member_session_id,
    const ucn_time_path_contract_t *contract,
    ucn_realtime_requirement_t requirement,
    uint64_t now_us,
    const ucn_time_event_key_t *t1_key,
    ucn_time_control_message_t *sync_message);

/* EN: Binds the local T1 completion to the exact Master transaction.
 * 中文：把本地 T1 完成事件绑定到精确 Master 事务。 */
ucn_result_t ucn_time_sync_master_record_t1(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key,
    const ucn_time_event_key_t *event_key,
    uint64_t now_us,
    uint64_t t1_us);

/* EN: Builds FOLLOW_UP only after the matching T1 event completed.
 * 中文：仅在匹配 T1 事件完成后构建 FOLLOW_UP。 */
ucn_result_t ucn_time_sync_master_build_follow_up(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key,
    uint64_t now_us,
    ucn_time_control_message_t *message);

/* EN: Opens the Member transaction and atomically binds its T2 event.
 * 中文：打开 Member 事务并原子绑定其 T2 事件。 */
ucn_result_t ucn_time_sync_member_receive_sync(
    ucn_time_sync_member_t *member,
    const ucn_time_control_message_t *message,
    const ucn_time_path_contract_t *contract,
    ucn_realtime_requirement_t requirement,
    uint64_t now_us,
    const ucn_time_event_key_t *t2_key,
    uint64_t t2_us);

/* EN: Accepts the matching FOLLOW_UP and records T1.
 * 中文：接收匹配的 FOLLOW_UP 并记录 T1。 */
ucn_result_t ucn_time_sync_member_receive_follow_up(
    ucn_time_sync_member_t *member,
    const ucn_time_control_message_t *message,
    uint64_t now_us);

/* EN: Reserves T3 ownership and builds DELAY_REQ.
 * 中文：保留 T3 所有权并构建 DELAY_REQ。 */
ucn_result_t ucn_time_sync_member_build_delay_req(
    ucn_time_sync_member_t *member,
    const ucn_time_event_key_t *t3_key,
    uint64_t now_us,
    ucn_time_control_message_t *message);

/* EN: Binds the local T3 completion to the Member transaction.
 * 中文：把本地 T3 完成事件绑定到 Member 事务。 */
ucn_result_t ucn_time_sync_member_record_t3(
    ucn_time_sync_member_t *member,
    const ucn_time_event_key_t *event_key,
    uint64_t now_us,
    uint64_t t3_us);

/* EN: Accepts DELAY_REQ and atomically binds the Master's local T4 event.
 * 中文：接收 DELAY_REQ 并原子绑定 Master 的本地 T4 事件。 */
ucn_result_t ucn_time_sync_master_receive_delay_req(
    ucn_time_sync_master_t *master,
    const ucn_time_control_message_t *message,
    const ucn_time_event_key_t *t4_key,
    uint64_t now_us,
    uint64_t t4_us);

/* EN: Builds DELAY_RESP only after the matching T4 event completed.
 * 中文：仅在匹配 T4 事件完成后构建 DELAY_RESP。 */
ucn_result_t ucn_time_sync_master_build_delay_resp(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key,
    uint64_t now_us,
    ucn_time_control_message_t *message);

/* EN: Completes T1..T4 and returns an effective or diagnostic sample.
 * 中文：完成 T1～T4 计算并返回有效或诊断样本。 */
ucn_result_t ucn_time_sync_member_receive_delay_resp(
    ucn_time_sync_member_t *member,
    const ucn_time_control_message_t *message,
    uint64_t now_us,
    uint64_t local_sample_us,
    ucn_time_sync_sample_t *sample);

/* EN: Releases the Master slot after the response was delivered.
 * 中文：响应交付后释放 Master pending 槽。 */
ucn_result_t ucn_time_sync_master_complete(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key,
    uint64_t now_us);

/* EN: Expires Master transactions at the exact half-open deadline.
 * 中文：在半开区间精确 deadline 到达时清理 Master 事务。 */
ucn_result_t ucn_time_sync_master_step(ucn_time_sync_master_t *master,
                                       uint64_t now_us);

/* EN: Expires the Member transaction at the exact half-open deadline.
 * 中文：在半开区间精确 deadline 到达时清理 Member 事务。 */
ucn_result_t ucn_time_sync_member_step(ucn_time_sync_member_t *member,
                                       uint64_t now_us);

/* EN: Aborts one exact Master transaction before Path switch/reopen and
 * queues every incomplete local event for Owner retirement.
 * 中文：在切路或 reopen 前中止精确 Master 事务，并把未完成本地事件交给
 * Owner 回收。 */
ucn_result_t ucn_time_sync_master_abort(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key);

/* EN: Aborts the exact Member transaction before Path switch/reopen.
 * 中文：在切路或 reopen 前中止精确 Member 事务。 */
ucn_result_t ucn_time_sync_member_abort(
    ucn_time_sync_member_t *member,
    const ucn_wire_time_txn_key_t *key);

/* EN: Peeks one abandoned Master event without removing the obligation. The
 * Owner retires it in Timed Link, then acknowledges the exact key below.
 * 中文：查看一个废弃 Master 事件但不移除义务；Owner 先在 Timed Link 中
 * 退休它，再用下方接口确认精确 key。 */
ucn_result_t ucn_time_sync_master_peek_released_event(
    const ucn_time_sync_master_t *master,
    ucn_time_event_key_t *key);

/* EN: Removes the exact Master obligation only after retirement succeeds.
 * 中文：仅在退休成功后移除精确的 Master 回收义务。 */
ucn_result_t ucn_time_sync_master_ack_released_event(
    ucn_time_sync_master_t *master,
    const ucn_time_event_key_t *key);

/* EN: Peeks one abandoned Member event without removing the obligation.
 * 中文：查看一个废弃 Member 事件但不移除回收义务。 */
ucn_result_t ucn_time_sync_member_peek_released_event(
    const ucn_time_sync_member_t *member,
    ucn_time_event_key_t *key);

/* EN: Removes the exact Member obligation only after retirement succeeds.
 * 中文：仅在退休成功后移除精确的 Member 回收义务。 */
ucn_result_t ucn_time_sync_member_ack_released_event(
    ucn_time_sync_member_t *member,
    const ucn_time_event_key_t *key);

#ifdef __cplusplus
}
#endif

#endif
