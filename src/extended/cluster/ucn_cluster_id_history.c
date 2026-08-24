/* CLV2-13-12: default-OFF bounded Cluster-ID allocation history. */

#include "ucn/ucn_cluster_rekey.h"

#include <string.h>

enum {
    HISTORY_MAGIC_OFFSET = 0U,
    HISTORY_VERSION_OFFSET = 4U,
    HISTORY_COUNT_OFFSET = 6U,
    HISTORY_GENERATION_OFFSET = 8U,
    HISTORY_ENTRIES_OFFSET = 12U,
    HISTORY_ENTRY_BYTES = 33U,
    HISTORY_CRC_OFFSET = 276U
};

#define HISTORY_MAGIC UINT32_C(0x55434948)
#define HISTORY_VERSION ((uint16_t)1U)

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8U); p[1] = (uint8_t)v;
}

static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24U); p[1] = (uint8_t)(v >> 16U);
    p[2] = (uint8_t)(v >> 8U); p[3] = (uint8_t)v;
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);
}

static uint32_t read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) |
           ((uint32_t)p[2] << 8U) | p[3];
}

static uint32_t history_crc(const uint8_t *record)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    size_t index;
    uint8_t bit;

    for (index = 0U; index < UCN_CLUSTER_ID_HISTORY_RECORD_BYTES; ++index) {
        uint8_t value = (index >= HISTORY_CRC_OFFSET) ? 0U : record[index];
        crc ^= value;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^
                  ((crc & 1U) != 0U ? UINT32_C(0xEDB88320) : 0U);
        }
    }
    return ~crc;
}

static bool identity_is_valid(const ucn_cluster_id_request_t *identity)
{
    bool parent_absent;
    bool parent_valid;

    if (identity == NULL ||
        identity->purpose < UCN_CLUSTER_ID_PURPOSE_ELECTION ||
        identity->purpose > UCN_CLUSTER_ID_PURPOSE_REKEY ||
        identity->local_node_id == 0U ||
        identity->local_node_id == UCN_NODE_BROADCAST ||
        identity->round == 0U ||
        identity->round > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        identity->parent_config_id > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD ||
        identity->recovery_round > UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return false;
    }
    parent_absent = identity->parent_cluster_id == 0U &&
                    identity->parent_term == 0U &&
                    identity->parent_config_id == 0U;
    parent_valid = identity->parent_cluster_id != 0U &&
                   identity->parent_cluster_id != UCN_NODE_BROADCAST &&
                   identity->parent_term != 0U &&
                   identity->parent_term <=
                       UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
    if (!parent_absent && !parent_valid) {
        return false;
    }
    if (identity->purpose != UCN_CLUSTER_ID_PURPOSE_RECOVERY &&
        identity->recovery_round != 0U) {
        return false;
    }
    return identity->purpose != UCN_CLUSTER_ID_PURPOSE_REKEY ||
           (parent_valid && identity->parent_config_id != 0U);
}

static bool identity_equal(const ucn_cluster_id_request_t *a,
                           const ucn_cluster_id_request_t *b)
{
    return a->purpose == b->purpose &&
           a->local_node_id == b->local_node_id &&
           a->parent_cluster_id == b->parent_cluster_id &&
           a->parent_term == b->parent_term &&
           a->parent_config_id == b->parent_config_id &&
           a->recovery_round == b->recovery_round &&
           a->incarnation == b->incarnation && a->round == b->round;
}

static bool history_is_canonical(const ucn_cluster_id_history_t *history)
{
    ucn_cluster_id_history_t rebuilt;
    size_t index;

    if (history == NULL ||
        history->count > UCN_CLUSTER_ID_HISTORY_CAPACITY) {
        return false;
    }
    ucn_cluster_id_history_init(&rebuilt);
    for (index = 0U; index < history->count; ++index) {
        const ucn_cluster_id_history_entry_t *entry =
            &history->entries[index];

        if (ucn_cluster_id_history_admit(&rebuilt, &entry->identity,
                                         entry->cluster_id) != UCN_OK ||
            rebuilt.count != (uint8_t)(index + 1U)) {
            return false;
        }
    }
    return true;
}

void ucn_cluster_id_history_init(ucn_cluster_id_history_t *history)
{
    if (history != NULL) {
        (void)memset(history, 0, sizeof(*history));
    }
}

static void fingerprint_byte(uint32_t *fingerprint, uint8_t value)
{
    *fingerprint ^= value;
    *fingerprint *= UINT32_C(16777619);
}

static void fingerprint_u32(uint32_t *fingerprint, uint32_t value)
{
    fingerprint_byte(fingerprint, (uint8_t)(value >> 24U));
    fingerprint_byte(fingerprint, (uint8_t)(value >> 16U));
    fingerprint_byte(fingerprint, (uint8_t)(value >> 8U));
    fingerprint_byte(fingerprint, (uint8_t)value);
}

uint32_t ucn_cluster_id_history_fingerprint(
    const ucn_cluster_id_history_t *history, uint32_t generation)
{
    uint32_t fingerprint = UINT32_C(2166136261);
    size_t index;

    if (generation == 0U || !history_is_canonical(history)) {
        return 0U;
    }
    fingerprint_u32(&fingerprint, HISTORY_MAGIC);
    fingerprint_byte(&fingerprint, (uint8_t)(HISTORY_VERSION >> 8U));
    fingerprint_byte(&fingerprint, (uint8_t)HISTORY_VERSION);
    fingerprint_byte(&fingerprint, history->count);
    fingerprint_u32(&fingerprint, generation);
    for (index = 0U; index < history->count; ++index) {
        const ucn_cluster_id_history_entry_t *entry =
            &history->entries[index];
        const ucn_cluster_id_request_t *identity = &entry->identity;

        fingerprint_u32(&fingerprint, entry->cluster_id);
        fingerprint_byte(&fingerprint, (uint8_t)identity->purpose);
        fingerprint_u32(&fingerprint, identity->local_node_id);
        fingerprint_u32(&fingerprint, identity->parent_cluster_id);
        fingerprint_u32(&fingerprint, identity->parent_term);
        fingerprint_u32(&fingerprint, identity->parent_config_id);
        fingerprint_u32(&fingerprint, identity->recovery_round);
        fingerprint_u32(&fingerprint, identity->incarnation);
        fingerprint_u32(&fingerprint, identity->round);
    }
    return fingerprint == 0U ? UINT32_C(1) : fingerprint;
}

ucn_result_t ucn_cluster_id_history_admit(
    ucn_cluster_id_history_t *history,
    const ucn_cluster_id_request_t *identity,
    uint32_t cluster_id)
{
    size_t index;

    if (history == NULL || !identity_is_valid(identity) || cluster_id == 0U ||
        cluster_id == UCN_NODE_BROADCAST ||
        (identity->parent_cluster_id != 0U &&
         cluster_id == identity->parent_cluster_id) ||
        history->count > UCN_CLUSTER_ID_HISTORY_CAPACITY) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < history->count; ++index) {
        bool same_id = history->entries[index].cluster_id == cluster_id;
        bool same_identity =
            identity_equal(&history->entries[index].identity, identity);

        if (same_id && same_identity) {
            return UCN_OK;
        }
        if (same_id || same_identity) {
            return UCN_ERR_REPLAY;
        }
    }
    if (history->count == UCN_CLUSTER_ID_HISTORY_CAPACITY) {
        return UCN_ERR_NO_SPACE;
    }
    history->entries[history->count].cluster_id = cluster_id;
    history->entries[history->count].identity = *identity;
    history->count++;
    return UCN_OK;
}

static void write_identity(uint8_t *p,
                           const ucn_cluster_id_history_entry_t *entry)
{
    write_u32(p, entry->cluster_id);
    p[4] = (uint8_t)entry->identity.purpose;
    write_u32(p + 5U, entry->identity.local_node_id);
    write_u32(p + 9U, entry->identity.parent_cluster_id);
    write_u32(p + 13U, entry->identity.parent_term);
    write_u32(p + 17U, entry->identity.parent_config_id);
    write_u32(p + 21U, entry->identity.recovery_round);
    write_u32(p + 25U, entry->identity.incarnation);
    write_u32(p + 29U, entry->identity.round);
}

static bool read_identity(const uint8_t *p,
                          ucn_cluster_id_history_entry_t *entry)
{
    (void)memset(entry, 0, sizeof(*entry));
    entry->cluster_id = read_u32(p);
    entry->identity.purpose = (ucn_cluster_id_purpose_t)p[4];
    entry->identity.local_node_id = read_u32(p + 5U);
    entry->identity.parent_cluster_id = read_u32(p + 9U);
    entry->identity.parent_term = read_u32(p + 13U);
    entry->identity.parent_config_id = read_u32(p + 17U);
    entry->identity.recovery_round = read_u32(p + 21U);
    entry->identity.incarnation = read_u32(p + 25U);
    entry->identity.round = read_u32(p + 29U);
    return identity_is_valid(&entry->identity) && entry->cluster_id != 0U &&
           entry->cluster_id != UCN_NODE_BROADCAST &&
           (entry->identity.parent_cluster_id == 0U ||
            entry->cluster_id != entry->identity.parent_cluster_id);
}

ucn_result_t ucn_cluster_id_history_record_encode(
    const ucn_cluster_id_history_t *history, uint32_t generation,
    uint8_t *output, size_t output_capacity)
{
    size_t index;

    if (history == NULL || output == NULL || generation == 0U ||
        !history_is_canonical(history)) {
        return UCN_ERR_ARGUMENT;
    }
    if (output_capacity < UCN_CLUSTER_ID_HISTORY_RECORD_BYTES) {
        return UCN_ERR_NO_SPACE;
    }
    (void)memset(output, 0, UCN_CLUSTER_ID_HISTORY_RECORD_BYTES);
    write_u32(output + HISTORY_MAGIC_OFFSET, HISTORY_MAGIC);
    write_u16(output + HISTORY_VERSION_OFFSET, HISTORY_VERSION);
    output[HISTORY_COUNT_OFFSET] = history->count;
    write_u32(output + HISTORY_GENERATION_OFFSET, generation);
    for (index = 0U; index < history->count; ++index) {
        write_identity(output + HISTORY_ENTRIES_OFFSET +
                       index * HISTORY_ENTRY_BYTES, &history->entries[index]);
    }
    write_u32(output + HISTORY_CRC_OFFSET, history_crc(output));
    return UCN_OK;
}

ucn_result_t ucn_cluster_id_history_record_decode(
    const uint8_t *record, size_t record_length, uint32_t *generation,
    ucn_cluster_id_history_t *history)
{
    ucn_cluster_id_history_t decoded;
    size_t index;

    if (record == NULL || generation == NULL || history == NULL ||
        record_length != UCN_CLUSTER_ID_HISTORY_RECORD_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    if (read_u32(record + HISTORY_MAGIC_OFFSET) != HISTORY_MAGIC ||
        read_u16(record + HISTORY_VERSION_OFFSET) != HISTORY_VERSION ||
        record[7U] != 0U ||
        record[HISTORY_COUNT_OFFSET] > UCN_CLUSTER_ID_HISTORY_CAPACITY ||
        read_u32(record + HISTORY_GENERATION_OFFSET) == 0U ||
        read_u32(record + HISTORY_CRC_OFFSET) != history_crc(record)) {
        return UCN_ERR_MALFORMED;
    }
    ucn_cluster_id_history_init(&decoded);
    for (index = 0U; index < record[HISTORY_COUNT_OFFSET]; ++index) {
        ucn_cluster_id_history_entry_t entry;
        ucn_result_t result;

        if (!read_identity(record + HISTORY_ENTRIES_OFFSET +
                           index * HISTORY_ENTRY_BYTES, &entry)) {
            return UCN_ERR_MALFORMED;
        }
        result = ucn_cluster_id_history_admit(&decoded, &entry.identity,
                                              entry.cluster_id);
        if (result != UCN_OK) {
            return UCN_ERR_MALFORMED;
        }
    }
    for (index = HISTORY_ENTRIES_OFFSET +
                 (size_t)record[HISTORY_COUNT_OFFSET] * HISTORY_ENTRY_BYTES;
         index < HISTORY_CRC_OFFSET; ++index) {
        if (record[index] != 0U) {
            return UCN_ERR_MALFORMED;
        }
    }
    *generation = read_u32(record + HISTORY_GENERATION_OFFSET);
    *history = decoded;
    return UCN_OK;
}
