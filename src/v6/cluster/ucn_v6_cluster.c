#include "../internal/ucn_v6_cluster_private.h"

#include <limits.h>
#include <string.h>

#define RECORD_MAGIC UINT32_C(0x56364352)
#define RECORD_BODY_BYTES (UCN_V6_CLUSTER_RECORD_BYTES - 4U)
#define CONTROL_BODY_BYTES (UCN_V6_CLUSTER_CONTROL_BYTES - 4U)

typedef char cluster_owner_storage_must_fit[
    sizeof(struct ucn_v6_cluster_owner) <= UCN_V6_CLUSTER_OWNER_STORAGE_BYTES ?
        1 : -1];

typedef struct byte_writer {
    uint8_t *bytes;
    size_t capacity;
    size_t offset;
    bool ok;
} byte_writer_t;

typedef struct byte_reader {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
    bool ok;
} byte_reader_t;

static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static bool serial_valid(uint32_t value)
{
    return value != 0U && value <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool principal_equal(const ucn_v6_principal_t *left,
                            const ucn_v6_principal_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool binding_equal(const ucn_v6_binding_key_t *left,
                          const ucn_v6_binding_key_t *right)
{
    return left != NULL && right != NULL &&
           left->realm_id == right->realm_id &&
           left->node_address == right->node_address &&
           left->binding_generation == right->binding_generation;
}

static bool epoch_equal(const ucn_v6_cluster_epoch_t *left,
                        const ucn_v6_cluster_epoch_t *right)
{
    return left != NULL && right != NULL &&
           left->cluster_id == right->cluster_id &&
           left->term == right->term &&
           principal_equal(&left->head_principal, &right->head_principal) &&
           binding_equal(&left->head_binding, &right->head_binding);
}

static bool voter_equal(const ucn_v6_cluster_voter_t *left,
                        const ucn_v6_cluster_voter_t *right)
{
    return left != NULL && right != NULL &&
           principal_equal(&left->principal, &right->principal) &&
           binding_equal(&left->binding, &right->binding);
}

static void writer_bytes(byte_writer_t *writer, const void *value, size_t length)
{
    if (writer == NULL || !writer->ok || value == NULL ||
        writer->offset > writer->capacity ||
        length > writer->capacity - writer->offset) {
        if (writer != NULL) {
            writer->ok = false;
        }
        return;
    }
    memcpy(&writer->bytes[writer->offset], value, length);
    writer->offset += length;
}

static void writer_skip(byte_writer_t *writer, size_t length)
{
    if (writer == NULL || !writer->ok || writer->offset > writer->capacity ||
        length > writer->capacity - writer->offset) {
        if (writer != NULL) {
            writer->ok = false;
        }
        return;
    }
    writer->offset += length;
}

static void writer_u8(byte_writer_t *writer, uint8_t value)
{
    writer_bytes(writer, &value, 1U);
}

static void writer_u16(byte_writer_t *writer, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_u32(byte_writer_t *writer, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void writer_u64(byte_writer_t *writer, uint64_t value)
{
    uint8_t bytes[8];
    size_t index;
    for (index = 0U; index < sizeof(bytes); ++index) {
        bytes[7U - index] = (uint8_t)value;
        value >>= 8U;
    }
    writer_bytes(writer, bytes, sizeof(bytes));
}

static void reader_bytes(byte_reader_t *reader, void *value, size_t length)
{
    if (reader == NULL || !reader->ok || value == NULL ||
        reader->offset > reader->length ||
        length > reader->length - reader->offset) {
        if (reader != NULL) {
            reader->ok = false;
        }
        return;
    }
    memcpy(value, &reader->bytes[reader->offset], length);
    reader->offset += length;
}

static bool reader_zero(byte_reader_t *reader, size_t length)
{
    size_t index;
    if (reader == NULL || !reader->ok || reader->offset > reader->length ||
        length > reader->length - reader->offset) {
        if (reader != NULL) {
            reader->ok = false;
        }
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (reader->bytes[reader->offset + index] != 0U) {
            reader->ok = false;
            return false;
        }
    }
    reader->offset += length;
    return true;
}

static uint8_t reader_u8(byte_reader_t *reader)
{
    uint8_t value = 0U;
    reader_bytes(reader, &value, 1U);
    return value;
}

static uint16_t reader_u16(byte_reader_t *reader)
{
    uint8_t bytes[2] = { 0U, 0U };
    reader_bytes(reader, bytes, sizeof(bytes));
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint32_t reader_u32(byte_reader_t *reader)
{
    uint8_t bytes[4] = { 0U, 0U, 0U, 0U };
    reader_bytes(reader, bytes, sizeof(bytes));
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | bytes[3];
}

static uint64_t reader_u64(byte_reader_t *reader)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | reader_u8(reader);
    }
    return value;
}

static void write_binding(byte_writer_t *writer,
                          const ucn_v6_binding_key_t *binding)
{
    writer_u32(writer, binding->realm_id);
    writer_u32(writer, binding->node_address);
    writer_u32(writer, binding->binding_generation);
}

static void read_binding(byte_reader_t *reader, ucn_v6_binding_key_t *binding)
{
    binding->realm_id = reader_u32(reader);
    binding->node_address = reader_u32(reader);
    binding->binding_generation = reader_u32(reader);
}

static void write_voter(byte_writer_t *writer,
                        const ucn_v6_cluster_voter_t *voter)
{
    writer_bytes(writer, voter->principal.bytes, sizeof(voter->principal.bytes));
    write_binding(writer, &voter->binding);
}

static void read_voter(byte_reader_t *reader, ucn_v6_cluster_voter_t *voter)
{
    reader_bytes(reader, voter->principal.bytes, sizeof(voter->principal.bytes));
    read_binding(reader, &voter->binding);
}

static void write_epoch(byte_writer_t *writer,
                        const ucn_v6_cluster_epoch_t *epoch)
{
    writer_u32(writer, epoch->cluster_id);
    writer_u32(writer, epoch->term);
    writer_bytes(writer, epoch->head_principal.bytes,
                 sizeof(epoch->head_principal.bytes));
    write_binding(writer, &epoch->head_binding);
}

static void read_epoch(byte_reader_t *reader, ucn_v6_cluster_epoch_t *epoch)
{
    epoch->cluster_id = reader_u32(reader);
    epoch->term = reader_u32(reader);
    reader_bytes(reader, epoch->head_principal.bytes,
                 sizeof(epoch->head_principal.bytes));
    read_binding(reader, &epoch->head_binding);
}

bool ucn_v6_cluster_epoch_is_valid(const ucn_v6_cluster_epoch_t *epoch)
{
    return epoch != NULL && serial_valid(epoch->cluster_id) &&
           serial_valid(epoch->term) &&
           ucn_v6_principal_is_valid(&epoch->head_principal) &&
           ucn_v6_binding_key_is_valid(&epoch->head_binding);
}

static int voter_compare(const ucn_v6_cluster_voter_t *left,
                         const ucn_v6_cluster_voter_t *right)
{
    int principal_order = memcmp(left->principal.bytes, right->principal.bytes,
                                 sizeof(left->principal.bytes));
    if (principal_order != 0) {
        return principal_order;
    }
    if (left->binding.realm_id != right->binding.realm_id) {
        return left->binding.realm_id < right->binding.realm_id ? -1 : 1;
    }
    if (left->binding.node_address != right->binding.node_address) {
        return left->binding.node_address < right->binding.node_address ? -1 : 1;
    }
    if (left->binding.binding_generation != right->binding.binding_generation) {
        return left->binding.binding_generation < right->binding.binding_generation ?
            -1 : 1;
    }
    return 0;
}

bool ucn_v6_cluster_config_is_valid(const ucn_v6_cluster_config_t *config)
{
    size_t index;
    if (config == NULL || !config->valid || !serial_valid(config->config_id) ||
        !serial_valid(config->generation) || config->voter_count == 0U ||
        config->voter_count > UCN_V6_CONFIG_CLUSTER_VOTERS) {
        return false;
    }
    for (index = 0U; index < config->voter_count; ++index) {
        if (!ucn_v6_principal_is_valid(&config->voters[index].principal) ||
            !ucn_v6_binding_key_is_valid(&config->voters[index].binding) ||
            (index != 0U && voter_compare(&config->voters[index - 1U],
                                         &config->voters[index]) >= 0)) {
            return false;
        }
    }
    for (; index < UCN_V6_CONFIG_CLUSTER_VOTERS; ++index) {
        ucn_v6_cluster_voter_t zero;
        memset(&zero, 0, sizeof(zero));
        if (memcmp(&config->voters[index], &zero, sizeof(zero)) != 0) {
            return false;
        }
    }
    return true;
}

static bool config_equal(const ucn_v6_cluster_config_t *left,
                         const ucn_v6_cluster_config_t *right)
{
    size_t index;
    if (left->valid != right->valid) {
        return false;
    }
    if (!left->valid) {
        return true;
    }
    if (left->config_id != right->config_id ||
        left->generation != right->generation ||
        left->voter_count != right->voter_count) {
        return false;
    }
    for (index = 0U; index < left->voter_count; ++index) {
        if (!voter_equal(&left->voters[index], &right->voters[index])) {
            return false;
        }
    }
    return true;
}

static void write_config(byte_writer_t *writer,
                         const ucn_v6_cluster_config_t *config)
{
    size_t index;
    writer_u8(writer, config->valid ? 1U : 0U);
    writer_skip(writer, 3U);
    if (config->valid) {
        writer_u32(writer, config->config_id);
        writer_u32(writer, config->generation);
        writer_u8(writer, config->voter_count);
    } else {
        writer_skip(writer, 9U);
    }
    writer_skip(writer, 3U);
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_VOTERS; ++index) {
        if (config->valid && index < config->voter_count) {
            write_voter(writer, &config->voters[index]);
        } else {
            writer_skip(writer, 28U);
        }
    }
}

static void read_config(byte_reader_t *reader,
                        ucn_v6_cluster_config_t *config)
{
    size_t index;
    uint8_t valid = reader_u8(reader);
    memset(config, 0, sizeof(*config));
    if (!reader_zero(reader, 3U) || valid > 1U) {
        return;
    }
    config->valid = valid != 0U;
    config->config_id = reader_u32(reader);
    config->generation = reader_u32(reader);
    config->voter_count = reader_u8(reader);
    if (!reader_zero(reader, 3U)) {
        return;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_VOTERS; ++index) {
        read_voter(reader, &config->voters[index]);
    }
    if (!config->valid) {
        ucn_v6_cluster_config_t zero;
        memset(&zero, 0, sizeof(zero));
        if (config->config_id != 0U || config->generation != 0U ||
            config->voter_count != 0U ||
            memcmp(config->voters, zero.voters, sizeof(config->voters)) != 0) {
            reader->ok = false;
        }
    }
}

static bool backup_is_valid(const ucn_v6_cluster_backup_t *backup)
{
    if (backup == NULL) {
        return false;
    }
    if (!backup->valid) {
        ucn_v6_principal_t zero_principal;
        memset(&zero_principal, 0, sizeof(zero_principal));
        return !backup->ready &&
               memcmp(&backup->principal, &zero_principal,
                      sizeof(zero_principal)) == 0 &&
               backup->binding.realm_id == 0U &&
               backup->binding.node_address == 0U &&
               backup->binding.binding_generation == 0U &&
               backup->generation == 0U &&
               backup->config_transaction_id == 0U &&
               backup->acknowledged_config_generation == 0U;
    }
    return ucn_v6_principal_is_valid(&backup->principal) &&
           ucn_v6_binding_key_is_valid(&backup->binding) &&
           serial_valid(backup->generation) &&
           (!backup->ready ||
            (backup->config_transaction_id != 0U &&
             serial_valid(backup->acknowledged_config_generation)));
}

static bool vote_is_valid(const ucn_v6_cluster_vote_id_t *vote)
{
    if (vote == NULL) {
        return false;
    }
    if (!vote->valid) {
        ucn_v6_cluster_epoch_t zero_epoch;
        ucn_v6_principal_t zero_principal;
        memset(&zero_epoch, 0, sizeof(zero_epoch));
        memset(&zero_principal, 0, sizeof(zero_principal));
        return memcmp(&vote->source_epoch, &zero_epoch,
                      sizeof(zero_epoch)) == 0 &&
               memcmp(&vote->candidate_principal, &zero_principal,
                      sizeof(zero_principal)) == 0 &&
               vote->candidate_binding.realm_id == 0U &&
               vote->candidate_binding.node_address == 0U &&
               vote->candidate_binding.binding_generation == 0U &&
               vote->backup_generation == 0U;
    }
    return ucn_v6_cluster_epoch_is_valid(&vote->source_epoch) &&
           ucn_v6_principal_is_valid(&vote->candidate_principal) &&
           ucn_v6_binding_key_is_valid(&vote->candidate_binding) &&
           serial_valid(vote->backup_generation);
}

static bool transition_is_valid(
    const ucn_v6_cluster_transition_proof_t *transition)
{
    if (transition == NULL) {
        return false;
    }
    if (!transition->valid) {
        ucn_v6_cluster_transition_proof_t zero;
        memset(&zero, 0, sizeof(zero));
        return !transition->ready &&
               transition->kind == UCN_V6_CLUSTER_TRANSITION_NONE &&
               transition->transaction_id == 0U &&
               memcmp(&transition->old_epoch, &zero.old_epoch,
                      sizeof(zero.old_epoch)) == 0 &&
               memcmp(&transition->target_epoch, &zero.target_epoch,
                      sizeof(zero.target_epoch)) == 0 &&
               transition->target_config_id == 0U &&
               transition->target_config_generation == 0U &&
               !transition->target_config.valid &&
               transition->old_voter_bitmap == 0U &&
               transition->new_voter_bitmap == 0U;
    }
    return transition->kind >= UCN_V6_CLUSTER_TRANSITION_TAKEOVER &&
           transition->kind <= UCN_V6_CLUSTER_TRANSITION_REKEY &&
           transition->transaction_id != 0U &&
           ucn_v6_cluster_epoch_is_valid(&transition->old_epoch) &&
           ucn_v6_cluster_epoch_is_valid(&transition->target_epoch) &&
           serial_valid(transition->target_config_id) &&
           serial_valid(transition->target_config_generation) &&
           ucn_v6_cluster_config_is_valid(&transition->target_config) &&
           transition->target_config.config_id ==
               transition->target_config_id &&
           transition->target_config.generation ==
               transition->target_config_generation;
}

static bool snapshot_is_valid(const ucn_v6_cluster_snapshot_t *snapshot)
{
    size_t index;
    if (snapshot == NULL || snapshot->record_generation == 0U ||
        snapshot->boot_incarnation == 0U ||
        snapshot->boot_incarnation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        snapshot->role > UCN_V6_CLUSTER_FENCED ||
        snapshot->phase > UCN_V6_CLUSTER_PHASE_FAULT ||
        snapshot->tombstone_count > UCN_V6_CONFIG_CLUSTER_TOMBSTONES) {
        return false;
    }
    if (!snapshot->active_epoch_valid) {
        ucn_v6_cluster_epoch_t zero;
        memset(&zero, 0, sizeof(zero));
        if (memcmp(&snapshot->active_epoch, &zero, sizeof(zero)) != 0 ||
            snapshot->max_epoch_valid || snapshot->stable_config.valid ||
            snapshot->joint_valid || snapshot->backup.valid ||
            snapshot->last_vote.valid || snapshot->transition.valid ||
            snapshot->role != UCN_V6_CLUSTER_OBSERVER ||
            snapshot->phase != UCN_V6_CLUSTER_PHASE_STABLE ||
            snapshot->authority_fenced) {
            return false;
        }
    } else {
        if (!ucn_v6_cluster_epoch_is_valid(&snapshot->active_epoch) ||
            !snapshot->max_epoch_valid ||
            !ucn_v6_cluster_epoch_is_valid(&snapshot->max_epoch) ||
            snapshot->active_epoch.cluster_id !=
                snapshot->max_epoch.cluster_id ||
            snapshot->active_epoch.term > snapshot->max_epoch.term ||
            !ucn_v6_cluster_config_is_valid(&snapshot->stable_config)) {
            return false;
        }
    }
    if (snapshot->joint_valid) {
        if (snapshot->phase != UCN_V6_CLUSTER_PHASE_JOINT ||
            snapshot->joint_transaction_id == 0U ||
            !ucn_v6_cluster_config_is_valid(&snapshot->joint_new_config)) {
            return false;
        }
    } else if (snapshot->joint_transaction_id != 0U ||
               snapshot->joint_new_config.valid) {
        return false;
    }
    if (!backup_is_valid(&snapshot->backup) ||
        !vote_is_valid(&snapshot->last_vote) ||
        !transition_is_valid(&snapshot->transition)) {
        return false;
    }
    if (snapshot->phase >= UCN_V6_CLUSTER_PHASE_TAKEOVER &&
        snapshot->phase <= UCN_V6_CLUSTER_PHASE_REKEY) {
        if (!snapshot->transition.valid ||
            (uint8_t)snapshot->transition.kind !=
                (uint8_t)snapshot->phase - 1U) {
            return false;
        }
    } else if (snapshot->transition.valid) {
        return false;
    }
    if (snapshot->authority_fenced && snapshot->role != UCN_V6_CLUSTER_FENCED) {
        return false;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_TOMBSTONES; ++index) {
        const ucn_v6_cluster_tombstone_t *item = &snapshot->tombstones[index];
        size_t prior;
        if (index < snapshot->tombstone_count) {
            if (!item->occupied || !serial_valid(item->retired_cluster_id) ||
                !serial_valid(item->replacement_cluster_id) ||
                item->retired_cluster_id == item->replacement_cluster_id ||
                item->transaction_id == 0U) {
                return false;
            }
            for (prior = 0U; prior < index; ++prior) {
                if (snapshot->tombstones[prior].retired_cluster_id ==
                    item->retired_cluster_id) {
                    return false;
                }
            }
        } else {
            if (item->occupied || item->retired_cluster_id != 0U ||
                item->replacement_cluster_id != 0U ||
                item->transaction_id != 0U) {
                return false;
            }
        }
    }
    return true;
}

static void write_backup(byte_writer_t *writer,
                         const ucn_v6_cluster_backup_t *backup)
{
    writer_u8(writer, backup->valid ? 1U : 0U);
    writer_u8(writer, backup->ready ? 1U : 0U);
    writer_skip(writer, 2U);
    if (backup->valid) {
        writer_bytes(writer, backup->principal.bytes,
                     sizeof(backup->principal.bytes));
        write_binding(writer, &backup->binding);
        writer_u32(writer, backup->generation);
        writer_u64(writer, backup->config_transaction_id);
        writer_u32(writer, backup->acknowledged_config_generation);
    } else {
        writer_skip(writer, 44U);
    }
}

static void read_backup(byte_reader_t *reader,
                        ucn_v6_cluster_backup_t *backup)
{
    uint8_t valid;
    uint8_t ready;
    memset(backup, 0, sizeof(*backup));
    valid = reader_u8(reader);
    ready = reader_u8(reader);
    if (valid > 1U || ready > 1U || !reader_zero(reader, 2U)) {
        return;
    }
    backup->valid = valid != 0U;
    backup->ready = ready != 0U;
    reader_bytes(reader, backup->principal.bytes,
                 sizeof(backup->principal.bytes));
    read_binding(reader, &backup->binding);
    backup->generation = reader_u32(reader);
    backup->config_transaction_id = reader_u64(reader);
    backup->acknowledged_config_generation = reader_u32(reader);
}

static void write_vote(byte_writer_t *writer,
                       const ucn_v6_cluster_vote_id_t *vote)
{
    writer_u8(writer, vote->valid ? 1U : 0U);
    writer_skip(writer, 3U);
    if (vote->valid) {
        write_epoch(writer, &vote->source_epoch);
        writer_bytes(writer, vote->candidate_principal.bytes,
                     sizeof(vote->candidate_principal.bytes));
        write_binding(writer, &vote->candidate_binding);
        writer_u32(writer, vote->backup_generation);
    } else {
        writer_skip(writer, 68U);
    }
}

static void read_vote(byte_reader_t *reader, ucn_v6_cluster_vote_id_t *vote)
{
    uint8_t valid;
    memset(vote, 0, sizeof(*vote));
    valid = reader_u8(reader);
    if (valid > 1U || !reader_zero(reader, 3U)) {
        return;
    }
    vote->valid = valid != 0U;
    read_epoch(reader, &vote->source_epoch);
    reader_bytes(reader, vote->candidate_principal.bytes,
                 sizeof(vote->candidate_principal.bytes));
    read_binding(reader, &vote->candidate_binding);
    vote->backup_generation = reader_u32(reader);
}

static void write_transition(
    byte_writer_t *writer,
    const ucn_v6_cluster_transition_proof_t *transition)
{
    writer_u8(writer, transition->valid ? 1U : 0U);
    writer_u8(writer, transition->ready ? 1U : 0U);
    writer_u8(writer, (uint8_t)transition->kind);
    writer_skip(writer, 1U);
    if (transition->valid) {
        writer_u64(writer, transition->transaction_id);
        write_epoch(writer, &transition->old_epoch);
        write_epoch(writer, &transition->target_epoch);
        writer_u32(writer, transition->target_config_id);
        writer_u32(writer, transition->target_config_generation);
        write_config(writer, &transition->target_config);
        writer_u32(writer, transition->old_voter_bitmap);
        writer_u32(writer, transition->new_voter_bitmap);
    } else {
        writer_skip(writer, 560U);
    }
}

static void read_transition(
    byte_reader_t *reader,
    ucn_v6_cluster_transition_proof_t *transition)
{
    uint8_t valid;
    uint8_t ready;
    uint8_t kind;
    memset(transition, 0, sizeof(*transition));
    valid = reader_u8(reader);
    ready = reader_u8(reader);
    kind = reader_u8(reader);
    if (valid > 1U || ready > 1U || !reader_zero(reader, 1U)) {
        return;
    }
    transition->valid = valid != 0U;
    transition->ready = ready != 0U;
    transition->kind = (ucn_v6_cluster_transition_kind_t)kind;
    transition->transaction_id = reader_u64(reader);
    read_epoch(reader, &transition->old_epoch);
    read_epoch(reader, &transition->target_epoch);
    transition->target_config_id = reader_u32(reader);
    transition->target_config_generation = reader_u32(reader);
    read_config(reader, &transition->target_config);
    transition->old_voter_bitmap = reader_u32(reader);
    transition->new_voter_bitmap = reader_u32(reader);
}

ucn_v6_result_t ucn_v6_cluster_snapshot_encode(
    const ucn_v6_cluster_snapshot_t *snapshot,
    uint8_t output[UCN_V6_CLUSTER_RECORD_BYTES])
{
    byte_writer_t writer;
    size_t index;
    uint16_t flags = 0U;
    uint32_t crc;
    if (!snapshot_is_valid(snapshot) || output == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(output, 0, UCN_V6_CLUSTER_RECORD_BYTES);
    writer.bytes = output;
    writer.capacity = RECORD_BODY_BYTES;
    writer.offset = 0U;
    writer.ok = true;
    if (snapshot->active_epoch_valid) flags |= UINT16_C(1) << 0;
    if (snapshot->max_epoch_valid) flags |= UINT16_C(1) << 1;
    if (snapshot->joint_valid) flags |= UINT16_C(1) << 2;
    if (snapshot->authority_fenced) flags |= UINT16_C(1) << 3;
    writer_u32(&writer, RECORD_MAGIC);
    writer_u16(&writer, UCN_V6_CLUSTER_RECORD_VERSION);
    writer_u16(&writer, flags);
    writer_u64(&writer, snapshot->record_generation);
    writer_u64(&writer, snapshot->transaction_high_water);
    writer_u32(&writer, snapshot->boot_incarnation);
    writer_u8(&writer, (uint8_t)snapshot->role);
    writer_u8(&writer, (uint8_t)snapshot->phase);
    writer_skip(&writer, 2U);
    if (snapshot->active_epoch_valid) write_epoch(&writer, &snapshot->active_epoch);
    else writer_skip(&writer, 36U);
    if (snapshot->max_epoch_valid) write_epoch(&writer, &snapshot->max_epoch);
    else writer_skip(&writer, 36U);
    write_config(&writer, &snapshot->stable_config);
    write_config(&writer, &snapshot->joint_new_config);
    writer_u64(&writer, snapshot->joint_transaction_id);
    write_backup(&writer, &snapshot->backup);
    write_vote(&writer, &snapshot->last_vote);
    write_transition(&writer, &snapshot->transition);
    writer_u8(&writer, snapshot->tombstone_count);
    writer_skip(&writer, 3U);
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_TOMBSTONES; ++index) {
        if (index < snapshot->tombstone_count) {
            writer_u8(&writer, 1U);
            writer_skip(&writer, 3U);
            writer_u32(&writer, snapshot->tombstones[index].retired_cluster_id);
            writer_u32(&writer, snapshot->tombstones[index].replacement_cluster_id);
            writer_u64(&writer, snapshot->tombstones[index].transaction_id);
        } else {
            writer_skip(&writer, 20U);
        }
    }
    if (!writer.ok || writer.offset > RECORD_BODY_BYTES) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    crc = ucn_v6_crc32c(output, RECORD_BODY_BYTES);
    output[RECORD_BODY_BYTES] = (uint8_t)(crc >> 24U);
    output[RECORD_BODY_BYTES + 1U] = (uint8_t)(crc >> 16U);
    output[RECORD_BODY_BYTES + 2U] = (uint8_t)(crc >> 8U);
    output[RECORD_BODY_BYTES + 3U] = (uint8_t)crc;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_cluster_snapshot_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_cluster_snapshot_t *scratch,
    ucn_v6_cluster_snapshot_t *snapshot)
{
    ucn_v6_cluster_snapshot_t *decoded;
    byte_reader_t reader;
    uint16_t flags;
    uint32_t expected_crc;
    uint32_t actual_crc;
    size_t index;
    if (input == NULL || scratch == NULL || snapshot == NULL ||
        scratch == snapshot ||
        input_length != UCN_V6_CLUSTER_RECORD_BYTES) {
        return UCN_V6_ERR_MALFORMED;
    }
    expected_crc = ((uint32_t)input[RECORD_BODY_BYTES] << 24U) |
                   ((uint32_t)input[RECORD_BODY_BYTES + 1U] << 16U) |
                   ((uint32_t)input[RECORD_BODY_BYTES + 2U] << 8U) |
                   input[RECORD_BODY_BYTES + 3U];
    actual_crc = ucn_v6_crc32c(input, RECORD_BODY_BYTES);
    if (expected_crc != actual_crc) {
        return UCN_V6_ERR_MALFORMED;
    }
    decoded = scratch;
    memset(decoded, 0, sizeof(*decoded));
    reader.bytes = input;
    reader.length = RECORD_BODY_BYTES;
    reader.offset = 0U;
    reader.ok = true;
    if (reader_u32(&reader) != RECORD_MAGIC ||
        reader_u16(&reader) != UCN_V6_CLUSTER_RECORD_VERSION) {
        return UCN_V6_ERR_MALFORMED;
    }
    flags = reader_u16(&reader);
    if ((flags & UINT16_C(0xFFF0)) != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    decoded->active_epoch_valid = (flags & (UINT16_C(1) << 0)) != 0U;
    decoded->max_epoch_valid = (flags & (UINT16_C(1) << 1)) != 0U;
    decoded->joint_valid = (flags & (UINT16_C(1) << 2)) != 0U;
    decoded->authority_fenced = (flags & (UINT16_C(1) << 3)) != 0U;
    decoded->record_generation = reader_u64(&reader);
    decoded->transaction_high_water = reader_u64(&reader);
    decoded->boot_incarnation = reader_u32(&reader);
    decoded->role = (ucn_v6_cluster_role_t)reader_u8(&reader);
    decoded->phase = (ucn_v6_cluster_phase_t)reader_u8(&reader);
    if (!reader_zero(&reader, 2U)) return UCN_V6_ERR_MALFORMED;
    read_epoch(&reader, &decoded->active_epoch);
    read_epoch(&reader, &decoded->max_epoch);
    read_config(&reader, &decoded->stable_config);
    read_config(&reader, &decoded->joint_new_config);
    decoded->joint_transaction_id = reader_u64(&reader);
    read_backup(&reader, &decoded->backup);
    read_vote(&reader, &decoded->last_vote);
    read_transition(&reader, &decoded->transition);
    decoded->tombstone_count = reader_u8(&reader);
    if (!reader_zero(&reader, 3U)) return UCN_V6_ERR_MALFORMED;
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_TOMBSTONES; ++index) {
        uint8_t occupied = reader_u8(&reader);
        if (occupied > 1U || !reader_zero(&reader, 3U)) {
            return UCN_V6_ERR_MALFORMED;
        }
        decoded->tombstones[index].occupied = occupied != 0U;
        decoded->tombstones[index].retired_cluster_id = reader_u32(&reader);
        decoded->tombstones[index].replacement_cluster_id = reader_u32(&reader);
        decoded->tombstones[index].transaction_id = reader_u64(&reader);
    }
    if (!reader.ok || !reader_zero(&reader, RECORD_BODY_BYTES - reader.offset) ||
        !snapshot_is_valid(decoded)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *snapshot = *decoded;
    return UCN_V6_OK;
}

static bool control_is_valid(const ucn_v6_cluster_control_t *control)
{
    if (control == NULL ||
        control->kind < UCN_V6_CLUSTER_CTL_ADVERTISE ||
        control->kind > UCN_V6_CLUSTER_CTL_REKEY_COMMIT ||
        (control->flags & UINT16_C(0xFFF0)) != 0U ||
        !ucn_v6_cluster_epoch_is_valid(&control->old_epoch)) {
        return false;
    }
    if (control->kind >= UCN_V6_CLUSTER_CTL_TAKEOVER_COMMIT &&
        !ucn_v6_cluster_epoch_is_valid(&control->target_epoch)) {
        return false;
    }
    if (control->kind >= UCN_V6_CLUSTER_CTL_CONFIG_PREPARE &&
        control->transaction_id == 0U) {
        return false;
    }
    if (control->kind == UCN_V6_CLUSTER_CTL_CONFIG_PREPARE ||
        control->kind == UCN_V6_CLUSTER_CTL_CONFIG_ACK ||
        control->kind == UCN_V6_CLUSTER_CTL_CONFIG_COMMIT ||
        control->kind == UCN_V6_CLUSTER_CTL_TAKEOVER_COMMIT ||
        control->kind == UCN_V6_CLUSTER_CTL_HANDOVER_PREPARE ||
        control->kind == UCN_V6_CLUSTER_CTL_HANDOVER_READY ||
        control->kind == UCN_V6_CLUSTER_CTL_HANDOVER_COMMIT ||
        control->kind == UCN_V6_CLUSTER_CTL_RECOVERY_COMMIT ||
        control->kind == UCN_V6_CLUSTER_CTL_REKEY_COMMIT) {
        if (!serial_valid(control->config_id) ||
            !serial_valid(control->config_generation)) {
            return false;
        }
    }
    if ((control->kind == UCN_V6_CLUSTER_CTL_BACKUP_ASSIGN ||
         control->kind == UCN_V6_CLUSTER_CTL_BACKUP_READY ||
         control->kind == UCN_V6_CLUSTER_CTL_TAKEOVER_VOTE ||
         control->kind == UCN_V6_CLUSTER_CTL_TAKEOVER_COMMIT) &&
        !serial_valid(control->backup_generation)) {
        return false;
    }
    return true;
}

ucn_v6_result_t ucn_v6_cluster_control_encode(
    const ucn_v6_cluster_control_t *control,
    uint8_t output[UCN_V6_CLUSTER_CONTROL_BYTES])
{
    uint8_t encoded[UCN_V6_CLUSTER_CONTROL_BYTES];
    byte_writer_t writer;
    uint32_t crc;
    if (!control_is_valid(control) || output == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    writer.bytes = encoded;
    writer.capacity = CONTROL_BODY_BYTES;
    writer.offset = 0U;
    writer.ok = true;
    writer_u8(&writer, UCN_V6_CLUSTER_CONTROL_VERSION);
    writer_u8(&writer, (uint8_t)control->kind);
    writer_u16(&writer, control->flags);
    writer_u64(&writer, control->transaction_id);
    write_epoch(&writer, &control->old_epoch);
    write_epoch(&writer, &control->target_epoch);
    writer_u32(&writer, control->config_id);
    writer_u32(&writer, control->config_generation);
    writer_u32(&writer, control->backup_generation);
    writer_u32(&writer, control->old_voter_bitmap);
    writer_u32(&writer, control->new_voter_bitmap);
    writer_skip(&writer, 4U);
    if (!writer.ok || writer.offset != CONTROL_BODY_BYTES) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    crc = ucn_v6_crc32c(encoded, CONTROL_BODY_BYTES);
    encoded[CONTROL_BODY_BYTES] = (uint8_t)(crc >> 24U);
    encoded[CONTROL_BODY_BYTES + 1U] = (uint8_t)(crc >> 16U);
    encoded[CONTROL_BODY_BYTES + 2U] = (uint8_t)(crc >> 8U);
    encoded[CONTROL_BODY_BYTES + 3U] = (uint8_t)crc;
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_cluster_control_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_cluster_control_t *control)
{
    ucn_v6_cluster_control_t decoded;
    byte_reader_t reader;
    uint32_t expected_crc;
    if (input == NULL || control == NULL ||
        input_length != UCN_V6_CLUSTER_CONTROL_BYTES) {
        return UCN_V6_ERR_MALFORMED;
    }
    expected_crc = ((uint32_t)input[CONTROL_BODY_BYTES] << 24U) |
                   ((uint32_t)input[CONTROL_BODY_BYTES + 1U] << 16U) |
                   ((uint32_t)input[CONTROL_BODY_BYTES + 2U] << 8U) |
                   input[CONTROL_BODY_BYTES + 3U];
    if (expected_crc != ucn_v6_crc32c(input, CONTROL_BODY_BYTES)) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    reader.bytes = input;
    reader.length = CONTROL_BODY_BYTES;
    reader.offset = 0U;
    reader.ok = true;
    if (reader_u8(&reader) != UCN_V6_CLUSTER_CONTROL_VERSION) {
        return UCN_V6_ERR_MALFORMED;
    }
    decoded.kind = (ucn_v6_cluster_control_kind_t)reader_u8(&reader);
    decoded.flags = reader_u16(&reader);
    decoded.transaction_id = reader_u64(&reader);
    read_epoch(&reader, &decoded.old_epoch);
    read_epoch(&reader, &decoded.target_epoch);
    decoded.config_id = reader_u32(&reader);
    decoded.config_generation = reader_u32(&reader);
    decoded.backup_generation = reader_u32(&reader);
    decoded.old_voter_bitmap = reader_u32(&reader);
    decoded.new_voter_bitmap = reader_u32(&reader);
    if (!reader_zero(&reader, 4U) || !reader.ok ||
        reader.offset != CONTROL_BODY_BYTES ||
        !control_is_valid(&decoded)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *control = decoded;
    return UCN_V6_OK;
}

static bool owner_is_valid(const ucn_v6_cluster_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_CLUSTER_OWNER_MAGIC &&
           owner->schema == UCN_V6_CLUSTER_OWNER_SCHEMA &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->initialized && owner->canary == UCN_V6_CLUSTER_OWNER_CANARY;
}

static bool store_is_valid(const ucn_v6_cluster_store_ops_t *store)
{
    return store != NULL && store->load != NULL && store->submit != NULL;
}

static bool gate_is_available(ucn_v6_cluster_owner_t *owner)
{
    return owner != NULL && owner->callback_gate != NULL &&
           !ucn_v6_callback_gate_is_active(owner->callback_gate);
}

static ucn_v6_result_t gate_enter(ucn_v6_cluster_owner_t *owner)
{
    if (!gate_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    return ucn_v6_callback_gate_try_enter(owner->callback_gate, owner);
}

static void gate_leave(ucn_v6_cluster_owner_t *owner)
{
    (void)ucn_v6_callback_gate_leave(owner->callback_gate, owner);
}

static void persistence_fault(ucn_v6_cluster_owner_t *owner)
{
    owner->persistence_faulted = true;
    owner->authority_active = false;
}

static ucn_v6_result_t persist_snapshot(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_cluster_snapshot_t *requested)
{
    size_t loaded_length = 0U;
    ucn_v6_result_t result;
    if (owner == NULL || requested == NULL || owner->persistence_faulted ||
        requested->record_generation == UINT64_MAX) {
        return UCN_V6_ERR_STATE;
    }
    owner->staging = *requested;
    owner->staging.record_generation = owner->durable.record_generation + 1U;
    result = ucn_v6_cluster_snapshot_encode(&owner->staging,
                                            owner->record_work);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = gate_enter(owner);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = owner->store.submit(owner->store.context, owner->record_work,
                                 UCN_V6_CLUSTER_RECORD_BYTES);
    if (result == UCN_V6_OK) {
        result = owner->store.load(owner->store.context, owner->record_verify,
                                   UCN_V6_CLUSTER_RECORD_BYTES,
                                   &loaded_length);
    }
    gate_leave(owner);
    if (result != UCN_V6_OK ||
        loaded_length != UCN_V6_CLUSTER_RECORD_BYTES ||
        memcmp(owner->record_work, owner->record_verify,
               UCN_V6_CLUSTER_RECORD_BYTES) != 0) {
        persistence_fault(owner);
        return result == UCN_V6_OK ? UCN_V6_ERR_STATE : result;
    }
    owner->durable = owner->staging;
    increment_saturated(&owner->persistence_commits);
    return UCN_V6_OK;
}

static int config_voter_index(const ucn_v6_cluster_config_t *config,
                              const ucn_v6_principal_t *principal,
                              const ucn_v6_binding_key_t *binding)
{
    size_t index;
    if (!ucn_v6_cluster_config_is_valid(config) || principal == NULL ||
        binding == NULL) {
        return -1;
    }
    for (index = 0U; index < config->voter_count; ++index) {
        if (principal_equal(&config->voters[index].principal, principal) &&
            binding_equal(&config->voters[index].binding, binding)) {
            return (int)index;
        }
    }
    return -1;
}

static ucn_v6_cluster_member_t *find_member(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_principal_t *principal,
    const ucn_v6_binding_key_t *binding)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_MEMBERS; ++index) {
        ucn_v6_cluster_member_t *member = &owner->members[index].value;
        if (member->occupied &&
            principal_equal(&member->session.principal, principal) &&
            binding_equal(&member->session.binding, binding)) {
            return member;
        }
    }
    return NULL;
}

static const ucn_v6_cluster_member_t *find_member_const(
    const ucn_v6_cluster_owner_t *owner,
    const ucn_v6_principal_t *principal,
    const ucn_v6_binding_key_t *binding)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_MEMBERS; ++index) {
        const ucn_v6_cluster_member_t *member = &owner->members[index].value;
        if (member->occupied &&
            principal_equal(&member->session.principal, principal) &&
            binding_equal(&member->session.binding, binding)) {
            return member;
        }
    }
    return NULL;
}

static bool voter_live(const ucn_v6_cluster_owner_t *owner,
                       const ucn_v6_cluster_voter_t *voter,
                       uint64_t now_us)
{
    const ucn_v6_cluster_member_t *member;
    if (principal_equal(&voter->principal, &owner->local_principal) &&
        binding_equal(&voter->binding, &owner->local_binding)) {
        return true;
    }
    member = find_member_const(owner, &voter->principal, &voter->binding);
    return member != NULL && member->lease_deadline_us > now_us &&
           member->voter;
}

static uint32_t config_live_bitmap(const ucn_v6_cluster_owner_t *owner,
                                   const ucn_v6_cluster_config_t *config,
                                   uint64_t now_us)
{
    uint32_t bitmap = 0U;
    size_t index;
    if (!ucn_v6_cluster_config_is_valid(config)) {
        return 0U;
    }
    for (index = 0U; index < config->voter_count; ++index) {
        if (voter_live(owner, &config->voters[index], now_us)) {
            bitmap |= UINT32_C(1) << index;
        }
    }
    return bitmap;
}

static uint8_t bit_count(uint32_t value)
{
    uint8_t count = 0U;
    while (value != 0U) {
        count = (uint8_t)(count + (uint8_t)(value & 1U));
        value >>= 1U;
    }
    return count;
}

static bool bitmap_has_quorum(uint32_t bitmap, uint8_t voter_count)
{
    uint8_t required = (uint8_t)(voter_count / 2U + 1U);
    uint32_t allowed = voter_count == 32U ? UINT32_MAX :
        ((UINT32_C(1) << voter_count) - 1U);
    return (bitmap & ~allowed) == 0U && bit_count(bitmap) >= required;
}

static bool config_has_live_quorum(const ucn_v6_cluster_owner_t *owner,
                                   const ucn_v6_cluster_config_t *config,
                                   uint64_t now_us)
{
    return bitmap_has_quorum(config_live_bitmap(owner, config, now_us),
                             config->voter_count);
}

static void recompute_authority(ucn_v6_cluster_owner_t *owner,
                                uint64_t now_us)
{
    bool stable_quorum;
    bool joint_quorum = true;
    if (owner == NULL || owner->persistence_faulted ||
        owner->durable.authority_fenced ||
        owner->durable.role != UCN_V6_CLUSTER_HEAD ||
        !owner->durable.active_epoch_valid ||
        !principal_equal(&owner->durable.active_epoch.head_principal,
                         &owner->local_principal) ||
        !binding_equal(&owner->durable.active_epoch.head_binding,
                       &owner->local_binding)) {
        if (owner != NULL) {
            owner->authority_active = false;
        }
        return;
    }
    stable_quorum = config_has_live_quorum(owner,
                                           &owner->durable.stable_config,
                                           now_us);
    if (owner->durable.joint_valid) {
        joint_quorum = config_has_live_quorum(
            owner, &owner->durable.joint_new_config, now_us);
    }
    owner->authority_active = stable_quorum && joint_quorum;
}

static bool opened_is_authorized(const ucn_v6_security_open_result_t *opened)
{
    return opened != NULL && opened->hop_authenticated &&
           opened->endpoint_authorized &&
           (opened->frame.flags & UCN_V6_FLAG_E2E_CONTEXT) != 0U &&
           ucn_v6_principal_is_valid(&opened->authenticated_principal) &&
           ucn_v6_binding_key_is_valid(&opened->ingress_peer_session.binding) &&
           principal_equal(&opened->authenticated_principal,
                           &opened->ingress_peer_session.principal) &&
           opened->ingress_peer_session.session_generation != 0U &&
           opened->ingress_peer_session.session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool local_is_voter(const ucn_v6_cluster_owner_t *owner,
                           const ucn_v6_cluster_config_t *config)
{
    return config_voter_index(config, &owner->local_principal,
                              &owner->local_binding) >= 0;
}

static bool next_serial(uint32_t current, uint32_t *next)
{
    if (!serial_valid(current) || current >= UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        next == NULL) {
        return false;
    }
    *next = current + 1U;
    return true;
}

ucn_v6_result_t ucn_v6_cluster_owner_init_in_place(
    void *storage, size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_principal_t *local_principal,
    const ucn_v6_binding_key_t *local_binding,
    uint32_t local_session_generation,
    const ucn_v6_cluster_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_cluster_owner_t **owner)
{
    ucn_v6_cluster_owner_t *target;
    size_t length = 0U;
    ucn_v6_result_t result;
    if (owner != NULL) {
        *owner = NULL;
    }
    if (owner == NULL || local_principal == NULL || local_binding == NULL ||
        !ucn_v6_principal_is_valid(local_principal) ||
        !ucn_v6_binding_key_is_valid(local_binding) ||
        !serial_valid(local_session_generation) || !store_is_valid(store) ||
        callback_gate == NULL ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        (manifest->feature_bits & UCN_V6_FEATURE_CLUSTER) == 0U ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_CLUSTER_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK ||
        ucn_v6_callback_gate_is_active(callback_gate)) {
        return UCN_V6_ERR_CONFIG;
    }
    memset(storage, 0, storage_bytes);
    target = (ucn_v6_cluster_owner_t *)storage;
    target->magic = UCN_V6_CLUSTER_OWNER_MAGIC;
    target->schema = UCN_V6_CLUSTER_OWNER_SCHEMA;
    target->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    target->local_principal = *local_principal;
    target->local_binding = *local_binding;
    target->local_session_generation = local_session_generation;
    target->store = *store;
    target->callback_gate = callback_gate;
    target->canary = UCN_V6_CLUSTER_OWNER_CANARY;
    result = ucn_v6_callback_gate_try_enter(callback_gate, storage);
    if (result != UCN_V6_OK) {
        memset(storage, 0, storage_bytes);
        return result;
    }
    result = store->load(store->context, target->record_verify,
                         UCN_V6_CLUSTER_RECORD_BYTES, &length);
    (void)ucn_v6_callback_gate_leave(callback_gate, storage);
    if (result == UCN_V6_ERR_NOT_FOUND) {
        memset(&target->durable, 0, sizeof(target->durable));
        target->durable.record_generation = 1U;
        target->durable.boot_incarnation = 1U;
        target->durable.role = UCN_V6_CLUSTER_OBSERVER;
        target->durable.phase = UCN_V6_CLUSTER_PHASE_STABLE;
    } else if (result == UCN_V6_OK &&
               length == UCN_V6_CLUSTER_RECORD_BYTES &&
               ucn_v6_cluster_snapshot_decode(target->record_verify, length,
                                               &target->staging,
                                               &target->durable) ==
                   UCN_V6_OK &&
               target->durable.record_generation != UINT64_MAX &&
               target->durable.boot_incarnation <
                   UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        ++target->durable.record_generation;
        ++target->durable.boot_incarnation;
    } else {
        memset(storage, 0, storage_bytes);
        return result == UCN_V6_OK ? UCN_V6_ERR_MALFORMED : result;
    }
    if (ucn_v6_cluster_snapshot_encode(&target->durable,
                                       target->record_work) != UCN_V6_OK) {
        memset(storage, 0, storage_bytes);
        return UCN_V6_ERR_STATE;
    }
    result = ucn_v6_callback_gate_try_enter(callback_gate, storage);
    if (result != UCN_V6_OK) {
        memset(storage, 0, storage_bytes);
        return result;
    }
    result = store->submit(store->context, target->record_work,
                           UCN_V6_CLUSTER_RECORD_BYTES);
    if (result == UCN_V6_OK) {
        length = 0U;
        result = store->load(store->context, target->record_verify,
                             UCN_V6_CLUSTER_RECORD_BYTES, &length);
    }
    (void)ucn_v6_callback_gate_leave(callback_gate, storage);
    if (result != UCN_V6_OK ||
        length != UCN_V6_CLUSTER_RECORD_BYTES ||
        memcmp(target->record_work, target->record_verify,
               UCN_V6_CLUSTER_RECORD_BYTES) != 0) {
        memset(storage, 0, storage_bytes);
        return result == UCN_V6_OK ? UCN_V6_ERR_STATE : result;
    }
    target->initialized = true;
    *owner = target;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_cluster_create(
    ucn_v6_cluster_owner_t *owner, uint32_t cluster_id,
    const ucn_v6_cluster_config_t *initial_config, uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    ucn_v6_result_t result;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        owner->durable.active_epoch_valid || !serial_valid(cluster_id) ||
        !ucn_v6_cluster_config_is_valid(initial_config) ||
        !local_is_voter(owner, initial_config)) {
        return UCN_V6_ERR_STATE;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->active_epoch_valid = true;
    next->active_epoch.cluster_id = cluster_id;
    next->active_epoch.term = 1U;
    next->active_epoch.head_principal = owner->local_principal;
    next->active_epoch.head_binding = owner->local_binding;
    next->max_epoch_valid = true;
    next->max_epoch = next->active_epoch;
    next->stable_config = *initial_config;
    next->role = UCN_V6_CLUSTER_HEAD;
    result = persist_snapshot(owner, next);
    if (result == UCN_V6_OK) {
        recompute_authority(owner, now_us);
    }
    return result;
}

ucn_v6_result_t ucn_v6_cluster_admit_member(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_cached_peer_capability_t *capability,
    uint64_t now_us, uint64_t lease_duration_us)
{
    ucn_v6_cluster_member_t *member;
    size_t index;
    uint64_t deadline;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        !opened_is_authorized(opened) || capability == NULL ||
        !capability->valid || lease_duration_us == 0U ||
        UINT64_MAX - now_us < lease_duration_us ||
        !principal_equal(&opened->authenticated_principal,
                         &capability->principal) ||
        !principal_equal(&opened->ingress_peer_session.principal,
                         &capability->principal) ||
        !binding_equal(&opened->ingress_peer_session.binding,
                       &capability->binding) ||
        opened->ingress_peer_session.session_generation !=
            capability->session_generation ||
        (capability->record.peer.feature_bits & UCN_V6_FEATURE_CLUSTER) == 0U ||
        !serial_valid(capability->record.capability_generation)) {
        if (owner_is_valid(owner)) increment_saturated(&owner->rejected_security);
        return UCN_V6_ERR_SECURITY;
    }
    deadline = now_us + lease_duration_us;
    member = find_member(owner, &capability->principal, &capability->binding);
    if (member == NULL) {
        for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_MEMBERS; ++index) {
            if (!owner->members[index].value.occupied) {
                member = &owner->members[index].value;
                break;
            }
        }
    }
    if (member == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(member, 0, sizeof(*member));
    member->occupied = true;
    member->session = opened->ingress_peer_session;
    member->capability_generation = capability->record.capability_generation;
    member->capability_feature_bits = capability->record.peer.feature_bits;
    member->voter = config_voter_index(&owner->durable.stable_config,
                                       &capability->principal,
                                       &capability->binding) >= 0 ||
                    (owner->durable.joint_valid &&
                     config_voter_index(&owner->durable.joint_new_config,
                                        &capability->principal,
                                        &capability->binding) >= 0);
    member->backup_eligible = member->voter;
    member->lease_deadline_us = deadline;
    recompute_authority(owner, now_us);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_cluster_assign_backup(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_cluster_voter_t *backup, uint32_t backup_generation,
    uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    const ucn_v6_cluster_member_t *member;
    if (!owner_is_valid(owner) || !gate_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    recompute_authority(owner, now_us);
    if (!owner->authority_active || backup == NULL ||
        !serial_valid(backup_generation) ||
        config_voter_index(&owner->durable.stable_config,
                           &backup->principal, &backup->binding) < 0 ||
        principal_equal(&backup->principal, &owner->local_principal)) {
        return UCN_V6_ERR_STATE;
    }
    member = find_member_const(owner, &backup->principal, &backup->binding);
    if (member == NULL || !member->backup_eligible) {
        return UCN_V6_ERR_ACCESS;
    }
    if (owner->durable.backup.valid &&
        backup_generation <= owner->durable.backup.generation) {
        return UCN_V6_ERR_REPLAY;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    memset(&next->backup, 0, sizeof(next->backup));
    next->backup.valid = true;
    next->backup.principal = backup->principal;
    next->backup.binding = backup->binding;
    next->backup.generation = backup_generation;
    return persist_snapshot(owner, next);
}

ucn_v6_result_t ucn_v6_cluster_backup_ack_config(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    uint64_t transaction_id, uint32_t config_id,
    uint32_t config_generation)
{
    ucn_v6_cluster_snapshot_t *next;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        !opened_is_authorized(opened) || !owner->durable.backup.valid ||
        !owner->durable.joint_valid || transaction_id == 0U ||
        transaction_id != owner->durable.joint_transaction_id ||
        config_id != owner->durable.joint_new_config.config_id ||
        config_generation != owner->durable.joint_new_config.generation ||
        !principal_equal(&opened->authenticated_principal,
                         &owner->durable.backup.principal) ||
        !binding_equal(&opened->ingress_peer_session.binding,
                       &owner->durable.backup.binding)) {
        return UCN_V6_ERR_ACCESS;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->backup.ready = true;
    next->backup.config_transaction_id = transaction_id;
    next->backup.acknowledged_config_generation = config_generation;
    return persist_snapshot(owner, next);
}

ucn_v6_result_t ucn_v6_cluster_prepare_joint(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    const ucn_v6_cluster_config_t *new_config, uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    uint32_t expected_id;
    uint32_t expected_generation;
    ucn_v6_result_t result;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        transaction_id == 0U || !ucn_v6_cluster_config_is_valid(new_config) ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_STABLE ||
        owner->durable.joint_valid ||
        transaction_id <= owner->durable.transaction_high_water ||
        !next_serial(owner->durable.stable_config.config_id, &expected_id) ||
        !next_serial(owner->durable.stable_config.generation,
                     &expected_generation) ||
        new_config->config_id != expected_id ||
        new_config->generation != expected_generation) {
        return UCN_V6_ERR_STATE;
    }
    recompute_authority(owner, now_us);
    if (!owner->authority_active) {
        increment_saturated(&owner->rejected_quorum);
        return UCN_V6_ERR_ACCESS;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->transaction_high_water = transaction_id;
    next->joint_valid = true;
    next->joint_transaction_id = transaction_id;
    next->joint_new_config = *new_config;
    next->phase = UCN_V6_CLUSTER_PHASE_JOINT;
    if (next->backup.valid) {
        next->backup.ready = false;
        next->backup.config_transaction_id = transaction_id;
        next->backup.acknowledged_config_generation = new_config->generation;
    }
    result = persist_snapshot(owner, next);
    if (result == UCN_V6_OK) {
        recompute_authority(owner, now_us);
    }
    return result;
}

ucn_v6_result_t ucn_v6_cluster_commit_joint(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    ucn_v6_result_t result;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        !owner->durable.joint_valid ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_JOINT ||
        transaction_id == 0U ||
        transaction_id != owner->durable.joint_transaction_id) {
        return UCN_V6_ERR_STATE;
    }
    recompute_authority(owner, now_us);
    if (!owner->authority_active ||
        (owner->durable.backup.valid &&
         (!owner->durable.backup.ready ||
          owner->durable.backup.config_transaction_id != transaction_id ||
          owner->durable.backup.acknowledged_config_generation !=
              owner->durable.joint_new_config.generation))) {
        increment_saturated(&owner->rejected_quorum);
        return UCN_V6_ERR_ACCESS;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->stable_config = next->joint_new_config;
    memset(&next->joint_new_config, 0, sizeof(next->joint_new_config));
    next->joint_valid = false;
    next->joint_transaction_id = 0U;
    next->phase = UCN_V6_CLUSTER_PHASE_STABLE;
    if (next->backup.valid) {
        next->backup.ready = false;
        next->backup.config_transaction_id = 0U;
        next->backup.acknowledged_config_generation = 0U;
        if (config_voter_index(&next->stable_config, &next->backup.principal,
                               &next->backup.binding) < 0) {
            memset(&next->backup, 0, sizeof(next->backup));
        }
    }
    result = persist_snapshot(owner, next);
    if (result == UCN_V6_OK) recompute_authority(owner, now_us);
    return result;
}

ucn_v6_result_t ucn_v6_cluster_abort_joint(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    const ucn_v6_cluster_config_t *expected_new_config, uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    ucn_v6_result_t result;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        !owner->durable.joint_valid || transaction_id == 0U ||
        owner->durable.joint_transaction_id != transaction_id ||
        !config_equal(&owner->durable.joint_new_config, expected_new_config)) {
        return UCN_V6_ERR_STATE;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    memset(&next->joint_new_config, 0, sizeof(next->joint_new_config));
    next->joint_valid = false;
    next->joint_transaction_id = 0U;
    next->phase = UCN_V6_CLUSTER_PHASE_STABLE;
    if (next->backup.valid) {
        next->backup.ready = false;
        next->backup.config_transaction_id = 0U;
        next->backup.acknowledged_config_generation = 0U;
    }
    result = persist_snapshot(owner, next);
    if (result == UCN_V6_OK) recompute_authority(owner, now_us);
    return result;
}

static void transition_initialize(
    ucn_v6_cluster_transition_proof_t *transition,
    ucn_v6_cluster_transition_kind_t kind, uint64_t transaction_id,
    const ucn_v6_cluster_epoch_t *old_epoch,
    const ucn_v6_cluster_epoch_t *target_epoch,
    const ucn_v6_cluster_config_t *target_config)
{
    memset(transition, 0, sizeof(*transition));
    transition->valid = true;
    transition->kind = kind;
    transition->transaction_id = transaction_id;
    transition->old_epoch = *old_epoch;
    transition->target_epoch = *target_epoch;
    transition->target_config_id = target_config->config_id;
    transition->target_config_generation = target_config->generation;
    transition->target_config = *target_config;
}

ucn_v6_result_t ucn_v6_cluster_begin_takeover(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint32_t backup_generation, uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    ucn_v6_cluster_epoch_t target;
    int self_index;
    uint32_t term;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        transaction_id == 0U || !serial_valid(backup_generation) ||
        transaction_id <= owner->durable.transaction_high_water ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_STABLE ||
        !owner->durable.backup.valid ||
        owner->durable.backup.generation != backup_generation ||
        !principal_equal(&owner->durable.backup.principal,
                         &owner->local_principal) ||
        !binding_equal(&owner->durable.backup.binding,
                       &owner->local_binding) ||
        !next_serial(owner->durable.active_epoch.term, &term)) {
        return UCN_V6_ERR_STATE;
    }
    {
        const ucn_v6_cluster_member_t *head_member = find_member_const(
            owner, &owner->durable.active_epoch.head_principal,
            &owner->durable.active_epoch.head_binding);
        if (head_member != NULL && head_member->lease_deadline_us > now_us) {
            return UCN_V6_ERR_ACCESS;
        }
    }
    self_index = config_voter_index(&owner->durable.stable_config,
                                    &owner->local_principal,
                                    &owner->local_binding);
    if (self_index < 0) return UCN_V6_ERR_ACCESS;
    target = owner->durable.active_epoch;
    target.term = term;
    target.head_principal = owner->local_principal;
    target.head_binding = owner->local_binding;
    owner->staging = owner->durable;
    next = &owner->staging;
    next->transaction_high_water = transaction_id;
    next->phase = UCN_V6_CLUSTER_PHASE_TAKEOVER;
    next->role = UCN_V6_CLUSTER_BACKUP;
    transition_initialize(&next->transition,
                          UCN_V6_CLUSTER_TRANSITION_TAKEOVER,
                          transaction_id, &owner->durable.active_epoch,
                          &target, &owner->durable.stable_config);
    next->transition.old_voter_bitmap = UINT32_C(1) << (uint32_t)self_index;
    next->transition.new_voter_bitmap = next->transition.old_voter_bitmap;
    memset(&next->last_vote, 0, sizeof(next->last_vote));
    next->last_vote.valid = true;
    next->last_vote.source_epoch = owner->durable.active_epoch;
    next->last_vote.candidate_principal = owner->local_principal;
    next->last_vote.candidate_binding = owner->local_binding;
    next->last_vote.backup_generation = backup_generation;
    owner->authority_active = false;
    return persist_snapshot(owner, next);
}

ucn_v6_result_t ucn_v6_cluster_record_transition_vote(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_cluster_control_t *vote)
{
    ucn_v6_cluster_snapshot_t *next;
    int old_index;
    int new_index;
    uint32_t old_bit = 0U;
    uint32_t new_bit = 0U;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        !opened_is_authorized(opened) || !control_is_valid(vote) ||
        !owner->durable.transition.valid ||
        owner->durable.transition.transaction_id != vote->transaction_id ||
        !epoch_equal(&owner->durable.transition.old_epoch,
                     &vote->old_epoch) ||
        !epoch_equal(&owner->durable.transition.target_epoch,
                     &vote->target_epoch) ||
        owner->durable.transition.target_config_id != vote->config_id ||
        owner->durable.transition.target_config_generation !=
            vote->config_generation ||
        (owner->durable.phase != UCN_V6_CLUSTER_PHASE_TAKEOVER &&
         owner->durable.phase != UCN_V6_CLUSTER_PHASE_RECOVERY)) {
        if (owner_is_valid(owner)) increment_saturated(&owner->rejected_security);
        return UCN_V6_ERR_ACCESS;
    }
    if ((owner->durable.phase == UCN_V6_CLUSTER_PHASE_TAKEOVER &&
         (vote->kind != UCN_V6_CLUSTER_CTL_TAKEOVER_VOTE ||
          vote->backup_generation !=
              owner->durable.last_vote.backup_generation)) ||
        (owner->durable.phase == UCN_V6_CLUSTER_PHASE_RECOVERY &&
         vote->kind != UCN_V6_CLUSTER_CTL_RECOVERY_VOTE)) {
        return UCN_V6_ERR_ACCESS;
    }
    old_index = config_voter_index(&owner->durable.stable_config,
                                   &opened->authenticated_principal,
                                   &opened->ingress_peer_session.binding);
    new_index = config_voter_index(
        &owner->durable.transition.target_config,
        &opened->authenticated_principal,
        &opened->ingress_peer_session.binding);
    if (old_index < 0 && new_index < 0) return UCN_V6_ERR_ACCESS;
    if (old_index >= 0) old_bit = UINT32_C(1) << (uint32_t)old_index;
    if (new_index >= 0) new_bit = UINT32_C(1) << (uint32_t)new_index;
    if ((old_bit == 0U ||
         (owner->durable.transition.old_voter_bitmap & old_bit) != 0U) &&
        (new_bit == 0U ||
         (owner->durable.transition.new_voter_bitmap & new_bit) != 0U)) {
        return UCN_V6_OK;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->transition.old_voter_bitmap |= old_bit;
    next->transition.new_voter_bitmap |= new_bit;
    return persist_snapshot(owner, next);
}

static bool transition_has_quorum(const ucn_v6_cluster_snapshot_t *snapshot)
{
    if (!snapshot->transition.valid ||
        !bitmap_has_quorum(snapshot->transition.old_voter_bitmap,
                           snapshot->stable_config.voter_count)) {
        return false;
    }
    return bitmap_has_quorum(snapshot->transition.new_voter_bitmap,
                             snapshot->transition.target_config.voter_count);
}

ucn_v6_result_t ucn_v6_cluster_commit_takeover(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    ucn_v6_result_t result;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_TAKEOVER ||
        !owner->durable.transition.valid ||
        owner->durable.transition.kind !=
            UCN_V6_CLUSTER_TRANSITION_TAKEOVER ||
        transaction_id == 0U ||
        owner->durable.transition.transaction_id != transaction_id ||
        !transition_has_quorum(&owner->durable)) {
        increment_saturated(&owner->rejected_quorum);
        return UCN_V6_ERR_ACCESS;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->active_epoch = next->transition.target_epoch;
    next->max_epoch = next->active_epoch;
    next->role = UCN_V6_CLUSTER_HEAD;
    next->phase = UCN_V6_CLUSTER_PHASE_STABLE;
    memset(&next->transition, 0, sizeof(next->transition));
    result = persist_snapshot(owner, next);
    if (result == UCN_V6_OK) recompute_authority(owner, now_us);
    return result;
}

ucn_v6_result_t ucn_v6_cluster_begin_handover(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    const ucn_v6_cluster_epoch_t *target_epoch,
    const ucn_v6_cluster_config_t *target_config, uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    uint32_t expected_term;
    if (!owner_is_valid(owner) || !gate_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    recompute_authority(owner, now_us);
    if (!owner->authority_active || transaction_id == 0U ||
        transaction_id <= owner->durable.transaction_high_water ||
        !ucn_v6_cluster_epoch_is_valid(target_epoch) ||
        !ucn_v6_cluster_config_is_valid(target_config) ||
        principal_equal(&target_epoch->head_principal,
                        &owner->local_principal) ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_STABLE) {
        return UCN_V6_ERR_STATE;
    }
    if (target_epoch->cluster_id == owner->durable.active_epoch.cluster_id) {
        if (!next_serial(owner->durable.active_epoch.term, &expected_term) ||
            target_epoch->term != expected_term ||
            !config_equal(target_config, &owner->durable.stable_config)) {
            return UCN_V6_ERR_STATE;
        }
    } else {
        size_t index;
        if (target_epoch->term != 1U ||
            owner->durable.tombstone_count >=
                UCN_V6_CONFIG_CLUSTER_TOMBSTONES) {
            return UCN_V6_ERR_STATE;
        }
        for (index = 0U; index < owner->durable.tombstone_count; ++index) {
            if (owner->durable.tombstones[index].retired_cluster_id ==
                    target_epoch->cluster_id ||
                owner->durable.tombstones[index].replacement_cluster_id ==
                    target_epoch->cluster_id) {
                return UCN_V6_ERR_REPLAY;
            }
        }
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->transaction_high_water = transaction_id;
    next->phase = UCN_V6_CLUSTER_PHASE_HANDOVER;
    transition_initialize(&next->transition,
                          UCN_V6_CLUSTER_TRANSITION_HANDOVER,
                          transaction_id, &owner->durable.active_epoch,
                          target_epoch, target_config);
    return persist_snapshot(owner, next);
}

ucn_v6_result_t ucn_v6_cluster_handover_ready(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_cluster_control_t *ready)
{
    ucn_v6_cluster_snapshot_t *next;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        !opened_is_authorized(opened) || !control_is_valid(ready) ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_HANDOVER ||
        !owner->durable.transition.valid ||
        ready->kind != UCN_V6_CLUSTER_CTL_HANDOVER_READY ||
        owner->durable.transition.transaction_id != ready->transaction_id ||
        !epoch_equal(&owner->durable.transition.old_epoch,
                     &ready->old_epoch) ||
        !epoch_equal(&owner->durable.transition.target_epoch,
                     &ready->target_epoch) ||
        owner->durable.transition.target_config_id != ready->config_id ||
        owner->durable.transition.target_config_generation !=
            ready->config_generation ||
        !principal_equal(&opened->authenticated_principal,
                         &owner->durable.transition.target_epoch.head_principal) ||
        !binding_equal(&opened->ingress_peer_session.binding,
                       &owner->durable.transition.target_epoch.head_binding)) {
        return UCN_V6_ERR_ACCESS;
    }
    if (owner->durable.transition.ready) return UCN_V6_OK;
    owner->staging = owner->durable;
    next = &owner->staging;
    next->transition.ready = true;
    return persist_snapshot(owner, next);
}

ucn_v6_result_t ucn_v6_cluster_commit_handover(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id)
{
    ucn_v6_cluster_snapshot_t *next;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_HANDOVER ||
        !owner->durable.transition.valid ||
        !owner->durable.transition.ready || transaction_id == 0U ||
        owner->durable.transition.transaction_id != transaction_id) {
        return UCN_V6_ERR_STATE;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    if (next->transition.target_epoch.cluster_id !=
        next->transition.old_epoch.cluster_id) {
        size_t index = next->tombstone_count;
        next->tombstones[index].occupied = true;
        next->tombstones[index].retired_cluster_id =
            next->transition.old_epoch.cluster_id;
        next->tombstones[index].replacement_cluster_id =
            next->transition.target_epoch.cluster_id;
        next->tombstones[index].transaction_id = transaction_id;
        ++next->tombstone_count;
    }
    next->active_epoch = next->transition.target_epoch;
    next->max_epoch = next->active_epoch;
    next->stable_config = next->transition.target_config;
    next->role = UCN_V6_CLUSTER_FENCED;
    next->phase = UCN_V6_CLUSTER_PHASE_STABLE;
    next->authority_fenced = true;
    memset(&next->transition, 0, sizeof(next->transition));
    owner->authority_active = false;
    return persist_snapshot(owner, next);
}

ucn_v6_result_t ucn_v6_cluster_begin_recovery(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    const ucn_v6_cluster_epoch_t *target_epoch, uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    size_t index;
    if (!owner_is_valid(owner) || !gate_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    recompute_authority(owner, now_us);
    if (owner->authority_active || transaction_id == 0U ||
        transaction_id <= owner->durable.transaction_high_water ||
        !ucn_v6_cluster_epoch_is_valid(target_epoch) ||
        !principal_equal(&target_epoch->head_principal,
                         &owner->local_principal) ||
        !binding_equal(&target_epoch->head_binding, &owner->local_binding) ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_STABLE ||
        target_epoch->cluster_id == owner->durable.active_epoch.cluster_id ||
        target_epoch->term != 1U ||
        owner->durable.tombstone_count >=
            UCN_V6_CONFIG_CLUSTER_TOMBSTONES) {
        return UCN_V6_ERR_STATE;
    }
    {
        const ucn_v6_cluster_member_t *head_member = find_member_const(
            owner, &owner->durable.active_epoch.head_principal,
            &owner->durable.active_epoch.head_binding);
        if (head_member != NULL && head_member->lease_deadline_us > now_us) {
            return UCN_V6_ERR_ACCESS;
        }
    }
    for (index = 0U; index < owner->durable.tombstone_count; ++index) {
        if (owner->durable.tombstones[index].retired_cluster_id ==
                target_epoch->cluster_id ||
            owner->durable.tombstones[index].replacement_cluster_id ==
                target_epoch->cluster_id) {
            return UCN_V6_ERR_REPLAY;
        }
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->transaction_high_water = transaction_id;
    next->phase = UCN_V6_CLUSTER_PHASE_RECOVERY;
    next->role = UCN_V6_CLUSTER_RECOVERY_HEAD;
    next->authority_fenced = false;
    transition_initialize(&next->transition,
                          UCN_V6_CLUSTER_TRANSITION_RECOVERY,
                          transaction_id, &owner->durable.active_epoch,
                          target_epoch, &owner->durable.stable_config);
    return persist_snapshot(owner, next);
}

ucn_v6_result_t ucn_v6_cluster_commit_recovery(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    ucn_v6_result_t result;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_RECOVERY ||
        !owner->durable.transition.valid || transaction_id == 0U ||
        owner->durable.transition.transaction_id != transaction_id ||
        !transition_has_quorum(&owner->durable)) {
        increment_saturated(&owner->rejected_quorum);
        return UCN_V6_ERR_ACCESS;
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    {
        size_t index = next->tombstone_count;
        next->tombstones[index].occupied = true;
        next->tombstones[index].retired_cluster_id =
            next->transition.old_epoch.cluster_id;
        next->tombstones[index].replacement_cluster_id =
            next->transition.target_epoch.cluster_id;
        next->tombstones[index].transaction_id = transaction_id;
        ++next->tombstone_count;
    }
    next->active_epoch = next->transition.target_epoch;
    next->max_epoch = next->active_epoch;
    next->role = UCN_V6_CLUSTER_HEAD;
    next->phase = UCN_V6_CLUSTER_PHASE_STABLE;
    memset(&next->transition, 0, sizeof(next->transition));
    result = persist_snapshot(owner, next);
    if (result == UCN_V6_OK) recompute_authority(owner, now_us);
    return result;
}

ucn_v6_result_t ucn_v6_cluster_rekey(
    ucn_v6_cluster_owner_t *owner, uint64_t transaction_id,
    uint32_t successor_cluster_id, uint64_t now_us)
{
    ucn_v6_cluster_snapshot_t *next;
    size_t index;
    if (!owner_is_valid(owner) || !gate_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    recompute_authority(owner, now_us);
    if (!owner->authority_active || transaction_id == 0U ||
        transaction_id <= owner->durable.transaction_high_water ||
        !serial_valid(successor_cluster_id) ||
        successor_cluster_id == owner->durable.active_epoch.cluster_id ||
        owner->durable.phase != UCN_V6_CLUSTER_PHASE_STABLE ||
        owner->durable.tombstone_count >= UCN_V6_CONFIG_CLUSTER_TOMBSTONES) {
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < owner->durable.tombstone_count; ++index) {
        if (owner->durable.tombstones[index].retired_cluster_id ==
                successor_cluster_id ||
            owner->durable.tombstones[index].replacement_cluster_id ==
                successor_cluster_id) {
            return UCN_V6_ERR_REPLAY;
        }
    }
    owner->staging = owner->durable;
    next = &owner->staging;
    next->transaction_high_water = transaction_id;
    index = next->tombstone_count;
    next->tombstones[index].occupied = true;
    next->tombstones[index].retired_cluster_id = next->active_epoch.cluster_id;
    next->tombstones[index].replacement_cluster_id = successor_cluster_id;
    next->tombstones[index].transaction_id = transaction_id;
    ++next->tombstone_count;
    next->active_epoch.cluster_id = successor_cluster_id;
    next->active_epoch.term = 1U;
    next->max_epoch = next->active_epoch;
    next->stable_config.config_id = 1U;
    next->stable_config.generation = 1U;
    return persist_snapshot(owner, next);
}

static bool directory_entry_is_valid(
    const ucn_v6_cluster_directory_entry_t *entry, uint64_t now_us)
{
    return entry != NULL && entry->occupied &&
           serial_valid(entry->remote_cluster_id) &&
           ucn_v6_cluster_epoch_is_valid(&entry->remote_epoch) &&
           entry->remote_epoch.cluster_id == entry->remote_cluster_id &&
           ucn_v6_principal_is_valid(&entry->next_hop.principal) &&
           ucn_v6_binding_key_is_valid(&entry->next_hop.binding) &&
           serial_valid(entry->next_hop.session_generation) &&
           serial_valid(entry->route_generation) && entry->path_id != 0U &&
           serial_valid(entry->path_generation) && entry->deadline_us > now_us;
}

ucn_v6_result_t ucn_v6_cluster_directory_install(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_cluster_directory_entry_t *entry, uint64_t now_us)
{
    size_t index;
    size_t empty = UCN_V6_CONFIG_CLUSTER_DIRECTORY;
    if (!owner_is_valid(owner) || !gate_is_available(owner) ||
        !opened_is_authorized(opened) ||
        !directory_entry_is_valid(entry, now_us) ||
        !principal_equal(&opened->authenticated_principal,
                         &entry->remote_epoch.head_principal)) {
        if (owner_is_valid(owner)) increment_saturated(&owner->rejected_security);
        return UCN_V6_ERR_SECURITY;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_DIRECTORY; ++index) {
        ucn_v6_cluster_directory_entry_t *current =
            &owner->directory[index].value;
        if (!current->occupied || current->deadline_us <= now_us) {
            if (empty == UCN_V6_CONFIG_CLUSTER_DIRECTORY) empty = index;
            continue;
        }
        if (current->remote_cluster_id == entry->remote_cluster_id) {
            if (entry->remote_epoch.term < current->remote_epoch.term ||
                (entry->remote_epoch.term == current->remote_epoch.term &&
                 (!principal_equal(&entry->remote_epoch.head_principal,
                                   &current->remote_epoch.head_principal) ||
                  !binding_equal(&entry->remote_epoch.head_binding,
                                 &current->remote_epoch.head_binding)))) {
                increment_saturated(&owner->rejected_replay);
                return UCN_V6_ERR_REPLAY;
            }
            *current = *entry;
            return UCN_V6_OK;
        }
    }
    if (empty == UCN_V6_CONFIG_CLUSTER_DIRECTORY) {
        return UCN_V6_ERR_NO_SPACE;
    }
    owner->directory[empty].value = *entry;
    return UCN_V6_OK;
}

static bool tunnel_is_valid(const ucn_v6_cluster_tunnel_t *tunnel,
                            uint64_t now_us)
{
    return tunnel != NULL && tunnel->occupied && tunnel->tunnel_id != 0U &&
           serial_valid(tunnel->source_cluster_id) &&
           serial_valid(tunnel->destination_cluster_id) &&
           tunnel->source_cluster_id != tunnel->destination_cluster_id &&
           ucn_v6_principal_is_valid(
               &tunnel->route_domain.origin_principal) &&
           ucn_v6_binding_key_is_valid(
               &tunnel->route_domain.origin_binding) &&
           serial_valid(tunnel->route_domain.origin_session_generation) &&
           ucn_v6_principal_is_valid(
               &tunnel->route_domain.destination_principal) &&
           ucn_v6_binding_key_is_valid(
               &tunnel->route_domain.destination_binding) &&
           tunnel->path.valid &&
           principal_equal(&tunnel->path.destination_principal,
                           &tunnel->route_domain.destination_principal) &&
           binding_equal(&tunnel->path.destination_binding,
                         &tunnel->route_domain.destination_binding) &&
           tunnel->path.session_generation != 0U &&
           serial_valid(tunnel->path.route_generation) &&
           tunnel->path.path_id != 0U &&
           serial_valid(tunnel->path.path_generation) &&
           tunnel->path.payload_budget != 0U &&
           tunnel->path.fragment_data_budget != 0U &&
           tunnel->path.deadline_us >= tunnel->deadline_us &&
           tunnel->deadline_us > now_us;
}

ucn_v6_result_t ucn_v6_cluster_tunnel_install(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_cluster_tunnel_t *tunnel, uint64_t now_us)
{
    size_t index;
    size_t empty = UCN_V6_CONFIG_CLUSTER_TUNNELS;
    if (!owner_is_valid(owner) || !gate_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    recompute_authority(owner, now_us);
    if (!owner->authority_active || !tunnel_is_valid(tunnel, now_us) ||
        tunnel->source_cluster_id != owner->durable.active_epoch.cluster_id ||
        !principal_equal(&tunnel->route_domain.origin_principal,
                         &owner->local_principal) ||
        !binding_equal(&tunnel->route_domain.origin_binding,
                       &owner->local_binding)) {
        return UCN_V6_ERR_ACCESS;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_TUNNELS; ++index) {
        ucn_v6_cluster_tunnel_t *current = &owner->tunnels[index].value;
        if (!current->occupied || current->deadline_us <= now_us) {
            if (empty == UCN_V6_CONFIG_CLUSTER_TUNNELS) empty = index;
            continue;
        }
        if (current->tunnel_id == tunnel->tunnel_id) {
            if (memcmp(current, tunnel, sizeof(*current)) == 0) {
                return UCN_V6_OK;
            }
            return UCN_V6_ERR_REPLAY;
        }
    }
    if (empty == UCN_V6_CONFIG_CLUSTER_TUNNELS) return UCN_V6_ERR_NO_SPACE;
    owner->tunnels[empty].value = *tunnel;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_cluster_copy_tunnel(
    const ucn_v6_cluster_owner_t *owner, uint64_t tunnel_id,
    uint64_t now_us, ucn_v6_cluster_tunnel_t *tunnel)
{
    size_t index;
    if (!owner_is_valid(owner) || tunnel_id == 0U || tunnel == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_TUNNELS; ++index) {
        if (owner->tunnels[index].value.occupied &&
            owner->tunnels[index].value.tunnel_id == tunnel_id &&
            owner->tunnels[index].value.deadline_us > now_us) {
            *tunnel = owner->tunnels[index].value;
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_cluster_step(
    ucn_v6_cluster_owner_t *owner, uint64_t now_us)
{
    size_t index;
    if (!owner_is_valid(owner) || !gate_is_available(owner)) {
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_MEMBERS; ++index) {
        if (owner->members[index].value.occupied &&
            owner->members[index].value.lease_deadline_us <= now_us) {
            memset(&owner->members[index], 0, sizeof(owner->members[index]));
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_DIRECTORY; ++index) {
        if (owner->directory[index].value.occupied &&
            owner->directory[index].value.deadline_us <= now_us) {
            memset(&owner->directory[index], 0, sizeof(owner->directory[index]));
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_TUNNELS; ++index) {
        if (owner->tunnels[index].value.occupied &&
            owner->tunnels[index].value.deadline_us <= now_us) {
            memset(&owner->tunnels[index], 0, sizeof(owner->tunnels[index]));
        }
    }
    recompute_authority(owner, now_us);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_cluster_copy_snapshot(
    const ucn_v6_cluster_owner_t *owner,
    ucn_v6_cluster_snapshot_t *snapshot)
{
    if (!owner_is_valid(owner) || snapshot == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *snapshot = owner->durable;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_cluster_copy_view(
    const ucn_v6_cluster_owner_t *owner, uint64_t now_us,
    ucn_v6_cluster_view_t *view)
{
    ucn_v6_cluster_view_t result;
    size_t index;
    bool stable_quorum;
    bool joint_quorum = true;
    if (!owner_is_valid(owner) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&result, 0, sizeof(result));
    stable_quorum = owner->durable.active_epoch_valid &&
        config_has_live_quorum(owner, &owner->durable.stable_config, now_us);
    if (owner->durable.joint_valid) {
        joint_quorum = config_has_live_quorum(
            owner, &owner->durable.joint_new_config, now_us);
    }
    result.role = owner->durable.role;
    result.phase = owner->durable.phase;
    result.quorum_met = stable_quorum;
    result.joint_quorum_met = stable_quorum && joint_quorum;
    result.authority_active = owner->authority_active && stable_quorum &&
                              joint_quorum && !owner->persistence_faulted;
    result.persistence_faulted = owner->persistence_faulted;
    result.persistence_commits = owner->persistence_commits;
    result.rejected_security = owner->rejected_security;
    result.rejected_quorum = owner->rejected_quorum;
    result.rejected_replay = owner->rejected_replay;
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_MEMBERS; ++index) {
        if (owner->members[index].value.occupied &&
            owner->members[index].value.lease_deadline_us > now_us) {
            ++result.members;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_DIRECTORY; ++index) {
        if (owner->directory[index].value.occupied &&
            owner->directory[index].value.deadline_us > now_us) {
            ++result.directory_entries;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_TUNNELS; ++index) {
        if (owner->tunnels[index].value.occupied &&
            owner->tunnels[index].value.deadline_us > now_us) {
            ++result.tunnels;
        }
    }
    *view = result;
    return UCN_V6_OK;
}
