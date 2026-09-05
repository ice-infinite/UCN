#ifndef UCN_V6_ROUTE_H
#define UCN_V6_ROUTE_H

#include "ucn/v6/ucn_v6_capability.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_ROUTE_PROPOSAL_DIGEST_BYTES ((size_t)16U)
#define UCN_V6_ROUTE_MAX_PATHS ((size_t)UCN_V6_CONFIG_ROUTE_PATHS_PER_SET)

typedef enum ucn_v6_route_policy {
    UCN_V6_ROUTE_POLICY_PINNED = 1,
    UCN_V6_ROUTE_POLICY_ACTIVE_STANDBY = 2,
    UCN_V6_ROUTE_POLICY_PER_FLOW_HASH = 3,
    UCN_V6_ROUTE_POLICY_WEIGHTED_MULTIPATH = 4
} ucn_v6_route_policy_t;

typedef struct ucn_v6_route_domain {
    ucn_v6_principal_t origin_principal;
    ucn_v6_binding_key_t origin_binding;
    uint32_t origin_session_generation;
    ucn_v6_principal_t destination_principal;
    ucn_v6_binding_key_t destination_binding;
    uint32_t destination_session_generation;
} ucn_v6_route_domain_t;

/* EN: The only stable path authority reference consumed outside Route Owner.
 * It binds both endpoint Session domains and the exact Route/Path generation;
 * mutable next-hop, Link and Capability claims are resolved by Route Owner.
 * 中文：Route Owner 之外唯一允许使用的稳定路径权威引用。它绑定两端
 * Session 域及精确 Route/Path 代际；可变下一跳、Link 与 Capability 声明
 * 必须由 Route Owner 解析。 */
typedef struct ucn_v6_route_path_ref {
    ucn_v6_route_domain_t domain;
    uint32_t route_generation;
    uint16_t path_id;
    uint32_t path_generation;
} ucn_v6_route_path_ref_t;

typedef struct ucn_v6_route_path {
    uint16_t path_id;
    uint32_t path_generation;
    ucn_v6_session_key_t next_hop;
    uint16_t egress_link_id;
    uint32_t egress_link_generation;
    uint32_t next_hop_capability_generation;
    uint16_t hop_count;
    uint16_t priority;
    uint16_t weight;
    bool available;
    ucn_v6_path_capability_t capability;
} ucn_v6_route_path_t;

/* EN: One read-only Route Owner resolution.  The Path is an immediate
 * snapshot only; long-lived consumers store the stable reference and one
 * complete parent-chain key so invalidation still works after Route/Capability
 * state has already been retired.
 * 中文：Route Owner 的一次只读解析结果。Path 仅是当前瞬时快照；长期持有者
 * 保存稳定引用及完整父链键，从而即使 Route/Capability 状态已先被回收，
 * 失效事件仍能精确撤销下游状态。 */
typedef struct ucn_v6_route_resolution {
    ucn_v6_route_path_t path;
    ucn_v6_stack_invalidation_t dependency;
} ucn_v6_route_resolution_t;

typedef struct ucn_v6_route_proposal {
    ucn_v6_route_domain_t domain;
    uint32_t route_generation;
    uint8_t path_count;
    uint8_t preferred_path_index;
    ucn_v6_route_path_t paths[UCN_V6_ROUTE_MAX_PATHS];
} ucn_v6_route_proposal_t;

typedef struct ucn_v6_route_activation {
    uint64_t candidate_transaction_id;
    ucn_v6_route_domain_t domain;
    uint32_t route_generation;
    uint8_t proposal_digest[UCN_V6_ROUTE_PROPOSAL_DIGEST_BYTES];
} ucn_v6_route_activation_t;

typedef struct ucn_v6_route_candidate_view {
    bool occupied;
    bool frozen;
    bool activation_sent;
    uint64_t candidate_transaction_id;
    ucn_v6_route_proposal_t proposal;
    uint16_t probed_mask;
    uint8_t activation_attempts;
    uint64_t deadline_us;
    uint64_t next_retry_us;
    uint8_t proposal_digest[UCN_V6_ROUTE_PROPOSAL_DIGEST_BYTES];
} ucn_v6_route_candidate_view_t;

typedef struct ucn_v6_route_select_request {
    ucn_v6_route_domain_t domain;
    uint64_t flow_id;
    uint64_t packet_sequence;
    ucn_v6_route_policy_t policy;
    uint16_t pinned_path_id;
    uint32_t pinned_path_generation;
    bool allow_reordering;
} ucn_v6_route_select_request_t;

typedef struct ucn_v6_route_selection {
    uint32_t route_generation;
    ucn_v6_route_path_t path;
    bool reused_flow_pin;
} ucn_v6_route_selection_t;

typedef struct ucn_v6_route_view {
    uint16_t route_sets;
    uint16_t candidates;
    uint16_t flow_pins;
    uint32_t activations;
    uint32_t failovers;
    uint32_t rejected_stale;
    uint32_t rejected_capacity;
    bool faulted;
} ucn_v6_route_view_t;

typedef struct ucn_v6_route_owner ucn_v6_route_owner_t;
#ifndef UCN_V6_ROUTE_OWNER_STORAGE_BYTES
#define UCN_V6_ROUTE_OWNER_STORAGE_BYTES                                  \
    ((size_t)(1024U + UCN_V6_CONFIG_ROUTE_SETS * 128U +                   \
              UCN_V6_CONFIG_ROUTE_SETS *                                  \
                  (512U + UCN_V6_CONFIG_ROUTE_PATHS_PER_SET * 384U) +     \
              UCN_V6_CONFIG_ROUTE_CANDIDATES *                            \
                  (384U + UCN_V6_CONFIG_ROUTE_PATHS_PER_SET * 384U) +     \
              UCN_V6_CONFIG_ROUTE_FLOW_PINS * 128U))
#endif
typedef union ucn_v6_route_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_ROUTE_OWNER_STORAGE_BYTES];
} ucn_v6_route_owner_storage_t;

/* EN: Initializes the fixed-capacity RouteSet owner, permanently binding the
 * Capability Owner used for every path preflight.  The Capability Owner must
 * outlive the Route Owner.
 * 中文：初始化固定容量 RouteSet Owner，并永久绑定所有 Path 预检使用的
 * Capability Owner；Capability Owner 的生命周期必须长于 Route Owner。 */
ucn_v6_result_t ucn_v6_route_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_capability_owner_t *capability_owner,
    uint64_t candidate_timeout_us,
    uint64_t activation_retry_us,
    uint8_t activation_max_attempts,
    uint64_t previous_generation_grace_us,
    uint64_t flow_pin_lease_us,
    ucn_v6_route_owner_t **owner);

/* EN: Opens one exact Candidate keyed by {Route Domain, transaction_id};
 * generation must be first or next. The same transaction number in another
 * Route Domain is independent and must not collide.
 * 中文：按 {Route Domain, transaction_id} 创建精确 Candidate；Route
 * Generation 必须为首值或精确后继。其他 Route Domain 中相同事务号相互
 * 独立，不得冲突。 */
ucn_v6_result_t ucn_v6_route_candidate_begin(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    const ucn_v6_route_domain_t *domain,
    uint32_t proposed_route_generation);
/* EN: Adds one authenticated RREP path before Probe freezes the proposal.
 * 中文：在 Probe 冻结提案前加入一条已认证 RREP Path。 */
ucn_v6_result_t ucn_v6_route_candidate_add_path(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    const ucn_v6_route_domain_t *domain,
    const ucn_v6_route_path_t *path);
/* EN: Freezes the whole proposal and records one exact Probe success.
 * 中文：永久冻结整个提案，并记录一条精确 Probe 成功证明。 */
ucn_v6_result_t ucn_v6_route_candidate_record_probe(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    const ucn_v6_route_domain_t *domain,
    uint16_t path_id,
    uint32_t path_generation);
/* EN: Returns an immutable activation proof when all paths were probed.
 * 中文：全部 Path 已 Probe 后返回不可变激活证明。 */
ucn_v6_result_t ucn_v6_route_candidate_prepare_activation(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    const ucn_v6_route_domain_t *domain,
    ucn_v6_route_activation_t *activation);
/* EN: Records the physical send outcome without mutating the frozen path.
 * 中文：记录物理发送结果，绝不改写冻结 Path。 */
ucn_v6_result_t ucn_v6_route_candidate_record_activation_send(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    uint64_t candidate_transaction_id,
    const ucn_v6_route_domain_t *domain,
    bool submitted);
/* EN: Atomically commits an exact ACK after capability revalidation.
 * 中文：能力重新校验后原子提交精确 ACK。 */
ucn_v6_result_t ucn_v6_route_candidate_commit_ack(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_route_activation_t *ack);

ucn_v6_result_t ucn_v6_route_copy_candidate(
    const ucn_v6_route_owner_t *owner,
    uint64_t candidate_transaction_id,
    const ucn_v6_route_domain_t *domain,
    ucn_v6_route_candidate_view_t *candidate);
ucn_v6_result_t ucn_v6_route_copy_set(
    const ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain,
    ucn_v6_route_proposal_t *current);
/* EN: Selects one route under pinned, primary/backup or multipath policy.
 * 中文：按固定、主备或多路径策略选择 Route。 */
ucn_v6_result_t ucn_v6_route_select(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_route_select_request_t *request,
    ucn_v6_route_selection_t *selection);
/* EN: Resolves one exact immutable Route reference from the current
 * generation or the still-live previous generation.  This is the only
 * long-lived ownership boundary used by downstream modules: callers provide
 * identity only and cannot inject a Path claim.  The result has been
 * revalidated against both the live next-hop Capability and destination Path
 * Capability and includes the canonical invalidation parent chain.
 * 中文：从当前 Route 代际或仍处于 Grace 的上一代际解析精确 Route 引用。
 * 这是下游长期状态唯一的所有权边界：调用方只提供身份，不能注入 Path 声明；
 * 结果已重新校验下一跳 Capability 与目标 Path Capability，并携带规范失效父链。 */
ucn_v6_result_t ucn_v6_route_resolve_ref(
    const ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_route_path_ref_t *reference,
    ucn_v6_route_resolution_t *resolution);
/* EN: Applies one exact RERR without touching a different generation/path.
 * 中文：应用精确 RERR，不影响其他 Generation/Path。 */
ucn_v6_result_t ucn_v6_route_mark_error(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_route_domain_t *domain,
    uint32_t route_generation,
    uint16_t path_id,
    uint32_t path_generation);
/* EN: Applies one canonical parent-chain invalidation.  A Link event removes
 * every proposal that uses that exact egress Link generation; deeper events
 * match the bound Session/Capability/Path identity.  Immutable proposals are
 * removed as a whole rather than silently rewritten in place.
 * 中文：应用一个规范父链失效事件。Link 事件删除使用该精确出口 Link 代际的
 * 全部 Proposal；更深事件匹配已绑定 Session/Capability/Path 身份。不可变
 * Proposal 整体删除，不在原地静默改写。 */
ucn_v6_result_t ucn_v6_route_apply_invalidation(
    ucn_v6_route_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation);
/* EN: Accepts only current or still-live previous inbound generation.
 * 中文：仅接受当前或仍在 Grace 内的上一代入站 Route Generation。 */
ucn_v6_result_t ucn_v6_route_accept_generation(
    const ucn_v6_route_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_route_domain_t *domain,
    uint32_t route_generation);
ucn_v6_result_t ucn_v6_route_expire(
    ucn_v6_route_owner_t *owner,
    uint64_t now_us);
ucn_v6_result_t ucn_v6_route_copy_view(
    const ucn_v6_route_owner_t *owner,
    ucn_v6_route_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
