#include "ucn/ucn_time_authority.h"

#include <pthread.h>
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

typedef struct authority_store {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    ucn_time_generation_witness_t witness;
    ucn_time_authority_state_record_t state;
    bool witness_present;
    bool state_present;
    bool block_load;
    bool load_entered;
    bool load_may_return;
    uint32_t load_calls;
    uint32_t reserve_calls;
    uint32_t state_load_calls;
    uint32_t state_store_calls;
} authority_store_t;

typedef struct start_thread {
    ucn_time_authority_t *authority;
    ucn_result_t result;
} start_thread_t;

static void gate_enter(void *context)
{
    (void)pthread_mutex_lock((pthread_mutex_t *)context);
}

static void gate_exit(void *context)
{
    (void)pthread_mutex_unlock((pthread_mutex_t *)context);
}

static ucn_port_ops_t gate_ports(void)
{
    ucn_port_ops_t ops;

    (void)memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_PORT_OPS_API_VERSION;
    ops.enter_critical = gate_enter;
    ops.exit_critical = gate_exit;
    return ops;
}

static void completion_committed(ucn_time_persist_completion_t *completion,
                                 uint32_t operation_id)
{
    (void)memset(completion, 0, sizeof(*completion));
    completion->state = UCN_TIME_PERSIST_COMPLETION_COMMITTED;
    completion->operation_id = operation_id;
    completion->result = UCN_OK;
}

/* EN: Optionally holds one Provider callback open while another Authority is
 * probed from the main thread.
 * 中文：可选择保持一个 Provider 回调不返回，同时由主线程探测另一 Authority。 */
static ucn_result_t witness_load(
    void *context,
    ucn_time_generation_witness_t *witness)
{
    authority_store_t *store = (authority_store_t *)context;

    (void)pthread_mutex_lock(&store->mutex);
    ++store->load_calls;
    if (store->block_load) {
        store->load_entered = true;
        (void)pthread_cond_broadcast(&store->condition);
        while (!store->load_may_return) {
            (void)pthread_cond_wait(&store->condition, &store->mutex);
        }
    }
    if (!store->witness_present) {
        (void)pthread_mutex_unlock(&store->mutex);
        return UCN_ERR_NOT_FOUND;
    }
    *witness = store->witness;
    (void)pthread_mutex_unlock(&store->mutex);
    return UCN_OK;
}

static ucn_result_t witness_reserve(
    void *context,
    const ucn_time_generation_witness_t *witness,
    uint32_t operation_id,
    ucn_time_persist_completion_t *completion)
{
    authority_store_t *store = (authority_store_t *)context;

    (void)pthread_mutex_lock(&store->mutex);
    ++store->reserve_calls;
    store->witness = *witness;
    store->witness_present = true;
    (void)pthread_mutex_unlock(&store->mutex);
    completion_committed(completion, operation_id);
    return UCN_OK;
}

static ucn_result_t witness_poll(
    void *context,
    uint32_t operation_id,
    ucn_time_persist_completion_t *completion)
{
    (void)context;
    (void)operation_id;
    (void)completion;
    return UCN_ERR_STATE;
}

static ucn_result_t state_load(
    void *context,
    ucn_time_authority_state_record_t *state)
{
    authority_store_t *store = (authority_store_t *)context;

    (void)pthread_mutex_lock(&store->mutex);
    ++store->state_load_calls;
    if (!store->state_present) {
        (void)pthread_mutex_unlock(&store->mutex);
        return UCN_ERR_NOT_FOUND;
    }
    *state = store->state;
    (void)pthread_mutex_unlock(&store->mutex);
    return UCN_OK;
}

static ucn_result_t state_store(
    void *context,
    const ucn_time_authority_state_record_t *state,
    uint32_t operation_id,
    ucn_time_persist_completion_t *completion)
{
    authority_store_t *store = (authority_store_t *)context;

    (void)pthread_mutex_lock(&store->mutex);
    ++store->state_store_calls;
    store->state = *state;
    store->state_present = true;
    (void)pthread_mutex_unlock(&store->mutex);
    completion_committed(completion, operation_id);
    return UCN_OK;
}

static ucn_result_t state_poll(
    void *context,
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

static ucn_time_authority_config_t authority_config(ucn_node_id_t node_id)
{
    ucn_time_authority_config_t config;

    (void)memset(&config, 0, sizeof(config));
    config.clock_domain_id = (uint16_t)(10U + node_id);
    config.master_node_id = node_id;
    config.master_session_id = 100U + node_id;
    config.allow_initial_commissioning = true;
    return config;
}

static void *start_authority(void *context)
{
    start_thread_t *thread = (start_thread_t *)context;

    thread->result = ucn_time_authority_start(thread->authority);
    return NULL;
}

/* EN: Proves two Authorities with independent Providers share one race-free
 * callback fence. This target is intended to run under TSan as well.
 * 中文：证明使用独立 Provider 的两个 Authority 共享同一个无数据竞争的回调
 * 围栏；该目标同时用于 TSan。 */
static bool test_two_authorities_share_one_gate(void)
{
    ucn_port_ops_t ports = gate_ports();
    pthread_mutex_t gate_mutex;
    authority_store_t store_a;
    authority_store_t store_b;
    ucn_time_authority_callback_gate_t gate;
    ucn_time_authority_t authority_a;
    ucn_time_authority_t authority_b;
    ucn_time_authority_t authority_b_before;
    ucn_time_authority_config_t config_a = authority_config(1U);
    ucn_time_authority_config_t config_b = authority_config(2U);
    start_thread_t thread;
    pthread_t worker;

    (void)memset(&store_a, 0, sizeof(store_a));
    (void)memset(&store_b, 0, sizeof(store_b));
    (void)memset(&thread, 0, sizeof(thread));
    TEST_ASSERT(pthread_mutex_init(&gate_mutex, NULL) == 0);
    TEST_ASSERT(pthread_mutex_init(&store_a.mutex, NULL) == 0);
    TEST_ASSERT(pthread_mutex_init(&store_b.mutex, NULL) == 0);
    TEST_ASSERT(pthread_cond_init(&store_a.condition, NULL) == 0);
    TEST_ASSERT(pthread_cond_init(&store_b.condition, NULL) == 0);
    store_a.block_load = true;

    TEST_ASSERT(ucn_time_authority_callback_gate_init(
                    &gate, &ports, &gate_mutex) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_init(
                    &authority_a, &config_a, &witness_ops, &store_a,
                    &state_ops, &store_a, &gate) == UCN_OK);
    TEST_ASSERT(ucn_time_authority_init(
                    &authority_b, &config_b, &witness_ops, &store_b,
                    &state_ops, &store_b, &gate) == UCN_OK);
    authority_b_before = authority_b;

    thread.authority = &authority_a;
    TEST_ASSERT(pthread_create(&worker, NULL, start_authority, &thread) == 0);
    (void)pthread_mutex_lock(&store_a.mutex);
    while (!store_a.load_entered) {
        (void)pthread_cond_wait(&store_a.condition, &store_a.mutex);
    }
    (void)pthread_mutex_unlock(&store_a.mutex);

    TEST_ASSERT(ucn_time_authority_start(&authority_b) == UCN_ERR_STATE);
    TEST_ASSERT(memcmp(&authority_b, &authority_b_before,
                       sizeof(authority_b)) == 0);
    TEST_ASSERT(store_b.load_calls == 0U && store_b.reserve_calls == 0U &&
                store_b.state_load_calls == 0U &&
                store_b.state_store_calls == 0U);

    (void)pthread_mutex_lock(&store_a.mutex);
    store_a.load_may_return = true;
    (void)pthread_cond_broadcast(&store_a.condition);
    (void)pthread_mutex_unlock(&store_a.mutex);
    TEST_ASSERT(pthread_join(worker, NULL) == 0);
    TEST_ASSERT(thread.result == UCN_OK &&
                ucn_time_authority_is_ready(&authority_a));
    TEST_ASSERT(ucn_time_authority_start(&authority_b) == UCN_OK &&
                ucn_time_authority_is_ready(&authority_b));

    TEST_ASSERT(pthread_cond_destroy(&store_b.condition) == 0);
    TEST_ASSERT(pthread_cond_destroy(&store_a.condition) == 0);
    TEST_ASSERT(pthread_mutex_destroy(&store_b.mutex) == 0);
    TEST_ASSERT(pthread_mutex_destroy(&store_a.mutex) == 0);
    TEST_ASSERT(pthread_mutex_destroy(&gate_mutex) == 0);
    return true;
}

int main(void)
{
    if (!test_two_authorities_share_one_gate()) {
        return 1;
    }
    (void)puts("time authority concurrency tests passed");
    return 0;
}
