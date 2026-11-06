#include "internal/ucn_runtime.h"

/* A prepared frame is submitted after the encoder's frame has unwound.
 * 编码器返回后才提交同一帧，原序号/预算/截止期不变。 */
bool ucn_i_node_tx_submit(ucn_node_t *node, uint16_t slot,
                               ucn_i_tx_action_t *action)
{
    ucn_i_tx_slot_t *tx;
    ucn_i_adapter_tx_view_t view;
    ucn_result_t result;
    if (node == NULL || action == NULL || slot >= UCN_TX_SLOT_COUNT)
        return false;
    tx = &node->tx_slots[slot];
    if (tx->state != UCN_I_TX_QUEUED || tx->encoded == 0U ||
        tx->frame_bytes == 0U || tx->frame_bytes > sizeof(tx->frame))
        return false;
    action->kind = 0U;
    result = ucn_i_adapter_tx_submit_ref(&node->adapter, &tx->adapter_token,
                                         tx->frame, tx->frame_bytes, &view);
    if (result != UCN_OK) {
        if (ucn_i_adapter_tx_view(&node->adapter, &(tx->adapter_token),
                                  &view) != UCN_OK) {
            node->faulted = 1U;
            node->lifecycle = UCN_LIFECYCLE_FAULT;
            return true;
        }
        if (view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
            !view.terminal_latched) {
            action->kind = 2U; action->slot = slot;
            return true;
        }
        if (view.state == UCN_I_ADAPTER_TX_COMPLETED ||
            view.state == UCN_I_ADAPTER_TX_CANCELLED ||
            (view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
             view.terminal_latched)) {
            action->kind = 1U; action->slot = slot; action->result = view.terminal_result;
            return true;
        }
        if (view.state == UCN_I_ADAPTER_TX_IN_DOUBT ||
            view.state == UCN_I_ADAPTER_TX_SUBMITTED) {
            tx->state = UCN_I_TX_SUBMITTED;
            if (tx->tracked) {
                uint16_t receipt_slot =
                    node->requests[tx->request_slot].receipt_slot;
                node->receipts[receipt_slot].view.link_outcome =
                    view.state == UCN_I_ADAPTER_TX_SUBMITTED
                        ? UCN_LINK_OUTCOME_SUBMITTED
                        : UCN_LINK_OUTCOME_IN_DOUBT;
            }
            return true;
        }
        if (result == UCN_ERR_STATE &&
            view.state == UCN_I_ADAPTER_TX_RESERVED) {
            return false;
        }
        node->faulted = 1U;
        node->lifecycle = UCN_LIFECYCLE_FAULT;
        return true;
    }
    if (view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
        !view.terminal_latched) {
        action->kind = 2U; action->slot = slot;
        return true;
    }
    tx->state = UCN_I_TX_SUBMITTED;
    if (view.state == UCN_I_ADAPTER_TX_COMPLETED ||
        view.state == UCN_I_ADAPTER_TX_CANCELLED ||
        (view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
         view.terminal_latched)) {
        action->kind = 1U; action->slot = slot; action->result = view.terminal_result;
    } else if (tx->tracked) {
        uint16_t receipt_slot =
            node->requests[tx->request_slot].receipt_slot;
        node->receipts[receipt_slot].view.link_outcome =
            view.state == UCN_I_ADAPTER_TX_SUBMITTED
                ? UCN_LINK_OUTCOME_SUBMITTED
                : UCN_LINK_OUTCOME_IN_DOUBT;
    }
    return true;
}
