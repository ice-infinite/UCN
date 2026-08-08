#ifndef UCN_LINK_H
#define UCN_LINK_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_link_status {
    bool is_up;
    size_t mtu;
    uint32_t tx_errors;
    uint32_t rx_errors;
} ucn_link_status_t;

/*
 * 由具体 Link 把 RSSI、SNR、重传、Bus-Off、CRC 错误或队列积压等
 * 归一成代价。数值越小表示越适合作为路由路径；未提供有效指标时 v3 Core
 * 使用保守的 UCN_UNKNOWN_LINK_ROUTE_COST，避免未知链路压过已测量路径。
 */
typedef struct ucn_link_metrics {
    bool route_cost_valid;
    uint16_t route_cost;
} ucn_link_metrics_t;

typedef struct ucn_link ucn_link_t;

typedef struct ucn_link_ops {
    ucn_result_t (*open)(ucn_link_t *link);
    ucn_result_t (*send)(ucn_link_t *link, const uint8_t *frame, size_t length);
    ucn_result_t (*poll_rx)(ucn_link_t *link);
    ucn_result_t (*get_status)(const ucn_link_t *link, ucn_link_status_t *status);
    void (*close)(ucn_link_t *link);
    ucn_result_t (*get_metrics)(const ucn_link_t *link, ucn_link_metrics_t *metrics);
} ucn_link_ops_t;

struct ucn_link {
    const ucn_link_ops_t *ops;
    void *context;
    uint8_t link_id;
    size_t mtu;
    ucn_node_id_t peer_node_id;
};

#ifdef __cplusplus
}
#endif

#endif
