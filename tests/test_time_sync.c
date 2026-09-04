#include "ucn/ucn_time_sync.h"

#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "assertion failed: %s:%d: %s\n",          \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (0)

/* EN: Returns one fixed bidirectional Path contract.
 * 中文：返回一组固定的双向 Path Contract。 */
static ucn_time_path_contract_t path_contract(bool asymmetry_known,
                                              bool dynamic_route)
{
    ucn_time_path_contract_t contract;

    (void)memset(&contract, 0, sizeof(contract));
    contract.forward_path.owner_node_id = 1U;
    contract.forward_path.owner_session_id = 100U;
    contract.forward_path.path_id = 11U;
    contract.forward_path.destination_node_id = 2U;
    contract.reverse_path.owner_node_id = 2U;
    contract.reverse_path.owner_session_id = 200U;
    contract.reverse_path.path_id = 22U;
    contract.reverse_path.destination_node_id = 1U;
    contract.max_asymmetry_us = 20U;
    contract.installed = !dynamic_route;
    contract.immutable_for_transaction = !dynamic_route;
    contract.ordinary_dynamic_route = dynamic_route;
    contract.asymmetry_bound_known = asymmetry_known;
    return contract;
}

static ucn_time_event_key_t event_key(uint8_t link_id,
                                      ucn_time_event_direction_t direction,
                                      uint32_t token)
{
    ucn_time_event_key_t key;

    (void)memset(&key, 0, sizeof(key));
    key.link_id = link_id;
    key.direction = direction;
    key.link_instance_generation = 1U;
    key.event_token = token;
    return key;
}

/* EN: Returns authenticated outer identity for one message direction.
 * 中文：返回一个消息方向的认证外层身份。 */
static ucn_time_control_outer_t outer_for(
    const ucn_wire_time_txn_key_t *key,
    bool forward)
{
    ucn_time_control_outer_t outer;

    (void)memset(&outer, 0, sizeof(outer));
    outer.source_node_id = forward ? key->master_node_id : key->member_node_id;
    outer.source_session_id = forward ? key->master_session_id :
                                        key->member_session_id;
    outer.destination_node_id = forward ? key->member_node_id :
                                          key->master_node_id;
    outer.path = forward ? key->forward_path : key->reverse_path;
    outer.e2e_authenticated = true;
    return outer;
}

/* EN: Passes one local outbound typed message through the strict Wire codec.
 * 中文：让一条本地出站语义消息经过严格 Wire Codec。 */
static bool wire_roundtrip(const ucn_time_control_message_t *outbound,
                           ucn_time_control_message_t *inbound,
                           size_t *wire_length)
{
    uint8_t wire[UCN_TIME_SYNC_MAX_PAYLOAD_BYTES];
    ucn_time_control_outer_t outer;
    size_t length = 0U;
    ucn_result_t status;
    bool forward;

    if (ucn_time_control_payload_encode(outbound, wire, sizeof(wire),
                                        &length) != UCN_OK) {
        return false;
    }
    forward = outbound->role != UCN_TIME_CONTROL_DELAY_REQ;
    outer = outer_for(&outbound->key, forward);
    if (outbound->role == UCN_TIME_CONTROL_SYNC) {
        status = ucn_time_control_sync_decode(&outer, wire, length, inbound);
    } else {
        status = ucn_time_control_existing_decode(outbound->role, &outer,
                                                  &outbound->key, wire,
                                                  length, inbound);
    }
    if (status != UCN_OK) {
        return false;
    }
    *wire_length = length;
    return true;
}

/* EN: Initializes matching Master and Member owners.
 * 中文：初始化相互匹配的 Master 与 Member Owner。 */
static bool init_pair(ucn_time_sync_master_t *master,
                      ucn_time_sync_member_t *member)
{
    ucn_time_sync_master_config_t master_config;
    ucn_time_sync_member_config_t member_config;

    (void)memset(&master_config, 0, sizeof(master_config));
    master_config.clock_domain_id = 10U;
    master_config.domain_generation = 7U;
    master_config.master_node_id = 1U;
    master_config.master_session_id = 100U;
    master_config.transaction_timeout_us = 1000U;
    (void)memset(&member_config, 0, sizeof(member_config));
    member_config.clock_domain_id = 10U;
    member_config.domain_generation = 7U;
    member_config.master_node_id = 1U;
    member_config.master_session_id = 100U;
    member_config.member_node_id = 2U;
    member_config.member_session_id = 200U;
    member_config.transaction_timeout_us = 1000U;
    member_config.uncertainty_components.timer_resolution_bound_us = 1U;
    member_config.uncertainty_components.link_timestamp_capture_bound_us = 2U;
    member_config.uncertainty_components.filter_residual_bound_us = 1U;
    member_config.uncertainty_components.arithmetic_rounding_bound_us = 1U;
    member_config.uncertainty_components.known_mask =
        UCN_REALTIME_UNCERTAINTY_SYNC_REQUIRED_MASK;
    return ucn_time_sync_master_init(master, &master_config) == UCN_OK &&
           ucn_time_sync_member_init(member, &member_config) == UCN_OK;
}

/* EN: Runs one complete four-message exchange and returns its sample.
 * 中文：运行一次完整四报文交换并返回样本。 */
static bool run_exchange(ucn_time_sync_master_t *master,
                         ucn_time_sync_member_t *member,
                         const ucn_time_path_contract_t *contract,
                         ucn_realtime_requirement_t requirement,
                         ucn_time_sync_sample_t *sample)
{
    ucn_time_event_key_t t1 = event_key(1U, UCN_TIME_EVENT_TX, 1U);
    ucn_time_event_key_t t2 = event_key(2U, UCN_TIME_EVENT_RX, 1U);
    ucn_time_event_key_t t3 = event_key(2U, UCN_TIME_EVENT_TX, 2U);
    ucn_time_event_key_t t4 = event_key(1U, UCN_TIME_EVENT_RX, 2U);
    ucn_time_control_message_t outbound;
    ucn_time_control_message_t inbound;
    ucn_wire_time_txn_key_t key;
    size_t wire_length;

    if (ucn_time_sync_master_begin(master, 2U, 200U, contract, requirement,
                                   10U, &t1, &outbound) != UCN_OK ||
        !wire_roundtrip(&outbound, &inbound, &wire_length) ||
        wire_length != 31U) {
        return false;
    }
    key = outbound.key;
    if (ucn_time_sync_member_receive_sync(member, &inbound, contract,
            requirement, 10U, &t2, UINT64_C(1001100)) != UCN_OK ||
        ucn_time_sync_master_record_t1(master, &key, &t1, 11U,
                                       UINT64_C(1000000)) != UCN_OK ||
        ucn_time_sync_master_build_follow_up(master, &key, 12U,
                                             &outbound) != UCN_OK ||
        !wire_roundtrip(&outbound, &inbound, &wire_length) ||
        wire_length != 19U ||
        ucn_time_sync_member_receive_follow_up(member, &inbound, 12U) !=
            UCN_OK ||
        ucn_time_sync_member_build_delay_req(member, &t3, 13U,
                                              &outbound) != UCN_OK ||
        !wire_roundtrip(&outbound, &inbound, &wire_length) ||
        wire_length != 11U ||
        ucn_time_sync_member_record_t3(member, &t3, 13U,
                                       UINT64_C(1001200)) != UCN_OK ||
        ucn_time_sync_master_receive_delay_req(master, &inbound, &t4, 14U,
                                               UINT64_C(1000300)) != UCN_OK ||
        ucn_time_sync_master_build_delay_resp(master, &key, 15U,
                                              &outbound) != UCN_OK ||
        !wire_roundtrip(&outbound, &inbound, &wire_length) ||
        wire_length != 19U ||
        ucn_time_sync_member_receive_delay_resp(member, &inbound, 15U,
                UINT64_C(1001100), sample) != UCN_OK ||
        ucn_time_sync_master_complete(master, &key, 15U) != UCN_OK) {
        return false;
    }
    return true;
}

/* EN: Verifies exact Wire bytes and authentication rejection.
 * 中文：验证精确 Wire 字节以及认证拒绝。 */
static bool test_wire_codec(void)
{
    static const uint8_t expected_sync[31] = {
        0x11U, 0x00U, 0x0AU, 0x00U, 0x00U, 0x00U, 0x07U,
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0xC8U,
        0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0xC8U,
        0x00U, 0x00U, 0x00U, 0x16U, 0x00U, 0x00U, 0x00U, 0x01U
    };
    ucn_time_sync_master_t master;
    ucn_time_sync_member_t member;
    ucn_time_path_contract_t contract = path_contract(true, false);
    ucn_time_event_key_t t1 = event_key(1U, UCN_TIME_EVENT_TX, 1U);
    ucn_time_control_message_t message;
    ucn_time_control_message_t decoded;
    ucn_time_control_message_t before;
    ucn_time_control_outer_t outer;
    uint8_t wire[UCN_TIME_SYNC_MAX_PAYLOAD_BYTES];
    size_t length = 0U;

    TEST_ASSERT(init_pair(&master, &member));
    TEST_ASSERT(ucn_time_sync_master_begin(&master, 2U, 200U, &contract,
            UCN_REALTIME_REQUIREMENT_REQUIRED, 0U, &t1, &message) == UCN_OK);
    TEST_ASSERT(ucn_time_control_payload_encode(&message, wire, sizeof(wire),
                                                &length) == UCN_OK);
    TEST_ASSERT(length == 31U && wire[0] == 0x11U && wire[1] == 0U &&
                wire[2] == 10U && wire[10] == 1U && wire[30] == 1U);
    TEST_ASSERT(memcmp(wire, expected_sync, sizeof(expected_sync)) == 0);
    outer = outer_for(&message.key, true);
    outer.e2e_authenticated = false;
    (void)memset(&decoded, 0xA5, sizeof(decoded));
    before = decoded;
    TEST_ASSERT(ucn_time_control_sync_decode(&outer, wire, length, &decoded) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(memcmp(&decoded, &before, sizeof(decoded)) == 0);
    wire[0] = 0x21U;
    outer.e2e_authenticated = true;
    TEST_ASSERT(ucn_time_control_sync_decode(&outer, wire, length, &decoded) ==
                UCN_ERR_VERSION);
    TEST_ASSERT(memcmp(&decoded, &before, sizeof(decoded)) == 0);
    TEST_ASSERT(ucn_time_control_existing_decode(
                    (ucn_time_control_role_t)9U, &outer, &message.key,
                    wire, 0U, &decoded) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(memcmp(&decoded, &before, sizeof(decoded)) == 0);
    (void)memset(wire, 0x5A, sizeof(wire));
    TEST_ASSERT(ucn_time_control_payload_encode(&message, wire, 30U,
                                                &length) == UCN_ERR_TOO_LARGE);
    {
        size_t index;
        for (index = 0U; index < sizeof(wire); ++index) {
            TEST_ASSERT(wire[index] == 0x5AU);
        }
    }
    return true;
}

/* EN: Verifies offset/delay calculation and Domain ingestion.
 * 中文：验证 offset/delay 计算和 Domain 样本接入。 */
static bool test_effective_exchange(void)
{
    ucn_time_sync_master_t master;
    ucn_time_sync_member_t member;
    ucn_time_path_contract_t contract = path_contract(true, false);
    ucn_time_sync_sample_t sample;
    ucn_time_domain_config_t config;
    ucn_time_domain_t domain;
    ucn_realtime_clock_view_t view;

    TEST_ASSERT(init_pair(&master, &member));
    TEST_ASSERT(run_exchange(&master, &member, &contract,
                             UCN_REALTIME_REQUIREMENT_REQUIRED, &sample));
    TEST_ASSERT(sample.kind == UCN_TIME_SAMPLE_VALID_SYNC &&
                sample.offset_us == -1000 &&
                sample.mean_path_delay_us == 100U &&
                sample.uncertainty_us == 25U);

    (void)memset(&config, 0, sizeof(config));
    config.clock_domain_id = 10U;
    config.master_node_id = 1U;
    config.master_session_id = 100U;
    config.domain_generation = 7U;
    config.lock_sample_count = 1U;
    config.sync_timeout_us = 1000U;
    config.max_holdover_us = 1000U;
    config.max_offset_jump_us = 2000U;
    config.max_slew_per_sample_us = 100U;
    config.max_rate_ppb = 100000U;
    config.oscillator_uncertainty_ppb = 10000U;
    config.oscillator_uncertainty_known = true;
    TEST_ASSERT(ucn_time_domain_init(&domain, &config) == UCN_OK);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_LOCKED);
    TEST_ASSERT(ucn_time_domain_get_clock_view(&domain, UINT64_C(1001200),
                                               &view) == UCN_OK);
    TEST_ASSERT(view.domain_time_us == UINT64_C(1000200));
    return true;
}

/* EN: Verifies diagnostic-only and dynamic Route admission separation.
 * 中文：验证仅诊断与动态 Route 准入分流。 */
static bool test_path_admission_separation(void)
{
    ucn_time_sync_master_t master;
    ucn_time_sync_master_t before;
    ucn_time_sync_member_t member;
    ucn_time_path_contract_t diagnostic = path_contract(false, false);
    ucn_time_path_contract_t dynamic = path_contract(false, true);
    ucn_time_sync_sample_t sample;
    ucn_time_domain_config_t domain_config;
    ucn_time_domain_t domain;
    ucn_time_event_key_t t1 = event_key(1U, UCN_TIME_EVENT_TX, 1U);
    ucn_time_control_message_t message;

    TEST_ASSERT(init_pair(&master, &member));
    TEST_ASSERT(run_exchange(&master, &member, &diagnostic,
                             UCN_REALTIME_REQUIREMENT_PREFERRED, &sample));
    TEST_ASSERT(sample.kind == UCN_TIME_SAMPLE_DIAGNOSTIC &&
                member.completed == 0U && member.diagnostic_completed == 1U);
    (void)memset(&domain_config, 0, sizeof(domain_config));
    domain_config.clock_domain_id = 10U;
    domain_config.master_node_id = 1U;
    domain_config.master_session_id = 100U;
    domain_config.domain_generation = 7U;
    domain_config.lock_sample_count = 1U;
    domain_config.sync_timeout_us = 1000U;
    domain_config.max_holdover_us = 1000U;
    domain_config.max_offset_jump_us = 2000U;
    domain_config.max_slew_per_sample_us = 100U;
    domain_config.max_rate_ppb = 100000U;
    domain_config.oscillator_uncertainty_ppb = 10000U;
    domain_config.oscillator_uncertainty_known = true;
    TEST_ASSERT(ucn_time_domain_init(&domain, &domain_config) == UCN_OK);
    TEST_ASSERT(ucn_time_domain_ingest_sample(&domain, &sample) == UCN_OK);
    TEST_ASSERT(domain.phase == UCN_TIME_DOMAIN_ACQUIRING &&
                domain.consecutive_valid_samples == 0U &&
                domain.stats.diagnostic_samples == 1U);

    TEST_ASSERT(init_pair(&master, &member));
    before = master;
    TEST_ASSERT(ucn_time_sync_master_begin(&master, 2U, 200U, &dynamic,
            UCN_REALTIME_REQUIREMENT_PREFERRED, 0U, &t1, &message) ==
                UCN_ERR_UNSUPPORTED);
    TEST_ASSERT(memcmp(&master, &before, sizeof(master)) == 0);
    TEST_ASSERT(ucn_time_sync_master_begin(&master, 2U, 200U, &diagnostic,
            UCN_REALTIME_REQUIREMENT_REQUIRED, 0U, &t1, &message) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(memcmp(&master, &before, sizeof(master)) == 0);
    return true;
}

/* EN: Verifies exact deadline expiry and late-event rejection.
 * 中文：验证精确 deadline 超时与迟到事件拒绝。 */
static bool test_timeout_and_replay(void)
{
    ucn_time_sync_master_t master;
    ucn_time_sync_member_t member;
    ucn_time_path_contract_t contract = path_contract(true, false);
    ucn_time_event_key_t t1 = event_key(1U, UCN_TIME_EVENT_TX, 1U);
    ucn_time_event_key_t t2 = event_key(2U, UCN_TIME_EVENT_RX, 1U);
    ucn_time_control_message_t outbound;
    ucn_time_control_message_t inbound;
    ucn_time_control_message_t late_follow;
    size_t wire_length;
    uint64_t old_deadline;

    TEST_ASSERT(init_pair(&master, &member));
    TEST_ASSERT(ucn_time_sync_master_begin(&master, 2U, 200U, &contract,
            UCN_REALTIME_REQUIREMENT_REQUIRED, 100U, &t1, &outbound) == UCN_OK);
    TEST_ASSERT(wire_roundtrip(&outbound, &inbound, &wire_length));
    TEST_ASSERT(ucn_time_sync_member_receive_sync(&member, &inbound, &contract,
            UCN_REALTIME_REQUIREMENT_REQUIRED, 100U, &t2, 200U) == UCN_OK);
    old_deadline = member.pending.deadline_us;
    TEST_ASSERT(ucn_time_sync_master_record_t1(&master, &outbound.key, &t1,
            101U, 1000U) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_build_follow_up(
                    &master, &outbound.key, 102U, &late_follow) == UCN_OK);
    TEST_ASSERT(wire_roundtrip(&late_follow, &late_follow, &wire_length));
    TEST_ASSERT(ucn_time_sync_member_receive_sync(&member, &inbound, &contract,
            UCN_REALTIME_REQUIREMENT_REQUIRED, 500U, &t2, 200U) == UCN_OK);
    TEST_ASSERT(member.pending.deadline_us == old_deadline);
    TEST_ASSERT(ucn_time_sync_member_step(&member, 1100U) == UCN_OK &&
                !member.pending.occupied && member.timed_out == 1U);
    TEST_ASSERT(ucn_time_sync_member_receive_follow_up(&member, &late_follow,
                                                       1100U) != UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_step(&master, 1100U) == UCN_OK &&
                master.timed_out == 1U);
    TEST_ASSERT(ucn_time_sync_master_record_t1(&master, &outbound.key, &t1,
            1100U, 1000U) == UCN_ERR_NOT_FOUND);
    return true;
}

/* EN: Verifies timeout, replacement and explicit abort transfer every
 * incomplete TX key back to the serialized Owner.
 * 中文：验证超时、替换和显式中止都会把未完成 TX key 交还串行 Owner。 */
static bool test_event_release_obligations(void)
{
    ucn_time_sync_master_t master;
    ucn_time_sync_member_t member;
    ucn_time_sync_member_t member_before;
    ucn_time_path_contract_t contract = path_contract(true, false);
    ucn_time_event_key_t t1 = event_key(1U, UCN_TIME_EVENT_TX, 1U);
    ucn_time_event_key_t t2 = event_key(2U, UCN_TIME_EVENT_RX, 1U);
    ucn_time_event_key_t t3 = event_key(2U, UCN_TIME_EVENT_TX, 2U);
    ucn_time_event_key_t t3_timeout = event_key(2U, UCN_TIME_EVENT_TX, 3U);
    ucn_time_event_key_t t3_abort = event_key(2U, UCN_TIME_EVENT_TX, 4U);
    ucn_time_event_key_t released;
    ucn_time_event_key_t wrong_release;
    ucn_time_event_key_t before;
    ucn_time_sync_master_t master_before_ack;
    ucn_time_sync_member_t member_before_ack;
    ucn_time_control_message_t sync;
    ucn_time_control_message_t inbound;
    ucn_time_control_message_t follow;
    ucn_time_control_message_t replacement;
    size_t wire_length;

    TEST_ASSERT(init_pair(&master, &member));
    TEST_ASSERT(ucn_time_sync_master_begin(&master, 2U, 200U, &contract,
            UCN_REALTIME_REQUIREMENT_REQUIRED, 100U, &t1, &sync) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_step(&master, 1100U) == UCN_OK);
    TEST_ASSERT(!master.pending[0].occupied &&
                master.released_event_count == 1U);
    TEST_ASSERT(ucn_time_sync_master_peek_released_event(&master,
                                                         &released) == UCN_OK);
    TEST_ASSERT(released.event_token == t1.event_token &&
                released.direction == UCN_TIME_EVENT_TX &&
                master.released_event_count == 1U);
    wrong_release = released;
    ++wrong_release.event_token;
    master_before_ack = master;
    TEST_ASSERT(ucn_time_sync_master_ack_released_event(
                    &master, &wrong_release) == UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&master, &master_before_ack, sizeof(master)) == 0);
    TEST_ASSERT(ucn_time_sync_master_ack_released_event(
                    &master, &released) == UCN_OK &&
                master.released_event_count == 0U);

    TEST_ASSERT(init_pair(&master, &member));
    TEST_ASSERT(ucn_time_sync_master_begin(&master, 2U, 200U, &contract,
            UCN_REALTIME_REQUIREMENT_REQUIRED, 100U, &t1, &sync) == UCN_OK);
    TEST_ASSERT(wire_roundtrip(&sync, &inbound, &wire_length));
    TEST_ASSERT(ucn_time_sync_member_receive_sync(&member, &inbound,
            &contract, UCN_REALTIME_REQUIREMENT_REQUIRED, 100U, &t2,
            1100U) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_record_t1(&master, &sync.key, &t1,
            101U, 1000U) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_build_follow_up(
                    &master, &sync.key, 102U, &follow) == UCN_OK);
    TEST_ASSERT(wire_roundtrip(&follow, &follow, &wire_length));
    TEST_ASSERT(ucn_time_sync_member_receive_follow_up(&member, &follow,
                                                       102U) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_member_build_delay_req(&member, &t3, 103U,
                                                     &follow) == UCN_OK);
    replacement = inbound;
    ++replacement.key.sync_sequence;
    TEST_ASSERT(ucn_time_sync_member_receive_sync(&member, &replacement,
            &contract, UCN_REALTIME_REQUIREMENT_REQUIRED, 104U, &t2,
            1200U) == UCN_OK);
    TEST_ASSERT(member.released_event_count == 1U);
    TEST_ASSERT(ucn_time_sync_member_peek_released_event(&member,
                                                         &released) == UCN_OK);
    TEST_ASSERT(released.event_token == t3.event_token &&
                released.direction == UCN_TIME_EVENT_TX &&
                member.released_event_count == 1U);
    wrong_release = released;
    ++wrong_release.event_token;
    member_before_ack = member;
    TEST_ASSERT(ucn_time_sync_member_ack_released_event(
                    &member, &wrong_release) == UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&member, &member_before_ack, sizeof(member)) == 0);
    TEST_ASSERT(ucn_time_sync_member_ack_released_event(
                    &member, &released) == UCN_OK &&
                member.released_event_count == 0U);
    (void)memset(&released, 0xA5, sizeof(released));
    before = released;
    TEST_ASSERT(ucn_time_sync_member_peek_released_event(&member,
                                                         &released) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(memcmp(&released, &before, sizeof(released)) == 0);

    (void)memset(&follow, 0, sizeof(follow));
    follow.role = UCN_TIME_CONTROL_FOLLOW_UP;
    follow.key = replacement.key;
    follow.timestamp_us = 1000U;
    follow.authenticated_outer = true;
    TEST_ASSERT(ucn_time_sync_member_receive_follow_up(&member, &follow,
                                                       105U) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_member_build_delay_req(
                    &member, &t3_timeout, 106U, &follow) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_member_step(&member, 1104U) == UCN_OK &&
                !member.pending.occupied);
    TEST_ASSERT(ucn_time_sync_member_peek_released_event(&member,
                                                         &released) == UCN_OK);
    TEST_ASSERT(released.event_token == t3_timeout.event_token);
    TEST_ASSERT(ucn_time_sync_member_ack_released_event(
                    &member, &released) == UCN_OK);

    ++replacement.key.sync_sequence;
    TEST_ASSERT(ucn_time_sync_member_receive_sync(&member, &replacement,
            &contract, UCN_REALTIME_REQUIREMENT_REQUIRED, 200U, &t2,
            1300U) == UCN_OK);
    (void)memset(&follow, 0, sizeof(follow));
    follow.role = UCN_TIME_CONTROL_FOLLOW_UP;
    follow.key = replacement.key;
    follow.timestamp_us = 1000U;
    follow.authenticated_outer = true;
    TEST_ASSERT(ucn_time_sync_member_receive_follow_up(&member, &follow,
                                                       201U) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_member_build_delay_req(
                    &member, &t3_abort, 202U, &follow) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_member_abort(&member, &replacement.key) ==
                UCN_OK);
    TEST_ASSERT(!member.pending.occupied);
    TEST_ASSERT(ucn_time_sync_member_peek_released_event(&member,
                                                         &released) == UCN_OK);
    TEST_ASSERT(released.event_token == t3_abort.event_token);
    TEST_ASSERT(ucn_time_sync_member_ack_released_event(
                    &member, &released) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_abort(&master, &sync.key) == UCN_OK);
    TEST_ASSERT(!master.pending[0].occupied);

    /* A replacement whose new deadline would overflow must not first move the
     * old T3 key into the release queue.  新 deadline 会溢出的替换不得先把旧
     * T3 key 移入 release 队列。 */
    TEST_ASSERT(init_pair(&master, &member));
    TEST_ASSERT(ucn_time_sync_master_begin(
                    &master, 2U, 200U, &contract,
                    UCN_REALTIME_REQUIREMENT_REQUIRED,
                    UINT64_MAX - UINT64_C(1000), &t1, &sync) == UCN_OK);
    TEST_ASSERT(wire_roundtrip(&sync, &inbound, &wire_length));
    TEST_ASSERT(ucn_time_sync_member_receive_sync(
                    &member, &inbound, &contract,
                    UCN_REALTIME_REQUIREMENT_REQUIRED,
                    UINT64_MAX - UINT64_C(1000), &t2, 1100U) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_record_t1(
                    &master, &sync.key, &t1,
                    UINT64_MAX - UINT64_C(999), 1000U) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_build_follow_up(
                    &master, &sync.key, UINT64_MAX - UINT64_C(998),
                    &follow) == UCN_OK);
    TEST_ASSERT(wire_roundtrip(&follow, &follow, &wire_length));
    TEST_ASSERT(ucn_time_sync_member_receive_follow_up(
                    &member, &follow, UINT64_MAX - UINT64_C(998)) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_member_build_delay_req(
                    &member, &t3, UINT64_MAX - UINT64_C(997),
                    &follow) == UCN_OK);
    replacement = inbound;
    ++replacement.key.sync_sequence;
    member_before = member;
    TEST_ASSERT(ucn_time_sync_member_receive_sync(
                    &member, &replacement, &contract,
                    UCN_REALTIME_REQUIREMENT_REQUIRED,
                    UINT64_MAX - UINT64_C(999), &t2, 1200U) ==
                UCN_ERR_EXHAUSTED);
    TEST_ASSERT(memcmp(&member, &member_before, sizeof(member)) == 0);
    return true;
}

/* EN: Rejects every unknown/zero synchronization uncertainty component.
 * 中文：拒绝任一未知或为零的同步误差分量。 */
static bool test_uncertainty_component_contract(void)
{
    ucn_time_sync_master_t master;
    ucn_time_sync_member_t member;
    ucn_time_sync_member_t output;
    ucn_time_sync_member_t before;
    ucn_time_sync_member_config_t config;

    TEST_ASSERT(init_pair(&master, &member));
    config = member.config;
    (void)memset(&output, 0xA5, sizeof(output));
    before = output;
    config.uncertainty_components.known_mask &=
        (uint8_t)~UCN_REALTIME_UNCERTAINTY_FILTER_RESIDUAL;
    TEST_ASSERT(ucn_time_sync_member_init(&output, &config) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(memcmp(&output, &before, sizeof(output)) == 0);
    config = member.config;
    config.uncertainty_components.arithmetic_rounding_bound_us = 0U;
    TEST_ASSERT(ucn_time_sync_member_init(&output, &config) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(memcmp(&output, &before, sizeof(output)) == 0);
    return true;
}

/* EN: Runs all RT-05 focused tests.
 * 中文：运行全部 RT-05 定向测试。 */
int main(void)
{
    if (!test_wire_codec() || !test_effective_exchange() ||
        !test_path_admission_separation() || !test_timeout_and_replay() ||
        !test_event_release_obligations() ||
        !test_uncertainty_component_contract()) {
        return 1;
    }
    (void)puts("ucn time sync tests passed");
    return 0;
}
