#include "ucn/ucn_cluster_config_store.h"

#include <string.h>

#include "ucn/ucn_cluster_config_persistence.h"

#define UCN_CLUSTER_CONFIG_STORE_MAGIC UINT32_C(0x55434647)
#define UCN_CLUSTER_CONFIG_STORE_MAGIC_OFFSET ((size_t)0U)
#define UCN_CLUSTER_CONFIG_STORE_SCHEMA_OFFSET ((size_t)4U)
#define UCN_CLUSTER_CONFIG_STORE_RESERVED_OFFSET ((size_t)6U)
#define UCN_CLUSTER_CONFIG_STORE_GENERATION_OFFSET ((size_t)8U)
#define UCN_CLUSTER_CONFIG_STORE_REF_OFFSET ((size_t)12U)
#define UCN_CLUSTER_CONFIG_STORE_BODY_OFFSET \
    UCN_CLUSTER_CONFIG_STORE_RECORD_HEADER_BYTES
#define UCN_CLUSTER_CONFIG_STORE_CRC_OFFSET \
    (UCN_CLUSTER_CONFIG_STORE_RECORD_BYTES - (size_t)4U)

typedef enum config_store_slot_state {
    CONFIG_STORE_SLOT_EMPTY = 0,
    CONFIG_STORE_SLOT_VALID = 1,
    CONFIG_STORE_SLOT_INVALID = 2
} config_store_slot_state_t;

typedef struct config_store_slot_view {
    ucn_cluster_config_state_t config;
    ucn_cluster_persist_config_ref_t reference;
    uint32_t generation;
} config_store_slot_view_t;

static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0U] = (uint8_t)(value >> 8U);
    output[1U] = (uint8_t)value;
}

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0U] << 8U) | (uint16_t)input[1U]);
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0U] = (uint8_t)(value >> 24U);
    output[1U] = (uint8_t)(value >> 16U);
    output[2U] = (uint8_t)(value >> 8U);
    output[3U] = (uint8_t)value;
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0U] << 24U) |
           ((uint32_t)input[1U] << 16U) |
           ((uint32_t)input[2U] << 8U) | (uint32_t)input[3U];
}

static uint32_t crc32_update(uint32_t crc, uint8_t value)
{
    uint32_t bit;

    crc ^= (uint32_t)value;
    for (bit = 0U; bit < 8U; ++bit) {
        crc = (crc >> 1U) ^
              ((crc & 1U) != 0U ? UINT32_C(0xEDB88320) : UINT32_C(0));
    }
    return crc;
}

static uint32_t record_crc32(const uint8_t *record)
{
    size_t index;
    uint32_t crc = UINT32_C(0xFFFFFFFF);

    for (index = 0U; index < UCN_CLUSTER_CONFIG_STORE_CRC_OFFSET; ++index) {
        crc = crc32_update(crc, record[index]);
    }
    return ~crc;
}

static bool ref_equal(const ucn_cluster_persist_config_ref_t *left,
                      const ucn_cluster_persist_config_ref_t *right)
{
    return left != NULL && right != NULL && left->valid == right->valid &&
           (!left->valid ||
            (left->config_id == right->config_id &&
             left->generation == right->generation &&
             memcmp(left->digest, right->digest, sizeof(left->digest)) == 0));
}

static bool ref_is_valid(const ucn_cluster_persist_config_ref_t *reference)
{
    return reference != NULL && reference->valid && reference->config_id != 0U &&
           reference->config_id != UCN_NODE_BROADCAST &&
           reference->config_id <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD &&
           reference->generation != 0U &&
           reference->generation <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static bool record_is_empty(const uint8_t *record)
{
    size_t index;
    bool all_zero = true;
    bool all_ff = true;

    for (index = 0U; index < UCN_CLUSTER_CONFIG_STORE_RECORD_BYTES; ++index) {
        if (record[index] != 0U) {
            all_zero = false;
        }
        if (record[index] != UINT8_MAX) {
            all_ff = false;
        }
    }
    return all_zero || all_ff;
}

static void write_ref(uint8_t *output,
                      const ucn_cluster_persist_config_ref_t *reference)
{
    output[0U] = reference->valid ? 1U : 0U;
    write_u32_be(output + 1U, reference->config_id);
    write_u32_be(output + 5U, reference->generation);
    (void)memcpy(output + 9U, reference->digest, sizeof(reference->digest));
}

static bool read_ref(const uint8_t *input,
                     ucn_cluster_persist_config_ref_t *reference)
{
    if (input[0U] != 1U) {
        return false;
    }
    (void)memset(reference, 0, sizeof(*reference));
    reference->valid = true;
    reference->config_id = read_u32_be(input + 1U);
    reference->generation = read_u32_be(input + 5U);
    (void)memcpy(reference->digest, input + 9U, sizeof(reference->digest));
    return ref_is_valid(reference);
}

static config_store_slot_state_t slot_decode(const uint8_t *record,
                                             config_store_slot_view_t *output)
{
    config_store_slot_view_t candidate;
    ucn_cluster_persist_config_ref_t calculated_ref;

    if (record == NULL || output == NULL) {
        return CONFIG_STORE_SLOT_INVALID;
    }
    if (record_is_empty(record)) {
        return CONFIG_STORE_SLOT_EMPTY;
    }
    if (read_u32_be(record + UCN_CLUSTER_CONFIG_STORE_MAGIC_OFFSET) !=
            UCN_CLUSTER_CONFIG_STORE_MAGIC ||
        read_u16_be(record + UCN_CLUSTER_CONFIG_STORE_SCHEMA_OFFSET) !=
            UCN_CLUSTER_CONFIG_STORE_RECORD_SCHEMA_VERSION ||
        record[UCN_CLUSTER_CONFIG_STORE_RESERVED_OFFSET] != 0U ||
        record[UCN_CLUSTER_CONFIG_STORE_RESERVED_OFFSET + 1U] != 0U ||
        read_u32_be(record + UCN_CLUSTER_CONFIG_STORE_GENERATION_OFFSET) == 0U ||
        read_u32_be(record + UCN_CLUSTER_CONFIG_STORE_CRC_OFFSET) !=
            record_crc32(record)) {
        return CONFIG_STORE_SLOT_INVALID;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.generation =
        read_u32_be(record + UCN_CLUSTER_CONFIG_STORE_GENERATION_OFFSET);
    if (!read_ref(record + UCN_CLUSTER_CONFIG_STORE_REF_OFFSET,
                  &candidate.reference) ||
        ucn_cluster_config_state_deserialize(
            record + UCN_CLUSTER_CONFIG_STORE_BODY_OFFSET,
            UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES, &candidate.config) != UCN_OK ||
        candidate.config.phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        ucn_cluster_config_persist_ref_from_state(&candidate.config,
                                                  &calculated_ref) != UCN_OK ||
        !ref_equal(&candidate.reference, &calculated_ref)) {
        return CONFIG_STORE_SLOT_INVALID;
    }
    *output = candidate;
    return CONFIG_STORE_SLOT_VALID;
}

static ucn_result_t record_encode(
    uint8_t *record,
    uint32_t generation,
    const ucn_cluster_config_state_t *stable_config,
    const ucn_cluster_persist_config_ref_t *reference)
{
    ucn_cluster_persist_config_ref_t calculated_ref;

    if (record == NULL || generation == 0U || stable_config == NULL ||
        reference == NULL ||
        !ucn_cluster_config_state_is_valid(stable_config) ||
        stable_config->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        !ref_is_valid(reference) ||
        ucn_cluster_config_persist_ref_from_state(stable_config,
                                                  &calculated_ref) != UCN_OK ||
        !ref_equal(reference, &calculated_ref)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(record, 0, UCN_CLUSTER_CONFIG_STORE_RECORD_BYTES);
    write_u32_be(record + UCN_CLUSTER_CONFIG_STORE_MAGIC_OFFSET,
                 UCN_CLUSTER_CONFIG_STORE_MAGIC);
    write_u16_be(record + UCN_CLUSTER_CONFIG_STORE_SCHEMA_OFFSET,
                 UCN_CLUSTER_CONFIG_STORE_RECORD_SCHEMA_VERSION);
    write_u32_be(record + UCN_CLUSTER_CONFIG_STORE_GENERATION_OFFSET,
                 generation);
    write_ref(record + UCN_CLUSTER_CONFIG_STORE_REF_OFFSET, reference);
    if (ucn_cluster_config_state_serialize(
            stable_config, record + UCN_CLUSTER_CONFIG_STORE_BODY_OFFSET,
            UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    write_u32_be(record + UCN_CLUSTER_CONFIG_STORE_CRC_OFFSET,
                 record_crc32(record));
    return UCN_OK;
}

void ucn_cluster_config_store_init_empty(ucn_cluster_config_store_t *store)
{
    if (store != NULL) {
        (void)memset(store, 0, sizeof(*store));
    }
}

ucn_result_t ucn_cluster_config_store_write_stable(
    ucn_cluster_config_store_t *store,
    const ucn_cluster_config_state_t *stable_config,
    const ucn_cluster_persist_config_ref_t *expected_ref)
{
    config_store_slot_view_t views[UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT];
    config_store_slot_state_t states[UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT];
    uint8_t record[UCN_CLUSTER_CONFIG_STORE_RECORD_BYTES];
    uint32_t highest_generation = 0U;
    size_t index;
    size_t target = 0U;

    if (store == NULL || stable_config == NULL || expected_ref == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT; ++index) {
        states[index] = slot_decode(store->slots[index], &views[index]);
        if (states[index] == CONFIG_STORE_SLOT_VALID) {
            if (ref_equal(&views[index].reference, expected_ref) &&
                memcmp(&views[index].config, stable_config,
                       sizeof(*stable_config)) == 0) {
                return UCN_OK;
            }
            if (views[index].generation > highest_generation) {
                highest_generation = views[index].generation;
            }
        }
    }
    if (highest_generation == UINT32_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    if (states[0U] != CONFIG_STORE_SLOT_VALID) {
        target = 0U;
    } else if (states[1U] != CONFIG_STORE_SLOT_VALID) {
        target = 1U;
    } else if (views[0U].generation <= views[1U].generation) {
        target = 0U;
    } else {
        target = 1U;
    }
    if (record_encode(record, highest_generation + 1U, stable_config,
                      expected_ref) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memcpy(store->slots[target], record, sizeof(record));
    return UCN_OK;
}

static bool find_latest_ref(const config_store_slot_view_t *views,
                            const config_store_slot_state_t *states,
                            const ucn_cluster_persist_config_ref_t *reference,
                            config_store_slot_view_t *output)
{
    size_t index;
    bool found = false;

    for (index = 0U; index < UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT; ++index) {
        if (states[index] == CONFIG_STORE_SLOT_VALID &&
            ref_equal(&views[index].reference, reference) &&
            (!found || views[index].generation > output->generation)) {
            *output = views[index];
            found = true;
        }
    }
    return found;
}

ucn_result_t ucn_cluster_config_store_recover(
    const ucn_cluster_config_store_t *store,
    const ucn_cluster_persist_state_t *durable_state,
    ucn_cluster_config_store_recovery_t *output)
{
    config_store_slot_view_t views[UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT];
    config_store_slot_state_t states[UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT];
    config_store_slot_view_t active;
    config_store_slot_view_t staged;
    ucn_cluster_config_store_recovery_t candidate;
    size_t index;

    if (store == NULL || durable_state == NULL || output == NULL ||
        !ucn_cluster_persist_state_is_valid(durable_state) ||
        durable_state->record_schema_version !=
            UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3 ||
        !ref_is_valid(&durable_state->committed_config)) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT; ++index) {
        states[index] = slot_decode(store->slots[index], &views[index]);
    }
    (void)memset(&active, 0, sizeof(active));
    if (!find_latest_ref(views, states, &durable_state->committed_config,
                         &active)) {
        return UCN_ERR_STATE;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.active_config = active.config;
    candidate.active_generation = active.generation;
    if (durable_state->config_transaction.phase ==
        UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED) {
        (void)memset(&staged, 0, sizeof(staged));
        if (!find_latest_ref(views, states,
                             &durable_state->config_transaction.staging_config,
                             &staged)) {
            return UCN_ERR_STATE;
        }
        candidate.staged_config = staged.config;
        candidate.staged_generation = staged.generation;
        candidate.has_staged_config = true;
    }
    *output = candidate;
    return UCN_OK;
}
