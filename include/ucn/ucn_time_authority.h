#ifndef UCN_TIME_AUTHORITY_H
#define UCN_TIME_AUTHORITY_H

/* Optional STATIC_MASTER generation anti-rollback startup owner.
 * 可选的 STATIC_MASTER generation 防回退启动 Owner。 */

#include "ucn/ucn_port.h"
#include "ucn/ucn_time_domain.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_TIME_AUTHORITY_STORAGE_SCHEMA ((uint16_t)1U)
#define UCN_TIME_AUTHORITY_PROVIDER_API_VERSION ((uint16_t)1U)

typedef struct ucn_time_generation_witness {
    uint16_t schema;
    uint16_t clock_domain_id;
    ucn_node_id_t master_node_id;
    uint32_t issued_generation_high_water;
    bool commissioned;
} ucn_time_generation_witness_t;

typedef struct ucn_time_authority_state_record {
    uint16_t schema;
    uint16_t clock_domain_id;
    ucn_node_id_t master_node_id;
    ucn_session_id_t master_session_id;
    uint32_t domain_generation;
    bool commissioned;
} ucn_time_authority_state_record_t;

typedef uint8_t ucn_time_persist_completion_state_t;
enum {
    UCN_TIME_PERSIST_COMPLETION_INVALID = 0U,
    UCN_TIME_PERSIST_COMPLETION_COMMITTED = 1U,
    UCN_TIME_PERSIST_COMPLETION_PENDING = 2U,
    UCN_TIME_PERSIST_COMPLETION_FAILED = 3U
};

typedef struct ucn_time_persist_completion {
    ucn_time_persist_completion_state_t state;
    uint32_t operation_id;
    ucn_result_t result;
} ucn_time_persist_completion_t;

typedef struct ucn_time_witness_ops {
    size_t struct_size;
    uint16_t api_version;
    ucn_result_t (*load)(void *context,
                         ucn_time_generation_witness_t *witness);
    ucn_result_t (*reserve)(void *context,
                            const ucn_time_generation_witness_t *witness,
                            uint32_t operation_id,
                            ucn_time_persist_completion_t *completion);
    ucn_result_t (*poll)(void *context,
                         uint32_t operation_id,
                         ucn_time_persist_completion_t *completion);
} ucn_time_witness_ops_t;

typedef struct ucn_time_authority_state_ops {
    size_t struct_size;
    uint16_t api_version;
    ucn_result_t (*load)(void *context,
                         ucn_time_authority_state_record_t *record);
    ucn_result_t (*store)(void *context,
                          const ucn_time_authority_state_record_t *record,
                          uint32_t operation_id,
                          ucn_time_persist_completion_t *completion);
    ucn_result_t (*poll)(void *context,
                         uint32_t operation_id,
                         ucn_time_persist_completion_t *completion);
} ucn_time_authority_state_ops_t;

typedef uint8_t ucn_time_authority_phase_t;
enum {
    UCN_TIME_AUTHORITY_IDLE = 0U,
    UCN_TIME_AUTHORITY_WITNESS_PENDING = 1U,
    UCN_TIME_AUTHORITY_STATE_PENDING = 2U,
    UCN_TIME_AUTHORITY_READY = 3U,
    UCN_TIME_AUTHORITY_FAULT = 4U
};

typedef struct ucn_time_authority_config {
    uint16_t clock_domain_id;
    ucn_node_id_t master_node_id;
    ucn_session_id_t master_session_id;
    bool commissioned;
    bool allow_initial_commissioning;
} ucn_time_authority_config_t;

/* EN: One execution domain owns one task/SMP-safe Provider callback gate.
 * Every Time Authority that may run concurrently must reference this same
 * caller-owned gate.  Provider callbacks are task-context operations; ISR
 * entry is not part of the Time Authority Provider contract.
 * 中文：一个执行域拥有一个任务/SMP 安全的 Provider 回调围栏。所有可能并发
 * 运行的 Time Authority 必须引用同一个调用方持有的围栏。Provider 回调属于
 * 任务上下文操作，Time Authority Provider 合同不包含 ISR 入口。 */
typedef struct ucn_time_authority_callback_gate {
    const ucn_port_ops_t *port_ops;
    void *port_context;
    const void *active_owner;
    bool initialized;
    bool active;
} ucn_time_authority_callback_gate_t;

/* The witness and state Providers are intentionally separate contracts.
 * Product integration MUST place them in independent anti-rollback/storage
 * failure domains as specified by RT-00A; two C callbacks alone cannot prove
 * physical independence.  Witness load must distinguish erased
 * UCN_ERR_NOT_FOUND from authenticated corruption UCN_ERR_CRC.  The state
 * Provider may report UCN_ERR_CRC and be rebuilt only when a valid witness
 * plus trusted commissioned configuration survives.
 *
 * witness 与 state Provider 有意拆为两个合同。产品集成必须依照 RT-00A
 * 将其放在独立的防回退/存储故障域；两个 C 回调本身不能证明物理独立性。
 * witness load 必须区分擦除态 UCN_ERR_NOT_FOUND 与认证损坏 UCN_ERR_CRC；
 * state Provider 仅在有效 witness 和可信 commissioned 配置仍存在时才可
 * 通过 UCN_ERR_CRC 路径重建。 */

typedef struct ucn_time_authority {
    ucn_time_authority_config_t config;
    const ucn_time_witness_ops_t *witness_ops;
    void *witness_context;
    const ucn_time_authority_state_ops_t *state_ops;
    void *state_context;
    ucn_time_authority_callback_gate_t *callback_gate;
    ucn_time_generation_witness_t desired_witness;
    ucn_time_authority_state_record_t desired_state;
    uint32_t active_operation_id;
    uint32_t next_operation_id;
    ucn_result_t last_failure;
    ucn_time_authority_phase_t phase;
    bool initialized;
    bool io_active;
} ucn_time_authority_t;

/* These functions belong to one serialized Owner context. Provider callbacks
 * may re-enter for fault injection, but any nested or concurrent Provider
 * operation across Authorities that share one gate fails closed. Callers must
 * not use one Authority object concurrently from multiple tasks/ISRs.
 *
 * 这些函数属于单一串行 Owner 上下文。Provider 回调可为故障注入而重入，
 * 但共享同一围栏的 Authority 之间，嵌套或并发 Provider 操作会失败关闭；
 * 调用方不得从多个任务/ISR 并发操作同一 Authority 对象。 */

/* EN: Initializes the caller-owned gate before any Authority references it.
 * The Port task critical-section callbacks must protect one common lock. Once
 * referenced, the gate must not be moved or reinitialized.
 * 中文：在任何 Authority 引用前初始化调用方持有的围栏。Port 的任务临界区
 * 回调必须保护同一个公共锁；围栏一旦被引用就不得移动或重新初始化。 */
ucn_result_t ucn_time_authority_callback_gate_init(
    ucn_time_authority_callback_gate_t *gate,
    const ucn_port_ops_t *port_ops,
    void *port_context);

/* EN: Validates the independent anti-rollback high-water witness.
 * 中文：校验独立的防回退高水位见证记录。 */
bool ucn_time_generation_witness_is_valid(
    const ucn_time_generation_witness_t *witness);
/* EN: Validates one reconstructible time-authority state record.
 * 中文：校验一条可重建的时间权威状态记录。 */
bool ucn_time_authority_state_record_is_valid(
    const ucn_time_authority_state_record_t *record);
/* EN: Validates a nonzero Provider completion for one exact operation.
 * 中文：校验与指定操作精确绑定的非零 Provider 完成结果。 */
bool ucn_time_persist_completion_is_valid(
    const ucn_time_persist_completion_t *completion,
    uint32_t expected_operation_id);

/* EN: Initializes an idle Owner without performing Provider I/O.
 * 中文：初始化空闲 Owner，但不执行任何 Provider I/O。 */
ucn_result_t ucn_time_authority_init(
    ucn_time_authority_t *authority,
    const ucn_time_authority_config_t *config,
    const ucn_time_witness_ops_t *witness_ops,
    void *witness_context,
    const ucn_time_authority_state_ops_t *state_ops,
    void *state_context,
    ucn_time_authority_callback_gate_t *callback_gate);
/* EN: Starts witness-first generation reservation and state publication.
 * 中文：启动见证优先的 generation 预留与状态发布流程。 */
ucn_result_t ucn_time_authority_start(ucn_time_authority_t *authority);
/* EN: Advances one pending asynchronous Provider operation.
 * 中文：推进一个尚未完成的异步 Provider 操作。 */
ucn_result_t ucn_time_authority_poll(ucn_time_authority_t *authority);
/* EN: Reports whether witness and state were both reloaded and verified.
 * 中文：报告见证与状态是否均已回读并验证。 */
bool ucn_time_authority_is_ready(const ucn_time_authority_t *authority);
/* EN: Returns the published generation only from the READY state.
 * 中文：仅在 READY 状态返回已发布的 generation。 */
ucn_result_t ucn_time_authority_get_generation(
    const ucn_time_authority_t *authority,
    uint32_t *generation);

#ifdef __cplusplus
}
#endif

#endif
