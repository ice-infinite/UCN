#include "ucn/ucn_cluster_config_state.h"

#include <string.h>

#define UCN_CLUSTER_CONFIG_HASH_OFFSET UINT32_C(2166136261)
#define UCN_CLUSTER_CONFIG_HASH_PRIME UINT32_C(16777619)

#if defined(_MSC_VER)
#define UCN_CLUSTER_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define UCN_CLUSTER_NOINLINE __attribute__((noinline))
#else
#define UCN_CLUSTER_NOINLINE
#endif

static bool config_serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

static uint32_t config_hash_byte(uint32_t hash, uint8_t value)
{
    return (hash ^ (uint32_t)value) * UCN_CLUSTER_CONFIG_HASH_PRIME;
}

static uint32_t config_hash_u32(uint32_t hash, uint32_t value)
{
    hash = config_hash_byte(hash, (uint8_t)(value >> 24U));
    hash = config_hash_byte(hash, (uint8_t)(value >> 16U));
    hash = config_hash_byte(hash, (uint8_t)(value >> 8U));
    return config_hash_byte(hash, (uint8_t)value);
}

static bool voter_sets_equal(const ucn_cluster_voter_set_t *left,
                             const ucn_cluster_voter_set_t *right)
{
    return left != NULL && right != NULL &&
           left->config_id == right->config_id && left->hash == right->hash &&
           left->count == right->count &&
           memcmp(left->node_ids, right->node_ids, sizeof(left->node_ids)) ==
               0;
}

bool ucn_cluster_config_phase_is_valid(ucn_cluster_config_phase_t phase)
{
    return phase == UCN_CLUSTER_CONFIG_PHASE_STABLE ||
           phase == UCN_CLUSTER_CONFIG_PHASE_JOINT;
}

UCN_CLUSTER_NOINLINE bool ucn_cluster_config_state_is_valid(
    const ucn_cluster_config_state_t *state)
{
    ucn_cluster_config_phase_t phase;

    if (state == NULL || !config_serial_is_valid(state->config_id) ||
        !ucn_cluster_voter_set_is_valid(&state->old_set) ||
        !ucn_cluster_voter_set_is_valid(&state->new_set) ||
        state->old_set_hash != state->old_set.hash ||
        state->new_set_hash != state->new_set.hash) {
        return false;
    }
    phase = (ucn_cluster_config_phase_t)state->phase;
    if (phase == UCN_CLUSTER_CONFIG_PHASE_STABLE) {
        return state->old_set.config_id == state->config_id &&
               state->new_set.config_id == state->config_id &&
               voter_sets_equal(&state->old_set, &state->new_set);
    }
    return phase == UCN_CLUSTER_CONFIG_PHASE_JOINT &&
           state->new_set.config_id == state->config_id &&
           state->old_set.config_id < state->config_id &&
           state->old_set.config_id + 1U == state->config_id;
}

bool ucn_cluster_config_state_rekey_required(
    const ucn_cluster_config_state_t *state)
{
    return ucn_cluster_config_state_is_valid(state) &&
           state->phase == (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE &&
           state->config_id >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD;
}

bool ucn_cluster_config_state_init_stable(
    ucn_cluster_config_state_t *output,
    uint32_t config_id,
    const ucn_node_id_t *voter_node_ids,
    size_t voter_count)
{
    ucn_cluster_config_state_t candidate;

    if (output == NULL || !config_serial_is_valid(config_id) ||
        voter_node_ids == NULL) {
        return false;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    if (!ucn_cluster_voter_set_build(&candidate.old_set, config_id,
                                     voter_node_ids, voter_count)) {
        return false;
    }
    candidate.new_set = candidate.old_set;
    candidate.config_id = config_id;
    candidate.phase = (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE;
    candidate.old_set_hash = candidate.old_set.hash;
    candidate.new_set_hash = candidate.new_set.hash;
    if (!ucn_cluster_config_state_is_valid(&candidate)) {
        return false;
    }
    *output = candidate;
    return true;
}

bool ucn_cluster_config_state_init_joint(
    ucn_cluster_config_state_t *output,
    const ucn_cluster_config_state_t *stable_old,
    const ucn_node_id_t *new_voter_node_ids,
    size_t new_voter_count)
{
    ucn_cluster_config_state_t candidate;
    uint32_t next_config_id;

    if (output == NULL || stable_old == NULL || new_voter_node_ids == NULL ||
        !ucn_cluster_config_state_is_valid(stable_old) ||
        stable_old->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE ||
        stable_old->config_id >= UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD) {
        return false;
    }
    next_config_id = stable_old->config_id + 1U;
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.old_set = stable_old->old_set;
    if (!ucn_cluster_voter_set_build(&candidate.new_set, next_config_id,
                                     new_voter_node_ids,
                                     new_voter_count)) {
        return false;
    }
    candidate.config_id = next_config_id;
    candidate.phase = (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT;
    candidate.old_set_hash = candidate.old_set.hash;
    candidate.new_set_hash = candidate.new_set.hash;
    if (!ucn_cluster_config_state_is_valid(&candidate)) {
        return false;
    }
    *output = candidate;
    return true;
}

bool ucn_cluster_config_state_promote_joint(
    ucn_cluster_config_state_t *output,
    const ucn_cluster_config_state_t *joint)
{
    ucn_cluster_config_state_t candidate;

    if (output == NULL || joint == NULL ||
        !ucn_cluster_config_state_is_valid(joint) ||
        joint->phase != (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT) {
        return false;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.config_id = joint->config_id;
    candidate.phase = (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE;
    candidate.old_set = joint->new_set;
    candidate.new_set = joint->new_set;
    candidate.old_set_hash = candidate.old_set.hash;
    candidate.new_set_hash = candidate.new_set.hash;
    if (!ucn_cluster_config_state_is_valid(&candidate)) {
        return false;
    }
    *output = candidate;
    return true;
}

uint32_t ucn_cluster_config_state_hash(
    const ucn_cluster_config_state_t *state)
{
    size_t index;
    uint32_t hash = UCN_CLUSTER_CONFIG_HASH_OFFSET;

    if (!ucn_cluster_config_state_is_valid(state)) {
        return 0U;
    }
    hash = config_hash_u32(hash, state->config_id);
    hash = config_hash_byte(hash, state->phase);
    hash = config_hash_u32(hash, state->old_set_hash);
    hash = config_hash_u32(hash, state->new_set_hash);
    hash = config_hash_u32(hash, state->old_set.config_id);
    hash = config_hash_byte(hash, state->old_set.count);
    for (index = 0U; index < UCN_CLUSTER_MAX_VOTERS; ++index) {
        hash = config_hash_u32(hash, state->old_set.node_ids[index]);
    }
    hash = config_hash_u32(hash, state->new_set.config_id);
    hash = config_hash_byte(hash, state->new_set.count);
    for (index = 0U; index < UCN_CLUSTER_MAX_VOTERS; ++index) {
        hash = config_hash_u32(hash, state->new_set.node_ids[index]);
    }
    return hash == 0U ? 1U : hash;
}

static UCN_CLUSTER_NOINLINE void write_u32_be(uint8_t *output,
                                              uint32_t value)
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

ucn_result_t ucn_cluster_config_state_serialize(
    const ucn_cluster_config_state_t *state,
    uint8_t *output,
    size_t output_capacity)
{
    uint8_t candidate[UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES];
    size_t index;
    size_t old_nodes_offset = 24U;
    size_t new_nodes_offset = old_nodes_offset +
                              ((size_t)4U * UCN_CLUSTER_MAX_VOTERS);

    if (state == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (output_capacity < sizeof(candidate)) {
        return UCN_ERR_NO_SPACE;
    }
    if (!ucn_cluster_config_state_is_valid(state)) {
        return UCN_ERR_CONFIG;
    }
    (void)memset(candidate, 0, sizeof(candidate));
    write_u32_be(candidate + 0U, state->config_id);
    candidate[4U] = state->phase;
    candidate[5U] = state->old_set.count;
    candidate[6U] = state->new_set.count;
    write_u32_be(candidate + 8U, state->old_set.config_id);
    write_u32_be(candidate + 12U, state->new_set.config_id);
    write_u32_be(candidate + 16U, state->old_set_hash);
    write_u32_be(candidate + 20U, state->new_set_hash);
    for (index = 0U; index < UCN_CLUSTER_MAX_VOTERS; ++index) {
        write_u32_be(candidate + old_nodes_offset + (index * 4U),
                     state->old_set.node_ids[index]);
        write_u32_be(candidate + new_nodes_offset + (index * 4U),
                     state->new_set.node_ids[index]);
    }
    (void)memcpy(output, candidate, sizeof(candidate));
    return UCN_OK;
}

ucn_result_t ucn_cluster_config_state_deserialize(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_config_state_t *output)
{
    ucn_cluster_config_state_t candidate;
    uint8_t canonical[UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES];
    size_t index;
    size_t old_nodes_offset = 24U;
    size_t new_nodes_offset = old_nodes_offset +
                              ((size_t)4U * UCN_CLUSTER_MAX_VOTERS);

    if (input == NULL || output == NULL ||
        input_length != UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES ||
        input[7U] != 0U || input[5U] > UCN_CLUSTER_MAX_VOTERS ||
        input[6U] > UCN_CLUSTER_MAX_VOTERS) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.config_id = read_u32_be(input + 0U);
    candidate.phase = input[4U];
    candidate.old_set_hash = read_u32_be(input + 16U);
    candidate.new_set_hash = read_u32_be(input + 20U);
    for (index = 0U; index < UCN_CLUSTER_MAX_VOTERS; ++index) {
        candidate.old_set.node_ids[index] =
            read_u32_be(input + old_nodes_offset + (index * 4U));
        candidate.new_set.node_ids[index] =
            read_u32_be(input + new_nodes_offset + (index * 4U));
    }
    if (!ucn_cluster_voter_set_build(&candidate.old_set,
                                     read_u32_be(input + 8U),
                                     candidate.old_set.node_ids, input[5U]) ||
        !ucn_cluster_voter_set_build(&candidate.new_set,
                                     read_u32_be(input + 12U),
                                     candidate.new_set.node_ids, input[6U]) ||
        !ucn_cluster_config_state_is_valid(&candidate) ||
        ucn_cluster_config_state_serialize(&candidate, canonical,
                                           sizeof(canonical)) != UCN_OK ||
        memcmp(input, canonical, sizeof(canonical)) != 0) {
        return UCN_ERR_CONFIG;
    }
    *output = candidate;
    return UCN_OK;
}
