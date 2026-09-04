#include "ucn/ucn_time_authority.h"

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

typedef enum fake_mode {
    FAKE_SYNC = 0,
    FAKE_ASYNC = 1,
    FAKE_FAIL = 2,
    FAKE_COMMIT_WITHOUT_WRITE = 3,
    FAKE_WRONG_OPERATION = 4
} fake_mode_t;

typedef struct witness_store {
    ucn_time_generation_witness_t record;
    ucn_time_generation_witness_t pending_record;
    ucn_time_authority_t *owner;
    bool present;
    bool pending;
    bool reenter;
    uint8_t pending_polls;
    uint8_t polls_before_commit;
    fake_mode_t mode;
    uint32_t operation_id;
    uint32_t load_calls;
    uint32_t reserve_calls;
    uint32_t poll_calls;
    ucn_result_t reenter_result;
} witness_store_t;

typedef struct state_store {
    ucn_time_authority_state_record_t record;
    ucn_time_authority_state_record_t pending_record;
    ucn_time_authority_t *owner;
    bool present;
    bool corrupt;
    bool pending;
    bool reenter;
    uint8_t pending_polls;
    uint8_t polls_before_commit;
    fake_mode_t mode;
    uint32_t operation_id;
    uint32_t load_calls;
    uint32_t store_calls;
    uint32_t poll_calls;
    ucn_result_t reenter_result;
} state_store_t;

static void completion_set(ucn_time_persist_completion_t *completion,
                           uint8_t state,
                           uint32_t operation_id,
                           ucn_result_t result)
{
    (void)memset(completion, 0, sizeof(*completion));
    completion->state = state;
    completion->operation_id = operation_id;
    completion->result = result;
}

static ucn_result_t witness_load(void *context,
                                 ucn_time_generation_witness_t *witness)
{
    witness_store_t *store = (witness_store_t *)context;

    ++store->load_calls;
    if (store->reenter && store->owner != NULL) {
        store->reenter_result = ucn_time_authority_start(store->owner);
    }
    if (!store->present) {
        return UCN_ERR_NOT_FOUND;
    }
    *witness = store->record;
    return UCN_OK;
}

static ucn_result_t witness_reserve(
    void *context,
    const ucn_time_generation_witness_t *witness,
    uint32_t operation_id,
    ucn_time_persist_completion_t *completion)
{
    witness_store_t *store = (witness_store_t *)context;

    ++store->reserve_calls;
    if (store->reenter && store->owner != NULL) {
        store->reenter_result = ucn_time_authority_poll(store->owner);
    }
    if (store->mode == FAKE_FAIL) {
        completion_set(completion, UCN_TIME_PERSIST_COMPLETION_FAILED,
                       operation_id, UCN_ERR_LINK_DOWN);
        return UCN_OK;
    }
    if (store->mode == FAKE_WRONG_OPERATION) {
        completion_set(completion, UCN_TIME_PERSIST_COMPLETION_COMMITTED,
                       operation_id + 1U, UCN_OK);
        return UCN_OK;
    }
    if (store->mode == FAKE_ASYNC) {
        store->pending_record = *witness;
        store->operation_id = operation_id;
        store->pending = true;
        store->pending_polls = 0U;
        completion_set(completion, UCN_TIME_PERSIST_COMPLETION_PENDING,
                       operation_id, UCN_OK);
        return UCN_OK;
    }
    if (store->mode != FAKE_COMMIT_WITHOUT_WRITE) {
        store->record = *witness;
        store->present = true;
    }
    completion_set(completion, UCN_TIME_PERSIST_COMPLETION_COMMITTED,
                   operation_id, UCN_OK);
    return UCN_OK;
}

static ucn_result_t witness_poll(void *context,
                                 uint32_t operation_id,
                                 ucn_time_persist_completion_t *completion)
{
    witness_store_t *store = (witness_store_t *)context;

    ++store->poll_calls;
    if (store->reenter && store->owner != NULL) {
        store->reenter_result = ucn_time_authority_poll(store->owner);
    }
    if (!store->pending || operation_id != store->operation_id) {
        return UCN_ERR_STATE;
    }
    ++store->pending_polls;
    if (store->pending_polls <= store->polls_before_commit) {
        completion_set(completion, UCN_TIME_PERSIST_COMPLETION_PENDING,
                       operation_id, UCN_OK);
        return UCN_OK;
    }
    store->record = store->pending_record;
    store->present = true;
    store->pending = false;
    completion_set(completion, UCN_TIME_PERSIST_COMPLETION_COMMITTED,
                   operation_id, UCN_OK);
    return UCN_OK;
}

static ucn_result_t state_load(void *context,
                               ucn_time_authority_state_record_t *record)
{
    state_store_t *store = (state_store_t *)context;

    ++store->load_calls;
    if (store->reenter && store->owner != NULL) {
        store->reenter_result = ucn_time_authority_start(store->owner);
    }
    if (store->corrupt) {
        return UCN_ERR_CRC;
    }
    if (!store->present) {
        return UCN_ERR_NOT_FOUND;
    }
    *record = store->record;
    return UCN_OK;
}

static ucn_result_t state_write(
    void *context,
    const ucn_time_authority_state_record_t *record,
    uint32_t operation_id,
    ucn_time_persist_completion_t *completion)
{
    state_store_t *store = (state_store_t *)context;

    ++store->store_calls;
    if (store->reenter && store->owner != NULL) {
        store->reenter_result = ucn_time_authority_poll(store->owner);
    }
    if (store->mode == FAKE_FAIL) {
        completion_set(completion, UCN_TIME_PERSIST_COMPLETION_FAILED,
                       operation_id, UCN_ERR_LINK_DOWN);
        return UCN_OK;
    }
    if (store->mode == FAKE_WRONG_OPERATION) {
        completion_set(completion, UCN_TIME_PERSIST_COMPLETION_COMMITTED,
                       operation_id + 1U, UCN_OK);
        return UCN_OK;
    }
    if (store->mode == FAKE_ASYNC) {
        store->pending_record = *record;
        store->operation_id = operation_id;
        store->pending = true;
        store->pending_polls = 0U;
        completion_set(completion, UCN_TIME_PERSIST_COMPLETION_PENDING,
                       operation_id, UCN_OK);
        return UCN_OK;
    }
    if (store->mode != FAKE_COMMIT_WITHOUT_WRITE) {
        store->record = *record;
        store->present = true;
    }
    completion_set(completion, UCN_TIME_PERSIST_COMPLETION_COMMITTED,
                   operation_id, UCN_OK);
    return UCN_OK;
}

static ucn_result_t state_poll(void *context,
                               uint32_t operation_id,
                               ucn_time_persist_completion_t *completion)
{
    state_store_t *store = (state_store_t *)context;

    ++store->poll_calls;
    if (store->reenter && store->owner != NULL) {
        store->reenter_result = ucn_time_authority_poll(store->owner);
    }
    if (!store->pending || operation_id != store->operation_id) {
        return UCN_ERR_STATE;
    }
    ++store->pending_polls;
    if (store->pending_polls <= store->polls_before_commit) {
        completion_set(completion, UCN_TIME_PERSIST_COMPLETION_PENDING,
                       operation_id, UCN_OK);
        return UCN_OK;
    }
    store->record = store->pending_record;
    store->present = true;
    store->pending = false;
    completion_set(completion, UCN_TIME_PERSIST_COMPLETION_COMMITTED,
                   operation_id, UCN_OK);
    return UCN_OK;
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
    state_write,
    state_poll
};

/* EN: Unit tests are single-threaded but still exercise the public shared-
 * gate contract.  The POSIX concurrency target below supplies a real mutex.
 * 中文：单元测试虽为单线程，仍走公共共享围栏合同；POSIX 并发目标会提供真实
 * mutex。 */
static void authority_gate_enter(void *context)
{
    (void)context;
}

static void authority_gate_exit(void *context)
{
    (void)context;
}

static const ucn_port_ops_t authority_gate_ports = {
    sizeof(ucn_port_ops_t),
    UCN_PORT_OPS_API_VERSION,
    NULL,
    NULL,
    NULL,
    NULL,
    authority_gate_enter,
    authority_gate_exit,
    NULL,
    NULL
};

static ucn_time_authority_callback_gate_t authority_gate;

static ucn_time_authority_config_t authority_config(bool commissioned,
                                                     bool allow_initial)
{
    ucn_time_authority_config_t config;

    (void)memset(&config, 0, sizeof(config));
    config.clock_domain_id = 7U;
    config.master_node_id = 1U;
    config.master_session_id = 100U;
    config.commissioned = commissioned;
    config.allow_initial_commissioning = allow_initial;
    return config;
}

/* EN: Covers fresh commissioning, restart and generation monotonicity.
 * 中文：覆盖首次投产、重启及 generation 单调性。 */
static bool test_sync_commission_and_restart(void)
{
    witness_store_t witness = {0};
    state_store_t state = {0};
    ucn_time_authority_t authority;
    ucn_time_authority_config_t config = authority_config(false, true);
    uint32_t generation = 99U;

    config.commissioned = true;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) ==
                UCN_ERR_ARGUMENT);
    config = authority_config(false, true);

    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_is_ready(&authority));
    TEST_ASSERT(ucn_time_authority_get_generation(&authority, &generation) ==
                UCN_OK);
    TEST_ASSERT(generation == 1U && witness.record.commissioned &&
                state.record.commissioned);

    config = authority_config(true, false);
    config.master_session_id = 101U;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_get_generation(&authority, &generation) ==
                UCN_OK);
    TEST_ASSERT(generation == 2U &&
                witness.record.issued_generation_high_water == 2U &&
                state.record.domain_generation == 2U &&
                state.record.master_session_id == 101U);
    return true;
}

/* EN: Covers repeated PENDING, exact reload proof and callback reentry fences.
 * 中文：覆盖多次 PENDING、精确回读证明及回调重入围栏。 */
static bool test_async_and_reentry(void)
{
    witness_store_t witness = {0};
    state_store_t state = {0};
    ucn_time_authority_t authority;
    ucn_time_authority_config_t config = authority_config(false, true);

    witness.mode = FAKE_ASYNC;
    witness.polls_before_commit = 1U;
    witness.reenter = true;
    witness.owner = &authority;
    state.mode = FAKE_ASYNC;
    state.polls_before_commit = 1U;
    state.reenter = true;
    state.owner = &authority;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_OK);
    TEST_ASSERT(authority.phase == UCN_TIME_AUTHORITY_WITNESS_PENDING);
    TEST_ASSERT(!ucn_time_authority_is_ready(&authority));
    TEST_ASSERT(witness.reenter_result == UCN_ERR_STATE);
    TEST_ASSERT(ucn_time_authority_poll(&authority) == UCN_OK);
    TEST_ASSERT(authority.phase == UCN_TIME_AUTHORITY_WITNESS_PENDING);
    TEST_ASSERT(ucn_time_authority_poll(&authority) == UCN_OK);
    TEST_ASSERT(authority.phase == UCN_TIME_AUTHORITY_STATE_PENDING);
    TEST_ASSERT(state.reenter_result == UCN_ERR_STATE);
    TEST_ASSERT(ucn_time_authority_poll(&authority) == UCN_OK);
    TEST_ASSERT(authority.phase == UCN_TIME_AUTHORITY_STATE_PENDING);
    TEST_ASSERT(ucn_time_authority_poll(&authority) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_is_ready(&authority));
    TEST_ASSERT(witness.poll_calls == 2U && state.poll_calls == 2U);
    return true;
}

/* EN: Proves a reserved-but-unpublished generation is skipped after restart.
 * 中文：证明已保留但未发布的 generation 在重启后会被跳过。 */
static bool test_partial_commit_skips_generation(void)
{
    witness_store_t witness = {0};
    state_store_t state = {0};
    ucn_time_authority_t authority;
    ucn_time_authority_config_t config = authority_config(false, true);
    uint32_t generation = 0U;

    state.mode = FAKE_FAIL;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_LINK_DOWN);
    TEST_ASSERT(authority.phase == UCN_TIME_AUTHORITY_FAULT);
    TEST_ASSERT(witness.record.issued_generation_high_water == 1U);
    TEST_ASSERT(!state.present);

    state.mode = FAKE_SYNC;
    config = authority_config(true, false);
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_get_generation(&authority, &generation) ==
                UCN_OK);
    TEST_ASSERT(generation == 2U);
    return true;
}

/* EN: Exercises corruption, rollback, exhaustion and no-write result paths.
 * 中文：验证损坏、回退、耗尽以及失败不写回路径。 */
static bool test_fail_closed_boundaries(void)
{
    witness_store_t witness = {0};
    state_store_t state = {0};
    ucn_time_authority_t authority;
    ucn_time_authority_config_t config = authority_config(true, false);
    ucn_time_persist_completion_t completion = {0};
    uint32_t generation = UINT32_C(0xA5A5A5A5);

    TEST_ASSERT(!ucn_time_persist_completion_is_valid(&completion, 1U));
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_STATE);
    TEST_ASSERT(ucn_time_authority_get_generation(&authority, &generation) ==
                UCN_ERR_STATE);
    TEST_ASSERT(generation == UINT32_C(0xA5A5A5A5));

    witness.present = true;
    witness.record.schema = UCN_TIME_AUTHORITY_STORAGE_SCHEMA;
    witness.record.clock_domain_id = 7U;
    witness.record.master_node_id = 1U;
    witness.record.issued_generation_high_water = 10U;
    witness.record.commissioned = true;
    state.present = true;
    state.record.schema = UCN_TIME_AUTHORITY_STORAGE_SCHEMA;
    state.record.clock_domain_id = 7U;
    state.record.master_node_id = 1U;
    state.record.master_session_id = 100U;
    state.record.domain_generation = 11U;
    state.record.commissioned = true;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_STATE);

    state.record.domain_generation = 10U;
    witness.record.issued_generation_high_water =
        UCN_REALTIME_DOMAIN_GENERATION_MAX;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_EXHAUSTED);

    witness.record.issued_generation_high_water = 10U;
    witness.mode = FAKE_COMMIT_WITHOUT_WRITE;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_STATE);
    TEST_ASSERT(authority.phase == UCN_TIME_AUTHORITY_FAULT);

    witness.mode = FAKE_SYNC;
    state.mode = FAKE_COMMIT_WITHOUT_WRITE;
    state.record.domain_generation = 10U;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_STATE);
    TEST_ASSERT(authority.phase == UCN_TIME_AUTHORITY_FAULT);

    witness.mode = FAKE_WRONG_OPERATION;
    state.mode = FAKE_SYNC;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_STATE);

    witness.mode = FAKE_SYNC;
    state.mode = FAKE_WRONG_OPERATION;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_STATE);
    return true;
}

/* EN: Rebuilds corrupted state only from an intact witness and trusted
 * commissioned product identity, then skips every previously issued value.
 * 中文：仅凭完好 witness 与可信已配网身份重建损坏状态，并跳过所有已签发值。 */
static bool test_corrupt_state_recovery_uses_next_generation(void)
{
    witness_store_t witness = {0};
    state_store_t state = {0};
    ucn_time_authority_t authority;
    ucn_time_authority_config_t config = authority_config(true, false);
    uint32_t generation = 0U;

    witness.present = true;
    witness.record.schema = UCN_TIME_AUTHORITY_STORAGE_SCHEMA;
    witness.record.clock_domain_id = 7U;
    witness.record.master_node_id = 1U;
    witness.record.issued_generation_high_water = 11U;
    witness.record.commissioned = true;
    state.corrupt = true;
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    /* Corruption is recoverable only because the independent witness and
     * commissioned identity remain authoritative. */
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_ERR_STATE);
    TEST_ASSERT(witness.record.issued_generation_high_water == 12U);

    /* A real atomic state Provider exposes the just-written replacement on
     * reload.  Clear the injected read fault and retry after reboot; 12 is
     * already reserved, so the next published value must be 13. */
    state.corrupt = false;
    config = authority_config(true, false);
    TEST_ASSERT(ucn_time_authority_init(&authority, &config, &witness_ops,
                                        &witness, &state_ops, &state,
                                        &authority_gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_start(&authority) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_get_generation(&authority, &generation) ==
                UCN_OK);
    TEST_ASSERT(generation == 13U);
    return true;
}

int main(void)
{
    if (ucn_time_authority_callback_gate_init(
            &authority_gate, &authority_gate_ports, NULL) != UCN_OK ||
        !test_sync_commission_and_restart() ||
        !test_async_and_reentry() ||
        !test_partial_commit_skips_generation() ||
        !test_fail_closed_boundaries() ||
        !test_corrupt_state_recovery_uses_next_generation()) {
        return 1;
    }
    (void)printf("time authority bytes: owner=%zu gate=%zu\n",
                 sizeof(ucn_time_authority_t),
                 sizeof(ucn_time_authority_callback_gate_t));
    (void)puts("time authority tests passed");
    return 0;
}
