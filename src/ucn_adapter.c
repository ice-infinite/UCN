#include <string.h>

#include "ucn/ucn_adapter.h"

static void queue_enter(ucn_adapter_rx_queue_t *queue)
{
    if (queue->port_ops != NULL) {
        queue->port_ops->enter_critical(queue->port_context);
    }
}

static void queue_exit(ucn_adapter_rx_queue_t *queue)
{
    if (queue->port_ops != NULL) {
        queue->port_ops->exit_critical(queue->port_context);
    }
}

bool ucn_adapter_address_is_valid(const ucn_adapter_address_t *address)
{
    return address != NULL && address->length != 0U &&
           address->length <= UCN_ADAPTER_PHYSICAL_ADDRESS_MAX;
}

bool ucn_adapter_address_equal(const ucn_adapter_address_t *left,
                               const ucn_adapter_address_t *right)
{
    if (!ucn_adapter_address_is_valid(left) ||
        !ucn_adapter_address_is_valid(right) || left->length != right->length) {
        return false;
    }
    return memcmp(left->bytes, right->bytes, left->length) == 0;
}

ucn_adapter_peer_binding_t *ucn_adapter_find_peer(
    ucn_adapter_peer_binding_t *bindings,
    size_t binding_count,
    const ucn_adapter_address_t *address)
{
    size_t index;

    if (bindings == NULL || !ucn_adapter_address_is_valid(address)) {
        return NULL;
    }
    for (index = 0U; index < binding_count; ++index) {
        if (bindings[index].occupied &&
            ucn_adapter_address_equal(&bindings[index].address, address)) {
            return &bindings[index];
        }
    }
    return NULL;
}

ucn_result_t ucn_adapter_bind_peer(ucn_adapter_peer_binding_t *bindings,
                                   size_t binding_count,
                                   const ucn_adapter_address_t *address,
                                   ucn_link_t *link)
{
    ucn_adapter_peer_binding_t *free_slot = NULL;
    size_t index;

    if (bindings == NULL || binding_count == 0U ||
        !ucn_adapter_address_is_valid(address) || link == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    for (index = 0U; index < binding_count; ++index) {
        if (bindings[index].occupied &&
            ucn_adapter_address_equal(&bindings[index].address, address)) {
            return bindings[index].link == link ? UCN_OK : UCN_ERR_CONFIG;
        }
        if (!bindings[index].occupied && free_slot == NULL) {
            free_slot = &bindings[index];
        }
    }

    if (free_slot == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    free_slot->occupied = true;
    free_slot->address = *address;
    free_slot->link = link;
    return UCN_OK;
}

ucn_result_t ucn_adapter_rx_queue_init(ucn_adapter_rx_queue_t *queue,
                                       const ucn_port_ops_t *port_ops,
                                       void *port_context)
{
    if (queue == NULL ||
        (port_ops != NULL && (port_ops->enter_critical == NULL ||
                              port_ops->exit_critical == NULL))) {
        return UCN_ERR_ARGUMENT;
    }

    (void)memset(queue, 0, sizeof(*queue));
    queue->port_ops = port_ops;
    queue->port_context = port_context;
    return UCN_OK;
}

ucn_result_t ucn_adapter_rx_enqueue(ucn_adapter_rx_queue_t *queue,
                                    ucn_link_t *ingress_link,
                                    const uint8_t *data,
                                    size_t length)
{
    ucn_adapter_rx_item_t *item;

    if (queue == NULL || ingress_link == NULL || data == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (length > UCN_MAX_FRAME_BYTES || length > UINT16_MAX) {
        return UCN_ERR_TOO_LARGE;
    }

    queue_enter(queue);
    if (queue->count >= UCN_ADAPTER_RX_QUEUE_DEPTH) {
        queue->stats.dropped_full++;
        queue_exit(queue);
        return UCN_ERR_NO_SPACE;
    }

    item = &queue->items[queue->tail];
    item->ingress_link = ingress_link;
    item->length = (uint16_t)length;
    if (length != 0U) {
        (void)memcpy(item->data, data, length);
    }
    queue->tail = (queue->tail + 1U) % UCN_ADAPTER_RX_QUEUE_DEPTH;
    queue->count++;
    queue->stats.enqueued++;
    queue_exit(queue);
    return UCN_OK;
}

ucn_result_t ucn_adapter_rx_pump(ucn_adapter_rx_queue_t *queue,
                                 ucn_node_t *node,
                                 size_t max_frames,
                                 size_t *pumped)
{
    size_t local_pumped = 0U;

    if (queue == NULL || node == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    while (local_pumped < max_frames) {
        ucn_adapter_rx_item_t item;
        ucn_result_t result;

        queue_enter(queue);
        if (queue->count == 0U) {
            queue_exit(queue);
            break;
        }
        item = queue->items[queue->head];
        queue->head = (queue->head + 1U) % UCN_ADAPTER_RX_QUEUE_DEPTH;
        queue->count--;
        queue_exit(queue);

        result = ucn_node_receive(node, item.ingress_link, item.data, item.length);
        queue->stats.pumped++;
        if (result != UCN_OK) {
            queue->stats.rejected_by_core++;
        }
        local_pumped++;
    }

    if (pumped != NULL) {
        *pumped = local_pumped;
    }
    return UCN_OK;
}

const ucn_adapter_rx_stats_t *ucn_adapter_rx_get_stats(
    const ucn_adapter_rx_queue_t *queue)
{
    return queue == NULL ? NULL : &queue->stats;
}
