/* Optional STATIC_MASTER generation anti-rollback startup owner.
 * 可选的 STATIC_MASTER generation 防回退启动 Owner。 */

#include "ucn/ucn_time_authority.h"

#include <string.h>

/* EN: Checks whether a Node ID can identify one unicast authority.
 * 中文：检查 Node ID 是否可标识单播时间权威。 */
static bool node_id_is_unicast(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

/* EN: Checks one no-wrap generation or operation serial.
 * 中文：检查一个禁止回绕的 generation 或 operation 序号。 */
static bool serial_is_valid(uint32_t value)
{
    return value != 0U && value <= UCN_REALTIME_DOMAIN_GENERATION_MAX;
}

/* EN: Validates one independently durable issued-generation witness.
 * 中文：校验一条独立持久化的已签发 generation 高水位记录。 */
bool ucn_time_generation_witness_is_valid(
    const ucn_time_generation_witness_t *witness)
{
    return witness != NULL &&
           witness->schema == UCN_TIME_AUTHORITY_STORAGE_SCHEMA &&
           witness->clock_domain_id != 0U &&
           witness->clock_domain_id <= UCN_REALTIME_CLOCK_DOMAIN_ID_MAX &&
           node_id_is_unicast(witness->master_node_id) &&
           serial_is_valid(witness->issued_generation_high_water) &&
           witness->commissioned;
}

/* EN: Validates one reloadable STATIC_MASTER authority state record.
 * 中文：校验一条可回读的 STATIC_MASTER 权威状态记录。 */
bool ucn_time_authority_state_record_is_valid(
    const ucn_time_authority_state_record_t *record)
{
    return record != NULL &&
           record->schema == UCN_TIME_AUTHORITY_STORAGE_SCHEMA &&
           record->clock_domain_id != 0U &&
           record->clock_domain_id <= UCN_REALTIME_CLOCK_DOMAIN_ID_MAX &&
           node_id_is_unicast(record->master_node_id) &&
           record->master_session_id != 0U &&
           serial_is_valid(record->domain_generation) &&
           record->commissioned;
}

/* EN: Rejects zero, stale and contradictory provider completions.
 * 中文：拒绝全零、陈旧及语义矛盾的 Provider 完成结果。 */
bool ucn_time_persist_completion_is_valid(
    const ucn_time_persist_completion_t *completion,
    uint32_t expected_operation_id)
{
    if (completion == NULL || !serial_is_valid(expected_operation_id) ||
        completion->operation_id != expected_operation_id) {
        return false;
    }
    if (completion->state == UCN_TIME_PERSIST_COMPLETION_COMMITTED ||
        completion->state == UCN_TIME_PERSIST_COMPLETION_PENDING) {
        return completion->result == UCN_OK;
    }
    if (completion->state == UCN_TIME_PERSIST_COMPLETION_FAILED) {
        return completion->result < UCN_OK;
    }
    return false;
}

/* EN: Validates the complete static-Master startup policy.
 * 中文：校验完整的静态 Master 启动策略。 */
static bool config_is_valid(const ucn_time_authority_config_t *config)
{
    return config != NULL && config->clock_domain_id != 0U &&
           config->clock_domain_id <= UCN_REALTIME_CLOCK_DOMAIN_ID_MAX &&
           node_id_is_unicast(config->master_node_id) &&
           config->master_session_id != 0U &&
           config->commissioned != config->allow_initial_commissioning;
}

/* EN: Validates the independent generation-witness Provider ABI.
 * 中文：校验独立 generation witness Provider ABI。 */
static bool witness_ops_are_valid(const ucn_time_witness_ops_t *ops)
{
    return ops != NULL && ops->struct_size >= sizeof(*ops) &&
           ops->api_version == UCN_TIME_AUTHORITY_PROVIDER_API_VERSION &&
           ops->load != NULL && ops->reserve != NULL && ops->poll != NULL;
}

/* EN: Validates the current-authority-state Provider ABI.
 * 中文：校验当前时间权威状态 Provider ABI。 */
static bool state_ops_are_valid(const ucn_time_authority_state_ops_t *ops)
{
    return ops != NULL && ops->struct_size >= sizeof(*ops) &&
           ops->api_version == UCN_TIME_AUTHORITY_PROVIDER_API_VERSION &&
           ops->load != NULL && ops->store != NULL && ops->poll != NULL;
}

/* EN: Requires one task/SMP-safe Port lock for the shared callback gate.
 * 中文：要求共享回调围栏具备任务/SMP 安全的 Port 锁。 */
static bool callback_gate_port_is_valid(const ucn_port_ops_t *ops)
{
    return ucn_port_ops_is_compatible(ops) &&
           ops->enter_critical != NULL && ops->exit_critical != NULL;
}

/* EN: Initializes the shared callback gate before concurrent use begins.
 * 中文：在开始并发使用前初始化共享回调围栏。 */
ucn_result_t ucn_time_authority_callback_gate_init(
    ucn_time_authority_callback_gate_t *gate,
    const ucn_port_ops_t *port_ops,
    void *port_context)
{
    ucn_time_authority_callback_gate_t initialized;

    if (gate == NULL || !callback_gate_port_is_valid(port_ops)) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(&initialized, 0, sizeof(initialized));
    initialized.port_ops = port_ops;
    initialized.port_context = port_context;
    initialized.initialized = true;
    *gate = initialized;
    return UCN_OK;
}

/* EN: Validates an initialized caller-owned gate without touching its owner.
 * 中文：在不访问活动 Owner 的情况下校验已初始化的调用方围栏。 */
static bool callback_gate_is_valid(
    const ucn_time_authority_callback_gate_t *gate)
{
    return gate != NULL && gate->initialized &&
           callback_gate_port_is_valid(gate->port_ops);
}

/* EN: Enters or leaves the shared gate's physical task lock.
 * 中文：进入或退出共享围栏的物理任务锁。 */
static void callback_gate_lock(ucn_time_authority_callback_gate_t *gate)
{
    gate->port_ops->enter_critical(gate->port_context);
}

static void callback_gate_unlock(ucn_time_authority_callback_gate_t *gate)
{
    gate->port_ops->exit_critical(gate->port_context);
}

/* EN: Reads the logical callback fence under its shared physical lock.
 * 中文：在共享物理锁保护下读取逻辑回调围栏。 */
static bool callback_gate_is_active(
    ucn_time_authority_callback_gate_t *gate)
{
    bool active;

    callback_gate_lock(gate);
    active = gate->active;
    callback_gate_unlock(gate);
    return active;
}

/* EN: Revalidates immutable Owner configuration before every public action.
 * 中文：在每个公开动作前复验 Owner 的不可变配置。 */
static bool authority_base_is_valid(const ucn_time_authority_t *authority)
{
    return authority != NULL && authority->initialized &&
           config_is_valid(&authority->config) &&
           witness_ops_are_valid(authority->witness_ops) &&
           state_ops_are_valid(authority->state_ops) &&
           authority->witness_context != NULL &&
           authority->state_context != NULL &&
           callback_gate_is_valid(authority->callback_gate);
}

/* EN: Compares witness records without relying on padding bytes.
 * 中文：不依赖填充字节比较 witness 记录。 */
static bool witness_equal(const ucn_time_generation_witness_t *left,
                          const ucn_time_generation_witness_t *right)
{
    return left->schema == right->schema &&
           left->clock_domain_id == right->clock_domain_id &&
           left->master_node_id == right->master_node_id &&
           left->issued_generation_high_water ==
               right->issued_generation_high_water &&
           left->commissioned == right->commissioned;
}

/* EN: Compares authority-state records without relying on padding bytes.
 * 中文：不依赖填充字节比较时间权威状态记录。 */
static bool state_equal(const ucn_time_authority_state_record_t *left,
                        const ucn_time_authority_state_record_t *right)
{
    return left->schema == right->schema &&
           left->clock_domain_id == right->clock_domain_id &&
           left->master_node_id == right->master_node_id &&
           left->master_session_id == right->master_session_id &&
           left->domain_generation == right->domain_generation &&
           left->commissioned == right->commissioned;
}

/* EN: Binds a durable witness to the configured Domain and Master.
 * 中文：把持久 witness 绑定到配置的 Domain 和 Master。 */
static bool witness_matches_config(
    const ucn_time_generation_witness_t *witness,
    const ucn_time_authority_config_t *config)
{
    return ucn_time_generation_witness_is_valid(witness) &&
           witness->clock_domain_id == config->clock_domain_id &&
           witness->master_node_id == config->master_node_id;
}

/* EN: Binds a durable authority state to the configured Domain and Master.
 * 中文：把持久权威状态绑定到配置的 Domain 和 Master。 */
static bool state_matches_config(
    const ucn_time_authority_state_record_t *state,
    const ucn_time_authority_config_t *config)
{
    return ucn_time_authority_state_record_is_valid(state) &&
           state->clock_domain_id == config->clock_domain_id &&
           state->master_node_id == config->master_node_id;
}

/* EN: Permanently fences this startup attempt after a contract failure.
 * 中文：合同失败后永久围栏本次启动尝试。 */
static ucn_result_t fail_closed(ucn_time_authority_t *authority,
                                ucn_result_t result)
{
    authority->phase = UCN_TIME_AUTHORITY_FAULT;
    authority->last_failure = result == UCN_OK ? UCN_ERR_STATE : result;
    authority->active_operation_id = 0U;
    return authority->last_failure;
}

/* EN: Establishes the execution-domain Provider callback fence atomically.
 * 中文：原子建立执行域 Provider 回调围栏。 */
static bool callback_enter(ucn_time_authority_t *authority)
{
    ucn_time_authority_callback_gate_t *gate;

    if (authority == NULL ||
        !callback_gate_is_valid(authority->callback_gate)) {
        return false;
    }
    gate = authority->callback_gate;
    callback_gate_lock(gate);
    if (gate->active || authority->io_active) {
        callback_gate_unlock(gate);
        return false;
    }
    gate->active_owner = authority;
    gate->active = true;
    authority->io_active = true;
    callback_gate_unlock(gate);
    return true;
}

/* EN: Releases the exact Provider callback owner under the shared lock.
 * 中文：在共享锁保护下释放精确匹配的 Provider 回调 Owner。 */
static void callback_leave(ucn_time_authority_t *authority)
{
    ucn_time_authority_callback_gate_t *gate = authority->callback_gate;

    callback_gate_lock(gate);
    if (gate->active && gate->active_owner == authority) {
        gate->active_owner = NULL;
        gate->active = false;
    }
    authority->io_active = false;
    callback_gate_unlock(gate);
}

/* EN: Loads the generation witness under the callback fence.
 * 中文：在回调围栏下加载 generation witness。 */
static ucn_result_t load_witness(
    ucn_time_authority_t *authority,
    ucn_time_generation_witness_t *witness)
{
    ucn_result_t result;

    if (!callback_enter(authority)) {
        return UCN_ERR_STATE;
    }
    result = authority->witness_ops->load(authority->witness_context, witness);
    callback_leave(authority);
    return result;
}

/* EN: Loads current authority state under the callback fence.
 * 中文：在回调围栏下加载当前时间权威状态。 */
static ucn_result_t load_state(
    ucn_time_authority_t *authority,
    ucn_time_authority_state_record_t *state)
{
    ucn_result_t result;

    if (!callback_enter(authority)) {
        return UCN_ERR_STATE;
    }
    result = authority->state_ops->load(authority->state_context, state);
    callback_leave(authority);
    return result;
}

/* EN: Allocates one no-wrap Provider operation identifier.
 * 中文：分配一个禁止回绕的 Provider operation ID。 */
static ucn_result_t allocate_operation(ucn_time_authority_t *authority)
{
    if (!serial_is_valid(authority->next_operation_id)) {
        return UCN_ERR_EXHAUSTED;
    }
    authority->active_operation_id = authority->next_operation_id;
    ++authority->next_operation_id;
    return UCN_OK;
}

/* EN: Reloads and proves the exact desired witness after commit.
 * 中文：提交后回读并证明精确的目标 witness。 */
static ucn_result_t reload_witness_exact(ucn_time_authority_t *authority)
{
    ucn_time_generation_witness_t loaded;
    ucn_result_t result;

    (void)memset(&loaded, 0, sizeof(loaded));
    result = load_witness(authority, &loaded);
    if (result != UCN_OK ||
        !ucn_time_generation_witness_is_valid(&loaded) ||
        !witness_equal(&loaded, &authority->desired_witness)) {
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

/* EN: Reloads and proves the exact desired authority state after commit.
 * 中文：提交后回读并证明精确的目标权威状态。 */
static ucn_result_t reload_state_exact(ucn_time_authority_t *authority)
{
    ucn_time_authority_state_record_t loaded;
    ucn_result_t result;

    (void)memset(&loaded, 0, sizeof(loaded));
    result = load_state(authority, &loaded);
    if (result != UCN_OK ||
        !ucn_time_authority_state_record_is_valid(&loaded) ||
        !state_equal(&loaded, &authority->desired_state)) {
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

static ucn_result_t begin_state_store(ucn_time_authority_t *authority);

/* EN: Resolves a synchronous or asynchronous witness reservation.
 * 中文：处理同步或异步 witness 高水位保留结果。 */
static ucn_result_t resolve_witness_completion(
    ucn_time_authority_t *authority,
    const ucn_time_persist_completion_t *completion)
{
    ucn_result_t result;

    if (!ucn_time_persist_completion_is_valid(
            completion, authority->active_operation_id)) {
        return fail_closed(authority, UCN_ERR_STATE);
    }
    if (completion->state == UCN_TIME_PERSIST_COMPLETION_PENDING) {
        authority->phase = UCN_TIME_AUTHORITY_WITNESS_PENDING;
        return UCN_OK;
    }
    if (completion->state == UCN_TIME_PERSIST_COMPLETION_FAILED) {
        return fail_closed(authority, completion->result);
    }
    result = reload_witness_exact(authority);
    if (result != UCN_OK) {
        return fail_closed(authority, result);
    }
    authority->active_operation_id = 0U;
    return begin_state_store(authority);
}

/* EN: Begins the persist-before-publish witness reservation.
 * 中文：开始“先持久化再发布”的 witness 保留。 */
static ucn_result_t begin_witness_reserve(ucn_time_authority_t *authority)
{
    ucn_time_persist_completion_t completion;
    ucn_result_t result;

    result = allocate_operation(authority);
    if (result != UCN_OK) {
        return fail_closed(authority, result);
    }
    (void)memset(&completion, 0, sizeof(completion));
    if (!callback_enter(authority)) {
        return fail_closed(authority, UCN_ERR_STATE);
    }
    result = authority->witness_ops->reserve(
        authority->witness_context, &authority->desired_witness,
        authority->active_operation_id, &completion);
    callback_leave(authority);
    if (result != UCN_OK) {
        return fail_closed(authority, result);
    }
    return resolve_witness_completion(authority, &completion);
}

/* EN: Resolves a synchronous or asynchronous current-state store.
 * 中文：处理同步或异步当前权威状态写入结果。 */
static ucn_result_t resolve_state_completion(
    ucn_time_authority_t *authority,
    const ucn_time_persist_completion_t *completion)
{
    ucn_result_t result;

    if (!ucn_time_persist_completion_is_valid(
            completion, authority->active_operation_id)) {
        return fail_closed(authority, UCN_ERR_STATE);
    }
    if (completion->state == UCN_TIME_PERSIST_COMPLETION_PENDING) {
        authority->phase = UCN_TIME_AUTHORITY_STATE_PENDING;
        return UCN_OK;
    }
    if (completion->state == UCN_TIME_PERSIST_COMPLETION_FAILED) {
        return fail_closed(authority, completion->result);
    }
    result = reload_state_exact(authority);
    if (result != UCN_OK) {
        return fail_closed(authority, result);
    }
    authority->active_operation_id = 0U;
    authority->phase = UCN_TIME_AUTHORITY_READY;
    authority->last_failure = UCN_OK;
    return UCN_OK;
}

/* EN: Begins storing the current authority state after witness proof.
 * 中文：在 witness 证明后开始保存当前时间权威状态。 */
static ucn_result_t begin_state_store(ucn_time_authority_t *authority)
{
    ucn_time_persist_completion_t completion;
    ucn_result_t result;

    result = allocate_operation(authority);
    if (result != UCN_OK) {
        return fail_closed(authority, result);
    }
    (void)memset(&completion, 0, sizeof(completion));
    if (!callback_enter(authority)) {
        return fail_closed(authority, UCN_ERR_STATE);
    }
    result = authority->state_ops->store(
        authority->state_context, &authority->desired_state,
        authority->active_operation_id, &completion);
    callback_leave(authority);
    if (result != UCN_OK) {
        return fail_closed(authority, result);
    }
    return resolve_state_completion(authority, &completion);
}

/* EN: Initializes configuration and Provider contracts without performing I/O.
 * 中文：初始化配置与 Provider 合同，但不执行任何 I/O。 */
ucn_result_t ucn_time_authority_init(
    ucn_time_authority_t *authority,
    const ucn_time_authority_config_t *config,
    const ucn_time_witness_ops_t *witness_ops,
    void *witness_context,
    const ucn_time_authority_state_ops_t *state_ops,
    void *state_context,
    ucn_time_authority_callback_gate_t *callback_gate)
{
    ucn_time_authority_t candidate;

    if (authority == NULL || !config_is_valid(config) ||
        !witness_ops_are_valid(witness_ops) ||
        !state_ops_are_valid(state_ops) || witness_context == NULL ||
        state_context == NULL || !callback_gate_is_valid(callback_gate)) {
        return UCN_ERR_ARGUMENT;
    }
    callback_gate_lock(callback_gate);
    if (callback_gate->active) {
        callback_gate_unlock(callback_gate);
        return UCN_ERR_STATE;
    }
    (void)memset(&candidate, 0, sizeof(candidate));
    candidate.config = *config;
    candidate.witness_ops = witness_ops;
    candidate.witness_context = witness_context;
    candidate.state_ops = state_ops;
    candidate.state_context = state_context;
    candidate.callback_gate = callback_gate;
    candidate.next_operation_id = 1U;
    candidate.phase = UCN_TIME_AUTHORITY_IDLE;
    candidate.initialized = true;
    *authority = candidate;
    callback_gate_unlock(callback_gate);
    return UCN_OK;
}

/* EN: Loads both records and reserves a never-reused generation before
 * publishing the corresponding current authority state.
 * 中文：加载两份记录，先保留绝不复用的 generation，再发布对应当前权威状态。 */
ucn_result_t ucn_time_authority_start(ucn_time_authority_t *authority)
{
    ucn_time_generation_witness_t witness;
    ucn_time_authority_state_record_t state;
    ucn_result_t witness_result;
    ucn_result_t state_result;
    uint32_t high_water = 0U;

    if (!authority_base_is_valid(authority) || authority->io_active ||
        authority->phase != UCN_TIME_AUTHORITY_IDLE) {
        return UCN_ERR_STATE;
    }
    if (callback_gate_is_active(authority->callback_gate)) {
        return UCN_ERR_STATE;
    }
    (void)memset(&witness, 0, sizeof(witness));
    (void)memset(&state, 0, sizeof(state));
    witness_result = load_witness(authority, &witness);
    if (witness_result != UCN_OK && witness_result != UCN_ERR_NOT_FOUND) {
        return fail_closed(authority, witness_result);
    }
    state_result = load_state(authority, &state);
    if (state_result != UCN_OK && state_result != UCN_ERR_NOT_FOUND &&
        state_result != UCN_ERR_CRC) {
        return fail_closed(authority, state_result);
    }

    if (witness_result == UCN_ERR_NOT_FOUND) {
        if (state_result != UCN_ERR_NOT_FOUND ||
            !authority->config.allow_initial_commissioning ||
            authority->config.commissioned) {
            return fail_closed(authority, UCN_ERR_STATE);
        }
    } else {
        if (!witness_matches_config(&witness, &authority->config) ||
            !authority->config.commissioned) {
            return fail_closed(authority, UCN_ERR_STATE);
        }
        high_water = witness.issued_generation_high_water;
    }

    if (state_result == UCN_OK) {
        if (!state_matches_config(&state, &authority->config) ||
            witness_result != UCN_OK ||
            state.domain_generation > high_water) {
            return fail_closed(authority, UCN_ERR_STATE);
        }
    } else if (witness_result == UCN_OK && !authority->config.commissioned) {
        return fail_closed(authority, UCN_ERR_STATE);
    }

    if (high_water >= UCN_REALTIME_DOMAIN_GENERATION_MAX) {
        return fail_closed(authority, UCN_ERR_EXHAUSTED);
    }
    (void)memset(&authority->desired_witness, 0,
                 sizeof(authority->desired_witness));
    authority->desired_witness.schema = UCN_TIME_AUTHORITY_STORAGE_SCHEMA;
    authority->desired_witness.clock_domain_id =
        authority->config.clock_domain_id;
    authority->desired_witness.master_node_id = authority->config.master_node_id;
    authority->desired_witness.issued_generation_high_water = high_water + 1U;
    authority->desired_witness.commissioned = true;

    (void)memset(&authority->desired_state, 0,
                 sizeof(authority->desired_state));
    authority->desired_state.schema = UCN_TIME_AUTHORITY_STORAGE_SCHEMA;
    authority->desired_state.clock_domain_id = authority->config.clock_domain_id;
    authority->desired_state.master_node_id = authority->config.master_node_id;
    authority->desired_state.master_session_id =
        authority->config.master_session_id;
    authority->desired_state.domain_generation = high_water + 1U;
    authority->desired_state.commissioned = true;
    return begin_witness_reserve(authority);
}

/* EN: Polls exactly one pending Provider and verifies committed data by reload.
 * 中文：轮询唯一待完成 Provider，并通过回读验证已提交数据。 */
ucn_result_t ucn_time_authority_poll(ucn_time_authority_t *authority)
{
    ucn_time_persist_completion_t completion;
    ucn_result_t result;

    if (!authority_base_is_valid(authority) || authority->io_active ||
        (authority->phase != UCN_TIME_AUTHORITY_WITNESS_PENDING &&
         authority->phase != UCN_TIME_AUTHORITY_STATE_PENDING)) {
        return UCN_ERR_STATE;
    }
    if (callback_gate_is_active(authority->callback_gate)) {
        return UCN_ERR_STATE;
    }
    if (!serial_is_valid(authority->active_operation_id) ||
        !ucn_time_generation_witness_is_valid(
            &authority->desired_witness) ||
        !ucn_time_authority_state_record_is_valid(
            &authority->desired_state)) {
        return fail_closed(authority, UCN_ERR_STATE);
    }
    (void)memset(&completion, 0, sizeof(completion));
    if (!callback_enter(authority)) {
        return UCN_ERR_STATE;
    }
    if (authority->phase == UCN_TIME_AUTHORITY_WITNESS_PENDING) {
        result = authority->witness_ops->poll(
            authority->witness_context, authority->active_operation_id,
            &completion);
    } else {
        result = authority->state_ops->poll(
            authority->state_context, authority->active_operation_id,
            &completion);
    }
    callback_leave(authority);
    if (result != UCN_OK) {
        return fail_closed(authority, result);
    }
    if (authority->phase == UCN_TIME_AUTHORITY_WITNESS_PENDING) {
        return resolve_witness_completion(authority, &completion);
    }
    return resolve_state_completion(authority, &completion);
}

/* EN: Reports whether a reload-proved generation may be advertised.
 * 中文：报告是否已有可发布且经回读证明的 generation。 */
bool ucn_time_authority_is_ready(const ucn_time_authority_t *authority)
{
    return authority_base_is_valid(authority) &&
           !authority->io_active &&
           authority->phase == UCN_TIME_AUTHORITY_READY &&
           authority->active_operation_id == 0U &&
           ucn_time_generation_witness_is_valid(
               &authority->desired_witness) &&
           ucn_time_authority_state_record_is_valid(&authority->desired_state) &&
           authority->desired_witness.issued_generation_high_water ==
               authority->desired_state.domain_generation;
}

/* EN: Returns the current generation only after both durable proofs succeed.
 * 中文：仅在两项持久化证明均成功后返回当前 generation。 */
ucn_result_t ucn_time_authority_get_generation(
    const ucn_time_authority_t *authority,
    uint32_t *generation)
{
    if (generation == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_time_authority_is_ready(authority)) {
        return UCN_ERR_STATE;
    }
    *generation = authority->desired_state.domain_generation;
    return UCN_OK;
}
