#include "internal/ucn_cluster.h"

#include "internal/ucn_checked.h"

#include <string.h>

static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static void write_be64(uint8_t *out, uint64_t value)
{
    write_be32(out, (uint32_t)(value >> 32U));
    write_be32(&out[4], (uint32_t)value);
}

static bool bytes_nonzero(const uint8_t *bytes, size_t count)
{
    size_t index;
    uint8_t value = 0U;
    for (index = 0U; index < count; ++index) value |= bytes[index];
    return value != 0U;
}

static uint32_t voter_mask(const ucn_i_cluster_config_view_t *config)
{
    uint32_t mask = 0U;
    uint8_t index;
    for (index = 0U; index < config->member_count; ++index) {
        if ((config->members[index].flags &
             UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U) {
            mask |= UINT32_C(1) << index;
        }
    }
    return mask;
}

static bool header_valid(const ucn_i_cluster_snapshot_header_t *header,
                         uint64_t now_us)
{
    return header != NULL && header->cluster_id != 0U &&
           header->term != 0U && header->config_id != 0U &&
           header->config_generation != 0U &&
           header->assignment_generation != 0U &&
           header->snapshot_sequence != 0U &&
           header->member_count != 0U &&
           header->member_count <= UCN_I_CLUSTER_RUNTIME_MEMBER_COUNT &&
           header->absolute_deadline_us > now_us &&
           header->reserved_zero[0] == 0U &&
           header->reserved_zero[1] == 0U &&
           header->reserved_zero[2] == 0U &&
           bytes_nonzero(header->expected_digest,
                         sizeof(header->expected_digest));
}

static void encode_header(const ucn_i_cluster_snapshot_header_t *header,
                          uint8_t *out)
{
    memset(out, 0, UCN_I_CLUSTER_SNAPSHOT_HEADER_BYTES);
    write_be32(&out[0], header->cluster_id);
    write_be32(&out[4], header->term);
    write_be32(&out[8], header->config_id);
    write_be32(&out[12], header->config_generation);
    write_be32(&out[16], header->assignment_generation);
    write_be32(&out[20], header->snapshot_sequence);
    write_be32(&out[24], header->protected_voter_bitmap);
    out[28] = header->member_count;
    write_be64(&out[32], header->absolute_deadline_us);
}

static void encode_member(const ucn_i_cluster_snapshot_member_t *member,
                          uint8_t *out)
{
    memset(out, 0, UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES);
    memcpy(&out[0], member->principal, UCN_I_CLUSTER_PRINCIPAL_BYTES);
    write_be32(&out[16], member->binding_generation);
    write_be32(&out[20], member->session_generation);
    write_be32(&out[24], member->capability_generation);
    write_be32(&out[28], member->route_generation);
    write_be32(&out[32], member->link_generation);
    write_be16(&out[36], member->link_id);
    out[38] = member->flags;
    memcpy(&out[40], member->capability_digest,
           UCN_I_CLUSTER_DIGEST_BYTES);
}

bool ucn_i_cluster_p_snapshot_current(
    const ucn_i_cluster_owner_t *owner)
{
    const ucn_i_cluster_snapshot_buffer_t *buffer;
    if (owner == NULL || owner->snapshot_sync.building != 0U) return false;
    buffer = &owner->snapshot_sync.buffers[owner->snapshot_sync.active_index];
    return buffer->valid != 0U && owner->state.backup_ready != 0U &&
           buffer->header.cluster_id == owner->state.epoch.cluster_id &&
           buffer->header.term == owner->state.epoch.term &&
           buffer->header.config_id == owner->state.stable_config.config_id &&
           buffer->header.config_generation ==
               owner->state.stable_config.generation &&
           buffer->header.assignment_generation ==
               owner->state.backup_assignment_generation &&
           buffer->header.protected_voter_bitmap ==
               owner->state.backup_coverage_mask &&
           memcmp(buffer->header.expected_digest,
                  owner->state.backup_snapshot_digest,
                  UCN_I_CLUSTER_DIGEST_BYTES) == 0;
}

ucn_result_t ucn_i_cluster_snapshot_begin(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_snapshot_header_t *header, uint64_t now_us)
{
    const ucn_i_cluster_snapshot_buffer_t *active;
    ucn_i_cluster_snapshot_buffer_t *buffer;
    uint8_t building_index;
    ucn_result_t result;

    if (!ucn_i_cluster_p_owner_valid(owner) || !header_valid(header, now_us) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), header,
                             sizeof(*header))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    active = &owner->snapshot_sync.buffers[
        owner->snapshot_sync.active_index];
    if (owner->pending_valid != 0U || owner->snapshot_sync.building != 0U ||
        owner->state.phase != UCN_I_CLUSTER_STABLE ||
        owner->state.role != UCN_I_CLUSTER_BACKUP ||
        header->cluster_id != owner->state.epoch.cluster_id ||
        header->term != owner->state.epoch.term ||
        header->config_id != owner->state.stable_config.config_id ||
        header->config_generation !=
            owner->state.stable_config.generation ||
        header->member_count != owner->state.stable_config.member_count ||
        header->protected_voter_bitmap !=
            voter_mask(&owner->state.stable_config) ||
        header->assignment_generation <=
            owner->state.backup_assignment_generation ||
        (active->valid == 0U &&
         (header->assignment_generation !=
              owner->state.backup_assignment_generation + 1U ||
          header->snapshot_sequence != 1U)) ||
        (active->valid != 0U &&
         (active->header.assignment_generation == UINT32_MAX ||
          active->header.snapshot_sequence == UINT32_MAX ||
          header->assignment_generation !=
              active->header.assignment_generation + 1U ||
          header->snapshot_sequence !=
              active->header.snapshot_sequence + 1U))) {
        result = UCN_ERR_STATE;
        goto done;
    }
    building_index = owner->snapshot_sync.active_index == 0U ? 1U : 0U;
    buffer = &owner->snapshot_sync.buffers[building_index];
    memset(buffer, 0, sizeof(*buffer));
    buffer->header = *header;
    encode_header(header, buffer->canonical);
    owner->snapshot_sync.building_index = building_index;
    owner->snapshot_sync.building = 1U;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_snapshot_member(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_snapshot_member_t *member, uint64_t now_us)
{
    ucn_i_cluster_snapshot_buffer_t *buffer;
    const ucn_i_cluster_config_member_t *expected;
    size_t offset;
    ucn_result_t result;

    if (!ucn_i_cluster_p_owner_valid(owner) || member == NULL ||
        member->binding_generation == 0U ||
        member->session_generation == 0U ||
        member->capability_generation == 0U ||
        member->route_generation == 0U || member->link_generation == 0U ||
        member->link_id == 0U || member->link_id == UINT16_MAX ||
        member->reserved_zero != 0U ||
        !bytes_nonzero(member->principal, sizeof(member->principal)) ||
        !bytes_nonzero(member->capability_digest,
                       sizeof(member->capability_digest)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), member,
                             sizeof(*member))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->snapshot_sync.building == 0U) {
        result = UCN_ERR_STATE;
        goto done;
    }
    buffer = &owner->snapshot_sync.buffers[
        owner->snapshot_sync.building_index];
    if (now_us >= buffer->header.absolute_deadline_us ||
        buffer->received_count >= buffer->header.member_count) {
        result = UCN_ERR_EXHAUSTED;
        goto done;
    }
    expected = &owner->state.stable_config.members[buffer->received_count];
    if (!ucn_i_cluster_p_principal_equal(expected->principal,
                                          member->principal) ||
        expected->binding_generation != member->binding_generation ||
        expected->flags != member->flags) {
        result = UCN_ERR_STATE;
        goto done;
    }
    offset = UCN_I_CLUSTER_SNAPSHOT_HEADER_BYTES +
             (size_t)buffer->received_count *
                 UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES;
    encode_member(member, &buffer->canonical[offset]);
    ++buffer->received_count;
    result = UCN_OK;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_snapshot_end(
    ucn_i_cluster_owner_t *owner, uint64_t now_us)
{
    ucn_i_cluster_snapshot_buffer_t *buffer;
    uint8_t digest[UCN_I_CLUSTER_DIGEST_BYTES];
    size_t canonical_bytes;
    ucn_result_t result;

    if (!ucn_i_cluster_p_owner_valid(owner)) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    if (owner->snapshot_sync.building == 0U) {
        result = UCN_ERR_STATE;
        goto done;
    }
    buffer = &owner->snapshot_sync.buffers[
        owner->snapshot_sync.building_index];
    if (now_us >= buffer->header.absolute_deadline_us ||
        buffer->received_count != buffer->header.member_count) {
        result = UCN_ERR_EXHAUSTED;
        goto discard;
    }
    canonical_bytes = UCN_I_CLUSTER_SNAPSHOT_HEADER_BYTES +
                      (size_t)buffer->received_count *
                          UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES;
    result = ucn_i_sha256_128(buffer->canonical, canonical_bytes, digest,
                              &owner->hash_workspace);
    if (result != UCN_OK) goto discard;
    if (memcmp(digest, buffer->header.expected_digest,
               sizeof(digest)) != 0) {
        result = UCN_ERR_SECURITY;
        goto discard;
    }
    buffer->valid = 1U;
    owner->snapshot_sync.active_index =
        owner->snapshot_sync.building_index;
    owner->snapshot_sync.building = 0U;
    result = UCN_OK;
    goto done;
discard:
    memset(buffer, 0, sizeof(*buffer));
    owner->snapshot_sync.building = 0U;
done:
    owner->state_lock.leave(owner->state_lock.context);
    return result;
}

ucn_result_t ucn_i_cluster_backup_ready_from_mirror_prepare(
    ucn_i_cluster_owner_t *owner,
    const ucn_i_cluster_durability_t *durability,
    ucn_i_cluster_requirement_t *requirement_out)
{
    ucn_i_cluster_config_view_t config;
    uint8_t digest[UCN_I_CLUSTER_DIGEST_BYTES];
    uint32_t assignment_generation;
    uint32_t protected_voter_bitmap;
    ucn_i_cluster_snapshot_buffer_t *buffer;
    ucn_result_t result;

    if (!ucn_i_cluster_p_owner_valid(owner) || durability == NULL ||
        requirement_out == NULL) return UCN_ERR_ARGUMENT;
    result = owner->state_lock.enter(owner->state_lock.context);
    if (result != UCN_OK) return result;
    buffer = &owner->snapshot_sync.buffers[owner->snapshot_sync.active_index];
    if (buffer->valid == 0U || owner->snapshot_sync.building != 0U ||
        buffer->header.cluster_id != owner->state.epoch.cluster_id ||
        buffer->header.term != owner->state.epoch.term ||
        buffer->header.config_id != owner->state.stable_config.config_id ||
        buffer->header.config_generation !=
            owner->state.stable_config.generation) {
        owner->state_lock.leave(owner->state_lock.context);
        return UCN_ERR_STATE;
    }
    config = owner->state.stable_config;
    assignment_generation = buffer->header.assignment_generation;
    protected_voter_bitmap = buffer->header.protected_voter_bitmap;
    memcpy(digest, buffer->header.expected_digest, sizeof(digest));
    owner->state_lock.leave(owner->state_lock.context);
    return ucn_i_cluster_backup_ready_prepare(
        owner, assignment_generation, &config, protected_voter_bitmap,
        digest, durability, requirement_out);
}
