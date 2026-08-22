/* CLV2-M08: opt-in Authority / Quorum / Grace / Fence Owner.
 *
 * It deliberately has no wire parser, no provider I/O and no Adapter calls.
 * The caller supplies only canonical M07 Config values and protocol-layer
 * lease evidence.  This keeps the implementation testable before M05 opens
 * production v4 RX/TX/FSM integration. */

#include "ucn/ucn_cluster_authority.h"

#include <string.h>

#include "ucn/ucn_cluster_config_quorum.h"
#include "ucn/ucn_time.h"

static bool duration_is_valid(uint32_t value)
{
    return ucn_duration_is_valid(value);
}

static bool add_duration(uint32_t left, uint32_t right, uint32_t *output)
{
    if (output == NULL || left > UCN_MAX_SAFE_DURATION_MS - right) {
        return false;
    }
    *output = left + right;
    return duration_is_valid(*output);
}

static bool multiply_duration(uint32_t value, uint32_t multiplier,
                              uint32_t *output)
{
    if (output == NULL || multiplier == 0U ||
        value > UCN_MAX_SAFE_DURATION_MS / multiplier) {
        return false;
    }
    *output = value * multiplier;
    return duration_is_valid(*output);
}

static bool authority_timing_is_valid(
    const ucn_cluster_authority_timing_t *timing)
{
    return timing != NULL && duration_is_valid(timing->owner_step_budget_ms) &&
           duration_is_valid(timing->control_window_ms) &&
           duration_is_valid(timing->voter_lease_ms) &&
           duration_is_valid(timing->authority_grace_ms) &&
           duration_is_valid(timing->quorum_restore_hold_ms) &&
           duration_is_valid(timing->member_takeover_grace_ms) &&
           duration_is_valid(timing->fenced_dissolve_ms) &&
           timing->authority_grace_ms <= timing->voter_lease_ms &&
           timing->quorum_restore_hold_ms <= timing->voter_lease_ms;
}

static bool phase_is_operational_head(ucn_cluster_phase_t phase)
{
    return phase == UCN_CLUSTER_PHASE_HEAD_NO_BACKUP ||
           phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING ||
           phase == UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING ||
           phase == UCN_CLUSTER_PHASE_HEAD_STABLE ||
           phase == UCN_CLUSTER_PHASE_HEAD_RECONFIGURING;
}

static ucn_cluster_phase_t runtime_head_phase(const ucn_cluster_t *cluster,
                                              const ucn_cluster_config_state_t *config)
{
    if (cluster == NULL || config == NULL ||
        cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        cluster->head_node_id != cluster->config.local_node_id) {
        return UCN_CLUSTER_PHASE_DISABLED;
    }
    if (config->phase == (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT) {
        return UCN_CLUSTER_PHASE_HEAD_RECONFIGURING;
    }
    if (cluster->backup_node_id == 0U) {
        return UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    }
    if (cluster->backup_ready) {
        return UCN_CLUSTER_PHASE_HEAD_STABLE;
    }
    if (cluster->backup_assign_pending) {
        return UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING;
    }
    return UCN_CLUSTER_PHASE_HEAD_BACKUP_SYNCING;
}

static bool config_contains_local_head(const ucn_cluster_t *cluster,
                                       const ucn_cluster_config_state_t *config)
{
    ucn_node_id_t local_node_id;

    if (cluster == NULL || config == NULL) {
        return false;
    }
    local_node_id = cluster->config.local_node_id;
    return ucn_cluster_voter_set_contains(&config->old_set, local_node_id) &&
           ucn_cluster_voter_set_contains(&config->new_set, local_node_id);
}

static bool union_contains(const ucn_node_id_t *node_ids, uint8_t count,
                           ucn_node_id_t node_id)
{
    uint8_t index;

    for (index = 0U; index < count; ++index) {
        if (node_ids[index] == node_id) {
            return true;
        }
    }
    return false;
}

static bool rebuild_voter_union(ucn_cluster_authority_runtime_t *runtime,
                                const ucn_cluster_config_state_t *config)
{
    ucn_node_id_t next_ids[UCN_CLUSTER_MAX_VOTERS];
    uint32_t next_deadlines[UCN_CLUSTER_MAX_VOTERS];
    uint8_t next_count = 0U;
    uint8_t set_index;
    uint8_t index;
    const ucn_cluster_voter_set_t *sets[2];

    if (runtime == NULL || config == NULL) {
        return false;
    }
    (void)memset(next_ids, 0, sizeof(next_ids));
    (void)memset(next_deadlines, 0, sizeof(next_deadlines));
    sets[0] = &config->old_set;
    sets[1] = &config->new_set;
    for (set_index = 0U; set_index < 2U; ++set_index) {
        for (index = 0U; index < sets[set_index]->count; ++index) {
            ucn_node_id_t node_id = sets[set_index]->node_ids[index];
            uint8_t old_index;

            if (union_contains(next_ids, next_count, node_id)) {
                continue;
            }
            if (next_count >= UCN_CLUSTER_MAX_VOTERS) {
                return false;
            }
            next_ids[next_count] = node_id;
            for (old_index = 0U; old_index < runtime->voter_count;
                 ++old_index) {
                if (runtime->voter_node_ids[old_index] == node_id) {
                    next_deadlines[next_count] =
                        runtime->voter_lease_deadlines_ms[old_index];
                    break;
                }
            }
            ++next_count;
        }
    }
    (void)memcpy(runtime->voter_node_ids, next_ids, sizeof(next_ids));
    (void)memcpy(runtime->voter_lease_deadlines_ms, next_deadlines,
                 sizeof(next_deadlines));
    runtime->voter_count = next_count;
    return true;
}

static int voter_index(const ucn_cluster_authority_runtime_t *runtime,
                       ucn_node_id_t node_id)
{
    uint8_t index;

    if (runtime == NULL) {
        return -1;
    }
    for (index = 0U; index < runtime->voter_count; ++index) {
        if (runtime->voter_node_ids[index] == node_id) {
            return (int)index;
        }
    }
    return -1;
}

static bool voter_is_live(const ucn_cluster_authority_runtime_t *runtime,
                          ucn_node_id_t node_id, uint32_t now_ms)
{
    int index;

    if (runtime == NULL || runtime->cluster == NULL) {
        return false;
    }
    if (node_id == runtime->cluster->config.local_node_id) {
        return true; /* Head self-vote is valid only after Config inclusion. */
    }
    index = voter_index(runtime, node_id);
    return index >= 0 && runtime->voter_lease_deadlines_ms[index] != 0U &&
           !ucn_deadline_expired(now_ms,
                                 runtime->voter_lease_deadlines_ms[index]);
}

static uint64_t live_bitmap_for_set(
    const ucn_cluster_authority_runtime_t *runtime,
    const ucn_cluster_voter_set_t *set,
    uint32_t now_ms)
{
    uint64_t bitmap = 0U;
    uint8_t index;

    if (runtime == NULL || !ucn_cluster_voter_set_is_valid(set)) {
        return 0U;
    }
    for (index = 0U; index < set->count; ++index) {
        if (voter_is_live(runtime, set->node_ids[index], now_ms)) {
            bitmap |= UINT64_C(1) << index;
        }
    }
    return bitmap;
}

static void authority_revoke(ucn_cluster_authority_runtime_t *runtime,
                             ucn_cluster_authority_fence_reason_t reason)
{
    runtime->cluster->authority_active = false;
    runtime->cluster->authority_fence_reason = reason;
}

static void authority_enter_grace(ucn_cluster_authority_runtime_t *runtime,
                                  uint32_t now_ms)
{
    ucn_cluster_t *cluster = runtime->cluster;

    authority_revoke(runtime, UCN_CLUSTER_AUTHORITY_FENCE_QUORUM_LOST);
    cluster->head_resume_phase = cluster->authority_phase;
    cluster->authority_phase = UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE;
    cluster->quorum_loss_deadline_ms = ucn_deadline_from_now(
        now_ms, runtime->timing.authority_grace_ms);
    cluster->quorum_restore_since_ms = 0U;
    runtime->restore_hold_armed = false;
}

static void authority_enter_fenced(ucn_cluster_authority_runtime_t *runtime,
                                   ucn_cluster_authority_fence_reason_t reason,
                                   uint32_t now_ms)
{
    ucn_cluster_t *cluster = runtime->cluster;

    authority_revoke(runtime, reason);
    runtime->fence_latched = true;
    runtime->restore_hold_armed = false;
    cluster->quorum_restore_since_ms = 0U;
    cluster->quorum_loss_deadline_ms = 0U;
    cluster->authority_phase = UCN_CLUSTER_PHASE_HEAD_FENCED;
    cluster->fenced_dissolve_deadline_ms = ucn_deadline_from_now(
        now_ms, runtime->timing.fenced_dissolve_ms);
}

ucn_result_t ucn_cluster_authority_timing_derive(
    const ucn_cluster_timing_budget_t *budget,
    ucn_cluster_authority_timing_t *output)
{
    ucn_cluster_authority_timing_t candidate;
    uint32_t window;

    if (budget == NULL || output == NULL ||
        !duration_is_valid(budget->owner_step_budget_ms) ||
        !duration_is_valid(budget->one_way_network_budget_ms) ||
        !duration_is_valid(budget->retry_budget_ms) ||
        !duration_is_valid(budget->scheduler_jitter_ms) ||
        !duration_is_valid(budget->clock_drift_budget_ms) ||
        !duration_is_valid(budget->safety_margin_ms)) {
        return UCN_ERR_ARGUMENT;
    }
    window = budget->owner_step_budget_ms;
    if (!add_duration(window, budget->one_way_network_budget_ms, &window) ||
        !add_duration(window, budget->retry_budget_ms, &window) ||
        !add_duration(window, budget->scheduler_jitter_ms, &window) ||
        !add_duration(window, budget->clock_drift_budget_ms, &window) ||
        !add_duration(window, budget->safety_margin_ms, &window) ||
        !multiply_duration(window, 3U, &candidate.voter_lease_ms) ||
        !multiply_duration(window, 2U, &candidate.authority_grace_ms) ||
        !multiply_duration(window, 1U, &candidate.quorum_restore_hold_ms) ||
        !multiply_duration(window, 2U, &candidate.member_takeover_grace_ms) ||
        !multiply_duration(window, 3U, &candidate.fenced_dissolve_ms)) {
        return UCN_ERR_CONFIG;
    }
    candidate.owner_step_budget_ms = budget->owner_step_budget_ms;
    candidate.control_window_ms = window;
    if (!authority_timing_is_valid(&candidate)) {
        return UCN_ERR_CONFIG;
    }
    *output = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_authority_member_takeover_grace_derive(
    const ucn_cluster_authority_timing_t *timing,
    uint32_t backup_lease_ms,
    uint32_t member_lease_ms,
    uint32_t takeover_window_ms,
    uint32_t *output)
{
    uint32_t candidate;
    uint32_t lease_delta;

    if (output == NULL || !authority_timing_is_valid(timing) ||
        !duration_is_valid(backup_lease_ms) ||
        !duration_is_valid(member_lease_ms) ||
        !duration_is_valid(takeover_window_ms)) {
        return UCN_ERR_ARGUMENT;
    }
    lease_delta = backup_lease_ms > member_lease_ms ?
                      backup_lease_ms - member_lease_ms : 0U;
    if (!add_duration(lease_delta, takeover_window_ms, &candidate) ||
        !add_duration(candidate, timing->control_window_ms, &candidate)) {
        return UCN_ERR_CONFIG;
    }
    *output = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_authority_runtime_install_config(
    ucn_cluster_authority_runtime_t *runtime,
    const ucn_cluster_config_state_t *config,
    uint32_t now_ms)
{
    ucn_cluster_authority_runtime_t candidate;
    ucn_cluster_phase_t phase;
    bool quorum;

    if (runtime == NULL || config == NULL || !runtime->initialized ||
        runtime->cluster == NULL || !ucn_cluster_config_state_is_valid(config) ||
        !config_contains_local_head(runtime->cluster, config)) {
        return UCN_ERR_ARGUMENT;
    }
    if (runtime->fence_latched) {
        return UCN_ERR_STATE;
    }
    candidate = *runtime;
    candidate.active_config = *config;
    if (!rebuild_voter_union(&candidate, config)) {
        return UCN_ERR_STATE;
    }
    phase = runtime_head_phase(candidate.cluster, config);
    if (!phase_is_operational_head(phase)) {
        return UCN_ERR_STATE;
    }
    quorum = ucn_cluster_authority_runtime_quorum_met(&candidate, now_ms);

    /* All candidate validation and quorum evaluation happened without
     * changing the live runtime.  Commit the new Config fail-closed: the
     * old permission is gone before any new Stable/Joint result is exposed. */
    candidate.cluster->active_voter_set = config->new_set;
    *runtime = candidate;
    runtime->owner_step_seen = true;
    runtime->last_owner_step_ms = now_ms;
    runtime->restore_hold_armed = false;
    runtime->cluster->authority_active = false;
    runtime->cluster->authority_phase = phase;
    runtime->cluster->head_resume_phase = phase;
    runtime->cluster->authority_fence_reason = UCN_CLUSTER_AUTHORITY_FENCE_NONE;
    runtime->cluster->quorum_loss_deadline_ms = 0U;
    runtime->cluster->quorum_restore_since_ms = 0U;
    if (!quorum) {
        authority_enter_grace(runtime, now_ms);
    } else {
        runtime->cluster->authority_active = true;
    }
    return UCN_OK;
}

ucn_result_t ucn_cluster_authority_runtime_init(
    ucn_cluster_authority_runtime_t *runtime,
    ucn_cluster_t *cluster,
    const ucn_cluster_config_state_t *config,
    const ucn_cluster_authority_timing_t *timing,
    uint32_t now_ms)
{
    ucn_cluster_authority_runtime_t candidate;
    ucn_cluster_phase_t phase;

    if (runtime == NULL || cluster == NULL || config == NULL ||
        !authority_timing_is_valid(timing) ||
        !ucn_cluster_config_state_is_valid(config) ||
        cluster->authority_runtime != NULL ||
        !config_contains_local_head(cluster, config)) {
        return UCN_ERR_ARGUMENT;
    }
    if (cluster->config.persistence_mode !=
            UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST ||
        cluster->config.lease_ms < timing->voter_lease_ms) {
        return UCN_ERR_CONFIG;
    }
    phase = runtime_head_phase(cluster, config);
    if (!phase_is_operational_head(phase)) {
        return UCN_ERR_STATE;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.cluster = cluster;
    candidate.active_config = *config;
    candidate.timing = *timing;
    candidate.initialized = true;
    candidate.owner_step_seen = true;
    candidate.last_owner_step_ms = now_ms;
    if (!rebuild_voter_union(&candidate, config)) {
        return UCN_ERR_STATE;
    }
    cluster->authority_active = false;
    cluster->authority_phase = phase;
    cluster->authority_fence_reason = UCN_CLUSTER_AUTHORITY_FENCE_NONE;
    cluster->head_resume_phase = phase;
    cluster->quorum_loss_deadline_ms = 0U;
    cluster->quorum_restore_since_ms = 0U;
    cluster->fenced_dissolve_deadline_ms = 0U;
    *runtime = candidate;
    cluster->authority_runtime = runtime;
    /* The local Head is a valid self-voter, but no remote lease is assumed. */
    return ucn_cluster_authority_runtime_note_voter_keepalive(
        runtime, cluster->config.local_node_id, now_ms);
}

ucn_result_t ucn_cluster_authority_runtime_note_voter_keepalive(
    ucn_cluster_authority_runtime_t *runtime,
    ucn_node_id_t voter_node_id,
    uint32_t now_ms)
{
    int index;

    if (runtime == NULL || !runtime->initialized || runtime->cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    index = voter_index(runtime, voter_node_id);
    if (index < 0) {
        return UCN_ERR_NOT_FOUND;
    }
    if (voter_node_id != runtime->cluster->config.local_node_id) {
        runtime->voter_lease_deadlines_ms[index] = ucn_deadline_from_now(
            now_ms, runtime->timing.voter_lease_ms);
    }
    return UCN_OK;
}

bool ucn_cluster_authority_runtime_quorum_met(
    const ucn_cluster_authority_runtime_t *runtime,
    uint32_t now_ms)
{
    uint64_t old_bitmap;
    uint64_t new_bitmap;

    if (runtime == NULL || !runtime->initialized || runtime->cluster == NULL ||
        !ucn_cluster_config_state_is_valid(&runtime->active_config) ||
        !config_contains_local_head(runtime->cluster, &runtime->active_config)) {
        return false;
    }
    old_bitmap = live_bitmap_for_set(runtime, &runtime->active_config.old_set,
                                     now_ms);
    new_bitmap = live_bitmap_for_set(runtime, &runtime->active_config.new_set,
                                     now_ms);
    return ucn_cluster_config_bitmap_reaches_quorum(
               &runtime->active_config.old_set, old_bitmap) &&
           ucn_cluster_config_bitmap_reaches_quorum(
               &runtime->active_config.new_set, new_bitmap);
}

ucn_result_t ucn_cluster_authority_runtime_step(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t now_ms)
{
    ucn_cluster_t *cluster;
    bool quorum;

    if (runtime == NULL || !runtime->initialized || runtime->cluster == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    cluster = runtime->cluster;
    if (cluster->authority_runtime != runtime) {
        return UCN_ERR_STATE;
    }
    if (cluster->persistence_faulted) {
        authority_enter_fenced(runtime,
                               UCN_CLUSTER_AUTHORITY_FENCE_PERSISTENCE_FAULT,
                               now_ms);
        return UCN_OK;
    }
    if (cluster->role == UCN_CLUSTER_ROLE_TERM_CONFLICT) {
        authority_enter_fenced(runtime,
                               UCN_CLUSTER_AUTHORITY_FENCE_TERM_CONFLICT,
                               now_ms);
        return UCN_OK;
    }
    if (runtime->higher_authority_seen) {
        if (!runtime->fence_latched) {
            authority_enter_fenced(runtime,
                                   UCN_CLUSTER_AUTHORITY_FENCE_HIGHER_AUTHORITY,
                                   now_ms);
        }
        /* The Fence latch remains set, but an authenticated higher stable
         * Authority selects the bounded M08-08 cleanup destination instead
         * of waiting for local dissolution.  M11 later owns the actual
         * epoch/role Join transaction. */
        cluster->authority_phase = UCN_CLUSTER_PHASE_JOIN_PENDING;
        return UCN_OK;
    }
    if (runtime->fence_latched) {
        authority_revoke(runtime, cluster->authority_fence_reason);
        if (cluster->authority_phase == UCN_CLUSTER_PHASE_HEAD_FENCED &&
            ucn_deadline_expired(now_ms, cluster->fenced_dissolve_deadline_ms)) {
            /* M08-08 records a bounded cleanup destination without mutating
             * the legacy role/epoch.  M11 owns authenticated join wiring. */
            cluster->authority_phase = UCN_CLUSTER_PHASE_RECOVERY_OBSERVE;
        }
        return UCN_OK;
    }
    if (runtime->owner_step_seen &&
        (uint32_t)(now_ms - runtime->last_owner_step_ms) >
            runtime->timing.owner_step_budget_ms) {
        authority_enter_fenced(runtime,
                               UCN_CLUSTER_AUTHORITY_FENCE_OWNER_STEP_BUDGET,
                               now_ms);
        runtime->last_owner_step_ms = now_ms;
        return UCN_OK;
    }
    runtime->owner_step_seen = true;
    runtime->last_owner_step_ms = now_ms;
    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        cluster->head_node_id != cluster->config.local_node_id) {
        authority_enter_fenced(runtime,
                               UCN_CLUSTER_AUTHORITY_FENCE_HIGHER_AUTHORITY,
                               now_ms);
        return UCN_OK;
    }
    quorum = ucn_cluster_authority_runtime_quorum_met(runtime, now_ms);
    if (cluster->authority_phase == UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE) {
        authority_revoke(runtime, UCN_CLUSTER_AUTHORITY_FENCE_QUORUM_LOST);
        if (!quorum) {
            runtime->restore_hold_armed = false;
            cluster->quorum_restore_since_ms = 0U;
        } else if (!runtime->restore_hold_armed) {
            runtime->restore_hold_armed = true;
            cluster->quorum_restore_since_ms = now_ms;
        } else if (ucn_elapsed_at_least(now_ms, cluster->quorum_restore_since_ms,
                                        runtime->timing.quorum_restore_hold_ms)) {
            cluster->authority_phase = cluster->head_resume_phase;
            cluster->authority_active = true;
            cluster->authority_fence_reason = UCN_CLUSTER_AUTHORITY_FENCE_NONE;
            cluster->quorum_loss_deadline_ms = 0U;
            cluster->quorum_restore_since_ms = 0U;
            runtime->restore_hold_armed = false;
        }
        if (cluster->authority_phase == UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE &&
            ucn_deadline_expired(now_ms, cluster->quorum_loss_deadline_ms)) {
            authority_enter_fenced(runtime,
                                   UCN_CLUSTER_AUTHORITY_FENCE_GRACE_EXPIRED,
                                   now_ms);
        }
        return UCN_OK;
    }
    if (!phase_is_operational_head(cluster->authority_phase)) {
        cluster->authority_phase = runtime_head_phase(cluster,
                                                      &runtime->active_config);
    }
    if (!phase_is_operational_head(cluster->authority_phase)) {
        authority_enter_fenced(runtime,
                               UCN_CLUSTER_AUTHORITY_FENCE_HIGHER_AUTHORITY,
                               now_ms);
        return UCN_OK;
    }
    if (!quorum) {
        /* The revoke is intentionally before the phase change: the caller
         * can prove same-step ordering by observing active=false and the
         * GRACE phase after this single Owner call. */
        authority_enter_grace(runtime, now_ms);
        return UCN_OK;
    }
    cluster->authority_active = true;
    cluster->authority_fence_reason = UCN_CLUSTER_AUTHORITY_FENCE_NONE;
    return UCN_OK;
}

ucn_result_t ucn_cluster_authority_runtime_preflight(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t now_ms)
{
    if (runtime == NULL) {
        return UCN_OK;
    }
    return ucn_cluster_authority_runtime_step(runtime, now_ms);
}

static bool higher_epoch_input_is_valid(
    const ucn_cluster_authority_runtime_t *runtime,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id)
{
    return runtime != NULL && runtime->initialized && runtime->cluster != NULL &&
           cluster_id != 0U && head_node_id != 0U &&
           head_node_id != UCN_NODE_BROADCAST &&
           cluster_id == runtime->cluster->cluster_id &&
           term > runtime->cluster->term &&
           head_node_id != runtime->cluster->config.local_node_id;
}

ucn_result_t ucn_cluster_authority_runtime_note_higher_term_observed(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id,
    uint32_t now_ms)
{
    if (!higher_epoch_input_is_valid(runtime, cluster_id, term, head_node_id)) {
        return UCN_ERR_ARGUMENT;
    }
    authority_enter_fenced(runtime,
                           UCN_CLUSTER_AUTHORITY_FENCE_HIGHER_AUTHORITY,
                           now_ms);
    return UCN_OK;
}

ucn_result_t ucn_cluster_authority_runtime_note_higher_authority(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id,
    uint32_t now_ms)
{
    if (!higher_epoch_input_is_valid(runtime, cluster_id, term, head_node_id)) {
        return UCN_ERR_ARGUMENT;
    }
    runtime->higher_authority_seen = true;
    /* The RX owner may still have work left in its current service step.
     * Fence before returning so no subsequent local Head transmission can
     * escape through that same step.  M08-08 selects JOIN_PENDING during the
     * following Owner step; M11 later owns the authenticated epoch switch. */
    authority_enter_fenced(runtime,
                           UCN_CLUSTER_AUTHORITY_FENCE_HIGHER_AUTHORITY,
                           now_ms);
    return UCN_OK;
}

ucn_result_t ucn_cluster_authority_runtime_note_term_conflict(
    ucn_cluster_authority_runtime_t *runtime,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t conflicting_head_node_id,
    uint32_t now_ms)
{
    if (runtime == NULL || !runtime->initialized || runtime->cluster == NULL ||
        cluster_id == 0U || conflicting_head_node_id == 0U ||
        conflicting_head_node_id == UCN_NODE_BROADCAST ||
        cluster_id != runtime->cluster->cluster_id ||
        term != runtime->cluster->term ||
        conflicting_head_node_id == runtime->cluster->config.local_node_id) {
        return UCN_ERR_ARGUMENT;
    }
    authority_enter_fenced(runtime, UCN_CLUSTER_AUTHORITY_FENCE_TERM_CONFLICT,
                           now_ms);
    return UCN_OK;
}

static bool message_requires_head_authority(ucn_cluster_message_type_t type,
                                            ucn_cluster_role_t sender_role)
{
    switch (type) {
    case UCN_CLUSTER_MSG_ADVERTISE:
    case UCN_CLUSTER_MSG_HEAD_DECLARE:
    case UCN_CLUSTER_MSG_JOIN_ACCEPT:
    case UCN_CLUSTER_MSG_JOIN_REJECT:
    case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
    case UCN_CLUSTER_MSG_HEAD_STEPDOWN:
    case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
    case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
    case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
        return sender_role == UCN_CLUSTER_ROLE_HEAD;
    case UCN_CLUSTER_MSG_BACKUP_READY:
    case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
    case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
        return sender_role == UCN_CLUSTER_ROLE_BACKUP;
    default:
        return false;
    }
}

bool ucn_cluster_authority_runtime_tx_allowed(
    const ucn_cluster_authority_runtime_t *runtime,
    ucn_cluster_message_type_t type,
    ucn_cluster_role_t sender_role)
{
    if (runtime == NULL || !runtime->initialized || runtime->cluster == NULL) {
        return true; /* no M08 owner: legacy product path is unchanged */
    }
    if (!message_requires_head_authority(type, sender_role)) {
        return true;
    }
    return runtime->cluster->authority_active &&
           phase_is_operational_head(runtime->cluster->authority_phase);
}

bool ucn_cluster_authority_active(const ucn_cluster_t *cluster)
{
    return cluster != NULL && cluster->authority_runtime != NULL &&
           cluster->authority_active;
}

bool ucn_cluster_authority_is_managed(const ucn_cluster_t *cluster)
{
    return cluster != NULL && cluster->authority_runtime != NULL;
}
