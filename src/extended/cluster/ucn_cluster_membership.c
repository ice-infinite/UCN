/* UCN CLV2-M02 (02-04): Cluster membership module.
 *
 * STRUCTURAL REFACTOR ONLY (M02 mandate): Join / Keepalive / Leave /
 * member-table allocation & expiry moved verbatim from the former
 * single ucn_cluster.c.  Every function body is UNCHANGED; M01 froze
 * the FSM semantics.  Do NOT "optimize" anything here.
 *
 * Cross-module: the member-table / join / keepalive entry points are
 * exposed via ucn_cluster_internal.h (de-static only); the send
 * infrastructure (send_message / send_cluster_message / next_nonce)
 * and the backup-domain helpers it calls (backup_resync /
 * assign_backup / queue_backup_assignment_for_member) stay in
 * ucn_cluster.c and are declared in the internal header.
 */

#include "ucn/ucn_cluster.h"

#include <assert.h>
#include <string.h>

#include "ucn/ucn_time.h"

#include "ucn_cluster_internal.h"

#define UCN_CLUSTER_VOTER_HASH_OFFSET UINT32_C(2166136261)
#define UCN_CLUSTER_VOTER_HASH_PRIME UINT32_C(16777619)

static uint32_t voter_hash_byte(uint32_t hash, uint8_t value)
{
    return (hash ^ (uint32_t)value) * UCN_CLUSTER_VOTER_HASH_PRIME;
}

static uint32_t voter_hash_u32(uint32_t hash, uint32_t value)
{
    hash = voter_hash_byte(hash, (uint8_t)(value >> 24U));
    hash = voter_hash_byte(hash, (uint8_t)(value >> 16U));
    hash = voter_hash_byte(hash, (uint8_t)(value >> 8U));
    return voter_hash_byte(hash, (uint8_t)value);
}

static uint32_t voter_set_hash_from_canonical(
    const ucn_cluster_voter_set_t *set)
{
    size_t index;
    uint32_t hash = UCN_CLUSTER_VOTER_HASH_OFFSET;

    hash = voter_hash_u32(hash, set->config_id);
    hash = voter_hash_byte(hash, set->count);
    for (index = 0U; index < (size_t)set->count; ++index) {
        hash = voter_hash_u32(hash, set->node_ids[index]);
    }
    return hash;
}

static bool voter_node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

bool ucn_cluster_member_status_is_valid(ucn_cluster_member_status_t status)
{
    return status == UCN_CLUSTER_MEMBER_STATUS_NONE ||
           status == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL ||
           status == UCN_CLUSTER_MEMBER_STATUS_COMMITTED ||
           status == UCN_CLUSTER_MEMBER_STATUS_REMOVING;
}

bool ucn_cluster_member_transition_is_valid(
    ucn_cluster_member_status_t previous,
    ucn_cluster_member_status_t next)
{
    if (!ucn_cluster_member_status_is_valid(previous) ||
        !ucn_cluster_member_status_is_valid(next)) {
        return false;
    }
    if (previous == next) {
        return true;
    }
    switch (previous) {
    case UCN_CLUSTER_MEMBER_STATUS_NONE:
        return next == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL ||
               next == UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    case UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL:
        return next == UCN_CLUSTER_MEMBER_STATUS_COMMITTED ||
               next == UCN_CLUSTER_MEMBER_STATUS_REMOVING ||
               next == UCN_CLUSTER_MEMBER_STATUS_NONE;
    case UCN_CLUSTER_MEMBER_STATUS_COMMITTED:
        return next == UCN_CLUSTER_MEMBER_STATUS_REMOVING;
    case UCN_CLUSTER_MEMBER_STATUS_REMOVING:
        return next == UCN_CLUSTER_MEMBER_STATUS_COMMITTED ||
               next == UCN_CLUSTER_MEMBER_STATUS_NONE;
    default:
        return false;
    }
}

bool ucn_cluster_member_record_is_valid(const ucn_cluster_member_t *member)
{
    ucn_cluster_member_status_t status;

    if (member == NULL) {
        return false;
    }
    status = (ucn_cluster_member_status_t)member->status;
    if (!member->occupied) {
        return status == UCN_CLUSTER_MEMBER_STATUS_NONE && !member->voting &&
               !member->provisional_deadline_armed &&
               member->wire_version == 0U && member->capabilities == 0U &&
               member->node_id == 0U && member->lease_expires_at_ms == 0U &&
               member->last_nonce == 0U && member->joined_at_ms == 0U &&
               member->last_keepalive_at_ms == 0U &&
               member->provisional_deadline_ms == 0U;
    }
    if (member->node_id == 0U || member->node_id == UCN_NODE_BROADCAST ||
        status == UCN_CLUSTER_MEMBER_STATUS_NONE ||
        !ucn_cluster_member_status_is_valid(status) ||
        (member->wire_version != UCN_CLUSTER_MEMBER_WIRE_VERSION_V3 &&
         member->wire_version != UCN_CLUSTER_MEMBER_WIRE_VERSION_V4)) {
        return false;
    }
    if (member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V3 &&
        member->capabilities != 0U) {
        return false;
    }
    if (status == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL) {
        return !member->voting && member->provisional_deadline_armed;
    }
    return !member->provisional_deadline_armed &&
           member->provisional_deadline_ms == 0U;
}

bool ucn_cluster_member_table_is_valid(
    const ucn_cluster_member_table_t *table)
{
    size_t index;

    if (table == NULL) {
        return false;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (!ucn_cluster_member_record_is_valid(&table->slots[index])) {
            return false;
        }
    }
    return true;
}

size_t ucn_cluster_member_table_count(
    const ucn_cluster_member_table_t *table)
{
    size_t index;
    size_t count = 0U;

    if (table == NULL) {
        return 0U;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (table->slots[index].occupied) {
            ++count;
        }
    }
    return count;
}

bool ucn_cluster_voter_set_is_valid(const ucn_cluster_voter_set_t *set)
{
    size_t index;

    if (set == NULL || set->count == 0U ||
        (size_t)set->count > UCN_CLUSTER_MAX_VOTERS) {
        return false;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_VOTERS; ++index) {
        ucn_node_id_t node_id = set->node_ids[index];

        if (index < (size_t)set->count) {
            if (!voter_node_id_is_valid(node_id) ||
                (index != 0U && node_id <= set->node_ids[index - 1U])) {
                return false;
            }
        } else if (node_id != 0U) {
            return false;
        }
    }
    return set->hash == voter_set_hash_from_canonical(set);
}

bool ucn_cluster_voter_set_build(ucn_cluster_voter_set_t *output,
                                 uint32_t config_id,
                                 const ucn_node_id_t *node_ids,
                                 size_t count)
{
    ucn_cluster_voter_set_t candidate;
    size_t index;
    size_t sorted;

    if (output == NULL || node_ids == NULL || count == 0U ||
        count > UCN_CLUSTER_MAX_VOTERS) {
        return false;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.config_id = config_id;
    candidate.count = (uint8_t)count;
    for (index = 0U; index < count; ++index) {
        if (!voter_node_id_is_valid(node_ids[index])) {
            return false;
        }
        candidate.node_ids[index] = node_ids[index];
    }
    /* Insertion sorting keeps the construction bounded and allocation-free. */
    for (index = 1U; index < count; ++index) {
        ucn_node_id_t value = candidate.node_ids[index];

        sorted = index;
        while (sorted != 0U && candidate.node_ids[sorted - 1U] > value) {
            candidate.node_ids[sorted] = candidate.node_ids[sorted - 1U];
            --sorted;
        }
        candidate.node_ids[sorted] = value;
    }
    for (index = 1U; index < count; ++index) {
        if (candidate.node_ids[index - 1U] == candidate.node_ids[index]) {
            return false;
        }
    }
    candidate.hash = voter_set_hash_from_canonical(&candidate);
    if (!ucn_cluster_voter_set_is_valid(&candidate)) {
        return false;
    }
    *output = candidate;
    return true;
}

bool ucn_cluster_voter_set_contains(const ucn_cluster_voter_set_t *set,
                                    ucn_node_id_t node_id)
{
    size_t index;

    if (!ucn_cluster_voter_set_is_valid(set) ||
        !voter_node_id_is_valid(node_id)) {
        return false;
    }
    for (index = 0U; index < (size_t)set->count; ++index) {
        if (set->node_ids[index] == node_id) {
            return true;
        }
        if (set->node_ids[index] > node_id) {
            break;
        }
    }
    return false;
}

uint8_t ucn_cluster_voter_set_quorum(const ucn_cluster_voter_set_t *set)
{
    if (!ucn_cluster_voter_set_is_valid(set)) {
        return 0U;
    }
    return (uint8_t)((set->count / 2U) + 1U);
}

bool ucn_cluster_voter_set_bitmap_for_node(const ucn_cluster_voter_set_t *set,
                                           ucn_node_id_t node_id,
                                           uint64_t *bitmap)
{
    size_t index;

    if (bitmap == NULL || !ucn_cluster_voter_set_is_valid(set) ||
        !voter_node_id_is_valid(node_id)) {
        return false;
    }
    for (index = 0U; index < (size_t)set->count; ++index) {
        if (set->node_ids[index] == node_id) {
            *bitmap = UINT64_C(1) << index;
            return true;
        }
        if (set->node_ids[index] > node_id) {
            break;
        }
    }
    return false;
}

bool member_initialize_legacy(ucn_cluster_member_t *member,
                              ucn_node_id_t node_id,
                              uint32_t now_ms,
                              uint32_t provisional_timeout_ms)
{
    if (member == NULL || node_id == 0U || node_id == UCN_NODE_BROADCAST) {
        return false;
    }
    (void)memset(member, 0, sizeof(*member));
    member->occupied = true;
    member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V3;
    member->node_id = node_id;
    member->joined_at_ms = now_ms;
    member->last_keepalive_at_ms = now_ms;
#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS)
    /* The historical Current-FSM unit target keeps its pre-M07 fixture model
     * behind the existing M01 test-hook boundary.  It is not a product
     * configuration and must not be confused with a Config Commit.  The
     * separate M06 legacy bridge and the production archive are removed from
     * this path by CLV2-07-12. */
    member->voting = true;
    member->status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    (void)provisional_timeout_ms;
#else
    member->voting = false;
    member->status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    member->provisional_deadline_armed = true;
    member->provisional_deadline_ms = ucn_deadline_from_now(
        now_ms, provisional_timeout_ms);
#endif
    return true;
}

void member_note_legacy_keepalive(ucn_cluster_member_t *member,
                                  uint32_t now_ms)
{
    if (member != NULL && member->occupied) {
        member->last_keepalive_at_ms = now_ms;
    }
}

uint16_t primary_member_count_u16(const ucn_cluster_t *cluster)
{
    return cluster == NULL ? 0U :
           (uint16_t)ucn_cluster_member_table_count(
               &cluster->primary_members);
}

bool primary_member_is_protected_voter(const ucn_cluster_member_t *member)
{
    if (member == NULL || !member->occupied) {
        return false;
    }
#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS)
    /* Historical Current-FSM fixture only; no product target receives this
     * definition.  M07 Config tests link the production archive instead. */
    return true;
#else
    return member->status == (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED &&
           member->voting &&
           member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
#endif
}

uint16_t primary_member_protected_voter_count_u16(
    const ucn_cluster_t *cluster)
{
    size_t index;
    uint16_t count = 0U;

    if (cluster == NULL) {
        return 0U;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (primary_member_is_protected_voter(
                &cluster->primary_members.slots[index])) {
            ++count;
        }
    }
    return count;
}

uint16_t primary_member_available_capacity(const ucn_cluster_t *cluster)
{
    uint16_t used = primary_member_count_u16(cluster);

    return used >= cluster->config.member_capacity ?
               0U : (uint16_t)(cluster->config.member_capacity - used);
}

ucn_cluster_member_t *primary_member_find(
    ucn_cluster_t *cluster,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->primary_members.slots[index].occupied &&
            cluster->primary_members.slots[index].node_id == node_id) {
            return &cluster->primary_members.slots[index];
        }
    }
    return NULL;
}

ucn_cluster_member_t *primary_member_allocate(
    ucn_cluster_t *cluster,
    ucn_node_id_t node_id,
    uint32_t now_ms)
{
    size_t index;
    ucn_cluster_member_t *member = primary_member_find(cluster, node_id);

    if (member != NULL) {
        return member;
    }
    if (primary_member_count_u16(cluster) >= cluster->config.member_capacity) {
        return NULL;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (!cluster->primary_members.slots[index].occupied) {
            if (!member_initialize_legacy(
                    &cluster->primary_members.slots[index], node_id,
                    now_ms, cluster->config.provisional_timeout_ms)) {
                return NULL;
            }
            return &cluster->primary_members.slots[index];
        }
    }
    return NULL;
}

ucn_result_t cluster_admit_verified_v4_provisional_member(
    ucn_cluster_t *cluster,
    ucn_node_id_t node_id,
    uint16_t capabilities,
    uint32_t now_ms,
    ucn_cluster_member_admission_reason_t *reason)
{
    ucn_cluster_member_t *member;

    if (reason != NULL) {
        *reason = UCN_CLUSTER_MEMBER_ADMISSION_NONE;
    }

    if (cluster == NULL || !voter_node_id_is_valid(node_id)) {
        if (reason != NULL) {
            *reason = UCN_CLUSTER_MEMBER_ADMISSION_ARGUMENT;
        }
        return UCN_ERR_ARGUMENT;
    }
    if (cluster->role != UCN_CLUSTER_ROLE_HEAD) {
        if (reason != NULL) {
            *reason = UCN_CLUSTER_MEMBER_ADMISSION_NOT_HEAD;
        }
        return UCN_ERR_ACCESS;
    }
    member = primary_member_find(cluster, node_id);
    if (member != NULL) {
        if (member->status ==
                (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL &&
            !member->voting &&
            member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4) {
            return UCN_OK;
        }
        if (reason != NULL) {
            *reason = UCN_CLUSTER_MEMBER_ADMISSION_MEMBER_CONFLICT;
        }
        return UCN_ERR_STATE;
    }
    if (primary_member_count_u16(cluster) >= cluster->config.member_capacity) {
        if (reason != NULL) {
            *reason = UCN_CLUSTER_MEMBER_ADMISSION_RUNTIME_CAPACITY;
        }
        return UCN_ERR_NO_SPACE;
    }
    for (size_t index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (!cluster->primary_members.slots[index].occupied) {
            member = &cluster->primary_members.slots[index];
            (void)memset(member, 0, sizeof(*member));
            member->occupied = true;
            member->voting = false;
            member->status =
                (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
            member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
            member->capabilities = capabilities;
            member->node_id = node_id;
            member->joined_at_ms = now_ms;
            member->last_keepalive_at_ms = now_ms;
            member->provisional_deadline_armed = true;
            member->provisional_deadline_ms = ucn_deadline_from_now(
                now_ms, cluster->config.provisional_timeout_ms);
            return UCN_OK;
        }
    }
    if (reason != NULL) {
        *reason = UCN_CLUSTER_MEMBER_ADMISSION_RUNTIME_CAPACITY;
    }
    return UCN_ERR_NO_SPACE;
}

ucn_result_t cluster_preflight_provisional_voter_commit(
    const ucn_cluster_t *cluster,
    ucn_node_id_t node_id,
    ucn_cluster_member_admission_reason_t *reason)
{
    const ucn_cluster_member_t *member = NULL;
    size_t index;

    if (reason != NULL) {
        *reason = UCN_CLUSTER_MEMBER_ADMISSION_NONE;
    }
    if (cluster == NULL || !voter_node_id_is_valid(node_id)) {
        if (reason != NULL) {
            *reason = UCN_CLUSTER_MEMBER_ADMISSION_ARGUMENT;
        }
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->primary_members.slots[index].occupied &&
            cluster->primary_members.slots[index].node_id == node_id) {
            member = &cluster->primary_members.slots[index];
            break;
        }
    }
    if (member == NULL ||
        member->status != (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL ||
        member->voting ||
        member->wire_version != UCN_CLUSTER_MEMBER_WIRE_VERSION_V4) {
        if (reason != NULL) {
            *reason = UCN_CLUSTER_MEMBER_ADMISSION_MEMBER_CONFLICT;
        }
        return UCN_ERR_STATE;
    }
    if (!ucn_cluster_voter_set_is_valid(&cluster->active_voter_set)) {
        if (reason != NULL) {
            *reason = UCN_CLUSTER_MEMBER_ADMISSION_CONFIG_UNAVAILABLE;
        }
        return UCN_ERR_STATE;
    }
    if (cluster->active_voter_set.count >= cluster->config.voter_capacity) {
        if (reason != NULL) {
            *reason = UCN_CLUSTER_MEMBER_ADMISSION_VOTER_CAPACITY;
        }
        return UCN_ERR_NO_SPACE;
    }
    return UCN_OK;
}

size_t primary_member_expire_provisionals(ucn_cluster_t *cluster,
                                          uint32_t now_ms)
{
    size_t index;
    size_t expired = 0U;

    if (cluster == NULL || cluster->role != UCN_CLUSTER_ROLE_HEAD) {
        return 0U;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        ucn_cluster_member_t *member = &cluster->primary_members.slots[index];

        if (member->occupied &&
            member->status ==
                (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL &&
            member->provisional_deadline_armed &&
            ucn_deadline_expired(now_ms, member->provisional_deadline_ms)) {
            (void)memset(member, 0, sizeof(*member));
            ++expired;
        }
    }
    cluster->stats.provisional_members_expired += (uint32_t)expired;
    return expired;
}

void remove_member(ucn_cluster_t *cluster, ucn_node_id_t node_id,
                          uint32_t now_ms)
{
    ucn_cluster_member_t *member = primary_member_find(cluster, node_id);

    if (cluster->backup_node_id == node_id) {
        /* CLV2-01-04d.4: preflight pattern (human auditor design) for the
         * backup-eviction branch - the FIRST irreversible-site wiring.
         * The preflight validates the PRE-CALL state with ZERO writes, so
         * a rejected validation (shadow desync / illegal pair / pre-mutated
         * phase fields) aborts BEFORE the member slot or the backup fields
         * are touched.  The transition then commits BEFORE the phase-
         * relevant clears: cluster_transition_validate() re-derives
         * old_phase from the legacy fields at commit time, so a site that
         * already cleared backup_node_id would be rejected as
         * 'pre-mutated'.  apply_legacy(HEAD_NO_BACKUP) writes role +
         * node_id=0 + ready=false, so the site's own clears below are
         * idempotent (d.0 framework note) and run in the original order. */
        ucn_cluster_phase_t old_phase =
            cluster_phase_from_legacy_state(cluster, now_ms);

        if (cluster_transition_preflight(cluster, old_phase,
                                         UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                                         now_ms) != UCN_OK) {
            /* Fail closed: NOTHING is touched - the member slot stays
             * occupied and the backup identity is untouched. */
            return;
        }
        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                               UCN_CLUSTER_REASON_BACKUP_LOST,
                               now_ms) != UCN_OK) {
            /* Fail closed: a rejected commit also leaves every field
             * untouched (nothing was mutated yet). */
            return;
        }
        /* Current irreversible side effects in original order: free the
         * member slot, clear the backup identity (idempotent with
         * apply_legacy), then resync (early-returns: node_id == 0). */
        if (member != NULL) {
            (void)memset(member, 0, sizeof(*member));
        }
        cluster->backup_node_id = 0U;
        cluster->backup_ready = false;
        backup_resync(cluster);
#if !defined(NDEBUG)
        /* CLV2-01-04d.4 post-commit derive assert: after the transition
         * AND the site clears the legacy state must derive HEAD_NO_BACKUP. */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
#endif
        return;
    }
    if (member != NULL) {
        (void)memset(member, 0, sizeof(*member));
    }
    backup_resync(cluster);
}

void primary_member_table_clear(ucn_cluster_t *cluster)
{
    if (cluster != NULL) {
        (void)memset(cluster->primary_members.slots, 0,
                     sizeof(cluster->primary_members.slots));
    }
}

ucn_result_t handle_join_request(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    const ucn_cluster_message_t *message,
    uint32_t now_ms)
{
    ucn_cluster_member_t *member;
    bool member_was_present;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        message->head_node_id != cluster->config.local_node_id ||
        message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term) {
        cluster->stats.joins_rejected++;
        return UCN_ERR_ACCESS;
    }
    member = primary_member_allocate(cluster, source, now_ms);
    if (member == NULL) {
        cluster->stats.joins_rejected++;
        (void)send_join_reply(cluster, source, UCN_CLUSTER_MSG_JOIN_REJECT,
                              message->nonce);
        return UCN_ERR_NO_SPACE;
    }
    member_was_present = member->last_nonce != 0U;
    if (member->last_nonce != 0U && message->nonce <= member->last_nonce) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    member->last_nonce = message->nonce;
    member->lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    member_note_legacy_keepalive(member, now_ms);
    cluster->stats.joins_accepted++;
    assign_backup(cluster, now_ms);
    if (!member_was_present) {
        backup_resync(cluster);
        queue_backup_assignment_for_member(cluster, source, now_ms);
    }
    /* C07.7 P1: echo the request nonce (join txid). */
    return send_join_reply(cluster, source, UCN_CLUSTER_MSG_JOIN_ACCEPT,
                           message->nonce);
}

ucn_result_t send_join_reply(ucn_cluster_t *cluster,
                                    ucn_node_id_t destination,
                                    ucn_cluster_message_type_t type,
                                    uint32_t join_nonce)
{
    ucn_cluster_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.type = type;
    message.role = UCN_CLUSTER_ROLE_HEAD;
    message.cluster_id = cluster->cluster_id;
    message.term = cluster->term;
    message.head_node_id = cluster->config.local_node_id;
    message.head_score = cluster->config.head_score;
    message.available_capacity = primary_member_available_capacity(cluster);
    message.lease_ms = cluster->config.lease_ms;
    message.nonce = join_nonce;
    return send_cluster_message(cluster, destination, &message);
}

ucn_result_t handle_join_accept(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    const ucn_cluster_message_t *message,
    uint32_t now_ms)
{
    {
        bool pre_assigned_backup = (cluster->role == UCN_CLUSTER_ROLE_BACKUP &&
                                     cluster->backup_syncing);

        if (!pre_assigned_backup &&
            cluster->role != UCN_CLUSTER_ROLE_JOIN_PENDING) {
            return UCN_ERR_ACCESS;
        }
        if (source != cluster->pending_head_node_id ||
            message->head_node_id != source ||
            message->cluster_id != cluster->pending_cluster_id ||
            message->term != cluster->pending_term ||
            /* C07.7 P1: the accept must echo the exact join txid. */
            message->nonce != cluster->pending_join_nonce) {
            return UCN_ERR_ACCESS;
        }
    }
    if (cluster->role == UCN_CLUSTER_ROLE_BACKUP) {
        /* CLV2-01-04b.4: a pre-assigned Backup node (BACKUP_ASSIGN(self)
         * won the race against this late JOIN_ACCEPT) must NOT transition
         * - it only refreshes the epoch fields below (keep current
         * behaviour; the shadow stays BACKUP_SYNCING). */
    } else {
        /* CLV2-01-04b.4: the role write IS the JOIN_PENDING ->
         * MEMBER_ACTIVE transition.  The runtime pre-derive (shadow ==
         * JOIN_PENDING + legacy derives JOIN_PENDING) rejects in Debug if
         * a site pre-mutated phase-relevant fields; fail closed BEFORE
         * the epoch refresh. */
        if (cluster_transition(cluster, UCN_CLUSTER_PHASE_JOIN_PENDING,
                               UCN_CLUSTER_PHASE_MEMBER_ACTIVE,
                               UCN_CLUSTER_REASON_JOIN_ACCEPTED,
                               now_ms) != UCN_OK) {
            /* Fail closed per the migration contract: a rejected
             * transition (shadow mismatch / illegal pair / pre-mutated
             * phase fields) leaves every field untouched, so do NOT run
             * the accept side effects on a non-MEMBER node. */
            return UCN_ERR_STATE;
        }
    }
    cluster->role_since_ms = now_ms;
    /* CLV2-M12.3: JOIN_ACCEPT is a stable-domain commit.  Scrub every
     * Recovery-domain identity before installing the stable Epoch; leaving
     * the old id/nonce/source live lets a delayed RECOVERY_DECLARE refresh
     * the stable Head lease or masquerade as the current Recovery round. */
    cluster->recovery_cluster_id = 0U;
    cluster->recovery_deadline_ms = 0U;
    cluster->recovery_cooldown_until_ms = 0U;
    cluster->recovery_backoff_deadline_ms = 0U;
    cluster->recovery_nonce = 0U;
    cluster->accepted_recovery_nonce = 0U;
    cluster->known_recovery_source = 0U;
    cluster->recovery_ack_count = 0U;
    cluster->recovery_acked = 0U;
    cluster->cluster_id = message->cluster_id;
    cluster->term = message->term;
    cluster->head_node_id = source;
    cluster_history_note_stable_epoch(cluster, cluster->cluster_id,
                                      cluster->term,
                                      cluster->head_node_id);
    /* CLV2-M12 (12-03): JOIN_ACCEPT only ever comes from a stable Head
     * (recovery-domain joins use RECOVERY_DECLARE/ACK), so this is the
     * single site that arms the sustained-stable-join lineage reset. */
    cluster_lineage_reset_arm(cluster, now_ms);
    cluster->current_head_score = message->head_score;
    cluster->head_lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, message->lease_ms);
    cluster->head_grace_deadline_ms = 0U;
    cluster->next_keepalive_ms = now_ms;
    cluster->pending_head_node_id = 0U;
    cluster->pending_cluster_id = 0U;
    cluster->pending_term = 0U;
    cluster->stats.joins_accepted++;
#if !defined(NDEBUG)
    /* CLV2-01-04b.4 post-commit derive assert: after the transition AND
     * every site side effect the join path must still derive
     * MEMBER_ACTIVE (derive depends only on role == MEMBER with no armed
     * grace deadline).  The pre-assigned Backup path performs no
     * transition, so its (unchanged) BACKUP_SYNCING shadow is untouched. */
    if (cluster->role != UCN_CLUSTER_ROLE_BACKUP) {
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_MEMBER_ACTIVE);
    }
#endif
    return UCN_OK;
}

ucn_result_t handle_keepalive(
    ucn_cluster_t *cluster,
    ucn_node_id_t source,
    const ucn_cluster_message_t *message,
    uint32_t now_ms)
{
    ucn_cluster_member_t *member;

    if (cluster->role != UCN_CLUSTER_ROLE_HEAD ||
        message->head_node_id != cluster->config.local_node_id ||
        message->cluster_id != cluster->cluster_id ||
        message->term != cluster->term) {
        return UCN_ERR_ACCESS;
    }
    member = primary_member_find(cluster, source);
    if (member == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (message->nonce <= member->last_nonce) {
        cluster->stats.stale_messages++;
        return UCN_ERR_REPLAY;
    }
    member->last_nonce = message->nonce;
    member->lease_expires_at_ms =
        ucn_deadline_from_now(now_ms, cluster->config.lease_ms);
    member_note_legacy_keepalive(member, now_ms);
    return UCN_OK;
}

void expire_members(ucn_cluster_t *cluster, uint32_t now_ms)
{
    size_t index;
    bool changed = primary_member_expire_provisionals(cluster, now_ms) != 0U;
    bool backup_expired = false;

    /* CLV2-01-04d.4: preflight pattern for the backup-expiry branch.  The
     * backup's expiry is detected on the PRE-CALL state (read-only), the
     * transition is validated + committed BEFORE the phase-relevant clears
     * (the d.0 derive check rejects a post-mutation commit; apply_legacy
     * clears node_id/ready, so the eviction loop's own clears below are
     * idempotent), and the Current irreversible side effects run after in
     * original order.  A non-backup expiry keeps the legacy path below: no
     * transition, no shadow write. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->backup_node_id != 0U &&
            cluster->primary_members.slots[index].occupied &&
            cluster->primary_members.slots[index].status !=
                (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL &&
            cluster->primary_members.slots[index].node_id == cluster->backup_node_id &&
            ucn_deadline_expired(now_ms,
                                 cluster->primary_members.slots[index].lease_expires_at_ms)) {
            backup_expired = true;
            break;
        }
    }

    if (backup_expired) {
        ucn_cluster_phase_t old_phase =
            cluster_phase_from_legacy_state(cluster, now_ms);

        if (cluster_transition_preflight(cluster, old_phase,
                                         UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                                         now_ms) != UCN_OK) {
            /* Fail closed: a rejected preflight leaves every member slot
             * AND the backup fields untouched (no partial eviction). */
            return;
        }
        if (cluster_transition(cluster, old_phase,
                               UCN_CLUSTER_PHASE_HEAD_NO_BACKUP,
                               UCN_CLUSTER_REASON_BACKUP_LOST,
                               now_ms) != UCN_OK) {
            /* Fail closed: a rejected commit leaves every field untouched. */
            return;
        }
    }

    /* Current irreversible side effects in original order: evict every
     * expired member (the Backup included when it expired), clear the
     * backup identity, then resync. */
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (cluster->primary_members.slots[index].occupied &&
            (cluster->primary_members.slots[index].status !=
                 (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL ||
             cluster->role == UCN_CLUSTER_ROLE_RECOVERY_HEAD) &&
            ucn_deadline_expired(now_ms,
                                 cluster->primary_members.slots[index].lease_expires_at_ms)) {
            ucn_node_id_t expired_id = cluster->primary_members.slots[index].node_id;

            (void)memset(&cluster->primary_members.slots[index], 0,
                         sizeof(cluster->primary_members.slots[index]));
            cluster->stats.member_leases_expired++;
            changed = true;
            if (cluster->backup_node_id == expired_id) {
                cluster->backup_node_id = 0U;
                cluster->backup_ready = false;
            }
        }
    }
    if (changed) {
        backup_resync(cluster);
    }
#if !defined(NDEBUG)
    if (backup_expired) {
        /* CLV2-01-04d.4 post-commit derive assert: after the transition
         * AND the eviction loop the legacy state must derive
         * HEAD_NO_BACKUP. */
        assert(cluster_phase_from_legacy_state(cluster, now_ms) ==
               UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    }
#endif
}

void send_join_request(ucn_cluster_t *cluster, uint32_t now_ms)
{
    ucn_cluster_message_t message;
    ucn_result_t result;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_REQUEST;
    message.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    message.cluster_id = cluster->pending_cluster_id;
    message.term = cluster->pending_term;
    message.head_node_id = cluster->pending_head_node_id;
    message.head_score = cluster->pending_head_score;
    message.lease_ms = cluster->config.lease_ms;
    /* C07.7 P1: the request nonce is the join transaction id;
     * JOIN_ACCEPT/JOIN_REJECT must echo it so a stale reject of an
     * earlier attempt cannot abort a newer one. */
    message.nonce = next_nonce(cluster);
    cluster->pending_join_nonce = message.nonce;
    result = send_cluster_message(cluster, cluster->pending_head_node_id,
                                   &message);

    if (result == UCN_OK) {
        cluster->stats.joins_requested++;
    }
    cluster->next_join_retry_ms =
        ucn_deadline_from_now(now_ms, cluster->config.join_retry_ms);
}

void send_keepalive(ucn_cluster_t *cluster, uint32_t now_ms)
{
    (void)send_message(cluster, cluster->head_node_id,
                       UCN_CLUSTER_MSG_KEEPALIVE, UCN_CLUSTER_ROLE_MEMBER,
                       cluster->cluster_id, cluster->term,
                       cluster->head_node_id, cluster->current_head_score, 0U);
    cluster->next_keepalive_ms =
        ucn_deadline_from_now(now_ms, cluster->config.keepalive_interval_ms);
}
