#ifndef UCN_V6_BOOTSTRAP_H
#define UCN_V6_BOOTSTRAP_H

#include "ucn/v6/ucn_v6_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_BOOTSTRAP_MAX_PENDING ((size_t)8U)
#define UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS ((size_t)8U)

typedef enum ucn_v6_bootstrap_flow {
    UCN_V6_BOOTSTRAP_FLOW_JOIN = 1,
    UCN_V6_BOOTSTRAP_FLOW_REAUTH = 2
} ucn_v6_bootstrap_flow_t;

typedef enum ucn_v6_bootstrap_phase {
    UCN_V6_BOOTSTRAP_EMPTY = 0,
    UCN_V6_BOOTSTRAP_COOKIE_VERIFIED = 1,
    UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED = 2,
    UCN_V6_BOOTSTRAP_DEVICE_VERIFIED = 3,
    UCN_V6_BOOTSTRAP_ADDRESS_OFFERED = 4,
    UCN_V6_BOOTSTRAP_DEVICE_COMMITTED = 5,
    UCN_V6_BOOTSTRAP_FINAL_DURABLE = 6,
    UCN_V6_BOOTSTRAP_ABORTED = 7
} ucn_v6_bootstrap_phase_t;

typedef struct ucn_v6_bootstrap_key {
    uint32_t ingress_link_generation;
    uint32_t local_peer_discriminator;
    ucn_v6_principal_t identity_digest;
    uint64_t transaction_id;
} ucn_v6_bootstrap_key_t;

typedef struct ucn_v6_bootstrap_transcript {
    uint8_t protocol_version;
    uint16_t bootstrap_header_contract;
    ucn_v6_principal_t joining_device_principal;
    ucn_v6_principal_t joining_device_identity_digest;
    ucn_v6_principal_t authority_principal;
    uint32_t authority_generation;
    uint64_t device_nonce;
    uint64_t authority_nonce;
    uint64_t transaction_id;
    uint64_t lease_freshness_challenge_nonce;
    uint32_t realm_id;
    uint32_t proposed_address;
    uint32_t address_binding_generation;
    uint8_t binding_lease_id[16];
    uint64_t binding_lease_duration_us;
    uint64_t authority_lease_sequence;
    uint16_t selected_hop_suite;
    uint32_t selected_hop_key_context;
    uint16_t selected_e2e_suite;
    uint32_t selected_e2e_key_context;
    uint8_t prior_messages_hash[32];
} ucn_v6_bootstrap_transcript_t;

typedef struct ucn_v6_bootstrap_pending {
    bool occupied;
    ucn_v6_bootstrap_flow_t flow;
    ucn_v6_bootstrap_phase_t phase;
    ucn_v6_bootstrap_key_t key;
    ucn_v6_bootstrap_transcript_t transcript;
    ucn_v6_binding_key_t existing_binding;
    uint64_t deadline_us;
} ucn_v6_bootstrap_pending_t;

typedef struct ucn_v6_bootstrap_link_budget {
    bool occupied;
    uint32_t ingress_link_generation;
    uint8_t tokens;
    uint64_t last_refill_us;
} ucn_v6_bootstrap_link_budget_t;

typedef struct ucn_v6_bootstrap_config {
    uint8_t max_pending;
    uint8_t max_pending_per_link;
    uint8_t token_burst;
    uint8_t tokens_per_second;
    uint64_t pending_timeout_us;
} ucn_v6_bootstrap_config_t;

typedef struct ucn_v6_bootstrap_owner {
    ucn_v6_bootstrap_config_t config;
    ucn_v6_bootstrap_pending_t join_pending[UCN_V6_BOOTSTRAP_MAX_PENDING];
    ucn_v6_bootstrap_pending_t reauth_pending[UCN_V6_BOOTSTRAP_MAX_PENDING];
    ucn_v6_bootstrap_link_budget_t budgets[UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS];
    bool initialized;
} ucn_v6_bootstrap_owner_t;

typedef enum ucn_v6_bootstrap_event {
    UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF = 1,
    UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF = 2,
    UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER = 3,
    UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT = 4,
    UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE = 5,
    UCN_V6_BOOTSTRAP_EVENT_ABORT = 6
} ucn_v6_bootstrap_event_t;

/* EN: Initializes bounded JOIN and REAUTH state owned by one protocol task.
 * 中文：初始化由单一协议任务持有的有界 JOIN/REAUTH 状态。 */
ucn_v6_result_t ucn_v6_bootstrap_owner_init(
    ucn_v6_bootstrap_owner_t *owner,
    const ucn_v6_bootstrap_config_t *config);
/* EN: Applies the no-amplification and per-Link pre-auth token budget.
 * 中文：执行无放大与每 Link 认证前令牌预算。 */
ucn_v6_result_t ucn_v6_bootstrap_admit_initial_hello(
    ucn_v6_bootstrap_owner_t *owner,
    uint32_t ingress_link_generation,
    uint64_t now_us,
    size_t request_bytes,
    size_t response_bytes);
/* EN: Opens a fixed pending slot only after a stateless Cookie succeeds.
 * 中文：仅在无状态 Cookie 通过后创建固定 pending 槽。 */
ucn_v6_result_t ucn_v6_bootstrap_open_after_cookie(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    bool cookie_verified,
    uint64_t now_us);
/* EN: Advances the mutually authenticated transcript in strict order.
 * 中文：按严格顺序推进双向认证 transcript。 */
ucn_v6_result_t ucn_v6_bootstrap_advance(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_event_t event,
    bool proof_verified,
    uint64_t now_us);
/* EN: Copies diagnostics without exposing a writable internal slot.
 * 中文：复制诊断快照，不暴露可写的内部槽位。 */
ucn_v6_result_t ucn_v6_bootstrap_copy_pending(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    ucn_v6_bootstrap_pending_t *pending);
/* EN: Explicitly expires half-open deadlines; hostile input never evicts.
 * 中文：显式清理半开截止期；恶意输入不能借机驱逐槽位。 */
size_t ucn_v6_bootstrap_expire(
    ucn_v6_bootstrap_owner_t *owner,
    uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
