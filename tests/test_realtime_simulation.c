#include "ucn/ucn_time_authority.h"
#include "ucn/ucn_time_capability.h"
#include "ucn/ucn_timed_link.h"

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

#define SIM_DOMAIN_COUNT ((size_t)2U)
#define SIM_MEMBERS_PER_DOMAIN ((size_t)4U)

#define EXCHANGE_REQUIRE(expression, stage)                                   \
    do {                                                                      \
        if (!(expression)) {                                                  \
            (void)fprintf(stderr, "exchange stage failed: %s\n", stage);    \
            return false;                                                     \
        }                                                                     \
    } while (0)

typedef struct sim_witness_store {
    ucn_time_generation_witness_t record;
    bool present;
} sim_witness_store_t;

typedef struct sim_state_store {
    ucn_time_authority_state_record_t record;
    bool present;
} sim_state_store_t;

typedef struct sim_timed_driver {
    ucn_timed_link_t *timed_link;
    ucn_time_tx_event_queue_t tx_events;
    ucn_time_timed_rx_queue_t rx_items;
    ucn_link_t ingress_link;
    uint8_t submitted[UCN_MAX_FRAME_BYTES];
    size_t submitted_length;
    uint64_t next_tx_timestamp_us;
    uint32_t reserve_calls;
    uint32_t submit_calls;
    uint32_t cancel_calls;
    uint32_t cancel_failures_remaining;
    uint32_t quiesce_calls;
} sim_timed_driver_t;

typedef struct sim_member {
    sim_timed_driver_t timed_driver;
    ucn_timed_link_t timed_link;
    ucn_time_sync_member_t sync;
    ucn_time_domain_t domain;
    ucn_time_capability_cache_t capabilities;
    ucn_node_id_t node_id;
    ucn_session_id_t session_id;
    int64_t initial_offset_us;
    int32_t drift_ppb;
    uint64_t epoch_true_us;
    uint64_t last_true_sample_us;
    uint32_t delivered_samples;
} sim_member_t;

typedef struct sim_domain {
    sim_witness_store_t witness_store;
    sim_state_store_t state_store;
    ucn_time_authority_t authority;
    sim_timed_driver_t timed_driver;
    ucn_timed_link_t timed_link;
    ucn_time_sync_master_t master;
    sim_member_t members[SIM_MEMBERS_PER_DOMAIN];
    uint16_t domain_id;
    ucn_node_id_t master_node_id;
    ucn_session_id_t master_session_id;
    uint32_t generation;
} sim_domain_t;

/* One test-owned callback gate represents the process-wide Timed-Link
 * execution domain.  It is reinitialized before each independent simulation.
 * 一个由测试持有的共享围栏代表进程级 Timed-Link 执行域；每个独立模拟开始前
 * 都会重新初始化。 */
static ucn_timed_link_callback_gate_t simulation_callback_gate;
static ucn_time_authority_callback_gate_t simulation_authority_gate;

static ucn_result_t witness_load(void *context,
                                 ucn_time_generation_witness_t *record)
{
    sim_witness_store_t *store = (sim_witness_store_t *)context;

    if (!store->present) {
        return UCN_ERR_NOT_FOUND;
    }
    *record = store->record;
    return UCN_OK;
}

static ucn_result_t witness_reserve(
    void *context,
    const ucn_time_generation_witness_t *record,
    uint32_t operation_id,
    ucn_time_persist_completion_t *completion)
{
    sim_witness_store_t *store = (sim_witness_store_t *)context;

    store->record = *record;
    store->present = true;
    completion->state = UCN_TIME_PERSIST_COMPLETION_COMMITTED;
    completion->operation_id = operation_id;
    completion->result = UCN_OK;
    return UCN_OK;
}

static ucn_result_t witness_poll(void *context,
                                 uint32_t operation_id,
                                 ucn_time_persist_completion_t *completion)
{
    (void)context;
    (void)operation_id;
    (void)completion;
    return UCN_ERR_STATE;
}

static ucn_result_t state_load(void *context,
                               ucn_time_authority_state_record_t *record)
{
    sim_state_store_t *store = (sim_state_store_t *)context;

    if (!store->present) {
        return UCN_ERR_NOT_FOUND;
    }
    *record = store->record;
    return UCN_OK;
}

static ucn_result_t state_store(
    void *context,
    const ucn_time_authority_state_record_t *record,
    uint32_t operation_id,
    ucn_time_persist_completion_t *completion)
{
    sim_state_store_t *store = (sim_state_store_t *)context;

    store->record = *record;
    store->present = true;
    completion->state = UCN_TIME_PERSIST_COMPLETION_COMMITTED;
    completion->operation_id = operation_id;
    completion->result = UCN_OK;
    return UCN_OK;
}

static ucn_result_t state_poll(void *context,
                               uint32_t operation_id,
                               ucn_time_persist_completion_t *completion)
{
    (void)context;
    (void)operation_id;
    (void)completion;
    return UCN_ERR_STATE;
}

static const ucn_time_witness_ops_t witness_ops = {
    sizeof(ucn_time_witness_ops_t),
    UCN_TIME_AUTHORITY_PROVIDER_API_VERSION,
    witness_load,
    witness_reserve,
    witness_poll
};

static const ucn_time_authority_state_ops_t state_ops = {
    sizeof(ucn_time_authority_state_ops_t),
    UCN_TIME_AUTHORITY_PROVIDER_API_VERSION,
    state_load,
    state_store,
    state_poll
};

static void sim_enter_task(void *context)
{
    (void)context;
}

static void sim_exit_task(void *context)
{
    (void)context;
}

static ucn_port_critical_token_t sim_enter_isr(void *context)
{
    (void)context;
    return (ucn_port_critical_token_t)1U;
}

static void sim_exit_isr(void *context, ucn_port_critical_token_t token)
{
    (void)context;
    (void)token;
}

static const ucn_port_ops_t simulation_ports = {
    sizeof(ucn_port_ops_t),
    UCN_PORT_OPS_API_VERSION,
    NULL,
    NULL,
    NULL,
    NULL,
    sim_enter_task,
    sim_exit_task,
    sim_enter_isr,
    sim_exit_isr
};

static ucn_result_t sim_timed_reserve(
    void *context,
    const ucn_time_event_key_t *key)
{
    sim_timed_driver_t *driver = (sim_timed_driver_t *)context;

    if (driver == NULL || key == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++driver->reserve_calls;
    return UCN_OK;
}

/* EN: Simulates an ISR completion callback and preserves frame/key/timestamp
 * as separate bounded owner queues.
 * 中文：模拟 ISR 完成回调，并通过有界 Owner 队列保存帧/key/时间戳。 */
static ucn_result_t sim_timed_submit(
    void *context,
    const ucn_time_event_key_t *key,
    const uint8_t *frame,
    size_t length)
{
    sim_timed_driver_t *driver = (sim_timed_driver_t *)context;
    ucn_time_tx_timestamp_event_t event;

    if (driver == NULL || key == NULL || frame == NULL || length == 0U ||
        length > sizeof(driver->submitted)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&event, 0, sizeof(event));
    event.key = *key;
    event.timestamp_us = driver->next_tx_timestamp_us;
    event.quality = 1U;
    event.completion = UCN_OK;
    if (ucn_time_tx_event_enqueue_from_isr(&driver->tx_events, &event) !=
        UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    (void)memcpy(driver->submitted, frame, length);
    driver->submitted_length = length;
    ++driver->submit_calls;
    return UCN_OK;
}

static ucn_result_t sim_timed_cancel(
    void *context,
    const ucn_time_event_key_t *key)
{
    sim_timed_driver_t *driver = (sim_timed_driver_t *)context;

    if (driver == NULL || key == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++driver->cancel_calls;
    if (driver->cancel_failures_remaining != 0U) {
        --driver->cancel_failures_remaining;
        return UCN_ERR_LINK_DOWN;
    }
    return UCN_OK;
}

static ucn_result_t sim_timed_quiesce(void *context)
{
    sim_timed_driver_t *driver = (sim_timed_driver_t *)context;

    if (driver == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    ++driver->quiesce_calls;
    return UCN_OK;
}

static bool sim_timed_driver_init(sim_timed_driver_t *driver,
                                  ucn_timed_link_t *link,
                                  uint8_t link_id)
{
    static const ucn_time_link_ops_t ops = {
        sizeof(ucn_time_link_ops_t),
        UCN_TIME_LINK_OPS_API_VERSION,
        sim_timed_reserve,
        sim_timed_submit,
        sim_timed_cancel,
        sim_timed_quiesce
    };
    (void)memset(driver, 0, sizeof(*driver));
    (void)memset(&driver->ingress_link, 0, sizeof(driver->ingress_link));
    driver->ingress_link.link_id = link_id;
    if (ucn_timed_link_init(link, link_id, 1U, &ops, driver,
                            &simulation_ports, driver,
                            &simulation_callback_gate) != UCN_OK ||
        ucn_time_tx_event_queue_init(&driver->tx_events, &simulation_ports,
                                     driver) !=
            UCN_OK ||
        ucn_time_timed_rx_queue_init(&driver->rx_items, &simulation_ports,
                                     driver) !=
            UCN_OK) {
        return false;
    }
    driver->timed_link = link;
    return true;
}

static ucn_time_path_contract_t member_path(const sim_domain_t *domain,
                                            const sim_member_t *member,
                                            uint32_t path_epoch,
                                            bool asymmetry_known,
                                            bool dynamic_route)
{
    ucn_time_path_contract_t contract;

    (void)memset(&contract, 0, sizeof(contract));
    contract.forward_path.owner_node_id = domain->master_node_id;
    contract.forward_path.owner_session_id = domain->master_session_id;
    contract.forward_path.path_id =
        path_epoch * 1000U + member->node_id * 2U;
    contract.forward_path.destination_node_id = member->node_id;
    contract.reverse_path.owner_node_id = member->node_id;
    contract.reverse_path.owner_session_id = member->session_id;
    contract.reverse_path.path_id = contract.forward_path.path_id + 1U;
    contract.reverse_path.destination_node_id = domain->master_node_id;
    contract.max_asymmetry_us = 50U;
    contract.installed = !dynamic_route;
    contract.immutable_for_transaction = !dynamic_route;
    contract.ordinary_dynamic_route = dynamic_route;
    contract.asymmetry_bound_known = asymmetry_known;
    return contract;
}

static ucn_time_capability_lease_t member_lease(
    const sim_domain_t *domain,
    const sim_member_t *member,
    const ucn_time_path_contract_t *contract,
    uint64_t expiry_us)
{
    ucn_time_capability_lease_t lease;

    (void)memset(&lease, 0, sizeof(lease));
    lease.destination_node_id = member->node_id;
    lease.destination_session_id = member->session_id;
    lease.endpoint = UCN_TIME_SYNC_ENDPOINT;
    lease.metadata_version = UCN_REALTIME_ENVELOPE_VERSION;
    lease.mode = UCN_REALTIME_MODE_DEADLINE;
    lease.clock_domain_id = domain->domain_id;
    lease.domain_generation = domain->generation;
    lease.capabilities = UCN_TIME_CAP_TIME_META_V1 |
                         UCN_TIME_CAP_SYNC_CLIENT_V1 |
                         UCN_TIME_CAP_LINK_RX_HW_STAMP |
                         UCN_TIME_CAP_LINK_TX_HW_STAMP;
    lease.forward_path = contract->forward_path;
    lease.reverse_path = contract->reverse_path;
    lease.expires_at_us = expiry_us;
    lease.occupied = true;
    return lease;
}

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

/* EN: Forces every simulated control message through its strict Wire codec.
 * 中文：让每一条模拟控制消息都经过严格 Wire Codec。 */
static bool wire_roundtrip(const ucn_time_control_message_t *outbound,
                           ucn_time_control_message_t *inbound)
{
    uint8_t wire[UCN_TIME_SYNC_MAX_PAYLOAD_BYTES];
    ucn_time_control_outer_t outer;
    size_t length = 0U;
    bool forward;

    if (ucn_time_control_payload_encode(outbound, wire, sizeof(wire),
                                        &length) != UCN_OK) {
        return false;
    }
    forward = outbound->role != UCN_TIME_CONTROL_DELAY_REQ;
    outer = outer_for(&outbound->key, forward);
    if (outbound->role == UCN_TIME_CONTROL_SYNC) {
        return ucn_time_control_sync_decode(&outer, wire, length, inbound) ==
               UCN_OK;
    }
    return ucn_time_control_existing_decode(outbound->role, &outer,
                                             &outbound->key, wire, length,
                                             inbound) == UCN_OK;
}

static bool sim_event_key_equal(const ucn_time_event_key_t *left,
                                const ucn_time_event_key_t *right)
{
    return left->link_id == right->link_id &&
           left->direction == right->direction &&
           left->link_instance_generation ==
               right->link_instance_generation &&
           left->event_token == right->event_token;
}

/* EN: Sends one timestamped control frame through the real Timed-Link API and
 * consumes its ISR completion from the bounded queue.
 * 中文：通过真实 Timed-Link API 发送时间戳控制帧，并从有界队列消费 ISR
 * 完成事件。 */
static bool sim_submit_timestamped(
    sim_timed_driver_t *driver,
    const ucn_time_event_key_t *key,
    const ucn_time_control_message_t *message,
    uint64_t timestamp_us,
    ucn_time_tx_timestamp_event_t *completion)
{
    uint8_t wire[UCN_TIME_SYNC_MAX_PAYLOAD_BYTES];
    size_t wire_length = 0U;
    ucn_time_tx_timestamp_event_t event;

    if (ucn_time_control_payload_encode(message, wire, sizeof(wire),
                                        &wire_length) != UCN_OK) {
        return false;
    }
    driver->next_tx_timestamp_us = timestamp_us;
    if (ucn_timed_link_submit(driver->timed_link, key, wire, wire_length) !=
            UCN_OK ||
        ucn_time_tx_event_dequeue(&driver->tx_events, &event) != UCN_OK ||
        !sim_event_key_equal(&event.key, key) ||
        event.timestamp_us != timestamp_us || event.completion != UCN_OK ||
        ucn_timed_link_complete_event(driver->timed_link, key) != UCN_OK) {
        return false;
    }
    *completion = event;
    return true;
}

/* EN: Delivers one timestamped frame as an atomic frame/key/timestamp RX item
 * before strict semantic decoding.
 * 中文：先原子交付 frame/key/timestamp RX 记录，再执行严格语义解码。 */
static bool sim_receive_timestamped(
    sim_timed_driver_t *receiver,
    const sim_timed_driver_t *sender,
    const ucn_time_control_message_t *outbound,
    uint64_t timestamp_us,
    ucn_time_control_message_t *inbound,
    ucn_time_event_key_t *rx_key)
{
    ucn_time_timed_rx_item_t item;
    ucn_time_timed_rx_item_t dequeued;
    ucn_time_control_outer_t outer;
    ucn_time_event_key_t allocated;
    ucn_result_t status;

    if (ucn_timed_link_allocate_rx_event_from_isr(
            receiver->timed_link, &allocated) != UCN_OK) {
        return false;
    }
    (void)memset(&item, 0, sizeof(item));
    item.ingress_link = &receiver->ingress_link;
    item.key = allocated;
    item.timestamp_us = timestamp_us;
    item.quality = 1U;
    item.length = (uint16_t)sender->submitted_length;
    (void)memcpy(item.data, sender->submitted, sender->submitted_length);
    if (ucn_time_timed_rx_enqueue_from_isr(&receiver->rx_items, &item) !=
            UCN_OK ||
        ucn_time_timed_rx_dequeue(&receiver->rx_items, &dequeued) != UCN_OK ||
        !sim_event_key_equal(&dequeued.key, &allocated) ||
        dequeued.timestamp_us != timestamp_us ||
        dequeued.length != sender->submitted_length ||
        memcmp(dequeued.data, sender->submitted,
               sender->submitted_length) != 0 ||
        ucn_timed_link_complete_event(receiver->timed_link, &allocated) !=
            UCN_OK) {
        return false;
    }
    outer = outer_for(&outbound->key,
                      outbound->role != UCN_TIME_CONTROL_DELAY_REQ);
    if (outbound->role == UCN_TIME_CONTROL_SYNC) {
        status = ucn_time_control_sync_decode(
            &outer, dequeued.data, dequeued.length, inbound);
    } else {
        status = ucn_time_control_existing_decode(
            outbound->role, &outer, &outbound->key,
            dequeued.data, dequeued.length, inbound);
    }
    if (status != UCN_OK) {
        return false;
    }
    *rx_key = allocated;
    return true;
}

/* EN: Drains every Time-Sync release obligation into the matching Link.
 * 中文：把全部 Time-Sync 回收义务交给对应 Link。 */
static bool sim_drain_master_releases(sim_domain_t *domain)
{
    ucn_time_event_key_t key;
    ucn_result_t status;

    while ((status = ucn_time_sync_master_peek_released_event(
                &domain->master, &key)) == UCN_OK) {
        if (ucn_timed_link_retire_event(&domain->timed_link, &key) != UCN_OK) {
            return false;
        }
        if (ucn_time_sync_master_ack_released_event(
                &domain->master, &key) != UCN_OK) {
            return false;
        }
    }
    return status == UCN_ERR_NOT_FOUND;
}

static bool sim_drain_member_releases(sim_member_t *member)
{
    ucn_time_event_key_t key;
    ucn_result_t status;

    while ((status = ucn_time_sync_member_peek_released_event(
                &member->sync, &key)) == UCN_OK) {
        if (ucn_timed_link_retire_event(&member->timed_link, &key) != UCN_OK) {
            return false;
        }
        if (ucn_time_sync_member_ack_released_event(
                &member->sync, &key) != UCN_OK) {
            return false;
        }
    }
    return status == UCN_ERR_NOT_FOUND;
}

static uint64_t member_local_time(const sim_member_t *member,
                                  uint64_t true_us)
{
    const uint64_t elapsed = true_us - member->epoch_true_us;
    const int64_t drift =
        ((int64_t)elapsed * member->drift_ppb) / INT64_C(1000000000);
    const int64_t local = (int64_t)true_us + member->initial_offset_us + drift;

    return local > 0 ? (uint64_t)local : 0U;
}

/* EN: Runs an authenticated four-message transaction with injected drift and
 * directional delay, optionally probing duplicate/reordered delivery.
 * 中文：在注入漂移和双向延迟下运行认证四报文事务，并可探测重复/乱序。 */
static bool run_exchange(sim_domain_t *domain,
                         sim_member_t *member,
                         const ucn_time_path_contract_t *contract,
                         ucn_realtime_requirement_t requirement,
                         uint64_t true_start_us,
                         uint32_t forward_delay_us,
                         uint32_t reverse_delay_us,
                         bool duplicate_sync,
                         bool reorder_follow_up,
                         ucn_time_sync_sample_t *sample)
{
    ucn_time_event_key_t t1;
    ucn_time_event_key_t t2;
    ucn_time_event_key_t t3;
    ucn_time_event_key_t t4;
    const uint64_t t1_us = true_start_us;
    const uint64_t t2_us = member_local_time(
        member, true_start_us + forward_delay_us);
    const uint64_t t3_us = member_local_time(
        member, true_start_us + forward_delay_us + 50U);
    const uint64_t t4_us = true_start_us + forward_delay_us + 50U +
                           reverse_delay_us;
    ucn_time_control_message_t outbound;
    ucn_time_control_message_t inbound;
    ucn_time_control_message_t follow_up;
    ucn_time_tx_timestamp_event_t completion;
    ucn_wire_time_txn_key_t key;
    uint64_t old_deadline;

    EXCHANGE_REQUIRE(ucn_timed_link_allocate_event(
                         &domain->timed_link, UCN_TIME_EVENT_TX, &t1) ==
                         UCN_OK,
                     "allocate T1");
    EXCHANGE_REQUIRE(ucn_time_sync_master_begin(
                         &domain->master, member->node_id,
                         member->session_id, contract, requirement,
                         true_start_us, &t1, &outbound) == UCN_OK,
                     "master begin");
    EXCHANGE_REQUIRE(sim_submit_timestamped(
                         &domain->timed_driver, &t1, &outbound, t1_us,
                         &completion),
                     "submit T1/SYNC");
    EXCHANGE_REQUIRE(sim_receive_timestamped(
                         &member->timed_driver, &domain->timed_driver,
                         &outbound, t2_us, &inbound, &t2),
                     "atomic T2/SYNC receive");
    key = outbound.key;
    EXCHANGE_REQUIRE(ucn_time_sync_master_record_t1(
                         &domain->master, &key, &t1,
                         true_start_us + 1U,
                         completion.timestamp_us) == UCN_OK,
                     "record T1");
    EXCHANGE_REQUIRE(ucn_time_sync_master_build_follow_up(
                         &domain->master, &key, true_start_us + 2U,
                         &follow_up) == UCN_OK,
                     "build FOLLOW_UP");
    EXCHANGE_REQUIRE(wire_roundtrip(&follow_up, &follow_up),
                     "FOLLOW_UP codec");
    if (reorder_follow_up &&
        ucn_time_sync_member_receive_follow_up(
            &member->sync, &follow_up, true_start_us + 2U) !=
            UCN_ERR_NOT_FOUND) {
        return false;
    }
    EXCHANGE_REQUIRE(ucn_time_sync_member_receive_sync(
                         &member->sync, &inbound, contract, requirement,
                         true_start_us + 3U, &t2, t2_us) == UCN_OK,
                     "member receive SYNC");
    old_deadline = member->sync.pending.deadline_us;
    if (duplicate_sync) {
        ucn_time_control_message_t duplicate;
        ucn_time_event_key_t duplicate_t2;

        if (!sim_receive_timestamped(
                &member->timed_driver, &domain->timed_driver, &outbound,
                t2_us, &duplicate, &duplicate_t2) ||
            ucn_time_sync_member_receive_sync(
                &member->sync, &duplicate, contract, requirement,
                true_start_us + 4U, &duplicate_t2, t2_us) != UCN_OK ||
            member->sync.pending.deadline_us != old_deadline) {
            return false;
        }
    }
    EXCHANGE_REQUIRE(ucn_time_sync_member_receive_follow_up(
                         &member->sync, &follow_up,
                         true_start_us + 5U) == UCN_OK,
                     "member receive FOLLOW_UP");
    EXCHANGE_REQUIRE(ucn_timed_link_allocate_event(
                         &member->timed_link, UCN_TIME_EVENT_TX, &t3) ==
                         UCN_OK,
                     "allocate T3");
    EXCHANGE_REQUIRE(ucn_time_sync_member_build_delay_req(
                         &member->sync, &t3, true_start_us + 6U,
                         &outbound) == UCN_OK,
                     "build DELAY_REQ");
    EXCHANGE_REQUIRE(sim_submit_timestamped(
                         &member->timed_driver, &t3, &outbound, t3_us,
                         &completion),
                     "submit T3/DELAY_REQ");
    EXCHANGE_REQUIRE(sim_receive_timestamped(
                         &domain->timed_driver, &member->timed_driver,
                         &outbound, t4_us, &inbound, &t4),
                     "atomic T4/DELAY_REQ receive");
    EXCHANGE_REQUIRE(ucn_time_sync_member_record_t3(
                         &member->sync, &t3, true_start_us + 7U,
                         completion.timestamp_us) == UCN_OK,
                     "record T3");
    EXCHANGE_REQUIRE(ucn_time_sync_master_receive_delay_req(
                         &domain->master, &inbound, &t4,
                         true_start_us + 8U, t4_us) == UCN_OK,
                     "master receive DELAY_REQ/T4");
    EXCHANGE_REQUIRE(ucn_time_sync_master_build_delay_resp(
                         &domain->master, &key, true_start_us + 9U,
                         &outbound) == UCN_OK,
                     "build DELAY_RESP");
    EXCHANGE_REQUIRE(wire_roundtrip(&outbound, &inbound),
                     "DELAY_RESP codec");
    EXCHANGE_REQUIRE(ucn_time_sync_member_receive_delay_resp(
                         &member->sync, &inbound, true_start_us + 10U,
                         t2_us, sample) == UCN_OK,
                     "member receive DELAY_RESP");
    EXCHANGE_REQUIRE(ucn_time_sync_master_complete(
                         &domain->master, &key,
                         true_start_us + 10U) == UCN_OK,
                     "master complete");
    member->last_true_sample_us = true_start_us + forward_delay_us;
    ++member->delivered_samples;
    return true;
}

/* EN: Starts and then deterministically expires one undelivered SYNC.
 * 中文：启动并确定性地使一条未交付 SYNC 超时。 */
static bool drop_one_sync(sim_domain_t *domain,
                          sim_member_t *member,
                          const ucn_time_path_contract_t *contract,
                          uint64_t now_us)
{
    ucn_time_event_key_t t1;
    ucn_time_control_message_t outbound;
    ucn_time_control_message_t encoded;
    uint32_t old_timeouts = domain->master.timed_out;

    return ucn_timed_link_allocate_event(
               &domain->timed_link, UCN_TIME_EVENT_TX, &t1) == UCN_OK &&
           ucn_time_sync_master_begin(
               &domain->master, member->node_id, member->session_id,
               contract, UCN_REALTIME_REQUIREMENT_REQUIRED, now_us,
               &t1, &outbound) == UCN_OK &&
           wire_roundtrip(&outbound, &encoded) &&
           ucn_time_sync_master_step(
               &domain->master, now_us + UINT64_C(100000)) == UCN_OK &&
           domain->master.timed_out == old_timeouts + 1U &&
           sim_drain_master_releases(domain) &&
           domain->timed_driver.cancel_calls != 0U;
}

static bool start_authority(sim_domain_t *domain,
                            ucn_session_id_t session_id)
{
    ucn_time_authority_config_t config;
    ucn_time_sync_master_config_t master_config;

    (void)memset(&config, 0, sizeof(config));
    config.clock_domain_id = domain->domain_id;
    config.master_node_id = domain->master_node_id;
    config.master_session_id = session_id;
    config.commissioned = domain->witness_store.present;
    config.allow_initial_commissioning = !config.commissioned;
    if (ucn_time_authority_init(
            &domain->authority, &config, &witness_ops,
            &domain->witness_store, &state_ops,
            &domain->state_store, &simulation_authority_gate) != UCN_OK ||
        ucn_time_authority_start(&domain->authority) != UCN_OK ||
        ucn_time_authority_get_generation(
            &domain->authority, &domain->generation) != UCN_OK) {
        return false;
    }
    domain->master_session_id = session_id;
    (void)memset(&master_config, 0, sizeof(master_config));
    master_config.clock_domain_id = domain->domain_id;
    master_config.domain_generation = domain->generation;
    master_config.master_node_id = domain->master_node_id;
    master_config.master_session_id = domain->master_session_id;
    master_config.transaction_timeout_us = UINT64_C(100000);
    return ucn_time_sync_master_init(&domain->master, &master_config) == UCN_OK;
}

static bool initialize_member(sim_domain_t *domain,
                              sim_member_t *member,
                              size_t member_index,
                              uint64_t epoch_true_us)
{
    ucn_time_sync_member_config_t sync_config;
    ucn_time_domain_config_t domain_config;
    ucn_time_path_contract_t contract;
    ucn_time_capability_lease_t lease;

    member->node_id = (ucn_node_id_t)(domain->master_node_id +
                                      10U + member_index);
    member->session_id = 1000U + member->node_id;
    member->initial_offset_us = (int64_t)(500U + member_index * 250U);
    member->drift_ppb = (int32_t)(-30000 + (int32_t)member_index * 20000);
    member->epoch_true_us = epoch_true_us;
    if (!sim_timed_driver_init(
            &member->timed_driver, &member->timed_link,
            (uint8_t)(20U + (domain->domain_id - 10U) * 10U +
                      member_index))) {
        return false;
    }

    (void)memset(&sync_config, 0, sizeof(sync_config));
    sync_config.clock_domain_id = domain->domain_id;
    sync_config.domain_generation = domain->generation;
    sync_config.master_node_id = domain->master_node_id;
    sync_config.master_session_id = domain->master_session_id;
    sync_config.member_node_id = member->node_id;
    sync_config.member_session_id = member->session_id;
    sync_config.transaction_timeout_us = UINT64_C(100000);
    sync_config.uncertainty_components.timer_resolution_bound_us = 2U;
    sync_config.uncertainty_components.link_timestamp_capture_bound_us = 5U;
    sync_config.uncertainty_components.filter_residual_bound_us = 2U;
    sync_config.uncertainty_components.arithmetic_rounding_bound_us = 1U;
    sync_config.uncertainty_components.known_mask =
        UCN_REALTIME_UNCERTAINTY_SYNC_REQUIRED_MASK;
    if (ucn_time_sync_member_init(&member->sync, &sync_config) != UCN_OK) {
        return false;
    }

    (void)memset(&domain_config, 0, sizeof(domain_config));
    domain_config.clock_domain_id = domain->domain_id;
    domain_config.master_node_id = domain->master_node_id;
    domain_config.master_session_id = domain->master_session_id;
    domain_config.domain_generation = domain->generation;
    domain_config.lock_sample_count = 3U;
    domain_config.sync_timeout_us = UINT64_C(2000000);
    domain_config.max_holdover_us = UINT64_C(3000000);
    domain_config.max_offset_jump_us = 2000U;
    domain_config.max_slew_per_sample_us = 200U;
    domain_config.max_rate_ppb = 100000U;
    domain_config.oscillator_uncertainty_ppb = 50000U;
    domain_config.oscillator_uncertainty_known = true;
    if (ucn_time_domain_init(&member->domain, &domain_config) != UCN_OK ||
        ucn_time_capability_cache_init(&member->capabilities) != UCN_OK) {
        return false;
    }
    contract = member_path(domain, member, 1U, true, false);
    lease = member_lease(domain, member, &contract,
                         epoch_true_us + UINT64_C(20000000));
    return ucn_time_capability_cache_install(
               &member->capabilities, &lease, epoch_true_us) == UCN_OK;
}

static bool initialize_simulation(sim_domain_t domains[SIM_DOMAIN_COUNT],
                                  uint64_t epoch_true_us)
{
    size_t domain_index;
    size_t member_index;

    (void)memset(domains, 0, sizeof(sim_domain_t) * SIM_DOMAIN_COUNT);
    if (ucn_timed_link_callback_gate_init(
            &simulation_callback_gate, &simulation_ports, NULL) != UCN_OK) {
        return false;
    }
    if (ucn_time_authority_callback_gate_init(
            &simulation_authority_gate, &simulation_ports, NULL) != UCN_OK) {
        return false;
    }
    for (domain_index = 0U; domain_index < SIM_DOMAIN_COUNT; ++domain_index) {
        domains[domain_index].domain_id = (uint16_t)(10U + domain_index);
        domains[domain_index].master_node_id =
            (ucn_node_id_t)(1U + domain_index * 100U);
        if (!sim_timed_driver_init(
                &domains[domain_index].timed_driver,
                &domains[domain_index].timed_link,
                (uint8_t)(1U + domain_index))) {
            return false;
        }
        if (!start_authority(&domains[domain_index],
                             (ucn_session_id_t)(100U + domain_index))) {
            return false;
        }
        for (member_index = 0U; member_index < SIM_MEMBERS_PER_DOMAIN;
             ++member_index) {
            if (!initialize_member(&domains[domain_index],
                                   &domains[domain_index].members[member_index],
                                   member_index, epoch_true_us)) {
                return false;
            }
        }
    }
    return true;
}

/* EN: Locks two independent domains under drift, loss, duplicate and reorder.
 * 中文：在漂移、丢包、重复和乱序下锁定两个互相独立的时间域。 */
static bool test_multi_domain_fault_matrix(void)
{
    sim_domain_t domains[SIM_DOMAIN_COUNT];
    const uint64_t epoch = UINT64_C(1000000);
    uint64_t worst_error_us = 0U;
    uint32_t valid_samples = 0U;
    size_t cycle;
    size_t domain_index;
    size_t member_index;

    TEST_ASSERT(initialize_simulation(domains, epoch));
    for (cycle = 0U; cycle < 5U; ++cycle) {
        for (domain_index = 0U; domain_index < SIM_DOMAIN_COUNT;
             ++domain_index) {
            for (member_index = 0U; member_index < SIM_MEMBERS_PER_DOMAIN;
                 ++member_index) {
                sim_domain_t *domain = &domains[domain_index];
                sim_member_t *member = &domain->members[member_index];
                ucn_time_path_contract_t contract =
                    member_path(domain, member, 1U, true, false);
                ucn_time_capability_lease_t lease =
                    member_lease(domain, member, &contract,
                                 epoch + UINT64_C(20000000));
                ucn_time_sync_sample_t sample;
                ucn_realtime_clock_view_t view;
                uint64_t true_start = epoch + cycle * UINT64_C(1000000) +
                                      domain_index * UINT64_C(10000) +
                                      member_index * UINT64_C(1000);
                uint64_t expected_domain;
                uint64_t error;

                TEST_ASSERT(ucn_time_capability_cache_admit(
                    &member->capabilities, &lease,
                    UCN_TIME_CAP_TIME_META_V1 |
                        UCN_TIME_CAP_SYNC_CLIENT_V1 |
                        UCN_TIME_CAP_LINK_RX_HW_STAMP,
                    true_start) == UCN_OK);
                /* One deterministic dropped exchange leaves enough later
                 * samples to lock and demonstrates bounded loss recovery. */
                if (cycle == 1U && domain_index == 0U && member_index == 3U) {
                    TEST_ASSERT(drop_one_sync(domain, member, &contract,
                                              true_start));
                    continue;
                }
                if (!run_exchange(
                        domain, member, &contract,
                        UCN_REALTIME_REQUIREMENT_REQUIRED, true_start,
                        (uint32_t)(100U + domain_index * 40U),
                        (uint32_t)(100U + domain_index * 60U),
                        cycle == 2U && member_index == 0U,
                        cycle == 3U && member_index == 1U, &sample)) {
                    (void)fprintf(stderr,
                                  "exchange failed domain=%zu member=%zu cycle=%zu\n",
                                  domain_index, member_index, cycle);
                    return false;
                }
                TEST_ASSERT(sample.kind == UCN_TIME_SAMPLE_VALID_SYNC);
                TEST_ASSERT(ucn_time_domain_ingest_sample(
                                &member->domain, &sample) == UCN_OK);
                ++valid_samples;
                if (member->domain.phase == UCN_TIME_DOMAIN_LOCKED) {
                    TEST_ASSERT(ucn_time_domain_get_clock_view(
                                    &member->domain, sample.local_sample_us,
                                    &view) == UCN_OK);
                    expected_domain = true_start +
                                      100U + domain_index * 40U;
                    error = view.domain_time_us > expected_domain ?
                        view.domain_time_us - expected_domain :
                        expected_domain - view.domain_time_us;
                    if (error > worst_error_us) {
                        worst_error_us = error;
                    }
                }
            }
        }
    }
    TEST_ASSERT(valid_samples == 39U);
    for (domain_index = 0U; domain_index < SIM_DOMAIN_COUNT; ++domain_index) {
        for (member_index = 0U; member_index < SIM_MEMBERS_PER_DOMAIN;
             ++member_index) {
            TEST_ASSERT(domains[domain_index].members[member_index].domain.phase ==
                        UCN_TIME_DOMAIN_LOCKED);
        }
    }
    TEST_ASSERT(worst_error_us <= 100U);
    {
        sim_domain_t *foreign_domain = &domains[1U];
        sim_member_t *foreign_member = &foreign_domain->members[0U];
        sim_member_t *victim = &domains[0U].members[0U];
        ucn_time_path_contract_t foreign_path =
            member_path(foreign_domain, foreign_member, 1U, true, false);
        ucn_time_event_key_t t1;
        ucn_time_event_key_t t2;
        ucn_time_tx_timestamp_event_t completion;
        ucn_time_control_message_t outbound;
        ucn_time_control_message_t inbound;
        ucn_time_sync_member_t before = victim->sync;

        TEST_ASSERT(ucn_timed_link_allocate_event(
                        &foreign_domain->timed_link, UCN_TIME_EVENT_TX,
                        &t1) == UCN_OK);
        TEST_ASSERT(ucn_time_sync_master_begin(
                        &foreign_domain->master, foreign_member->node_id,
                        foreign_member->session_id, &foreign_path,
                        UCN_REALTIME_REQUIREMENT_REQUIRED,
                        epoch + UINT64_C(6000000), &t1, &outbound) == UCN_OK);
        TEST_ASSERT(sim_submit_timestamped(
                        &foreign_domain->timed_driver, &t1, &outbound,
                        epoch + UINT64_C(6000000), &completion));
        TEST_ASSERT(ucn_time_sync_master_record_t1(
                        &foreign_domain->master, &outbound.key, &t1,
                        epoch + UINT64_C(6000001),
                        completion.timestamp_us) == UCN_OK);
        TEST_ASSERT(sim_receive_timestamped(
                        &victim->timed_driver,
                        &foreign_domain->timed_driver, &outbound,
                        epoch + UINT64_C(6000100), &inbound, &t2));
        TEST_ASSERT(ucn_time_sync_member_receive_sync(
                        &victim->sync, &inbound, &foreign_path,
                        UCN_REALTIME_REQUIREMENT_REQUIRED,
                        epoch + UINT64_C(6000001), &t2,
                        epoch + UINT64_C(6000100)) == UCN_ERR_REPLAY);
        TEST_ASSERT(memcmp(&victim->sync, &before, sizeof(before)) == 0);
        TEST_ASSERT(ucn_time_sync_master_step(
                        &foreign_domain->master,
                        epoch + UINT64_C(6100000)) == UCN_OK);
    }
    (void)printf("RT07 multi-domain: nodes=10 valid=%u worst_error_us=%llu\n",
                 (unsigned)valid_samples,
                 (unsigned long long)worst_error_us);
    return true;
}

/* EN: Verifies diagnostic-only asymmetry handling and dynamic-route refusal.
 * 中文：验证仅诊断的非对称处理和动态 Route 拒绝。 */
static bool test_diagnostic_and_dynamic_route(void)
{
    sim_domain_t domains[SIM_DOMAIN_COUNT];
    sim_domain_t *domain;
    sim_member_t member;
    ucn_time_path_contract_t diagnostic;
    ucn_time_path_contract_t dynamic;
    ucn_time_sync_sample_t sample;
    ucn_time_control_message_t message;
    ucn_time_sync_master_t before;
    ucn_time_event_key_t t1;
    const uint64_t epoch = UINT64_C(1000000);
    size_t cycle;

    TEST_ASSERT(initialize_simulation(domains, epoch));
    domain = &domains[1U];
    (void)memset(&member, 0, sizeof(member));
    TEST_ASSERT(initialize_member(domain, &member, 20U, epoch));
    diagnostic = member_path(domain, &member, 2U, false, false);
    for (cycle = 0U; cycle < 3U; ++cycle) {
        TEST_ASSERT(run_exchange(
            domain, &member, &diagnostic,
            UCN_REALTIME_REQUIREMENT_PREFERRED,
            epoch + cycle * UINT64_C(1000000), 100U, 100U,
            false, false, &sample));
        TEST_ASSERT(sample.kind == UCN_TIME_SAMPLE_DIAGNOSTIC);
        TEST_ASSERT(ucn_time_domain_ingest_sample(&member.domain, &sample) ==
                    UCN_OK);
    }
    TEST_ASSERT(member.domain.phase == UCN_TIME_DOMAIN_ACQUIRING &&
                member.domain.stats.valid_samples == 0U &&
                member.domain.stats.diagnostic_samples == 3U);

    dynamic = member_path(domain, &member, 3U, false, true);
    before = domain->master;
    TEST_ASSERT(ucn_timed_link_allocate_event(
                    &domain->timed_link, UCN_TIME_EVENT_TX, &t1) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_begin(
                    &domain->master, member.node_id, member.session_id,
                    &dynamic, UCN_REALTIME_REQUIREMENT_PREFERRED,
                    epoch + UINT64_C(4000000), &t1, &message) ==
                UCN_ERR_UNSUPPORTED);
    TEST_ASSERT(memcmp(&before, &domain->master, sizeof(before)) == 0);
    TEST_ASSERT(ucn_timed_link_cancel(&domain->timed_link, &t1) == UCN_OK);
    return true;
}

/* EN: Proves a Path switch abort drains T3 before Link reopen and rejects the
 * stale generation afterwards.
 * 中文：证明切路中止会在 Link reopen 前回收 T3，并在随后拒绝旧 generation。 */
static bool test_path_switch_release_and_reopen(void)
{
    sim_domain_t domains[SIM_DOMAIN_COUNT];
    sim_domain_t *domain;
    sim_member_t *member;
    ucn_time_path_contract_t contract;
    ucn_time_event_key_t t1;
    ucn_time_event_key_t t2;
    ucn_time_event_key_t t3;
    ucn_time_control_message_t sync;
    ucn_time_control_message_t inbound;
    ucn_time_control_message_t follow;
    ucn_time_tx_timestamp_event_t completion;
    uint32_t old_cancel_count;
    const uint64_t epoch = UINT64_C(1000000);

    TEST_ASSERT(initialize_simulation(domains, epoch));
    domain = &domains[0U];
    member = &domain->members[0U];
    contract = member_path(domain, member, 1U, true, false);
    TEST_ASSERT(ucn_timed_link_allocate_event(
                    &domain->timed_link, UCN_TIME_EVENT_TX, &t1) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_begin(
                    &domain->master, member->node_id, member->session_id,
                    &contract, UCN_REALTIME_REQUIREMENT_REQUIRED, epoch,
                    &t1, &sync) == UCN_OK);
    TEST_ASSERT(sim_submit_timestamped(
                    &domain->timed_driver, &t1, &sync, epoch, &completion));
    TEST_ASSERT(sim_receive_timestamped(
                    &member->timed_driver, &domain->timed_driver, &sync,
                    member_local_time(member, epoch + 100U), &inbound, &t2));
    TEST_ASSERT(ucn_time_sync_member_receive_sync(
                    &member->sync, &inbound, &contract,
                    UCN_REALTIME_REQUIREMENT_REQUIRED, epoch + 1U, &t2,
                    member_local_time(member, epoch + 100U)) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_record_t1(
                    &domain->master, &sync.key, &t1, epoch + 1U,
                    completion.timestamp_us) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_master_build_follow_up(
                    &domain->master, &sync.key, epoch + 2U, &follow) ==
                UCN_OK);
    TEST_ASSERT(wire_roundtrip(&follow, &follow));
    TEST_ASSERT(ucn_time_sync_member_receive_follow_up(
                    &member->sync, &follow, epoch + 2U) == UCN_OK);
    TEST_ASSERT(ucn_timed_link_allocate_event(
                    &member->timed_link, UCN_TIME_EVENT_TX, &t3) == UCN_OK);
    TEST_ASSERT(ucn_time_sync_member_build_delay_req(
                    &member->sync, &t3, epoch + 3U, &follow) == UCN_OK);

    old_cancel_count = member->timed_driver.cancel_calls;
    TEST_ASSERT(ucn_time_sync_member_abort(&member->sync, &sync.key) ==
                UCN_OK);
    TEST_ASSERT(sim_drain_member_releases(member));
    TEST_ASSERT(member->timed_driver.cancel_calls == old_cancel_count + 1U);
    TEST_ASSERT(ucn_time_sync_master_abort(&domain->master, &sync.key) ==
                UCN_OK);
    TEST_ASSERT(ucn_timed_link_reopen(&member->timed_link) == UCN_OK);
    TEST_ASSERT(member->timed_link.link_instance_generation == 2U &&
                !ucn_timed_link_key_is_current(&member->timed_link, &t3));
    return true;
}

/* EN: Restarts one Master, invalidates old leases and rejects old envelopes.
 * 中文：重启一个 Master、使旧租约失效并拒绝旧 Envelope。 */
static bool test_restart_path_lease_and_deadline(void)
{
    sim_domain_t domains[SIM_DOMAIN_COUNT];
    sim_domain_t *domain;
    sim_member_t *member;
    ucn_time_path_contract_t old_contract;
    ucn_time_path_contract_t new_contract;
    ucn_time_capability_lease_t old_lease;
    ucn_time_capability_lease_t new_lease;
    ucn_time_sync_member_config_t member_config;
    ucn_time_sync_sample_t sample;
    ucn_realtime_clock_view_t old_view;
    ucn_realtime_clock_view_t new_view;
    ucn_realtime_policy_t policy;
    ucn_realtime_send_request_t request;
    ucn_realtime_send_result_t send_result;
    ucn_realtime_receive_context_t receive_context;
    ucn_realtime_receive_view_t receive_view;
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
    const uint8_t business[4] = {1U, 2U, 3U, 4U};
    const uint64_t epoch = UINT64_C(1000000);
    const uint64_t lease_expiry = epoch + UINT64_C(30000000);
    size_t cycle;
    size_t index;

    TEST_ASSERT(initialize_simulation(domains, epoch));
    domain = &domains[0U];
    member = &domain->members[0U];
    old_contract = member_path(domain, member, 1U, true, false);
    for (cycle = 0U; cycle < 3U; ++cycle) {
        TEST_ASSERT(run_exchange(domain, member, &old_contract,
                    UCN_REALTIME_REQUIREMENT_REQUIRED,
                    epoch + cycle * UINT64_C(1000000), 100U, 100U,
                    false, false, &sample));
        TEST_ASSERT(ucn_time_domain_ingest_sample(&member->domain, &sample) ==
                    UCN_OK);
    }
    TEST_ASSERT(ucn_time_domain_get_clock_view(
                    &member->domain, member->domain.last_sample_local_us,
                    &old_view) == UCN_OK);

    (void)memset(&policy, 0, sizeof(policy));
    policy.mode = UCN_REALTIME_MODE_DEADLINE;
    policy.requirement = UCN_REALTIME_REQUIREMENT_REQUIRED;
    policy.clock_domain_id = domain->domain_id;
    policy.max_age_us = 5000U;
    policy.max_uncertainty_us = 500U;
    policy.require_e2e_protection = true;
    (void)memset(&request, 0, sizeof(request));
    request.capture_time_us = old_view.domain_time_us;
    request.clock = &old_view;
    request.business_payload = business;
    request.business_length = sizeof(business);
    request.sample_capture_bound_us = 1U;
    request.sample_capture_bound_known = true;
    request.e2e_protected = true;
    TEST_ASSERT(ucn_realtime_payload_prepare(
                    &policy, &request, payload, sizeof(payload),
                    &send_result) == UCN_OK);

    TEST_ASSERT(start_authority(domain, 1100U));
    TEST_ASSERT(domain->generation == 2U);
    TEST_ASSERT(domains[1U].generation == 1U);
    {
        ucn_time_control_message_t outbound;
        ucn_time_control_message_t inbound;
        ucn_time_event_key_t t1;
        ucn_time_event_key_t t2;
        ucn_time_tx_timestamp_event_t completion;

        new_contract = member_path(domain, member, 2U, true, false);
        TEST_ASSERT(ucn_timed_link_allocate_event(
                        &domain->timed_link, UCN_TIME_EVENT_TX, &t1) ==
                    UCN_OK);
        TEST_ASSERT(ucn_time_sync_master_begin(
                        &domain->master, member->node_id, member->session_id,
                        &new_contract, UCN_REALTIME_REQUIREMENT_REQUIRED,
                        epoch + UINT64_C(5000000), &t1, &outbound) == UCN_OK);
        TEST_ASSERT(sim_submit_timestamped(
                        &domain->timed_driver, &t1, &outbound,
                        epoch + UINT64_C(5000000), &completion));
        TEST_ASSERT(ucn_time_sync_master_record_t1(
                        &domain->master, &outbound.key, &t1,
                        epoch + UINT64_C(5000001),
                        completion.timestamp_us) == UCN_OK);
        TEST_ASSERT(sim_receive_timestamped(
                        &member->timed_driver, &domain->timed_driver,
                        &outbound,
                        member_local_time(member,
                                          epoch + UINT64_C(5000100)),
                        &inbound, &t2));
        TEST_ASSERT(ucn_time_sync_member_receive_sync(
                        &member->sync, &inbound, &new_contract,
                        UCN_REALTIME_REQUIREMENT_REQUIRED,
                        epoch + UINT64_C(5000001), &t2,
                        member_local_time(member,
                                          epoch + UINT64_C(5000100))) ==
                    UCN_ERR_REPLAY);
        TEST_ASSERT(ucn_time_sync_master_step(
                        &domain->master, epoch + UINT64_C(5100000)) == UCN_OK);
    }
    for (index = 0U; index < SIM_MEMBERS_PER_DOMAIN; ++index) {
        sim_member_t *current = &domain->members[index];
        ucn_time_path_contract_t identity_contract =
            member_path(domain, current, 1U, true, false);

        old_lease = member_lease(domain, current, &identity_contract,
                                 epoch + UINT64_C(20000000));
        /* `old_lease` now carries the new Domain identity but the old cache
         * still contains generation 1; exact admission must fail. */
        TEST_ASSERT(ucn_time_capability_cache_admit(
                        &current->capabilities, &old_lease,
                        UCN_TIME_CAP_TIME_META_V1, epoch) ==
                    UCN_ERR_NOT_FOUND);
        TEST_ASSERT(ucn_time_capability_cache_invalidate_node(
                        &current->capabilities, current->node_id) == UCN_OK);

        (void)memset(&member_config, 0, sizeof(member_config));
        member_config.clock_domain_id = domain->domain_id;
        member_config.domain_generation = domain->generation;
        member_config.master_node_id = domain->master_node_id;
        member_config.master_session_id = domain->master_session_id;
        member_config.member_node_id = current->node_id;
        member_config.member_session_id = current->session_id;
        member_config.transaction_timeout_us = UINT64_C(100000);
        member_config.uncertainty_components.timer_resolution_bound_us = 2U;
        member_config.uncertainty_components.link_timestamp_capture_bound_us =
            5U;
        member_config.uncertainty_components.filter_residual_bound_us = 2U;
        member_config.uncertainty_components.arithmetic_rounding_bound_us = 1U;
        member_config.uncertainty_components.known_mask =
            UCN_REALTIME_UNCERTAINTY_SYNC_REQUIRED_MASK;
        TEST_ASSERT(ucn_time_sync_member_init(&current->sync,
                                               &member_config) == UCN_OK);
        TEST_ASSERT(ucn_time_domain_rebind_master(
                        &current->domain, domain->master_session_id,
                        domain->generation) == UCN_OK);
        new_contract = member_path(domain, current, 2U, true, false);
        new_lease = member_lease(domain, current, &new_contract,
                                 lease_expiry);
        TEST_ASSERT(ucn_time_capability_cache_install(
                        &current->capabilities, &new_lease, epoch) == UCN_OK);
        TEST_ASSERT(ucn_time_capability_cache_admit(
                        &current->capabilities, &new_lease,
                        UCN_TIME_CAP_TIME_META_V1 |
                            UCN_TIME_CAP_SYNC_CLIENT_V1 |
                            UCN_TIME_CAP_LINK_RX_HW_STAMP |
                            UCN_TIME_CAP_LINK_TX_HW_STAMP,
                        epoch) == UCN_OK);
    }

    member = &domain->members[0U];
    new_contract = member_path(domain, member, 2U, true, false);
    old_lease = member_lease(domain, member, &new_contract, lease_expiry);
    old_lease.forward_path.path_id -= 1000U;
    old_lease.reverse_path.path_id -= 1000U;
    old_lease.expires_at_us += 1000U;
    TEST_ASSERT(ucn_time_capability_cache_install(
                    &member->capabilities, &old_lease, epoch) ==
                UCN_ERR_REPLAY);

    for (cycle = 0U; cycle < 3U; ++cycle) {
        TEST_ASSERT(run_exchange(domain, member, &new_contract,
                    UCN_REALTIME_REQUIREMENT_REQUIRED,
                    epoch + UINT64_C(6000000) +
                        cycle * UINT64_C(1000000),
                    120U, 120U, false, false, &sample));
        TEST_ASSERT(ucn_time_domain_ingest_sample(&member->domain, &sample) ==
                    UCN_OK);
    }
    TEST_ASSERT(member->domain.phase == UCN_TIME_DOMAIN_LOCKED);
    TEST_ASSERT(ucn_time_domain_get_clock_view(
                    &member->domain, member->domain.last_sample_local_us,
                    &new_view) == UCN_OK);
    TEST_ASSERT(new_view.domain_generation == 2U);

    (void)memset(&receive_context, 0, sizeof(receive_context));
    receive_context.clock = &new_view;
    receive_context.e2e_protected = true;
    receive_context.source_acl_authorized = true;
    TEST_ASSERT(ucn_realtime_payload_evaluate(
                    &policy, &receive_context, payload,
                    send_result.payload_length, &receive_view) == UCN_OK);
    TEST_ASSERT(!receive_view.accepted &&
                receive_view.reason == UCN_REALTIME_REJECT_DOMAIN_MISMATCH);

    new_lease = member_lease(domain, member, &new_contract,
                             lease_expiry);
    TEST_ASSERT(ucn_time_capability_cache_step(
                    &member->capabilities, new_lease.expires_at_us) == UCN_OK);
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &member->capabilities, &new_lease,
                    UCN_TIME_CAP_TIME_META_V1, new_lease.expires_at_us) ==
                UCN_ERR_NOT_FOUND);
    return true;
}

/* EN: Verifies exact HOLDOVER/UNSYNCED transitions and fixed pending bounds.
 * 中文：验证精确 HOLDOVER/UNSYNCED 转换与固定 pending 上限。 */
static bool test_holdover_and_capacity(void)
{
    sim_domain_t domains[SIM_DOMAIN_COUNT];
    sim_domain_t *domain;
    sim_member_t *member;
    ucn_time_path_contract_t contract;
    ucn_time_sync_sample_t sample;
    ucn_realtime_clock_view_t view;
    ucn_time_sync_master_t bounded_master;
    ucn_time_sync_master_config_t config;
    ucn_time_control_message_t message;
    uint64_t last_sample;
    const uint64_t epoch = UINT64_C(1000000);
    size_t cycle;
    size_t index;

    TEST_ASSERT(initialize_simulation(domains, epoch));
    domain = &domains[1U];
    member = &domain->members[1U];
    contract = member_path(domain, member, 1U, true, false);
    for (cycle = 0U; cycle < 3U; ++cycle) {
        TEST_ASSERT(run_exchange(domain, member, &contract,
                    UCN_REALTIME_REQUIREMENT_REQUIRED,
                    epoch + cycle * UINT64_C(1000000),
                    100U, 100U, false, false, &sample));
        TEST_ASSERT(ucn_time_domain_ingest_sample(&member->domain, &sample) ==
                    UCN_OK);
    }
    last_sample = member->domain.last_sample_local_us;
    TEST_ASSERT(ucn_time_domain_step(
                    &member->domain, last_sample + UINT64_C(2000000)) == UCN_OK);
    TEST_ASSERT(member->domain.phase == UCN_TIME_DOMAIN_HOLDOVER);
    TEST_ASSERT(ucn_time_domain_get_clock_view(
                    &member->domain, last_sample + UINT64_C(2000000),
                    &view) == UCN_OK && view.holdover);
    TEST_ASSERT(ucn_time_domain_step(
                    &member->domain, last_sample + UINT64_C(5000000)) == UCN_OK);
    TEST_ASSERT(member->domain.phase == UCN_TIME_DOMAIN_UNSYNCED);
    TEST_ASSERT(ucn_time_domain_get_clock_view(
                    &member->domain, last_sample + UINT64_C(5000000),
                    &view) == UCN_ERR_STATE);

    (void)memset(&config, 0, sizeof(config));
    config.clock_domain_id = domain->domain_id;
    config.domain_generation = domain->generation;
    config.master_node_id = domain->master_node_id;
    config.master_session_id = domain->master_session_id;
    config.transaction_timeout_us = 1000U;
    TEST_ASSERT(ucn_time_sync_master_init(&bounded_master, &config) == UCN_OK);
    for (index = 0U; index < UCN_TIME_SYNC_MAX_PEERS; ++index) {
        sim_member_t *current = &domain->members[index];
        ucn_time_event_key_t key;
        ucn_time_path_contract_t path =
            member_path(domain, current, 1U, true, false);

        TEST_ASSERT(ucn_timed_link_allocate_event(
                        &domain->timed_link, UCN_TIME_EVENT_TX, &key) ==
                    UCN_OK);
        TEST_ASSERT(ucn_time_sync_master_begin(
                        &bounded_master, current->node_id,
                        current->session_id, &path,
                        UCN_REALTIME_REQUIREMENT_REQUIRED, 0U,
                        &key, &message) == UCN_OK);
    }
    {
        sim_member_t extra = domain->members[0U];
        ucn_time_event_key_t key;
        ucn_time_path_contract_t path;

        extra.node_id += 50U;
        extra.session_id += 50U;
        path = member_path(domain, &extra, 1U, true, false);
        TEST_ASSERT(ucn_timed_link_allocate_event(
                        &domain->timed_link, UCN_TIME_EVENT_TX, &key) ==
                    UCN_OK);
        TEST_ASSERT(ucn_time_sync_master_begin(
                        &bounded_master, extra.node_id, extra.session_id,
                        &path, UCN_REALTIME_REQUIREMENT_REQUIRED, 0U,
                        &key, &message) == UCN_ERR_NO_SPACE);
        TEST_ASSERT(ucn_timed_link_cancel(&domain->timed_link, &key) ==
                    UCN_OK);
    }
    TEST_ASSERT(ucn_time_sync_master_step(&bounded_master, 1000U) == UCN_OK);
    TEST_ASSERT(bounded_master.timed_out == UCN_TIME_SYNC_MAX_PEERS);
    {
        ucn_time_event_key_t released;

        domain->timed_driver.cancel_failures_remaining = 1U;
        TEST_ASSERT(ucn_time_sync_master_peek_released_event(
                        &bounded_master, &released) == UCN_OK);
        TEST_ASSERT(ucn_timed_link_retire_event(
                        &domain->timed_link, &released) == UCN_ERR_LINK_DOWN);
        TEST_ASSERT(bounded_master.released_event_count ==
                    UCN_TIME_SYNC_MAX_PEERS);
        {
            ucn_time_event_key_t retry;

            TEST_ASSERT(ucn_time_sync_master_peek_released_event(
                            &bounded_master, &retry) == UCN_OK);
            TEST_ASSERT(memcmp(&retry, &released, sizeof(retry)) == 0);
        }
        for (index = 0U; index < UCN_TIME_SYNC_MAX_PEERS; ++index) {
            TEST_ASSERT(ucn_time_sync_master_peek_released_event(
                            &bounded_master, &released) == UCN_OK);
            TEST_ASSERT(ucn_timed_link_retire_event(
                            &domain->timed_link, &released) == UCN_OK);
            TEST_ASSERT(ucn_time_sync_master_ack_released_event(
                            &bounded_master, &released) == UCN_OK);
        }
        TEST_ASSERT(ucn_time_sync_master_peek_released_event(
                        &bounded_master, &released) == UCN_ERR_NOT_FOUND);
    }
    return true;
}

int main(void)
{
    if (!test_multi_domain_fault_matrix() ||
        !test_diagnostic_and_dynamic_route() ||
        !test_path_switch_release_and_reopen() ||
        !test_restart_path_lease_and_deadline() ||
        !test_holdover_and_capacity()) {
        return 1;
    }
    (void)printf("RT07 object bytes: sim_domain=%zu member=%zu\n",
                 sizeof(sim_domain_t), sizeof(sim_member_t));
    (void)puts("realtime simulation tests passed");
    return 0;
}
