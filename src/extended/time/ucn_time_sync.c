/* Optional four-message Time Sync v1 codec and bounded transaction owners.
 * 可选的四报文 Time Sync v1 Codec 与有界事务 Owner。 */

#include "ucn/ucn_time_sync.h"

#include <limits.h>
#include <string.h>

#define CONTROL_VERSION_SHIFT ((uint8_t)4U)
#define CONTROL_ROLE_MASK ((uint8_t)0x0FU)
#define CONTROL_BASE_BYTES ((size_t)11U)
#define CONTROL_SYNC_BYTES ((size_t)31U)
#define CONTROL_FOLLOW_UP_BYTES ((size_t)19U)
#define CONTROL_DELAY_REQ_BYTES ((size_t)11U)
#define CONTROL_DELAY_RESP_BYTES ((size_t)19U)

/* EN: Reads one big-endian 16-bit word.
 * 中文：读取一个大端 16 位字。 */
static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

/* EN: Reads one big-endian 32-bit word.
 * 中文：读取一个大端 32 位字。 */
static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | input[3];
}

/* EN: Reads one big-endian 64-bit word.
 * 中文：读取一个大端 64 位字。 */
static uint64_t read_u64_be(const uint8_t *input)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

/* EN: Writes one big-endian 16-bit word.
 * 中文：写入一个大端 16 位字。 */
static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

/* EN: Writes one big-endian 32-bit word.
 * 中文：写入一个大端 32 位字。 */
static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

/* EN: Writes one big-endian 64-bit word.
 * 中文：写入一个大端 64 位字。 */
static void write_u64_be(uint8_t *output, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        output[7U - index] = (uint8_t)value;
        value >>= 8U;
    }
}

/* EN: Saturating-increments a transaction statistic.
 * 中文：对事务统计执行饱和递增。 */
static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
}

/* EN: Checks a Node ID for unicast identity use.
 * 中文：检查 Node ID 是否可用作单播身份。 */
static bool node_id_is_unicast(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

/* EN: Validates a complete installed Path identity.
 * 中文：校验一个完整的已安装 Path 身份。 */
bool ucn_time_path_identity_is_valid(const ucn_time_path_identity_t *identity)
{
    return identity != NULL &&
           node_id_is_unicast(identity->owner_node_id) &&
           identity->owner_session_id != 0U && identity->path_id != 0U &&
           node_id_is_unicast(identity->destination_node_id) &&
           identity->owner_node_id != identity->destination_node_id;
}

/* EN: Compares complete Path identities field by field.
 * 中文：逐字段比较完整 Path 身份。 */
static bool path_identity_equal(const ucn_time_path_identity_t *left,
                                const ucn_time_path_identity_t *right)
{
    return left->owner_node_id == right->owner_node_id &&
           left->owner_session_id == right->owner_session_id &&
           left->path_id == right->path_id &&
           left->destination_node_id == right->destination_node_id;
}

/* EN: Validates the complete cross-node transaction identity.
 * 中文：校验完整的跨节点时间事务身份。 */
bool ucn_wire_time_txn_key_is_valid(const ucn_wire_time_txn_key_t *key)
{
    return key != NULL && key->clock_domain_id != 0U &&
           key->clock_domain_id <= UCN_REALTIME_CLOCK_DOMAIN_ID_MAX &&
           key->domain_generation != 0U &&
           key->domain_generation <= UCN_REALTIME_DOMAIN_GENERATION_MAX &&
           key->sync_sequence != 0U &&
           key->sync_sequence <= UCN_TIME_SYNC_SEQUENCE_MAX &&
           node_id_is_unicast(key->master_node_id) &&
           key->master_session_id != 0U &&
           node_id_is_unicast(key->member_node_id) &&
           key->member_session_id != 0U &&
           key->master_node_id != key->member_node_id &&
           ucn_time_path_identity_is_valid(&key->forward_path) &&
           ucn_time_path_identity_is_valid(&key->reverse_path) &&
           key->forward_path.owner_node_id == key->master_node_id &&
           key->forward_path.owner_session_id == key->master_session_id &&
           key->forward_path.destination_node_id == key->member_node_id &&
           key->reverse_path.owner_node_id == key->member_node_id &&
           key->reverse_path.owner_session_id == key->member_session_id &&
           key->reverse_path.destination_node_id == key->master_node_id;
}

/* EN: Compares complete transaction keys without struct padding.
 * 中文：不依赖结构体填充地比较完整事务键。 */
bool ucn_wire_time_txn_key_equal(const ucn_wire_time_txn_key_t *left,
                                 const ucn_wire_time_txn_key_t *right)
{
    return ucn_wire_time_txn_key_is_valid(left) &&
           ucn_wire_time_txn_key_is_valid(right) &&
           left->clock_domain_id == right->clock_domain_id &&
           left->domain_generation == right->domain_generation &&
           left->sync_sequence == right->sync_sequence &&
           left->master_node_id == right->master_node_id &&
           left->master_session_id == right->master_session_id &&
           left->member_node_id == right->member_node_id &&
           left->member_session_id == right->member_session_id &&
           path_identity_equal(&left->forward_path, &right->forward_path) &&
           path_identity_equal(&left->reverse_path, &right->reverse_path);
}

/* EN: Applies the fixed-Path/dynamic-Route/asymmetry admission contract.
 * 中文：应用固定 Path、动态 Route 与非对称上界准入合同。 */
ucn_result_t ucn_time_sync_path_admit(
    const ucn_time_path_contract_t *contract,
    ucn_realtime_requirement_t requirement,
    ucn_time_path_admission_t *admission)
{
    ucn_time_path_admission_t result;

    if (contract == NULL || admission == NULL ||
        (requirement != UCN_REALTIME_REQUIREMENT_PREFERRED &&
         requirement != UCN_REALTIME_REQUIREMENT_REQUIRED) ||
        !ucn_time_path_identity_is_valid(&contract->forward_path) ||
        !ucn_time_path_identity_is_valid(&contract->reverse_path)) {
        return UCN_ERR_ARGUMENT;
    }
    if (contract->ordinary_dynamic_route) {
        result = requirement == UCN_REALTIME_REQUIREMENT_PREFERRED ?
            UCN_TIME_PATH_FALLBACK_ONLY : UCN_TIME_PATH_REJECTED;
    } else if (!contract->installed ||
               !contract->immutable_for_transaction) {
        result = UCN_TIME_PATH_REJECTED;
    } else if (!contract->asymmetry_bound_known) {
        result = requirement == UCN_REALTIME_REQUIREMENT_PREFERRED ?
            UCN_TIME_PATH_DIAGNOSTIC_ONLY : UCN_TIME_PATH_REJECTED;
    } else {
        result = UCN_TIME_PATH_EFFECTIVE_SAMPLE;
    }
    *admission = result;
    return UCN_OK;
}

/* EN: Returns the exact payload length for one control role.
 * 中文：返回一个控制角色的精确 Payload 长度。 */
static size_t control_role_length(ucn_time_control_role_t role)
{
    switch (role) {
    case UCN_TIME_CONTROL_SYNC:
        return CONTROL_SYNC_BYTES;
    case UCN_TIME_CONTROL_FOLLOW_UP:
        return CONTROL_FOLLOW_UP_BYTES;
    case UCN_TIME_CONTROL_DELAY_REQ:
        return CONTROL_DELAY_REQ_BYTES;
    case UCN_TIME_CONTROL_DELAY_RESP:
        return CONTROL_DELAY_RESP_BYTES;
    default:
        return 0U;
    }
}

/* EN: Writes the common version/Domain/generation/sequence prefix.
 * 中文：写入公共版本、Domain、generation 与 sequence 前缀。 */
static void control_write_base(uint8_t *output,
                               const ucn_time_control_message_t *message)
{
    output[0] = (uint8_t)((UCN_TIME_SYNC_CONTROL_VERSION <<
                           CONTROL_VERSION_SHIFT) | message->role);
    write_u16_be(&output[1], message->key.clock_domain_id);
    write_u32_be(&output[3], message->key.domain_generation);
    write_u32_be(&output[7], message->key.sync_sequence);
}

/* EN: Encodes a strict role-specific Time Sync payload.
 * 中文：编码严格的角色专用 Time Sync Payload。 */
ucn_result_t ucn_time_control_payload_encode(
    const ucn_time_control_message_t *message,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint8_t encoded[UCN_TIME_SYNC_MAX_PAYLOAD_BYTES];
    size_t length;

    if (message == NULL || output == NULL || output_length == NULL ||
        !ucn_wire_time_txn_key_is_valid(&message->key)) {
        return UCN_ERR_ARGUMENT;
    }
    length = control_role_length(message->role);
    if (length == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (output_capacity < length) {
        return UCN_ERR_TOO_LARGE;
    }
    (void)memset(encoded, 0, sizeof(encoded));
    control_write_base(encoded, message);
    if (message->role == UCN_TIME_CONTROL_SYNC) {
        if (message->timestamp_us != 0U) {
            return UCN_ERR_ARGUMENT;
        }
        write_u32_be(&encoded[11], message->key.member_session_id);
        write_u32_be(&encoded[15],
                     message->key.reverse_path.owner_node_id);
        write_u32_be(&encoded[19],
                     message->key.reverse_path.owner_session_id);
        write_u32_be(&encoded[23], message->key.reverse_path.path_id);
        write_u32_be(&encoded[27],
                     message->key.reverse_path.destination_node_id);
    } else if (message->role == UCN_TIME_CONTROL_FOLLOW_UP ||
               message->role == UCN_TIME_CONTROL_DELAY_RESP) {
        write_u64_be(&encoded[11], message->timestamp_us);
    } else if (message->timestamp_us != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memcpy(output, encoded, length);
    *output_length = length;
    return UCN_OK;
}

/* EN: Strictly validates and parses the common control prefix.
 * 中文：严格校验并解析公共控制前缀。 */
static ucn_result_t control_read_base(ucn_time_control_role_t expected_role,
                                      const uint8_t *payload,
                                      size_t payload_length,
                                      uint16_t *domain_id,
                                      uint32_t *generation,
                                      uint32_t *sequence)
{
    const size_t expected_length = control_role_length(expected_role);

    if (payload == NULL || expected_length == 0U ||
        payload_length != expected_length) {
        return UCN_ERR_MALFORMED;
    }
    if ((payload[0] >> CONTROL_VERSION_SHIFT) !=
            UCN_TIME_SYNC_CONTROL_VERSION ||
        (payload[0] & CONTROL_ROLE_MASK) != expected_role) {
        return UCN_ERR_VERSION;
    }
    *domain_id = read_u16_be(&payload[1]);
    *generation = read_u32_be(&payload[3]);
    *sequence = read_u32_be(&payload[7]);
    return UCN_OK;
}

/* EN: Validates an authenticated outer identity and Path.
 * 中文：校验认证的外层身份与 Path。 */
static bool outer_is_valid(const ucn_time_control_outer_t *outer)
{
    return outer != NULL && outer->e2e_authenticated &&
           node_id_is_unicast(outer->source_node_id) &&
           outer->source_session_id != 0U &&
           node_id_is_unicast(outer->destination_node_id) &&
           ucn_time_path_identity_is_valid(&outer->path) &&
           outer->path.owner_node_id == outer->source_node_id &&
           outer->path.owner_session_id == outer->source_session_id &&
           outer->path.destination_node_id == outer->destination_node_id;
}

/* EN: Decodes the only message allowed to establish a new WireTimeTxnKey.
 * 中文：解码唯一允许建立新 WireTimeTxnKey 的 SYNC 消息。 */
ucn_result_t ucn_time_control_sync_decode(
    const ucn_time_control_outer_t *outer,
    const uint8_t *payload,
    size_t payload_length,
    ucn_time_control_message_t *message)
{
    ucn_time_control_message_t decoded;
    ucn_result_t status;

    if (message == NULL || !outer_is_valid(outer)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.role = UCN_TIME_CONTROL_SYNC;
    status = control_read_base(decoded.role, payload, payload_length,
                               &decoded.key.clock_domain_id,
                               &decoded.key.domain_generation,
                               &decoded.key.sync_sequence);
    if (status != UCN_OK) {
        return status;
    }
    decoded.key.master_node_id = outer->source_node_id;
    decoded.key.master_session_id = outer->source_session_id;
    decoded.key.member_node_id = outer->destination_node_id;
    decoded.key.member_session_id = read_u32_be(&payload[11]);
    decoded.key.forward_path = outer->path;
    decoded.key.reverse_path.owner_node_id = read_u32_be(&payload[15]);
    decoded.key.reverse_path.owner_session_id = read_u32_be(&payload[19]);
    decoded.key.reverse_path.path_id = read_u32_be(&payload[23]);
    decoded.key.reverse_path.destination_node_id = read_u32_be(&payload[27]);
    if (!ucn_wire_time_txn_key_is_valid(&decoded.key)) {
        return UCN_ERR_MALFORMED;
    }
    decoded.authenticated_outer = true;
    *message = decoded;
    return UCN_OK;
}

/* EN: Checks that outer identity follows the frozen direction for a role.
 * 中文：检查外层身份是否符合控制角色的冻结方向。 */
static bool outer_matches_existing(
    ucn_time_control_role_t role,
    const ucn_time_control_outer_t *outer,
    const ucn_wire_time_txn_key_t *key)
{
    const bool forward = role == UCN_TIME_CONTROL_FOLLOW_UP ||
                         role == UCN_TIME_CONTROL_DELAY_RESP;
    const ucn_time_path_identity_t *path =
        forward ? &key->forward_path : &key->reverse_path;
    const ucn_node_id_t source =
        forward ? key->master_node_id : key->member_node_id;
    const ucn_session_id_t session =
        forward ? key->master_session_id : key->member_session_id;
    const ucn_node_id_t destination =
        forward ? key->member_node_id : key->master_node_id;

    return outer_is_valid(outer) && outer->source_node_id == source &&
           outer->source_session_id == session &&
           outer->destination_node_id == destination &&
           path_identity_equal(&outer->path, path);
}

/* EN: Decodes a message that must match an existing pending transaction.
 * 中文：解码必须匹配既有 pending 事务的消息。 */
ucn_result_t ucn_time_control_existing_decode(
    ucn_time_control_role_t expected_role,
    const ucn_time_control_outer_t *outer,
    const ucn_wire_time_txn_key_t *expected_key,
    const uint8_t *payload,
    size_t payload_length,
    ucn_time_control_message_t *message)
{
    ucn_time_control_message_t decoded;
    uint16_t domain_id;
    uint32_t generation;
    uint32_t sequence;
    ucn_result_t status;

    if (message == NULL || !ucn_wire_time_txn_key_is_valid(expected_key) ||
        (expected_role != UCN_TIME_CONTROL_FOLLOW_UP &&
         expected_role != UCN_TIME_CONTROL_DELAY_REQ &&
         expected_role != UCN_TIME_CONTROL_DELAY_RESP) ||
        !outer_matches_existing(expected_role, outer, expected_key)) {
        return UCN_ERR_ARGUMENT;
    }
    status = control_read_base(expected_role, payload, payload_length,
                               &domain_id, &generation, &sequence);
    if (status != UCN_OK) {
        return status;
    }
    if (domain_id != expected_key->clock_domain_id ||
        generation != expected_key->domain_generation ||
        sequence != expected_key->sync_sequence) {
        return UCN_ERR_REPLAY;
    }
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.role = expected_role;
    decoded.key = *expected_key;
    decoded.authenticated_outer = true;
    if (expected_role == UCN_TIME_CONTROL_FOLLOW_UP ||
        expected_role == UCN_TIME_CONTROL_DELAY_RESP) {
        decoded.timestamp_us = read_u64_be(&payload[11]);
    }
    *message = decoded;
    return UCN_OK;
}

/* EN: Validates shared Master/Member timeout configuration.
 * 中文：校验 Master/Member 共享的事务超时配置。 */
static bool timeout_is_valid(uint64_t timeout_us)
{
    return timeout_us != 0U && timeout_us <= (uint64_t)INT64_MAX;
}

/* EN: Initializes a fixed-capacity Master transaction owner.
 * 中文：初始化固定容量的 Master 事务 Owner。 */
ucn_result_t ucn_time_sync_master_init(
    ucn_time_sync_master_t *master,
    const ucn_time_sync_master_config_t *config)
{
    ucn_time_sync_master_t initialized;

    if (master == NULL || config == NULL || config->clock_domain_id == 0U ||
        config->clock_domain_id > UCN_REALTIME_CLOCK_DOMAIN_ID_MAX ||
        config->domain_generation == 0U ||
        config->domain_generation > UCN_REALTIME_DOMAIN_GENERATION_MAX ||
        !node_id_is_unicast(config->master_node_id) ||
        config->master_session_id == 0U ||
        !timeout_is_valid(config->transaction_timeout_us)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&initialized, 0, sizeof(initialized));
    initialized.config = *config;
    initialized.initialized = true;
    *master = initialized;
    return UCN_OK;
}

/* EN: Initializes one Member with one bounded pending slot.
 * 中文：初始化一个只拥有单一有界 pending 槽的 Member。 */
ucn_result_t ucn_time_sync_member_init(
    ucn_time_sync_member_t *member,
    const ucn_time_sync_member_config_t *config)
{
    ucn_time_sync_member_t initialized;
    uint32_t minimum_aggregate;

    if (member == NULL || config == NULL || config->clock_domain_id == 0U ||
        config->clock_domain_id > UCN_REALTIME_CLOCK_DOMAIN_ID_MAX ||
        config->domain_generation == 0U ||
        config->domain_generation > UCN_REALTIME_DOMAIN_GENERATION_MAX ||
        !node_id_is_unicast(config->master_node_id) ||
        config->master_session_id == 0U ||
        !node_id_is_unicast(config->member_node_id) ||
        config->member_session_id == 0U ||
        config->master_node_id == config->member_node_id ||
        !timeout_is_valid(config->transaction_timeout_us) ||
        ucn_realtime_uncertainty_aggregate(
            &config->uncertainty_components, true, 1U,
            &minimum_aggregate) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&initialized, 0, sizeof(initialized));
    initialized.config = *config;
    initialized.initialized = true;
    *member = initialized;
    return UCN_OK;
}

/* EN: Finds a Master pending slot by exact key.
 * 中文：按精确 key 查找 Master pending 槽。 */
static ucn_time_sync_master_pending_t *master_find(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key)
{
    size_t index;

    for (index = 0U; index < UCN_TIME_SYNC_MAX_PEERS; ++index) {
        if (master->pending[index].occupied &&
            ucn_wire_time_txn_key_equal(&master->pending[index].key, key)) {
            return &master->pending[index];
        }
    }
    return NULL;
}

/* EN: Checks a local event key's direction and bounded identity.
 * 中文：检查本地事件键的方向和有界身份。 */
static bool local_event_key_is_valid(const ucn_time_event_key_t *key,
                                     ucn_time_event_direction_t direction)
{
    return key != NULL && key->link_id != 0U && key->direction == direction &&
           key->link_instance_generation != 0U &&
           key->link_instance_generation <= UCN_TIME_EVENT_SERIAL_MAX &&
           key->event_token != 0U &&
           key->event_token <= UCN_TIME_EVENT_SERIAL_MAX;
}

/* EN: Compares local event keys without padding.
 * 中文：不依赖填充地比较本地事件键。 */
static bool local_event_key_equal(const ucn_time_event_key_t *left,
                                  const ucn_time_event_key_t *right)
{
    return left->link_id == right->link_id &&
           left->direction == right->direction &&
           left->link_instance_generation == right->link_instance_generation &&
           left->event_token == right->event_token;
}

/* EN: Returns whether a pending key owns a real local timestamp event.
 * 中文：判断 pending key 是否持有真实本地时间戳事件。 */
static bool local_event_key_present(const ucn_time_event_key_t *key)
{
    return key->event_token != 0U;
}

/* EN: Queues one incomplete Master event for serialized Owner retirement.
 * 中文：把一个未完成 Master 事件排队交给串行 Owner 回收。 */
static ucn_result_t master_release_push(ucn_time_sync_master_t *master,
                                       const ucn_time_event_key_t *key)
{
    if (master->released_event_count >=
        UCN_TIME_SYNC_MASTER_RELEASE_CAPACITY) {
        return UCN_ERR_NO_SPACE;
    }
    master->released_events[master->released_event_count++] = *key;
    return UCN_OK;
}

/* EN: Queues one incomplete Member event for serialized Owner retirement.
 * 中文：把一个未完成 Member 事件排队交给串行 Owner 回收。 */
static ucn_result_t member_release_push(ucn_time_sync_member_t *member,
                                       const ucn_time_event_key_t *key)
{
    if (member->released_event_count >=
        UCN_TIME_SYNC_MEMBER_RELEASE_CAPACITY) {
        return UCN_ERR_NO_SPACE;
    }
    member->released_events[member->released_event_count++] = *key;
    return UCN_OK;
}

/* EN: Creates one new Master transaction after every admission check.
 * 中文：在全部准入通过后创建一个新的 Master 事务。 */
ucn_result_t ucn_time_sync_master_begin(
    ucn_time_sync_master_t *master,
    ucn_node_id_t member_node_id,
    ucn_session_id_t member_session_id,
    const ucn_time_path_contract_t *contract,
    ucn_realtime_requirement_t requirement,
    uint64_t now_us,
    const ucn_time_event_key_t *t1_key,
    ucn_time_control_message_t *sync_message)
{
    ucn_time_sync_master_pending_t pending;
    ucn_time_control_message_t message;
    ucn_time_path_admission_t admission;
    size_t free_index = UCN_TIME_SYNC_MAX_PEERS;
    size_t index;

    if (master == NULL || !master->initialized || sync_message == NULL ||
        !node_id_is_unicast(member_node_id) || member_session_id == 0U ||
        member_node_id == master->config.master_node_id ||
        !local_event_key_is_valid(t1_key, UCN_TIME_EVENT_TX) ||
        ucn_time_sync_path_admit(contract, requirement, &admission) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_master_step(master, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    if (master->released_event_count != 0U) {
        return UCN_ERR_NO_SPACE;
    }
    if (admission == UCN_TIME_PATH_REJECTED) {
        return UCN_ERR_ACCESS;
    }
    if (admission == UCN_TIME_PATH_FALLBACK_ONLY) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (contract->forward_path.owner_node_id !=
            master->config.master_node_id ||
        contract->forward_path.owner_session_id !=
            master->config.master_session_id ||
        contract->forward_path.destination_node_id != member_node_id ||
        contract->reverse_path.owner_node_id != member_node_id ||
        contract->reverse_path.owner_session_id != member_session_id ||
        contract->reverse_path.destination_node_id !=
            master->config.master_node_id) {
        return UCN_ERR_ARGUMENT;
    }
    if (UINT64_MAX - now_us < master->config.transaction_timeout_us) {
        return UCN_ERR_EXHAUSTED;
    }
    for (index = 0U; index < UCN_TIME_SYNC_MAX_PEERS; ++index) {
        if (master->pending[index].occupied &&
            master->pending[index].key.member_node_id == member_node_id) {
            return UCN_ERR_NO_SPACE;
        }
        if (!master->pending[index].occupied &&
            free_index == UCN_TIME_SYNC_MAX_PEERS) {
            free_index = index;
        }
    }
    if (free_index == UCN_TIME_SYNC_MAX_PEERS) {
        return UCN_ERR_NO_SPACE;
    }
    if (master->next_sequence == UCN_TIME_SYNC_SEQUENCE_MAX) {
        return UCN_ERR_EXHAUSTED;
    }

    (void)memset(&pending, 0, sizeof(pending));
    pending.occupied = true;
    pending.deadline_us = now_us + master->config.transaction_timeout_us;
    pending.t1_key = *t1_key;
    pending.key.clock_domain_id = master->config.clock_domain_id;
    pending.key.domain_generation = master->config.domain_generation;
    pending.key.sync_sequence = master->next_sequence + 1U;
    pending.key.master_node_id = master->config.master_node_id;
    pending.key.master_session_id = master->config.master_session_id;
    pending.key.member_node_id = member_node_id;
    pending.key.member_session_id = member_session_id;
    pending.key.forward_path = contract->forward_path;
    pending.key.reverse_path = contract->reverse_path;
    if (!ucn_wire_time_txn_key_is_valid(&pending.key)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&message, 0, sizeof(message));
    message.role = UCN_TIME_CONTROL_SYNC;
    message.key = pending.key;

    master->pending[free_index] = pending;
    master->next_sequence = pending.key.sync_sequence;
    increment_saturated(&master->started);
    *sync_message = message;
    return UCN_OK;
}

/* EN: Completes the Master-owned T1 event exactly once.
 * 中文：精确一次完成 Master 拥有的 T1 事件。 */
ucn_result_t ucn_time_sync_master_record_t1(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key,
    const ucn_time_event_key_t *event_key,
    uint64_t now_us,
    uint64_t t1_us)
{
    ucn_time_sync_master_pending_t *pending;

    if (master == NULL || !master->initialized ||
        !local_event_key_is_valid(event_key, UCN_TIME_EVENT_TX)) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_master_step(master, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    pending = master_find(master, key);
    if (pending == NULL || !local_event_key_equal(&pending->t1_key, event_key)) {
        return UCN_ERR_NOT_FOUND;
    }
    if (pending->t1_complete) {
        return pending->t1_us == t1_us ? UCN_OK : UCN_ERR_REPLAY;
    }
    pending->t1_us = t1_us;
    pending->t1_complete = true;
    return UCN_OK;
}

/* EN: Builds FOLLOW_UP only after the local T1 callback completed.
 * 中文：仅在本地 T1 回调完成后构造 FOLLOW_UP。 */
ucn_result_t ucn_time_sync_master_build_follow_up(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key,
    uint64_t now_us,
    ucn_time_control_message_t *message)
{
    ucn_time_sync_master_pending_t *pending;
    ucn_time_control_message_t built;

    if (master == NULL || message == NULL || !master->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_master_step(master, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    pending = master_find(master, key);
    if (pending == NULL || !pending->t1_complete) {
        return UCN_ERR_STATE;
    }
    (void)memset(&built, 0, sizeof(built));
    built.role = UCN_TIME_CONTROL_FOLLOW_UP;
    built.key = pending->key;
    built.timestamp_us = pending->t1_us;
    *message = built;
    return UCN_OK;
}

/* EN: Checks a Path contract is the same one carried by a Wire key.
 * 中文：检查 Path Contract 与 Wire key 携带的是同一组 Path。 */
static bool contract_matches_key(const ucn_time_path_contract_t *contract,
                                 const ucn_wire_time_txn_key_t *key)
{
    return path_identity_equal(&contract->forward_path, &key->forward_path) &&
           path_identity_equal(&contract->reverse_path, &key->reverse_path);
}

/* EN: Establishes or atomically replaces the Member's one pending slot.
 * 中文：建立或原子替换 Member 的唯一 pending 槽。 */
ucn_result_t ucn_time_sync_member_receive_sync(
    ucn_time_sync_member_t *member,
    const ucn_time_control_message_t *message,
    const ucn_time_path_contract_t *contract,
    ucn_realtime_requirement_t requirement,
    uint64_t now_us,
    const ucn_time_event_key_t *t2_key,
    uint64_t t2_us)
{
    ucn_time_sync_member_pending_t pending;
    ucn_time_path_admission_t admission;

    if (member == NULL || message == NULL || !member->initialized ||
        message->role != UCN_TIME_CONTROL_SYNC ||
        !message->authenticated_outer ||
        !ucn_wire_time_txn_key_is_valid(&message->key) ||
        !local_event_key_is_valid(t2_key, UCN_TIME_EVENT_RX) ||
        ucn_time_sync_path_admit(contract, requirement, &admission) != UCN_OK) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_member_step(member, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    if (admission == UCN_TIME_PATH_REJECTED) {
        return UCN_ERR_ACCESS;
    }
    if (admission == UCN_TIME_PATH_FALLBACK_ONLY) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (!contract_matches_key(contract, &message->key) ||
        message->key.clock_domain_id != member->config.clock_domain_id ||
        message->key.domain_generation != member->config.domain_generation ||
        message->key.master_node_id != member->config.master_node_id ||
        message->key.master_session_id != member->config.master_session_id ||
        message->key.member_node_id != member->config.member_node_id ||
        message->key.member_session_id != member->config.member_session_id) {
        return UCN_ERR_REPLAY;
    }
    if (message->key.sync_sequence <= member->last_consumed_sequence) {
        return UCN_ERR_REPLAY;
    }
    if (UINT64_MAX - now_us < member->config.transaction_timeout_us) {
        return UCN_ERR_EXHAUSTED;
    }
    if (member->pending.occupied) {
        if (ucn_wire_time_txn_key_equal(&member->pending.key,
                                        &message->key)) {
            return UCN_OK;
        }
        if (message->key.sync_sequence <=
            member->pending.key.sync_sequence) {
            return UCN_ERR_REPLAY;
        }
        if (local_event_key_present(&member->pending.t3_key) &&
            !member->pending.t3_complete &&
            member_release_push(member, &member->pending.t3_key) != UCN_OK) {
            return UCN_ERR_NO_SPACE;
        }
    }
    (void)memset(&pending, 0, sizeof(pending));
    pending.key = message->key;
    pending.t2_key = *t2_key;
    pending.t2_us = t2_us;
    pending.t2_complete = true;
    pending.deadline_us = now_us + member->config.transaction_timeout_us;
    pending.path_admission = admission;
    pending.max_asymmetry_us = contract->max_asymmetry_us;
    pending.occupied = true;
    if (member->pending.occupied) {
        member->last_consumed_sequence = member->pending.key.sync_sequence;
    }
    member->pending = pending;
    return UCN_OK;
}

/* EN: Accepts FOLLOW_UP only for the current exact pending key.
 * 中文：仅为当前精确 pending key 接受 FOLLOW_UP。 */
ucn_result_t ucn_time_sync_member_receive_follow_up(
    ucn_time_sync_member_t *member,
    const ucn_time_control_message_t *message,
    uint64_t now_us)
{
    if (member == NULL || message == NULL || !member->initialized ||
        message->role != UCN_TIME_CONTROL_FOLLOW_UP ||
        !message->authenticated_outer) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_member_step(member, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    if (!member->pending.occupied ||
        !ucn_wire_time_txn_key_equal(&member->pending.key, &message->key)) {
        return UCN_ERR_NOT_FOUND;
    }
    if (member->pending.t1_complete) {
        return member->pending.t1_us == message->timestamp_us ? UCN_OK :
                                                               UCN_ERR_REPLAY;
    }
    member->pending.t1_us = message->timestamp_us;
    member->pending.t1_complete = true;
    return UCN_OK;
}

/* EN: Reserves the Member-owned T3 key before building DELAY_REQ.
 * 中文：构造 DELAY_REQ 前保留 Member 拥有的 T3 key。 */
ucn_result_t ucn_time_sync_member_build_delay_req(
    ucn_time_sync_member_t *member,
    const ucn_time_event_key_t *t3_key,
    uint64_t now_us,
    ucn_time_control_message_t *message)
{
    ucn_time_control_message_t built;

    if (member == NULL || message == NULL || !member->initialized ||
        !local_event_key_is_valid(t3_key, UCN_TIME_EVENT_TX)) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_member_step(member, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    if (!member->pending.occupied || !member->pending.t1_complete ||
        !member->pending.t2_complete || member->pending.t3_key.event_token != 0U) {
        return UCN_ERR_STATE;
    }
    (void)memset(&built, 0, sizeof(built));
    built.role = UCN_TIME_CONTROL_DELAY_REQ;
    built.key = member->pending.key;
    member->pending.t3_key = *t3_key;
    *message = built;
    return UCN_OK;
}

/* EN: Completes the Member-owned T3 event exactly once.
 * 中文：精确一次完成 Member 拥有的 T3 事件。 */
ucn_result_t ucn_time_sync_member_record_t3(
    ucn_time_sync_member_t *member,
    const ucn_time_event_key_t *event_key,
    uint64_t now_us,
    uint64_t t3_us)
{
    if (member == NULL || !member->initialized ||
        !local_event_key_is_valid(event_key, UCN_TIME_EVENT_TX)) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_member_step(member, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    if (!member->pending.occupied ||
        !local_event_key_equal(&member->pending.t3_key, event_key)) {
        return UCN_ERR_NOT_FOUND;
    }
    if (member->pending.t3_complete) {
        return member->pending.t3_us == t3_us ? UCN_OK : UCN_ERR_REPLAY;
    }
    member->pending.t3_us = t3_us;
    member->pending.t3_complete = true;
    return UCN_OK;
}

/* EN: Captures Master-owned T4 for an exact DELAY_REQ.
 * 中文：为精确匹配的 DELAY_REQ 捕获 Master 拥有的 T4。 */
ucn_result_t ucn_time_sync_master_receive_delay_req(
    ucn_time_sync_master_t *master,
    const ucn_time_control_message_t *message,
    const ucn_time_event_key_t *t4_key,
    uint64_t now_us,
    uint64_t t4_us)
{
    ucn_time_sync_master_pending_t *pending;

    if (master == NULL || message == NULL || !master->initialized ||
        message->role != UCN_TIME_CONTROL_DELAY_REQ ||
        !message->authenticated_outer || message->timestamp_us != 0U ||
        !local_event_key_is_valid(t4_key, UCN_TIME_EVENT_RX)) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_master_step(master, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    pending = master_find(master, &message->key);
    if (pending == NULL || !pending->t1_complete) {
        return UCN_ERR_NOT_FOUND;
    }
    if (pending->t4_complete) {
        return local_event_key_equal(&pending->t4_key, t4_key) &&
                       pending->t4_us == t4_us ? UCN_OK : UCN_ERR_REPLAY;
    }
    pending->t4_key = *t4_key;
    pending->t4_us = t4_us;
    pending->t4_complete = true;
    return UCN_OK;
}

/* EN: Builds DELAY_RESP only after local T4 completion.
 * 中文：仅在本地 T4 完成后构造 DELAY_RESP。 */
ucn_result_t ucn_time_sync_master_build_delay_resp(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key,
    uint64_t now_us,
    ucn_time_control_message_t *message)
{
    ucn_time_sync_master_pending_t *pending;
    ucn_time_control_message_t built;

    if (master == NULL || message == NULL || !master->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_master_step(master, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    pending = master_find(master, key);
    if (pending == NULL || !pending->t4_complete) {
        return UCN_ERR_STATE;
    }
    (void)memset(&built, 0, sizeof(built));
    built.role = UCN_TIME_CONTROL_DELAY_RESP;
    built.key = pending->key;
    built.timestamp_us = pending->t4_us;
    *message = built;
    return UCN_OK;
}

/* EN: Converts two unsigned timestamps to a checked signed difference.
 * 中文：把两个无符号时间戳转换成经过检查的有符号差值。 */
static bool timestamp_difference(uint64_t left,
                                 uint64_t right,
                                 int64_t *difference)
{
    if (left >= right) {
        if (left - right > (uint64_t)INT64_MAX) {
            return false;
        }
        *difference = (int64_t)(left - right);
        return true;
    }
    if (right - left > (uint64_t)INT64_MAX) {
        return false;
    }
    *difference = -(int64_t)(right - left);
    return true;
}

/* EN: Completes the Member transaction and emits one effective/diagnostic sample.
 * 中文：完成 Member 事务并输出一条有效或诊断样本。 */
ucn_result_t ucn_time_sync_member_receive_delay_resp(
    ucn_time_sync_member_t *member,
    const ucn_time_control_message_t *message,
    uint64_t now_us,
    uint64_t local_sample_us,
    ucn_time_sync_sample_t *sample)
{
    ucn_time_sync_sample_t completed;
    int64_t forward_delta;
    int64_t reverse_delta;
    int64_t delay_sum;
    int64_t offset_numerator;
    uint32_t uncertainty = 0U;
    bool uncertainty_known = false;

    if (member == NULL || message == NULL || sample == NULL ||
        !member->initialized || message->role != UCN_TIME_CONTROL_DELAY_RESP ||
        !message->authenticated_outer) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_time_sync_member_step(member, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    if (!member->pending.occupied || !member->pending.t1_complete ||
        !member->pending.t2_complete || !member->pending.t3_complete ||
        !ucn_wire_time_txn_key_equal(&member->pending.key, &message->key)) {
        return UCN_ERR_STATE;
    }
    if (member->pending.t3_us < member->pending.t2_us ||
        local_sample_us < member->pending.t2_us ||
        local_sample_us > member->pending.t3_us) {
        return UCN_ERR_STATE;
    }
    if (!timestamp_difference(member->pending.t2_us,
                              member->pending.t1_us, &forward_delta) ||
        !timestamp_difference(message->timestamp_us,
                              member->pending.t3_us, &reverse_delta) ||
        (reverse_delta > 0 && forward_delta > INT64_MAX - reverse_delta) ||
        (reverse_delta < 0 && forward_delta < INT64_MIN - reverse_delta)) {
        return UCN_ERR_STATE;
    }
    delay_sum = forward_delta + reverse_delta;
    if (delay_sum < 0 ||
        (forward_delta > 0 && reverse_delta < INT64_MIN + forward_delta) ||
        (forward_delta < 0 && reverse_delta > INT64_MAX + forward_delta)) {
        return UCN_ERR_STATE;
    }
    offset_numerator = reverse_delta - forward_delta;
    (void)memset(&completed, 0, sizeof(completed));
    completed.kind = member->pending.path_admission ==
            UCN_TIME_PATH_EFFECTIVE_SAMPLE ? UCN_TIME_SAMPLE_VALID_SYNC :
                                             UCN_TIME_SAMPLE_DIAGNOSTIC;
    completed.clock_domain_id = member->config.clock_domain_id;
    completed.master_node_id = member->config.master_node_id;
    completed.master_session_id = member->config.master_session_id;
    completed.domain_generation = member->config.domain_generation;
    completed.local_sample_us = local_sample_us;
    completed.offset_us = offset_numerator / 2;
    completed.mean_path_delay_us = (uint64_t)(delay_sum / 2);
    if (completed.kind == UCN_TIME_SAMPLE_VALID_SYNC) {
        if (ucn_realtime_uncertainty_aggregate(
                &member->config.uncertainty_components, true,
                member->pending.max_asymmetry_us, &uncertainty) != UCN_OK) {
            return UCN_ERR_STATE;
        }
        uncertainty_known = true;
    }
    completed.uncertainty_us = uncertainty;
    completed.uncertainty_known = uncertainty_known;

    member->last_consumed_sequence = member->pending.key.sync_sequence;
    (void)memset(&member->pending, 0, sizeof(member->pending));
    if (completed.kind == UCN_TIME_SAMPLE_VALID_SYNC) {
        increment_saturated(&member->completed);
    } else {
        increment_saturated(&member->diagnostic_completed);
    }
    *sample = completed;
    return UCN_OK;
}

/* EN: Releases one completed Master slot after response submission.
 * 中文：在响应提交后释放一个已完成的 Master 槽。 */
ucn_result_t ucn_time_sync_master_complete(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key,
    uint64_t now_us)
{
    ucn_time_sync_master_pending_t *pending;

    if (master == NULL || !master->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (master->released_event_count >
        UCN_TIME_SYNC_MASTER_RELEASE_CAPACITY) {
        return UCN_ERR_STATE;
    }
    if (ucn_time_sync_master_step(master, now_us) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    pending = master_find(master, key);
    if (pending == NULL || !pending->t4_complete) {
        return UCN_ERR_STATE;
    }
    (void)memset(pending, 0, sizeof(*pending));
    increment_saturated(&master->completed);
    return UCN_OK;
}

/* EN: Expires Master transactions without evicting a live slot early.
 * 中文：使 Master 事务超时，且不会提前淘汰活动槽。 */
ucn_result_t ucn_time_sync_master_step(ucn_time_sync_master_t *master,
                                       uint64_t now_us)
{
    size_t index;
    size_t required;

    if (master == NULL || !master->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    required = master->released_event_count;
    for (index = 0U; index < UCN_TIME_SYNC_MAX_PEERS; ++index) {
        if (master->pending[index].occupied &&
            now_us >= master->pending[index].deadline_us &&
            local_event_key_present(&master->pending[index].t1_key) &&
            !master->pending[index].t1_complete) {
            ++required;
        }
    }
    if (required > UCN_TIME_SYNC_MASTER_RELEASE_CAPACITY) {
        return UCN_ERR_NO_SPACE;
    }
    for (index = 0U; index < UCN_TIME_SYNC_MAX_PEERS; ++index) {
        if (master->pending[index].occupied &&
            now_us >= master->pending[index].deadline_us) {
            if (local_event_key_present(&master->pending[index].t1_key) &&
                !master->pending[index].t1_complete) {
                (void)master_release_push(
                    master, &master->pending[index].t1_key);
            }
            (void)memset(&master->pending[index], 0,
                         sizeof(master->pending[index]));
            increment_saturated(&master->timed_out);
        }
    }
    return UCN_OK;
}

/* EN: Expires the Member slot and consumes its sequence against late events.
 * 中文：使 Member 槽超时并消费 sequence，以拒绝迟到事件。 */
ucn_result_t ucn_time_sync_member_step(ucn_time_sync_member_t *member,
                                       uint64_t now_us)
{
    if (member == NULL || !member->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (member->released_event_count >
        UCN_TIME_SYNC_MEMBER_RELEASE_CAPACITY) {
        return UCN_ERR_STATE;
    }
    if (member->pending.occupied && now_us >= member->pending.deadline_us) {
        if (local_event_key_present(&member->pending.t3_key) &&
            !member->pending.t3_complete &&
            member->released_event_count >=
                UCN_TIME_SYNC_MEMBER_RELEASE_CAPACITY) {
            return UCN_ERR_NO_SPACE;
        }
        if (local_event_key_present(&member->pending.t3_key) &&
            !member->pending.t3_complete) {
            (void)member_release_push(member, &member->pending.t3_key);
        }
        member->last_consumed_sequence = member->pending.key.sync_sequence;
        (void)memset(&member->pending, 0, sizeof(member->pending));
        increment_saturated(&member->timed_out);
    }
    return UCN_OK;
}

/* EN: Aborts one Master transaction and preserves incomplete event ownership.
 * 中文：中止一个 Master 事务并保留未完成事件的所有权交接。 */
ucn_result_t ucn_time_sync_master_abort(
    ucn_time_sync_master_t *master,
    const ucn_wire_time_txn_key_t *key)
{
    ucn_time_sync_master_pending_t *pending;

    if (master == NULL || !master->initialized ||
        !ucn_wire_time_txn_key_is_valid(key)) {
        return UCN_ERR_ARGUMENT;
    }
    if (master->released_event_count >
        UCN_TIME_SYNC_MASTER_RELEASE_CAPACITY) {
        return UCN_ERR_STATE;
    }
    pending = master_find(master, key);
    if (pending == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (local_event_key_present(&pending->t1_key) && !pending->t1_complete &&
        master_release_push(master, &pending->t1_key) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    (void)memset(pending, 0, sizeof(*pending));
    return UCN_OK;
}

/* EN: Aborts the Member transaction and preserves incomplete T3 ownership.
 * 中文：中止 Member 事务并保留未完成 T3 的所有权交接。 */
ucn_result_t ucn_time_sync_member_abort(
    ucn_time_sync_member_t *member,
    const ucn_wire_time_txn_key_t *key)
{
    if (member == NULL || !member->initialized ||
        !ucn_wire_time_txn_key_is_valid(key)) {
        return UCN_ERR_ARGUMENT;
    }
    if (member->released_event_count >
        UCN_TIME_SYNC_MEMBER_RELEASE_CAPACITY) {
        return UCN_ERR_STATE;
    }
    if (!member->pending.occupied ||
        !ucn_wire_time_txn_key_equal(&member->pending.key, key)) {
        return UCN_ERR_NOT_FOUND;
    }
    if (local_event_key_present(&member->pending.t3_key) &&
        !member->pending.t3_complete &&
        member_release_push(member, &member->pending.t3_key) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    member->last_consumed_sequence = member->pending.key.sync_sequence;
    (void)memset(&member->pending, 0, sizeof(member->pending));
    return UCN_OK;
}

/* EN: Peeks one Master release obligation without transferring ownership.
 * 中文：查看一个 Master 回收义务，但不转移其所有权。 */
ucn_result_t ucn_time_sync_master_peek_released_event(
    const ucn_time_sync_master_t *master,
    ucn_time_event_key_t *key)
{
    if (master == NULL || key == NULL || !master->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (master->released_event_count >
        UCN_TIME_SYNC_MASTER_RELEASE_CAPACITY) {
        return UCN_ERR_STATE;
    }
    if (master->released_event_count == 0U) {
        return UCN_ERR_NOT_FOUND;
    }
    *key = master->released_events[0];
    return UCN_OK;
}

/* EN: Acknowledges the exact head Master obligation after Driver retirement.
 * 中文：Driver 退休成功后确认队首的精确 Master 回收义务。 */
ucn_result_t ucn_time_sync_master_ack_released_event(
    ucn_time_sync_master_t *master,
    const ucn_time_event_key_t *key)
{
    size_t index;

    if (master == NULL || key == NULL || !master->initialized ||
        !local_event_key_present(key)) {
        return UCN_ERR_ARGUMENT;
    }
    if (master->released_event_count >
        UCN_TIME_SYNC_MASTER_RELEASE_CAPACITY) {
        return UCN_ERR_STATE;
    }
    if (master->released_event_count == 0U) {
        return UCN_ERR_NOT_FOUND;
    }
    if (!local_event_key_equal(&master->released_events[0], key)) {
        return UCN_ERR_STATE;
    }
    for (index = 1U; index < master->released_event_count; ++index) {
        master->released_events[index - 1U] =
            master->released_events[index];
    }
    --master->released_event_count;
    (void)memset(&master->released_events[master->released_event_count], 0,
                 sizeof(master->released_events[0]));
    return UCN_OK;
}

/* EN: Peeks one Member release obligation without transferring ownership.
 * 中文：查看一个 Member 回收义务，但不转移其所有权。 */
ucn_result_t ucn_time_sync_member_peek_released_event(
    const ucn_time_sync_member_t *member,
    ucn_time_event_key_t *key)
{
    if (member == NULL || key == NULL || !member->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    if (member->released_event_count >
        UCN_TIME_SYNC_MEMBER_RELEASE_CAPACITY) {
        return UCN_ERR_STATE;
    }
    if (member->released_event_count == 0U) {
        return UCN_ERR_NOT_FOUND;
    }
    *key = member->released_events[0];
    return UCN_OK;
}

/* EN: Acknowledges the exact Member obligation after Driver retirement.
 * 中文：Driver 退休成功后确认精确的 Member 回收义务。 */
ucn_result_t ucn_time_sync_member_ack_released_event(
    ucn_time_sync_member_t *member,
    const ucn_time_event_key_t *key)
{
    if (member == NULL || key == NULL || !member->initialized ||
        !local_event_key_present(key)) {
        return UCN_ERR_ARGUMENT;
    }
    if (member->released_event_count >
        UCN_TIME_SYNC_MEMBER_RELEASE_CAPACITY) {
        return UCN_ERR_STATE;
    }
    if (member->released_event_count == 0U) {
        return UCN_ERR_NOT_FOUND;
    }
    if (!local_event_key_equal(&member->released_events[0], key)) {
        return UCN_ERR_STATE;
    }
    member->released_event_count = 0U;
    (void)memset(&member->released_events[0], 0,
                 sizeof(member->released_events[0]));
    return UCN_OK;
}
