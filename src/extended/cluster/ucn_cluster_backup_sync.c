/* CLV2-M09 (09-03): strict, wire-agnostic SYNC_BEGIN owner. */

#include "ucn/ucn_cluster_backup_sync.h"

#include <string.h>

#include "ucn/ucn_time.h"

#define UCN_CLUSTER_BACKUP_SYNC_HASH_OFFSET UINT32_C(2166136261)
#define UCN_CLUSTER_BACKUP_SYNC_HASH_PRIME UINT32_C(16777619)

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool source_is_assigned_head(const ucn_cluster_backup_sync_owner_t *owner,
                                    ucn_node_id_t source_node_id)
{
    return owner != NULL && source_node_id == owner->assigned_epoch.head_node_id;
}

static uint32_t hash_byte(uint32_t hash, uint8_t value)
{
    return (hash ^ (uint32_t)value) * UCN_CLUSTER_BACKUP_SYNC_HASH_PRIME;
}

static uint32_t hash_u16(uint32_t hash, uint16_t value)
{
    hash = hash_byte(hash, (uint8_t)(value >> 8U));
    return hash_byte(hash, (uint8_t)value);
}

static uint32_t hash_u32(uint32_t hash, uint32_t value)
{
    hash = hash_byte(hash, (uint8_t)(value >> 24U));
    hash = hash_byte(hash, (uint8_t)(value >> 16U));
    hash = hash_byte(hash, (uint8_t)(value >> 8U));
    return hash_byte(hash, (uint8_t)value);
}

static bool coverage_entry_state_is_valid(uint8_t state)
{
    return state == (uint8_t)UCN_CLUSTER_BACKUP_PEER_ADMITTED ||
           state == (uint8_t)UCN_CLUSTER_BACKUP_PEER_SUSPECT ||
           state == (uint8_t)UCN_CLUSTER_BACKUP_PEER_REMOVED;
}

static bool coverage_contains_admitted(const ucn_cluster_backup_coverage_t *coverage,
                                       ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < (size_t)coverage->count; ++index) {
        if (coverage->entries[index].node_id == node_id) {
            return coverage->entries[index].state ==
                   (uint8_t)UCN_CLUSTER_BACKUP_PEER_ADMITTED;
        }
    }
    return false;
}

static bool coverage_contains_removed(const ucn_cluster_backup_coverage_t *coverage,
                                      ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < (size_t)coverage->count; ++index) {
        if (coverage->entries[index].node_id == node_id) {
            return coverage->entries[index].state ==
                   (uint8_t)UCN_CLUSTER_BACKUP_PEER_REMOVED;
        }
    }
    return false;
}

static bool coverage_contains_suspect(const ucn_cluster_backup_coverage_t *coverage,
                                      ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < (size_t)coverage->count; ++index) {
        if (coverage->entries[index].node_id == node_id) {
            return coverage->entries[index].state ==
                   (uint8_t)UCN_CLUSTER_BACKUP_PEER_SUSPECT;
        }
    }
    return false;
}

static bool config_set_is_covered(const ucn_cluster_voter_set_t *set,
                                   const ucn_cluster_backup_coverage_t *coverage)
{
    size_t index;

    for (index = 0U; index < (size_t)set->count; ++index) {
        if (!coverage_contains_admitted(coverage, set->node_ids[index])) {
            return false;
        }
    }
    return true;
}

static bool config_set_has_removed_member(
    const ucn_cluster_voter_set_t *set,
    const ucn_cluster_backup_coverage_t *coverage)
{
    size_t index;

    for (index = 0U; index < (size_t)set->count; ++index) {
        if (coverage_contains_removed(coverage, set->node_ids[index])) {
            return true;
        }
    }
    return false;
}

static bool config_set_has_missing_member(
    const ucn_cluster_voter_set_t *set,
    const ucn_cluster_backup_coverage_t *coverage)
{
    size_t index;

    for (index = 0U; index < (size_t)set->count; ++index) {
        ucn_node_id_t node_id = set->node_ids[index];

        if (!coverage_contains_admitted(coverage, node_id) &&
            !coverage_contains_suspect(coverage, node_id) &&
            !coverage_contains_removed(coverage, node_id)) {
            return true;
        }
    }
    return false;
}

static bool config_set_has_suspect_member(
    const ucn_cluster_voter_set_t *set,
    const ucn_cluster_backup_coverage_t *coverage)
{
    size_t index;

    for (index = 0U; index < (size_t)set->count; ++index) {
        if (coverage_contains_suspect(coverage, set->node_ids[index])) {
            return true;
        }
    }
    return false;
}

static bool config_has_removed_protected_voter(
    const ucn_cluster_config_state_t *config,
    const ucn_cluster_backup_coverage_t *coverage)
{
    return config_set_has_removed_member(&config->old_set, coverage) ||
           config_set_has_removed_member(&config->new_set, coverage);
}

static bool config_has_missing_protected_voter(
    const ucn_cluster_config_state_t *config,
    const ucn_cluster_backup_coverage_t *coverage)
{
    return config_set_has_missing_member(&config->old_set, coverage) ||
           config_set_has_missing_member(&config->new_set, coverage);
}

static bool config_has_suspect_protected_voter(
    const ucn_cluster_config_state_t *config,
    const ucn_cluster_backup_coverage_t *coverage)
{
    return config_set_has_suspect_member(&config->old_set, coverage) ||
           config_set_has_suspect_member(&config->new_set, coverage);
}

static bool sync_epoch_is_active(const ucn_cluster_backup_sync_owner_t *owner,
                                 const ucn_cluster_snapshot_epoch_t *epoch)
{
    return owner != NULL && epoch != NULL && owner->mirror.staging_active &&
           ucn_cluster_snapshot_epoch_is_exact(
               ucn_cluster_backup_mirror_staging_epoch(&owner->mirror), epoch);
}

static bool table_contains_node(const ucn_cluster_member_table_t *table,
                                ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (table->slots[index].occupied && table->slots[index].node_id == node_id) {
            return true;
        }
    }
    return false;
}

static ucn_cluster_member_t *table_first_free(ucn_cluster_member_table_t *table)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (!table->slots[index].occupied) {
            return &table->slots[index];
        }
    }
    return NULL;
}

static ucn_cluster_member_t *table_find_node(ucn_cluster_member_table_t *table,
                                             ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (table->slots[index].occupied && table->slots[index].node_id == node_id) {
            return &table->slots[index];
        }
    }
    return NULL;
}

/* A live Delta is only a freshness update for a member already proven by the
 * committed full Snapshot.  Membership, voter status, Wire capabilities and
 * provisional lifetime remain frozen until the next full Snapshot / Config
 * transaction.  This prevents a later Delta sequence from manufacturing a
 * new mirror member or changing static eligibility beneath M10. */
static bool delta_preserves_member_identity(const ucn_cluster_member_t *current,
                                            const ucn_cluster_member_t *incoming)
{
    return current != NULL && incoming != NULL && current->occupied &&
           incoming->occupied && current->node_id == incoming->node_id &&
           current->voting == incoming->voting &&
           current->provisional_deadline_armed ==
               incoming->provisional_deadline_armed &&
           current->status == incoming->status &&
           current->wire_version == incoming->wire_version &&
           current->capabilities == incoming->capabilities &&
           current->joined_at_ms == incoming->joined_at_ms &&
           current->provisional_deadline_ms == incoming->provisional_deadline_ms &&
           incoming->last_nonce >= current->last_nonce;
}

static uint32_t table_snapshot_hash(const ucn_cluster_member_table_t *table)
{
    size_t index;
    uint32_t hash = UCN_CLUSTER_BACKUP_SYNC_HASH_OFFSET;

    if (!ucn_cluster_member_table_is_valid(table)) {
        return 0U;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_MEMBERS; ++index) {
        if (table->slots[index].occupied) {
            hash = ucn_cluster_backup_sync_member_hash_update(hash,
                                                               &table->slots[index]);
            if (hash == 0U) {
                return 0U;
            }
        }
    }
    return hash;
}

bool ucn_cluster_backup_sync_owner_is_valid(
    const ucn_cluster_backup_sync_owner_t *owner)
{
    return owner != NULL && owner->initialized &&
           ucn_cluster_backup_mirror_is_valid(&owner->mirror) &&
           ucn_cluster_backup_epoch_is_valid(&owner->assigned_epoch) &&
           ucn_cluster_config_state_is_valid(&owner->active_config);
}

bool ucn_cluster_backup_coverage_is_valid(
    const ucn_cluster_backup_coverage_t *coverage)
{
    size_t index;

    if (coverage == NULL || (size_t)coverage->count > UCN_CLUSTER_MAX_VOTERS) {
        return false;
    }
    for (index = 0U; index < UCN_CLUSTER_MAX_VOTERS; ++index) {
        const ucn_cluster_backup_coverage_entry_t *entry = &coverage->entries[index];

        if (index < (size_t)coverage->count) {
            if (entry->node_id == 0U || entry->node_id == UCN_NODE_BROADCAST ||
                !coverage_entry_state_is_valid(entry->state) ||
                (index != 0U && entry->node_id <= coverage->entries[index - 1U].node_id)) {
                return false;
            }
        } else if (entry->node_id != 0U || entry->state != 0U) {
            return false;
        }
    }
    return true;
}

bool ucn_cluster_backup_coverage_initial_ready(
    const ucn_cluster_config_state_t *config,
    const ucn_cluster_backup_coverage_t *coverage)
{
    return ucn_cluster_config_state_is_valid(config) &&
           ucn_cluster_backup_coverage_is_valid(coverage) &&
           config_set_is_covered(&config->old_set, coverage) &&
           config_set_is_covered(&config->new_set, coverage);
}

uint32_t ucn_cluster_backup_sync_member_hash_update(
    uint32_t hash, const ucn_cluster_member_t *member)
{
    if (hash == 0U || member == NULL || !member->occupied ||
        member->last_nonce == 0U || !ucn_cluster_member_record_is_valid(member)) {
        return 0U;
    }
    hash = hash_byte(hash, 1U);
    hash = hash_byte(hash, member->voting ? 1U : 0U);
    hash = hash_byte(hash, member->provisional_deadline_armed ? 1U : 0U);
    hash = hash_byte(hash, member->status);
    hash = hash_byte(hash, member->wire_version);
    hash = hash_u16(hash, member->capabilities);
    hash = hash_u32(hash, member->node_id);
    hash = hash_u32(hash, member->lease_expires_at_ms);
    hash = hash_u32(hash, member->last_nonce);
    hash = hash_u32(hash, member->joined_at_ms);
    hash = hash_u32(hash, member->last_keepalive_at_ms);
    return hash_u32(hash, member->provisional_deadline_ms);
}

ucn_result_t ucn_cluster_backup_sync_owner_init(
    ucn_cluster_backup_sync_owner_t *owner,
    const ucn_cluster_backup_epoch_t *assigned_epoch,
    const ucn_cluster_config_state_t *active_config)
{
    ucn_cluster_backup_sync_owner_t candidate;

    if (owner == NULL || assigned_epoch == NULL || active_config == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_epoch_is_valid(assigned_epoch) ||
        !ucn_cluster_config_state_is_valid(active_config)) {
        return UCN_ERR_STATE;
    }
    if (ucn_cluster_backup_epoch_rekey_required(assigned_epoch)) {
        return UCN_ERR_EXHAUSTED;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    ucn_cluster_backup_mirror_reset(&candidate.mirror);
    candidate.assigned_epoch = *assigned_epoch;
    candidate.active_config = *active_config;
    candidate.initialized = true;
    if (!ucn_cluster_backup_sync_owner_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    /* This is an atomic local replacement.  It cannot preserve an old
     * committed/staging mirror across a BackupEpoch reassignment. */
    *owner = candidate;
    return UCN_OK;
}

ucn_result_t ucn_cluster_backup_sync_owner_begin(
    ucn_cluster_backup_sync_owner_t *owner,
    ucn_node_id_t source_node_id,
    uint32_t begin_sequence,
    const ucn_cluster_snapshot_epoch_t *snapshot_epoch)
{
    if (owner == NULL || snapshot_epoch == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_sync_owner_is_valid(owner)) {
        return UCN_ERR_STATE;
    }
    if (!source_is_assigned_head(owner, source_node_id)) {
        return UCN_ERR_ACCESS;
    }
    if (begin_sequence != 0U) {
        return UCN_ERR_REPLAY;
    }
    if (!ucn_cluster_backup_epoch_is_exact(&snapshot_epoch->backup_epoch,
                                           &owner->assigned_epoch) ||
        !ucn_cluster_snapshot_epoch_matches_config(snapshot_epoch,
                                                   &owner->active_config)) {
        return UCN_ERR_REPLAY;
    }
    {
        ucn_result_t result = ucn_cluster_backup_mirror_begin_staging(
            &owner->mirror, snapshot_epoch);

        if (result != UCN_OK) {
            return result;
        }
    }
    owner->next_member_sequence = 1U;
    owner->received_member_count = 0U;
    owner->running_snapshot_hash = UCN_CLUSTER_BACKUP_SYNC_HASH_OFFSET;
    owner->resync_required = false;
    return UCN_OK;
}

ucn_result_t ucn_cluster_backup_sync_owner_abort(
    ucn_cluster_backup_sync_owner_t *owner)
{
    ucn_result_t result;

    if (owner == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_sync_owner_is_valid(owner)) {
        return UCN_ERR_STATE;
    }
    result = ucn_cluster_backup_mirror_abort_staging(&owner->mirror);
    if (result != UCN_OK) {
        return result;
    }
    owner->next_member_sequence = 0U;
    owner->received_member_count = 0U;
    owner->running_snapshot_hash = 0U;
    return UCN_OK;
}

ucn_result_t ucn_cluster_backup_sync_owner_member(
    ucn_cluster_backup_sync_owner_t *owner,
    ucn_node_id_t source_node_id,
    const ucn_cluster_snapshot_epoch_t *snapshot_epoch,
    uint32_t sequence,
    const ucn_cluster_member_t *member)
{
    ucn_cluster_member_table_t *staging;
    ucn_cluster_member_t *slot;
    uint32_t next_hash;

    if (owner == NULL || snapshot_epoch == NULL || member == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_sync_owner_is_valid(owner)) {
        return UCN_ERR_STATE;
    }
    if (!source_is_assigned_head(owner, source_node_id)) {
        return UCN_ERR_ACCESS;
    }
    if (!sync_epoch_is_active(owner, snapshot_epoch) ||
        !ucn_cluster_snapshot_epoch_matches_config(snapshot_epoch,
                                                   &owner->active_config) ||
        !serial_is_valid(sequence) || sequence != owner->next_member_sequence ||
        owner->received_member_count >= UCN_CLUSTER_MAX_MEMBERS ||
        !member->occupied || member->last_nonce == 0U ||
        !ucn_cluster_member_record_is_valid(member)) {
        return UCN_ERR_REPLAY;
    }
    staging = ucn_cluster_backup_mirror_staging(&owner->mirror);
    if (staging == NULL) {
        return UCN_ERR_STATE;
    }
    if (table_contains_node(staging, member->node_id)) {
        return UCN_ERR_REPLAY;
    }
    slot = table_first_free(staging);
    next_hash = ucn_cluster_backup_sync_member_hash_update(
        owner->running_snapshot_hash, member);
    if (slot == NULL || next_hash == 0U ||
        sequence >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return slot == NULL ? UCN_ERR_NO_SPACE : UCN_ERR_EXHAUSTED;
    }
    *slot = *member;
    owner->running_snapshot_hash = next_hash;
    owner->received_member_count++;
    owner->next_member_sequence = sequence + 1U;
    return UCN_OK;
}

ucn_result_t ucn_cluster_backup_sync_owner_end(
    ucn_cluster_backup_sync_owner_t *owner,
    ucn_node_id_t source_node_id,
    const ucn_cluster_snapshot_epoch_t *snapshot_epoch,
    uint32_t final_sequence,
    uint32_t member_count,
    uint32_t snapshot_hash,
    const ucn_cluster_backup_coverage_t *coverage)
{
    uint32_t expected_final_sequence;
    ucn_result_t result;

    if (owner == NULL || snapshot_epoch == NULL || coverage == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_sync_owner_is_valid(owner)) {
        return UCN_ERR_STATE;
    }
    if (!source_is_assigned_head(owner, source_node_id)) {
        return UCN_ERR_ACCESS;
    }
    if (!sync_epoch_is_active(owner, snapshot_epoch) ||
        !ucn_cluster_snapshot_epoch_matches_config(snapshot_epoch,
                                                   &owner->active_config) ||
        member_count != owner->received_member_count ||
        snapshot_hash == 0U || snapshot_hash != owner->running_snapshot_hash ||
        !ucn_cluster_backup_coverage_initial_ready(&owner->active_config,
                                                   coverage)) {
        return UCN_ERR_REPLAY;
    }
    /* Target FSM §22.3: BEGIN=0, MEMBER=1..N and END=N+1.  The next
     * member sequence is therefore the exact END sequence, including one
     * for an empty snapshot. */
    expected_final_sequence = owner->next_member_sequence;
    if (final_sequence != expected_final_sequence ||
        final_sequence >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return UCN_ERR_REPLAY;
    }
    result = ucn_cluster_backup_mirror_commit_staging_exact(&owner->mirror,
                                                             snapshot_epoch);
    if (result != UCN_OK) {
        return result;
    }
    owner->committed_final_sequence = final_sequence;
    owner->committed_snapshot_hash = snapshot_hash;
    owner->next_delta_sequence = final_sequence == 0U ? 1U :
                                 final_sequence + 1U;
    owner->next_member_sequence = 0U;
    owner->received_member_count = 0U;
    owner->running_snapshot_hash = 0U;
    return UCN_OK;
}

ucn_result_t ucn_cluster_backup_sync_owner_verify_ready(
    const ucn_cluster_backup_sync_owner_t *owner,
    const ucn_cluster_backup_ready_t *ready)
{
    const ucn_cluster_snapshot_epoch_t *committed_epoch;

    if (owner == NULL || ready == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_sync_owner_is_valid(owner)) {
        return UCN_ERR_STATE;
    }
    committed_epoch = ucn_cluster_backup_mirror_committed_epoch(&owner->mirror);
    if (committed_epoch == NULL ||
        ready->source_node_id != owner->assigned_epoch.backup_node_id ||
        !ucn_cluster_snapshot_epoch_is_exact(&ready->snapshot_epoch,
                                             committed_epoch) ||
        !ucn_cluster_snapshot_epoch_matches_config(&ready->snapshot_epoch,
                                                   &owner->active_config) ||
        ready->final_sequence != owner->committed_final_sequence ||
        ready->snapshot_hash == 0U ||
        ready->snapshot_hash != owner->committed_snapshot_hash) {
        return UCN_ERR_REPLAY;
    }
    return UCN_OK;
}

bool ucn_cluster_backup_sync_owner_resync_required(
    const ucn_cluster_backup_sync_owner_t *owner)
{
    return ucn_cluster_backup_sync_owner_is_valid(owner) && owner->resync_required;
}

ucn_result_t ucn_cluster_backup_sync_owner_delta(
    ucn_cluster_backup_sync_owner_t *owner,
    ucn_node_id_t source_node_id,
    const ucn_cluster_backup_delta_t *delta)
{
    const ucn_cluster_snapshot_epoch_t *committed_epoch;
    ucn_cluster_backup_mirror_t mirror_candidate;
    ucn_cluster_member_t *slot;
    uint32_t calculated_hash;

    if (owner == NULL || delta == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_sync_owner_is_valid(owner)) {
        return UCN_ERR_STATE;
    }
    if (!source_is_assigned_head(owner, source_node_id)) {
        return UCN_ERR_ACCESS;
    }
    committed_epoch = ucn_cluster_backup_mirror_committed_epoch(&owner->mirror);
    if (committed_epoch == NULL || owner->mirror.staging_active ||
        owner->resync_required ||
        !ucn_cluster_snapshot_epoch_is_exact(&delta->snapshot_epoch,
                                             committed_epoch) ||
        !ucn_cluster_snapshot_epoch_matches_config(&delta->snapshot_epoch,
                                                   &owner->active_config) ||
        !serial_is_valid(delta->sequence) ||
        delta->previous_snapshot_hash == 0U ||
        delta->previous_snapshot_hash != owner->committed_snapshot_hash ||
        delta->resulting_snapshot_hash == 0U || !delta->member.occupied ||
        delta->member.last_nonce == 0U ||
        !ucn_cluster_member_record_is_valid(&delta->member)) {
        return UCN_ERR_REPLAY;
    }
    if (delta->sequence < owner->next_delta_sequence) {
        return UCN_ERR_REPLAY;
    }
    if (delta->sequence > owner->next_delta_sequence) {
        owner->resync_required = true;
        return UCN_ERR_REPLAY;
    }
    if (delta->sequence >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return UCN_ERR_EXHAUSTED;
    }
    mirror_candidate = owner->mirror;
    slot = table_find_node(&mirror_candidate.committed_members,
                           delta->member.node_id);
    if (!delta_preserves_member_identity(slot, &delta->member)) {
        return UCN_ERR_REPLAY;
    }
    *slot = delta->member;
    calculated_hash = table_snapshot_hash(&mirror_candidate.committed_members);
    if (calculated_hash == 0U || calculated_hash !=
                                    delta->resulting_snapshot_hash ||
        !ucn_cluster_backup_mirror_is_valid(&mirror_candidate)) {
        return UCN_ERR_REPLAY;
    }
    owner->mirror = mirror_candidate;
    owner->committed_snapshot_hash = calculated_hash;
    owner->next_delta_sequence = delta->sequence + 1U;
    return UCN_OK;
}

bool ucn_cluster_backup_sync_owner_takeover_eligible(
    const ucn_cluster_backup_sync_owner_t *owner)
{
    return ucn_cluster_backup_sync_owner_is_valid(owner) &&
           owner->mirror.committed_valid && !owner->takeover_ineligible;
}

ucn_result_t ucn_cluster_backup_sync_owner_update_coverage(
    ucn_cluster_backup_sync_owner_t *owner,
    const ucn_cluster_backup_coverage_t *coverage,
    uint32_t now_ms)
{
    if (owner == NULL || coverage == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_sync_owner_is_valid(owner) ||
        !owner->mirror.committed_valid ||
        !ucn_cluster_backup_coverage_is_valid(coverage)) {
        return UCN_ERR_STATE;
    }
    if (owner->takeover_ineligible) {
        return UCN_ERR_STATE;
    }
    if (ucn_cluster_backup_coverage_initial_ready(&owner->active_config,
                                                   coverage)) {
        owner->coverage_grace_armed = false;
        owner->coverage_grace_deadline_ms = 0U;
        return UCN_OK;
    }
    /* Target FSM §25.2 permits hysteresis for explicit SUSPECT only.  A
     * missing protected-voter entry is not a known transient state, and a
     * Core-confirmed REMOVED voter is immediately unsafe; both fail closed
     * until a new Backup assignment establishes a fresh mirror. */
    if (config_has_missing_protected_voter(&owner->active_config, coverage) ||
        config_has_removed_protected_voter(&owner->active_config, coverage)) {
        owner->coverage_grace_armed = false;
        owner->coverage_grace_deadline_ms = 0U;
        owner->takeover_ineligible = true;
        return UCN_ERR_STATE;
    }
    if (!config_has_suspect_protected_voter(&owner->active_config, coverage)) {
        owner->coverage_grace_armed = false;
        owner->coverage_grace_deadline_ms = 0U;
        owner->takeover_ineligible = true;
        return UCN_ERR_STATE;
    }
    if (!owner->coverage_grace_armed) {
        owner->coverage_grace_deadline_ms = ucn_deadline_from_now(
            now_ms, UCN_CLUSTER_BACKUP_COVERAGE_GRACE_MS);
        if (owner->coverage_grace_deadline_ms == 0U) {
            return UCN_ERR_STATE;
        }
        owner->coverage_grace_armed = true;
        return UCN_OK;
    }
    if (ucn_deadline_expired(now_ms, owner->coverage_grace_deadline_ms)) {
        owner->coverage_grace_armed = false;
        owner->coverage_grace_deadline_ms = 0U;
        owner->takeover_ineligible = true;
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}
