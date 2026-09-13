#ifndef UCN_PRODUCT_H
#define UCN_PRODUCT_H

#include "ucn/ucn_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef ucn_result_t (*ucn_lock_enter_fn)(void *context);
typedef void (*ucn_lock_leave_fn)(void *context);

typedef struct ucn_lock_ops {
    uint16_t struct_size;
    uint16_t api_version;
    void *context;
    ucn_lock_enter_fn enter;
    ucn_lock_leave_fn leave;
} ucn_lock_ops_t;

typedef struct ucn_link_port {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t link_instance;
    uint16_t frame_mtu;
    uint16_t reserved_zero;
    void *context;
    ucn_tx_port_vtable_t tx;
} ucn_link_port_t;

typedef struct ucn_ports {
    uint16_t struct_size;
    uint16_t api_version;
    const ucn_link_port_t *links;
    uint16_t link_count;
    uint16_t reserved_zero;
    /* EN: Short task/ISR/SMP-safe lock for Adapter-owned state. It must not
     * be held across a Driver callback.
     * 中文：保护 Adapter 自有状态的短临界区锁，不能跨 Driver 回调持有。 */
    ucn_lock_ops_t state_lock;
    /* EN: Non-blocking, caller-owned gate shared by every Runtime that can
     * enter the same Driver callback domain. enter() is try-enter: contention
     * returns an error instead of waiting. This gate is held across
     * submit/cancel and must therefore be distinct from state_lock.
     * 中文：由调用方持有、供同一 Driver 回调域内所有 Runtime 共享的非阻塞门。
     * enter() 必须是 try-enter，竞争时返回错误而不是等待；该门会跨
     * submit/cancel 持有，因此必须与 state_lock 分离。 */
    ucn_lock_ops_t driver_callback_gate;
} ucn_ports_t;

#ifdef __cplusplus
}
#endif

#endif
