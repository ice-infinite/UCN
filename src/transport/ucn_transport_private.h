#ifndef UCN_TRANSPORT_PRIVATE_H
#define UCN_TRANSPORT_PRIVATE_H

#include "internal/ucn_transport.h"

#define UCN_I_TRANSPORT_MAGIC UINT32_C(0x55435452)
#define UCN_I_TRANSPORT_RELIABLE_TX_KIND UINT8_C(0x51)
#define UCN_I_TRANSPORT_TRANSFER_TX_KIND UINT8_C(0x52)
#define UCN_I_TRANSPORT_TRANSFER_RX_KIND UINT8_C(0x53)
#define UCN_I_TRANSPORT_PARENT_KIND UINT8_C(0x54)
#define UCN_I_TRANSPORT_TRANSFER_RECEIPT_KIND UINT8_C(0x55)

bool ucn_i_transport_p_lock_valid(const ucn_i_lock_ops_t *lock);
ucn_result_t ucn_i_transport_p_lock(ucn_i_transport_owner_t *owner);
void ucn_i_transport_p_unlock(ucn_i_transport_owner_t *owner);
bool ucn_i_transport_p_bytes_nonzero(const uint8_t *bytes, size_t length);
bool ucn_i_transport_p_key_valid(
    const ucn_i_transport_reliable_key_t *key);
bool ucn_i_transport_p_security_valid(
    const ucn_i_transport_security_facts_t *security);
bool ucn_i_transport_p_key_equal(
    const ucn_i_transport_reliable_key_t *left,
    const ucn_i_transport_reliable_key_t *right);
bool ucn_i_transport_p_setup_equal(
    const ucn_i_transport_transfer_setup_t *left,
    const ucn_i_transport_transfer_setup_t *right);
ucn_i_transport_reliable_tx_record_t *ucn_i_transport_p_reliable_tx(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_i_transport_transfer_tx_record_t *ucn_i_transport_p_transfer_tx(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_i_transport_transfer_rx_record_t *ucn_i_transport_p_transfer_rx(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_i_transport_terminal_receipt_record_t *
ucn_i_transport_p_transfer_receipt(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_i_transport_parent_record_t *ucn_i_transport_p_parent(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle);
ucn_handle_t ucn_i_transport_p_handle(
    const ucn_i_transport_owner_t *owner,
    uint16_t slot,
    uint16_t generation,
    uint8_t kind);

#endif
