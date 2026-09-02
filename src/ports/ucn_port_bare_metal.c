#include "ucn/ports/ucn_port_bare_metal.h"

/*
 * EN: Initializes the bare-metal Port object from validated caller-owned configuration without heap allocation.
 * 中文：使用经验证的调用方配置初始化 裸机 Port 对象，且不使用堆内存。
 */
ucn_result_t ucn_bare_metal_port_init(
    ucn_bare_metal_port_t *port,
    const ucn_protocol_owner_config_t *config)
{
    return port == NULL ? UCN_ERR_ARGUMENT :
                          ucn_protocol_owner_init(&port->owner, config);
}

/*
 * EN: Copies `rx_enqueue` into a bounded bare-metal Port queue.
 * 中文：把 `rx_enqueue` 复制到固定容量的 裸机 Port 队列。
 */
ucn_result_t ucn_bare_metal_port_rx_enqueue(
    ucn_bare_metal_port_t *port,
    ucn_link_t *ingress_link,
    const uint8_t *data,
    size_t length)
{
    return port == NULL ? UCN_ERR_ARGUMENT :
                          ucn_protocol_owner_rx_enqueue(&port->owner, ingress_link,
                                                        data, length);
}

/*
 * EN: Processes one bounded batch of `poll` work for bare-metal Port.
 * 中文：为 裸机 Port 处理一批有界的 `poll` 工作。
 */
ucn_result_t ucn_bare_metal_port_poll(
    ucn_bare_metal_port_t *port,
    size_t *pumped,
    uint8_t *bridged)
{
    return port == NULL ? UCN_ERR_ARGUMENT :
                          ucn_protocol_owner_step(&port->owner, pumped, bridged);
}

const ucn_protocol_owner_stats_t *
/*
 * EN: Returns the current `stats` view from bare-metal Port state.
 * 中文：从 裸机 Port 状态返回当前 `stats` 视图。
 */
ucn_bare_metal_port_get_stats(const ucn_bare_metal_port_t *port)
{
    return port == NULL ? NULL : ucn_protocol_owner_get_stats(&port->owner);
}
