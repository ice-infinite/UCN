#ifndef UCN_CLUSTER_CONFIG_PERSISTENCE_H
#define UCN_CLUSTER_CONFIG_PERSISTENCE_H

/* CLV2-07-06: explicit test/experiment Config persistence owner.
 *
 * It uses the M04 provider contract directly but is not wired into the
 * product Cluster RX/TX/FSM. A caller receives durable=true only after a
 * COMMITTED completion has been followed by load-and-exact-journal proof.
 * Therefore an experimental CONFIG_ACK/CONFIG_COMMIT owner can safely keep
 * its wire side silent while a write is pending or fails.
 */

#include "ucn/ucn_cluster_config_quorum.h"
#include "ucn/ucn_cluster_config_store.h"
#include "ucn/ucn_cluster_persist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These two value models intentionally remain separate modules.  The Config
 * persistence header only needs their identities for the Commit gate. */
struct ucn_cluster_config_joint_runtime;
struct ucn_cluster_config_backup_gate;

typedef enum ucn_cluster_config_persist_action {
    UCN_CLUSTER_CONFIG_PERSIST_ACTION_NONE = 0,
    UCN_CLUSTER_CONFIG_PERSIST_ACTION_PREPARE = 1,
    UCN_CLUSTER_CONFIG_PERSIST_ACTION_JOINT = 2,
    UCN_CLUSTER_CONFIG_PERSIST_ACTION_COMMIT = 3,
    UCN_CLUSTER_CONFIG_PERSIST_ACTION_ABORT = 4
} ucn_cluster_config_persist_action_t;

typedef struct ucn_cluster_config_persist_owner {
    const ucn_cluster_persist_provider_t *provider;
    ucn_cluster_config_store_t *config_store;
    ucn_cluster_persist_state_t durable_state;
    ucn_cluster_persist_request_t pending_request;
    ucn_cluster_persist_token_t pending_token;
    uint8_t pending_action;
    /* Established before *any* provider load/submit/poll callback.  A
     * callback may not recursively start, abort, commit or poll another
     * Config transaction while the outer operation is unresolved. */
    bool io_active;
    bool pending;
} ucn_cluster_config_persist_owner_t;

/* Builds the immutable M04 Config reference for one canonical Config State.
 * The digest is a deterministic identity checksum, not a security MAC. */
ucn_result_t ucn_cluster_config_persist_ref_from_state(
    const ucn_cluster_config_state_t *config_state,
    ucn_cluster_persist_config_ref_t *output);

/* Derives the identity of C_new from a Joint state by first canonicalizing it
 * to Stable(C_new).  M04 committed_config and CONFIG_PREPARED staging_config
 * always name a Stable Config identity; they never name the temporary Joint
 * envelope that carried C_old and C_new together. */
ucn_result_t ucn_cluster_config_persist_ref_from_joint_new(
    const ucn_cluster_config_state_t *joint_state,
    ucn_cluster_persist_config_ref_t *output);

/* Loads one READY, schema-v2 snapshot with an existing committed Config.
 * Factory-empty bootstrap is intentionally outside M07 reconfiguration. */
ucn_result_t ucn_cluster_config_persist_owner_init(
    ucn_cluster_config_persist_owner_t *owner,
    const ucn_cluster_persist_provider_t *provider,
    ucn_cluster_config_store_t *config_store);

bool ucn_cluster_config_persist_owner_is_pending(
    const ucn_cluster_config_persist_owner_t *owner);
const ucn_cluster_persist_state_t *ucn_cluster_config_persist_owner_state(
    const ucn_cluster_config_persist_owner_t *owner);

/* Persist the exact C_new reference before an experimental CONFIG_ACK may be
 * released. durable=false is a valid pending result and is never an ACK
 * permission. */
ucn_result_t ucn_cluster_config_persist_begin_prepare(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    bool *durable);

/* Journals the exact PREPARED(C_new)/txid after dual quorum and before the
 * local runtime enters Joint.  It is an explicit durable proof, not a
 * CONFIG_COMMIT and not a wire/Authority permission. */
ucn_result_t ucn_cluster_config_persist_begin_joint(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    bool *durable);

/* Only permits a durable CONFIG_COMMIT after an exact durable Joint proof,
 * the transaction's independent C_old/C_new quorum, a live Joint runtime,
 * and the Backup staging gate have all succeeded.  These checks happen
 * before submit(), so a denied Backup can never leave C_new durable. */
ucn_result_t ucn_cluster_config_persist_begin_commit(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    const struct ucn_cluster_config_joint_runtime *joint_runtime,
    const struct ucn_cluster_config_backup_gate *backup_gate,
    bool *durable);

/* A timeout abort is durable before the local runtime may return to C_old.
 * It records the same txid as completed and never changes committed Config. */
ucn_result_t ucn_cluster_config_persist_begin_abort(
    ucn_cluster_config_persist_owner_t *owner,
    uint32_t operation_id,
    const ucn_cluster_config_tx_t *tx,
    uint32_t now_ms,
    bool *durable);

/* Advances an asynchronous provider. Output durable=true means its exact
 * pending request was reloaded and proved; FAILED never mutates durable_state. */
ucn_result_t ucn_cluster_config_persist_poll(
    ucn_cluster_config_persist_owner_t *owner,
    bool *durable,
    ucn_cluster_config_persist_action_t *completed_action);

#ifdef __cplusplus
}
#endif

#endif
