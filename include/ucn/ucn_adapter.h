#ifndef UCN_ADAPTER_H
#define UCN_ADAPTER_H

#include "ucn/ucn_node.h"
#include "ucn/ucn_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This is an Adapter-owned queue, not Core state.  Products may reduce it
 * to one frame or replace it with the platform driver's bounded queue. */
#ifndef UCN_ADAPTER_RX_QUEUE_DEPTH
#define UCN_ADAPTER_RX_QUEUE_DEPTH 2U
#endif

#if UCN_ADAPTER_RX_QUEUE_DEPTH < 1
#error "UCN_ADAPTER_RX_QUEUE_DEPTH must be at least 1"
#endif

/* Six-byte MAC addresses and eight-byte EUI addresses are both representable.
 * CAN/UART adapters encode their local physical endpoint in the same field. */
#ifndef UCN_ADAPTER_PHYSICAL_ADDRESS_MAX
#define UCN_ADAPTER_PHYSICAL_ADDRESS_MAX 8U
#endif

#if UCN_ADAPTER_PHYSICAL_ADDRESS_MAX < 1
#error "UCN_ADAPTER_PHYSICAL_ADDRESS_MAX must be at least 1"
#endif

typedef struct ucn_adapter_address {
    uint8_t length;
    uint8_t bytes[UCN_ADAPTER_PHYSICAL_ADDRESS_MAX];
} ucn_adapter_address_t;

typedef struct ucn_adapter_peer_binding {
    bool occupied;
    ucn_adapter_address_t address;
    ucn_link_t *link;
} ucn_adapter_peer_binding_t;

typedef struct ucn_adapter_rx_item {
    ucn_link_t *ingress_link;
    uint16_t length;
    uint8_t data[UCN_MAX_FRAME_BYTES];
} ucn_adapter_rx_item_t;

typedef struct ucn_adapter_rx_stats {
    uint32_t enqueued;
    uint32_t dropped_full;
    uint32_t pumped;
    uint32_t rejected_by_core;
} ucn_adapter_rx_stats_t;

typedef struct ucn_adapter_rx_queue {
    ucn_adapter_rx_item_t items[UCN_ADAPTER_RX_QUEUE_DEPTH];
    size_t head;
    size_t tail;
    size_t count;
    const ucn_port_ops_t *port_ops;
    void *port_context;
    ucn_adapter_rx_stats_t stats;
} ucn_adapter_rx_queue_t;

bool ucn_adapter_address_is_valid(const ucn_adapter_address_t *address);
bool ucn_adapter_address_equal(const ucn_adapter_address_t *left,
                               const ucn_adapter_address_t *right);
ucn_adapter_peer_binding_t *ucn_adapter_find_peer(
    ucn_adapter_peer_binding_t *bindings,
    size_t binding_count,
    const ucn_adapter_address_t *address);
ucn_result_t ucn_adapter_bind_peer(ucn_adapter_peer_binding_t *bindings,
                                   size_t binding_count,
                                   const ucn_adapter_address_t *address,
                                   ucn_link_t *link);
ucn_result_t ucn_adapter_rx_queue_init(ucn_adapter_rx_queue_t *queue,
                                       const ucn_port_ops_t *port_ops,
                                       void *port_context);
ucn_result_t ucn_adapter_rx_enqueue(ucn_adapter_rx_queue_t *queue,
                                    ucn_link_t *ingress_link,
                                    const uint8_t *data,
                                    size_t length);
ucn_result_t ucn_adapter_rx_pump(ucn_adapter_rx_queue_t *queue,
                                 ucn_node_t *node,
                                 size_t max_frames,
                                 size_t *pumped);
const ucn_adapter_rx_stats_t *ucn_adapter_rx_get_stats(
    const ucn_adapter_rx_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
