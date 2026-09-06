#ifndef UCN_V6_CLUSTER_H
#define UCN_V6_CLUSTER_H

#include "ucn/v6/ucn_v6_owner.h"
#include "ucn/v6/ucn_v6_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_CLUSTER_RECORD_VERSION UINT16_C(3)
#define UCN_V6_CLUSTER_RECORD_BYTES ((size_t)8192U)
#define UCN_V6_CLUSTER_CONTROL_VERSION ((uint8_t)1U)
#define UCN_V6_CLUSTER_CONTROL_BYTES ((size_t)120U)
#define UCN_V6_CLUSTER_DIRECTORY_VERSION ((uint8_t)1U)
#define UCN_V6_CLUSTER_DIRECTORY_BYTES ((size_t)108U)
#define UCN_V6_CLUSTER_AUTHORITY_PROOF_DIGEST_BYTES ((size_t)16U)

typedef enum ucn_v6_cluster_role {
    UCN_V6_CLUSTER_OBSERVER = 0,
    UCN_V6_CLUSTER_MEMBER = 1,
    UCN_V6_CLUSTER_BACKUP = 2,
    UCN_V6_CLUSTER_HEAD = 3,
    UCN_V6_CLUSTER_RECOVERY_HEAD = 4,
    UCN_V6_CLUSTER_FENCED = 5
} ucn_v6_cluster_role_t;

typedef enum ucn_v6_cluster_phase {
    UCN_V6_CLUSTER_PHASE_STABLE = 0,
    UCN_V6_CLUSTER_PHASE_JOINT = 1,
    UCN_V6_CLUSTER_PHASE_TAKEOVER = 2,
    UCN_V6_CLUSTER_PHASE_HANDOVER = 3,
    UCN_V6_CLUSTER_PHASE_RECOVERY = 4,
    UCN_V6_CLUSTER_PHASE_REKEY = 5,
    UCN_V6_CLUSTER_PHASE_FAULT = 6
} ucn_v6_cluster_phase_t;

typedef enum ucn_v6_cluster_control_kind {
    UCN_V6_CLUSTER_CTL_ADVERTISE = 1,
    UCN_V6_CLUSTER_CTL_JOIN = 2,
    UCN_V6_CLUSTER_CTL_CONFIG_PREPARE = 3,
    UCN_V6_CLUSTER_CTL_CONFIG_ACK = 4,
    UCN_V6_CLUSTER_CTL_CONFIG_COMMIT = 5,
    UCN_V6_CLUSTER_CTL_BACKUP_ASSIGN = 6,
    UCN_V6_CLUSTER_CTL_BACKUP_READY = 7,
    UCN_V6_CLUSTER_CTL_TAKEOVER_VOTE = 8,
    UCN_V6_CLUSTER_CTL_TAKEOVER_COMMIT = 9,
    UCN_V6_CLUSTER_CTL_HANDOVER_PREPARE = 10,
    UCN_V6_CLUSTER_CTL_HANDOVER_READY = 11,
    UCN_V6_CLUSTER_CTL_HANDOVER_COMMIT = 12,
    UCN_V6_CLUSTER_CTL_RECOVERY_VOTE = 13,
    UCN_V6_CLUSTER_CTL_RECOVERY_COMMIT = 14,
    UCN_V6_CLUSTER_CTL_REKEY_COMMIT = 15
} ucn_v6_cluster_control_kind_t;

typedef enum ucn_v6_cluster_transition_kind {
    UCN_V6_CLUSTER_TRANSITION_NONE = 0,
    UCN_V6_CLUSTER_TRANSITION_TAKEOVER = 1,
    UCN_V6_CLUSTER_TRANSITION_HANDOVER = 2,
    UCN_V6_CLUSTER_TRANSITION_RECOVERY = 3,
    UCN_V6_CLUSTER_TRANSITION_REKEY = 4
} ucn_v6_cluster_transition_kind_t;

typedef struct ucn_v6_cluster_epoch {
    uint32_t cluster_id;
    uint32_t term;
    ucn_v6_principal_t head_principal;
    ucn_v6_binding_key_t head_binding;
} ucn_v6_cluster_epoch_t;

typedef struct ucn_v6_cluster_voter {
    ucn_v6_principal_t principal;
    ucn_v6_binding_key_t binding;
} ucn_v6_cluster_voter_t;

typedef struct ucn_v6_cluster_config {
    bool valid;
    uint32_t config_id;
    uint32_t generation;
    uint8_t voter_count;
    ucn_v6_cluster_voter_t voters[UCN_V6_CONFIG_CLUSTER_VOTERS];
} ucn_v6_cluster_config_t;

typedef struct ucn_v6_cluster_vote_id {
    bool valid;
    ucn_v6_cluster_epoch_t source_epoch;
    ucn_v6_principal_t candidate_principal;
    ucn_v6_binding_key_t candidate_binding;
    uint32_t backup_generation;
} ucn_v6_cluster_vote_id_t;

typedef struct ucn_v6_cluster_backup {
    bool valid;
    ucn_v6_principal_t principal;
    ucn_v6_binding_key_t binding;
    uint32_t generation;
    bool ready;
    uint64_t config_transaction_id;
    uint32_t acknowledged_config_generation;
} ucn_v6_cluster_backup_t;

typedef struct ucn_v6_cluster_authority_proof_ref {
    /* EN: Wire-visible certificate identity.  This is not a local array
     * index.  proof_id is stable within one remote Epoch and generation is
     * strictly non-decreasing; the verifier resolves both in the
     * authenticated remote-Cluster proof namespace.
     * 中文：Wire 可见的证书身份；它不是本地数组下标。同一远端 Epoch 内
     * proof_id 固定且 generation 严格不回退，验证器在经过认证的远端
     * Cluster 证明命名空间中解析二者。 */
    uint32_t proof_id;
    uint32_t generation;
} ucn_v6_cluster_authority_proof_ref_t;

/* EN: Durable identity of the exact admission/capability instance that cast
 * one transition vote.  A bitmap bit without matching evidence is invalid;
 * re-admission, Link/session replacement or Capability replacement cannot
 * revive the old promise.
 * 中文：一张转换票对应的精确准入/Capability 实例持久证据。没有匹配证据的
 * bitmap 位无效；重新准入、Link/Session 或 Capability 替换都不能复活旧承诺。 */
typedef struct ucn_v6_cluster_transition_vote_evidence {
    bool valid;
    ucn_v6_session_key_t session;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint32_t capability_generation;
    uint8_t capability_digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
} ucn_v6_cluster_transition_vote_evidence_t;

typedef struct ucn_v6_cluster_transition_proof {
    bool valid;
    bool ready;
    ucn_v6_cluster_transition_kind_t kind;
    uint64_t transaction_id;
    uint32_t started_boot_incarnation;
    uint64_t deadline_us;
    ucn_v6_cluster_epoch_t old_epoch;
    ucn_v6_cluster_epoch_t target_epoch;
    uint32_t target_config_id;
    uint32_t target_config_generation;
    ucn_v6_cluster_config_t target_config;
    uint32_t old_voter_bitmap;
    uint32_t new_voter_bitmap;
    ucn_v6_cluster_transition_vote_evidence_t
        old_vote_evidence[UCN_V6_CONFIG_CLUSTER_VOTERS];
    ucn_v6_cluster_transition_vote_evidence_t
        new_vote_evidence[UCN_V6_CONFIG_CLUSTER_VOTERS];
    ucn_v6_cluster_authority_proof_ref_t target_authority_proof;
} ucn_v6_cluster_transition_proof_t;

typedef struct ucn_v6_cluster_tombstone {
    bool occupied;
    uint32_t retired_cluster_id;
    uint32_t replacement_cluster_id;
    uint64_t transaction_id;
} ucn_v6_cluster_tombstone_t;

typedef struct ucn_v6_cluster_snapshot {
    uint64_t record_generation;
    uint64_t transaction_high_water;
    uint32_t boot_incarnation;
    ucn_v6_cluster_role_t role;
    ucn_v6_cluster_phase_t phase;
    bool active_epoch_valid;
    ucn_v6_cluster_epoch_t active_epoch;
    bool max_epoch_valid;
    ucn_v6_cluster_epoch_t max_epoch;
    ucn_v6_cluster_config_t stable_config;
    bool joint_valid;
    uint64_t joint_transaction_id;
    ucn_v6_cluster_config_t joint_new_config;
    ucn_v6_cluster_backup_t backup;
    ucn_v6_cluster_vote_id_t last_vote;
    ucn_v6_cluster_transition_proof_t transition;
    uint8_t tombstone_count;
    ucn_v6_cluster_tombstone_t
        tombstones[UCN_V6_CONFIG_CLUSTER_TOMBSTONES];
    bool authority_fenced;
} ucn_v6_cluster_snapshot_t;

typedef struct ucn_v6_cluster_member {
    bool occupied;
    ucn_v6_session_key_t session;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint32_t capability_generation;
    uint32_t capability_feature_bits;
    bool voter;
    bool backup_eligible;
    uint64_t discovery_deadline_us;
    uint64_t capability_deadline_us;
    uint64_t lease_deadline_us;
} ucn_v6_cluster_member_t;

typedef struct ucn_v6_cluster_control {
    ucn_v6_cluster_control_kind_t kind;
    uint16_t flags;
    uint64_t transaction_id;
    ucn_v6_cluster_epoch_t old_epoch;
    ucn_v6_cluster_epoch_t target_epoch;
    uint32_t config_id;
    uint32_t config_generation;
    uint32_t backup_generation;
    uint32_t old_voter_bitmap;
    uint32_t new_voter_bitmap;
    ucn_v6_cluster_authority_proof_ref_t authority_proof;
} ucn_v6_cluster_control_t;

/* EN: A trusted proof Owner returns this complete, immutable verification
 * result only after checking the voter evidence/signatures for both Stable
 * and (when present) Joint Config.  It is an in-process view, never Wire data.
 * 中文：可信 Proof Owner 仅在校验 Stable 及（存在时）Joint Config 的投票
 * 证据/签名后返回此完整且不可变的验证结果；它是进程内视图，不是 Wire 数据。 */
typedef struct ucn_v6_cluster_authority_proof_view {
    bool valid;
    ucn_v6_cluster_authority_proof_ref_t ref;
    ucn_v6_cluster_epoch_t epoch;
    uint32_t stable_config_id;
    uint32_t stable_config_generation;
    bool joint_valid;
    uint32_t joint_config_id;
    uint32_t joint_config_generation;
    bool stable_quorum_verified;
    bool joint_quorum_verified;
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
    uint8_t evidence_digest[UCN_V6_CLUSTER_AUTHORITY_PROOF_DIGEST_BYTES];
    uint64_t lease_deadline_us;
} ucn_v6_cluster_authority_proof_view_t;

typedef struct ucn_v6_cluster_authority_proof_owner_ops {
    void *context;
    /* EN: The resolved certificate must be immutable for its ref and remain
     * valid until lease_deadline_us.  The Provider owns voter/signature
     * evidence, restart recovery and no-ABA generation history; NOT_FOUND is
     * fail-closed and never authorizes a cached Directory fallback.
     * 中文：同一证明引用的解析结果必须不可变，并持续有效到 lease_deadline_us。
     * Provider 负责投票/签名证据、重启恢复及防 ABA 代际历史；NOT_FOUND 必须
     * 失败关闭，绝不能授权回退到缓存 Directory。 */
    ucn_v6_result_t (*resolve_verified)(
        void *context,
        const ucn_v6_cluster_authority_proof_ref_t *ref,
        uint64_t now_us,
        ucn_v6_cluster_authority_proof_view_t *view);
} ucn_v6_cluster_authority_proof_owner_ops_t;

typedef struct ucn_v6_cluster_directory_entry {
    bool occupied;
    uint32_t remote_cluster_id;
    ucn_v6_cluster_epoch_t remote_epoch;
    ucn_v6_session_key_t next_hop;
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
    ucn_v6_cluster_authority_proof_ref_t authority_proof;
    /* Authenticated relative lease; receiver derives its own monotonic
     * deadline and never compares another node's uptime directly. */
    uint64_t lease_duration_us;
} ucn_v6_cluster_directory_entry_t;

typedef struct ucn_v6_cluster_tunnel {
    bool occupied;
    uint64_t tunnel_id;
    uint32_t source_cluster_id;
    uint32_t destination_cluster_id;
    ucn_v6_route_path_ref_t route_ref;
    uint64_t deadline_us;
} ucn_v6_cluster_tunnel_t;

typedef struct ucn_v6_cluster_tunnel_request {
    uint64_t tunnel_id;
    uint32_t source_cluster_id;
    uint32_t destination_cluster_id;
    ucn_v6_route_path_ref_t route_ref;
    uint64_t deadline_us;
} ucn_v6_cluster_tunnel_request_t;

typedef struct ucn_v6_cluster_store_ops {
    void *context;
    ucn_v6_result_t (*load_generation_witness)(void *context,
                                                uint64_t *generation);
    ucn_v6_result_t (*reserve_generation_witness)(void *context,
                                                   uint64_t generation);
    ucn_v6_result_t (*load)(void *context, uint8_t *record,
                            size_t record_capacity, size_t *record_length);
    ucn_v6_result_t (*submit)(void *context, const uint8_t *record,
                              size_t record_length);
} ucn_v6_cluster_store_ops_t;

typedef struct ucn_v6_cluster_view {
    ucn_v6_cluster_role_t role;
    ucn_v6_cluster_phase_t phase;
    bool authority_active;
    bool quorum_met;
    bool joint_quorum_met;
    bool persistence_faulted;
    uint16_t members;
    uint16_t directory_entries;
    uint16_t tunnels;
    uint32_t persistence_commits;
    uint32_t rejected_security;
    uint32_t rejected_quorum;
    uint32_t rejected_replay;
} ucn_v6_cluster_view_t;

typedef struct ucn_v6_cluster_owner ucn_v6_cluster_owner_t;
typedef union ucn_v6_cluster_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_CLUSTER_OWNER_STORAGE_BYTES];
} ucn_v6_cluster_owner_storage_t;

/* EN: Validates an identity-bound Cluster Epoch.
 * 中文：校验绑定身份的 Cluster Epoch。 */
bool ucn_v6_cluster_epoch_is_valid(const ucn_v6_cluster_epoch_t *epoch);
/* EN: Validates one canonical voter configuration.
 * 中文：校验一个规范化的投票者配置。 */
bool ucn_v6_cluster_config_is_valid(const ucn_v6_cluster_config_t *config);
/* EN: Encodes/decodes the only accepted v6 Cluster Record schema. Decode
 * uses caller-owned scratch so malformed input cannot modify the output or
 * consume a multi-kilobyte MCU task stack frame.
 * 中文：编解码唯一接受的 v6 Cluster Record。解码使用调用方工作区，保证畸形输入
 * 不改写输出，也不在 MCU 任务栈上分配数 KB 临时对象。 */
ucn_v6_result_t ucn_v6_cluster_snapshot_encode(
    const ucn_v6_cluster_snapshot_t *snapshot,
    uint8_t output[UCN_V6_CLUSTER_RECORD_BYTES]);
ucn_v6_result_t ucn_v6_cluster_snapshot_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_cluster_snapshot_t *scratch,
    ucn_v6_cluster_snapshot_t *snapshot);
/* EN: Encodes/decodes the fixed semantic Cluster control payload.
 * 中文：编解码固定语义的 Cluster 控制载荷。 */
ucn_v6_result_t ucn_v6_cluster_control_encode(
    const ucn_v6_cluster_control_t *control,
    uint8_t output[UCN_V6_CLUSTER_CONTROL_BYTES]);
ucn_v6_result_t ucn_v6_cluster_control_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_cluster_control_t *control);
ucn_v6_result_t ucn_v6_cluster_directory_encode(
    const ucn_v6_cluster_directory_entry_t *entry,
    uint8_t output[UCN_V6_CLUSTER_DIRECTORY_BYTES]);
ucn_v6_result_t ucn_v6_cluster_directory_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_cluster_directory_entry_t *entry);

/* EN: Loads v6-only durable state and advances boot incarnation before use.
 * 中文：只加载 v6 持久状态，并在使用前持久推进启动代际。 */
ucn_v6_result_t ucn_v6_cluster_owner_init_in_place(
    void *storage, size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_principal_t *local_principal,
    const ucn_v6_binding_key_t *local_binding,
    uint32_t local_session_generation,
    const ucn_v6_capability_owner_t *capability_owner,
    const ucn_v6_route_owner_t *route_owner,
    const ucn_v6_cluster_authority_proof_owner_ops_t *authority_proof_owner,
    const ucn_v6_cluster_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_cluster_owner_t **owner);
/* EN: Creates the first durable Cluster and local Head authority.
 * 中文：创建首个持久 Cluster，并建立本地 Head 权威。 */
ucn_v6_result_t ucn_v6_cluster_create(
    ucn_v6_cluster_owner_t *owner, uint32_t cluster_id,
    const ucn_v6_cluster_config_t *initial_config, uint64_t now_us);
/* EN: Admits or refreshes one authenticated member lease.
 * 中文：准入或刷新一个已经认证的成员租约。 */
ucn_v6_result_t ucn_v6_cluster_admit_member(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_peer_ref_t *peer_ref,
    uint64_t now_us, uint64_t lease_duration_us);
/* EN: Assigns a committed voter as the durable Backup.
 * 中文：把已提交投票者指定为持久 Backup。 */
ucn_v6_result_t ucn_v6_cluster_assign_backup(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_cluster_voter_t *backup, uint32_t backup_generation,
    uint64_t now_us);
/* EN: Marks exact Backup staging and acknowledgement for a Config transaction.
 * 中文：记录指定 Backup 对 Config 事务的 staging 与确认。 */
ucn_v6_result_t ucn_v6_cluster_backup_ack_config(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    uint64_t now_us);
/* EN: Persists entry into C_old/C_new Joint Config before any acknowledgement.
 * 中文：在任何确认前持久进入 C_old/C_new Joint Config。 */
ucn_v6_result_t ucn_v6_cluster_prepare_joint(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    const ucn_v6_cluster_config_t *new_config, uint64_t now_us);
/* EN: Commits C_new only after exact old/new quorum and Backup admission.
 * 中文：仅在新旧双法定人数及 Backup 门禁通过后提交 C_new。 */
ucn_v6_result_t ucn_v6_cluster_commit_joint(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us);
/* EN: Durably aborts an exact Joint transaction without changing C_old.
 * 中文：持久中止精确 Joint 事务，保持 C_old 不变。 */
ucn_v6_result_t ucn_v6_cluster_abort_joint(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    const ucn_v6_cluster_config_t *expected_new_config, uint64_t now_us);

/* EN: Starts a fenced Backup takeover and binds the complete VoteId.
 * 中文：启动已围栏的 Backup 接管并绑定完整 VoteId。 */
ucn_v6_result_t ucn_v6_cluster_begin_takeover(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint32_t backup_generation, uint64_t now_us);
/* EN: Adds one authenticated committed-voter certificate bit.
 * 中文：加入一个经过认证的已提交投票者证书位。 */
ucn_v6_result_t ucn_v6_cluster_record_transition_vote(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    uint64_t now_us);
/* EN: Persists the takeover Epoch before activating local Head authority.
 * 中文：激活本地 Head 权威前先持久提交接管 Epoch。 */
ucn_v6_result_t ucn_v6_cluster_commit_takeover(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us);

/* EN: Persists a full old/target Epoch handover proposal.
 * 中文：持久记录完整旧/目标 Epoch 的 Handover 提案。 */
ucn_v6_result_t ucn_v6_cluster_begin_handover(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    const ucn_v6_cluster_epoch_t *target_epoch,
    const ucn_v6_cluster_config_t *target_config, uint64_t now_us);
/* EN: Records authenticated READY from the exact target authority.
 * 中文：记录来自精确目标权威的认证 READY。 */
ucn_v6_result_t ucn_v6_cluster_handover_ready(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    uint64_t now_us);
/* EN: Commits handover and permanently fences the old local authority.
 * 中文：提交 Handover，并永久围栏旧本地权威。 */
ucn_v6_result_t ucn_v6_cluster_commit_handover(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us);

/* EN: Starts Recovery with a new identity and no inherited authority.
 * 中文：以新身份启动 Recovery，不继承旧权威。 */
ucn_v6_result_t ucn_v6_cluster_begin_recovery(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    const ucn_v6_cluster_epoch_t *target_epoch, uint64_t now_us);
/* EN: Commits Recovery only after the stable voter quorum proves it.
 * 中文：仅在 Stable 投票者法定人数证明后提交 Recovery。 */
ucn_v6_result_t ucn_v6_cluster_commit_recovery(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us);

/* EN: Durably aborts the exact pending Takeover/Handover/Recovery. Expired
 * or pre-restart transactions remain abortable but never committable. A
 * READY Handover abort permanently fences the old Head.
 * 中文：持久中止精确匹配的 Takeover/Handover/Recovery。已超时或属于重启前
 * 的事务仍可中止但绝不能提交；READY 后的 Handover 中止会永久围栏旧 Head。 */
ucn_v6_result_t ucn_v6_cluster_abort_transition(
    ucn_v6_cluster_owner_t *owner,
    ucn_v6_cluster_transition_kind_t expected_kind,
    uint64_t transaction_id,
    const ucn_v6_cluster_epoch_t *expected_target_epoch,
    const ucn_v6_cluster_config_t *expected_target_config,
    uint64_t now_us);
/* EN: Rekeys into a never-retired Cluster identity and writes a Tombstone.
 * 中文：迁移到从未退休的 Cluster 身份并写入 Tombstone。 */
ucn_v6_result_t ucn_v6_cluster_rekey(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint32_t successor_cluster_id, uint64_t now_us);

/* EN: Installs a bounded Directory authority result only after the trusted
 * proof Owner validates the complete Epoch, Config quorum, Route, Path and
 * lease binding. ingress_peer_ref names the live last-Hop Peer; the remote
 * Head is independently bound to the immutable Wire Source and proof.  A
 * missing verifier or proof is fail-closed; this API never creates a
 * non-authoritative routing hint.
 * 中文：仅在可信 Proof Owner 校验完整 Epoch、Config 法定人数、Route、Path
 * 与租约绑定后安装有界 Directory 权威结果。ingress_peer_ref 表示最后一跳
 * 活跃 Peer；远端 Head 由不可变 Wire Source 和证明独立绑定。缺少验证器或
 * 证明必须失败关闭；本接口绝不创建非权威路由提示。 */
ucn_v6_result_t ucn_v6_cluster_directory_install(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_peer_ref_t *ingress_peer_ref,
    uint64_t now_us);
/* EN: Installs a local-authority bounded inter-cluster Tunnel.
 * 中文：安装由本地权威许可的有界跨簇 Tunnel。 */
ucn_v6_result_t ucn_v6_cluster_tunnel_install(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_cluster_tunnel_request_t *request,
    uint64_t now_us);
/* EN: Copies an unexpired Tunnel as the exact cross-cluster Transfer path.
 * 中文：复制未过期 Tunnel，作为精确的跨簇 Transfer 路径。 */
ucn_v6_result_t ucn_v6_cluster_copy_tunnel(
    const ucn_v6_cluster_owner_t *owner, uint64_t tunnel_id,
    uint64_t now_us, ucn_v6_cluster_tunnel_t *tunnel);
/* EN: Expires leases/caches and recomputes authority before side effects.
 * 中文：在产生副作用前过期租约/缓存并重新计算权威。 */
ucn_v6_result_t ucn_v6_cluster_step(
    ucn_v6_cluster_owner_t *owner, uint64_t now_us);
/* EN: Immediately revokes Cluster state derived from an invalidated Link,
 * Session, Capability or Path generation.
 * 中文：立即撤销依赖已失效 Link、Session、Capability 或 Path 代际的簇状态。 */
ucn_v6_result_t ucn_v6_cluster_apply_invalidation(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation);
/* EN: Copies durable state or diagnostics without exposing mutable storage.
 * 中文：复制持久状态或诊断，不暴露可变内部存储。 */
ucn_v6_result_t ucn_v6_cluster_copy_snapshot(
    const ucn_v6_cluster_owner_t *owner,
    ucn_v6_cluster_snapshot_t *snapshot);
ucn_v6_result_t ucn_v6_cluster_copy_view(
    const ucn_v6_cluster_owner_t *owner, uint64_t now_us,
    ucn_v6_cluster_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
