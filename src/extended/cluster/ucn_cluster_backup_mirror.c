/* CLV2-M09 (09-01): fixed-storage committed/staging Backup mirror model. */

#include "ucn/ucn_cluster_backup_mirror.h"

#include <stdint.h>
#include <string.h>

static bool object_is_canonical_empty(const void *object, size_t object_size)
{
    const uint8_t *bytes;
    size_t index;

    if (object == NULL) {
        return false;
    }
    bytes = (const uint8_t *)object;
    for (index = 0U; index < object_size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static ucn_result_t serial_next_checked(uint32_t current, uint32_t *next)
{
    if (next == NULL || !serial_is_valid(current)) {
        return UCN_ERR_ARGUMENT;
    }
    if (current >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return UCN_ERR_EXHAUSTED;
    }
    *next = current + 1U;
    return UCN_OK;
}

bool ucn_cluster_backup_epoch_is_valid(
    const ucn_cluster_backup_epoch_t *epoch)
{
    return epoch != NULL && epoch->cluster_id != 0U &&
           epoch->cluster_id != UCN_NODE_BROADCAST &&
           serial_is_valid(epoch->term) &&
           epoch->head_node_id != 0U &&
           epoch->head_node_id != UCN_NODE_BROADCAST &&
           epoch->backup_node_id != 0U &&
           epoch->backup_node_id != UCN_NODE_BROADCAST &&
           epoch->head_node_id != epoch->backup_node_id &&
           serial_is_valid(epoch->backup_generation);
}

bool ucn_cluster_backup_epoch_is_exact(
    const ucn_cluster_backup_epoch_t *left,
    const ucn_cluster_backup_epoch_t *right)
{
    return ucn_cluster_backup_epoch_is_valid(left) &&
           ucn_cluster_backup_epoch_is_valid(right) &&
           left->cluster_id == right->cluster_id && left->term == right->term &&
           left->head_node_id == right->head_node_id &&
           left->backup_node_id == right->backup_node_id &&
           left->backup_generation == right->backup_generation;
}

bool ucn_cluster_snapshot_epoch_from_config(
    ucn_cluster_snapshot_epoch_t *output,
    const ucn_cluster_backup_epoch_t *backup_epoch,
    uint32_t snapshot_id,
    const ucn_cluster_config_state_t *config)
{
    ucn_cluster_snapshot_epoch_t candidate;
    uint32_t config_hash;

    if (output == NULL || !ucn_cluster_backup_epoch_is_valid(backup_epoch) ||
        !serial_is_valid(snapshot_id) ||
        !ucn_cluster_config_state_is_valid(config)) {
        return false;
    }
    config_hash = ucn_cluster_config_state_hash(config);
    if (config_hash == 0U) {
        return false;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.backup_epoch = *backup_epoch;
    candidate.snapshot_id = snapshot_id;
    candidate.config_id = config->config_id;
    candidate.config_hash = config_hash;
    candidate.config_phase = config->phase;
    *output = candidate;
    return true;
}

bool ucn_cluster_snapshot_epoch_is_valid(
    const ucn_cluster_snapshot_epoch_t *epoch)
{
    return epoch != NULL &&
           ucn_cluster_backup_epoch_is_valid(&epoch->backup_epoch) &&
           serial_is_valid(epoch->snapshot_id) &&
           serial_is_valid(epoch->config_id) && epoch->config_hash != 0U &&
           ucn_cluster_config_phase_is_valid(
               (ucn_cluster_config_phase_t)epoch->config_phase);
}

bool ucn_cluster_snapshot_epoch_is_exact(
    const ucn_cluster_snapshot_epoch_t *left,
    const ucn_cluster_snapshot_epoch_t *right)
{
    return ucn_cluster_snapshot_epoch_is_valid(left) &&
           ucn_cluster_snapshot_epoch_is_valid(right) &&
           ucn_cluster_backup_epoch_is_exact(&left->backup_epoch,
                                             &right->backup_epoch) &&
           left->snapshot_id == right->snapshot_id &&
           left->config_id == right->config_id &&
           left->config_hash == right->config_hash &&
           left->config_phase == right->config_phase;
}

bool ucn_cluster_snapshot_epoch_matches_config(
    const ucn_cluster_snapshot_epoch_t *epoch,
    const ucn_cluster_config_state_t *config)
{
    return ucn_cluster_snapshot_epoch_is_valid(epoch) &&
           ucn_cluster_config_state_is_valid(config) &&
           epoch->config_id == config->config_id &&
           epoch->config_phase == config->phase &&
           epoch->config_hash == ucn_cluster_config_state_hash(config);
}

bool ucn_cluster_snapshot_epoch_rotation_required(
    const ucn_cluster_snapshot_epoch_t *epoch)
{
    return ucn_cluster_snapshot_epoch_is_valid(epoch) &&
           epoch->snapshot_id >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

bool ucn_cluster_backup_epoch_rekey_required(
    const ucn_cluster_backup_epoch_t *epoch)
{
    return ucn_cluster_backup_epoch_is_valid(epoch) &&
           epoch->backup_generation >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

ucn_result_t ucn_cluster_backup_epoch_next_generation(
    ucn_cluster_backup_epoch_t *output,
    const ucn_cluster_backup_epoch_t *current)
{
    ucn_cluster_backup_epoch_t candidate;
    uint32_t next_generation;

    if (output == NULL || current == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_epoch_is_valid(current)) {
        return UCN_ERR_STATE;
    }
    if (current->backup_generation >=
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD - 1U) {
        return UCN_ERR_EXHAUSTED;
    }
    if (serial_next_checked(current->backup_generation, &next_generation) !=
        UCN_OK) {
        return UCN_ERR_EXHAUSTED;
    }
    candidate = *current;
    candidate.backup_generation = next_generation;
    if (!ucn_cluster_backup_epoch_is_valid(&candidate) ||
        ucn_cluster_backup_epoch_rekey_required(&candidate)) {
        return UCN_ERR_EXHAUSTED;
    }
    *output = candidate;
    return UCN_OK;
}

void ucn_cluster_backup_mirror_reset(ucn_cluster_backup_mirror_t *mirror)
{
    if (mirror != NULL) {
        (void)memset(mirror, 0, sizeof(*mirror));
    }
}

bool ucn_cluster_backup_mirror_is_valid(
    const ucn_cluster_backup_mirror_t *mirror)
{
    if (mirror == NULL ||
        !ucn_cluster_member_table_is_valid(&mirror->committed_members) ||
        !ucn_cluster_member_table_is_valid(&mirror->staging_members)) {
        return false;
    }
    if (!mirror->committed_valid &&
        !object_is_canonical_empty(&mirror->committed_members,
                                   sizeof(mirror->committed_members))) {
        return false;
    }
    if (!mirror->committed_valid &&
        !object_is_canonical_empty(&mirror->committed_epoch,
                                   sizeof(mirror->committed_epoch))) {
        return false;
    }
    if (mirror->committed_valid &&
        !ucn_cluster_snapshot_epoch_is_valid(&mirror->committed_epoch)) {
        return false;
    }
    if (!mirror->staging_active &&
        !object_is_canonical_empty(&mirror->staging_members,
                                   sizeof(mirror->staging_members))) {
        return false;
    }
    if (!mirror->staging_active &&
        !object_is_canonical_empty(&mirror->staging_epoch,
                                   sizeof(mirror->staging_epoch))) {
        return false;
    }
    if (mirror->staging_active &&
        !ucn_cluster_snapshot_epoch_is_valid(&mirror->staging_epoch)) {
        return false;
    }
    return true;
}

ucn_result_t ucn_cluster_backup_mirror_begin_staging(
    ucn_cluster_backup_mirror_t *mirror,
    const ucn_cluster_snapshot_epoch_t *snapshot_epoch)
{
    if (mirror == NULL || snapshot_epoch == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_mirror_is_valid(mirror) ||
        !ucn_cluster_snapshot_epoch_is_valid(snapshot_epoch)) {
        return UCN_ERR_STATE;
    }
    if (ucn_cluster_snapshot_epoch_rotation_required(snapshot_epoch)) {
        return UCN_ERR_EXHAUSTED;
    }
    if (mirror->staging_active) {
        return UCN_ERR_STATE;
    }
    if (mirror->committed_valid) {
        if (!ucn_cluster_backup_epoch_is_exact(
                &mirror->committed_epoch.backup_epoch,
                &snapshot_epoch->backup_epoch)) {
            return UCN_ERR_STATE;
        }
        if (snapshot_epoch->snapshot_id <=
            mirror->committed_epoch.snapshot_id) {
            return UCN_ERR_REPLAY;
        }
    }
    (void)memset(&mirror->staging_members, 0,
                 sizeof(mirror->staging_members));
    mirror->staging_epoch = *snapshot_epoch;
    mirror->staging_active = true;
    return UCN_OK;
}

ucn_result_t ucn_cluster_backup_mirror_abort_staging(
    ucn_cluster_backup_mirror_t *mirror)
{
    if (mirror == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_mirror_is_valid(mirror)) {
        return UCN_ERR_STATE;
    }
    (void)memset(&mirror->staging_members, 0,
                 sizeof(mirror->staging_members));
    (void)memset(&mirror->staging_epoch, 0, sizeof(mirror->staging_epoch));
    mirror->staging_active = false;
    return UCN_OK;
}

ucn_result_t ucn_cluster_backup_mirror_commit_staging_exact(
    ucn_cluster_backup_mirror_t *mirror,
    const ucn_cluster_snapshot_epoch_t *expected_epoch)
{
    ucn_cluster_backup_mirror_t candidate;

    if (mirror == NULL || expected_epoch == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_cluster_backup_mirror_is_valid(mirror) ||
        !mirror->staging_active ||
        !ucn_cluster_snapshot_epoch_is_exact(&mirror->staging_epoch,
                                             expected_epoch)) {
        return UCN_ERR_STATE;
    }
    candidate = *mirror;
    candidate.committed_members = candidate.staging_members;
    candidate.committed_epoch = candidate.staging_epoch;
    candidate.committed_valid = true;
    (void)memset(&candidate.staging_members, 0,
                 sizeof(candidate.staging_members));
    (void)memset(&candidate.staging_epoch, 0,
                 sizeof(candidate.staging_epoch));
    candidate.staging_active = false;
    if (!ucn_cluster_backup_mirror_is_valid(&candidate)) {
        return UCN_ERR_STATE;
    }
    *mirror = candidate;
    return UCN_OK;
}

const ucn_cluster_snapshot_epoch_t *ucn_cluster_backup_mirror_committed_epoch(
    const ucn_cluster_backup_mirror_t *mirror)
{
    if (!ucn_cluster_backup_mirror_is_valid(mirror) ||
        !mirror->committed_valid) {
        return NULL;
    }
    return &mirror->committed_epoch;
}

const ucn_cluster_snapshot_epoch_t *ucn_cluster_backup_mirror_staging_epoch(
    const ucn_cluster_backup_mirror_t *mirror)
{
    if (!ucn_cluster_backup_mirror_is_valid(mirror) ||
        !mirror->staging_active) {
        return NULL;
    }
    return &mirror->staging_epoch;
}

const ucn_cluster_member_table_t *ucn_cluster_backup_mirror_committed(
    const ucn_cluster_backup_mirror_t *mirror)
{
    if (!ucn_cluster_backup_mirror_is_valid(mirror) ||
        !mirror->committed_valid) {
        return NULL;
    }
    return &mirror->committed_members;
}

ucn_cluster_member_table_t *ucn_cluster_backup_mirror_staging(
    ucn_cluster_backup_mirror_t *mirror)
{
    if (!ucn_cluster_backup_mirror_is_valid(mirror) ||
        !mirror->staging_active) {
        return NULL;
    }
    return &mirror->staging_members;
}
