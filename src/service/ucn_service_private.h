#ifndef UCN_SERVICE_PRIVATE_H
#define UCN_SERVICE_PRIVATE_H

#include "internal/ucn_service.h"

#define UCN_I_SERVICE_MAGIC UINT32_C(0x55435356)
#define UCN_I_SERVICE_REQUEST_KIND UINT8_C(0x61)
#define UCN_I_SERVICE_RECEIPT_KIND UINT8_C(0x62)
#define UCN_I_SERVICE_QOS_KIND UINT8_C(0x63)
#define UCN_I_SERVICE_OPERATION_KIND UINT8_C(0x64)

ucn_result_t ucn_i_service_p_lock(ucn_i_service_owner_t *owner);
void ucn_i_service_p_unlock(ucn_i_service_owner_t *owner);
bool ucn_i_service_p_key_valid(const ucn_i_service_key_t *key);
bool ucn_i_service_p_key_equal(const ucn_i_service_key_t *left,
                               const ucn_i_service_key_t *right);
bool ucn_i_service_p_response_key_matches(
    const ucn_i_service_key_t *request,
    const ucn_i_service_key_t *response);
ucn_handle_t ucn_i_service_p_handle(const ucn_i_service_owner_t *owner,
                                    uint16_t slot,
                                    uint16_t generation,
                                    uint8_t kind);
ucn_i_service_request_record_t *ucn_i_service_p_request(
    ucn_i_service_owner_t *owner, ucn_handle_t handle);
ucn_i_service_receipt_record_t *ucn_i_service_p_receipt(
    ucn_i_service_owner_t *owner, ucn_handle_t handle);
ucn_i_service_qos_record_t *ucn_i_service_p_qos(
    ucn_i_service_owner_t *owner, ucn_handle_t handle);
ucn_i_service_operation_record_t *ucn_i_service_p_operation(
    ucn_i_service_owner_t *owner, ucn_handle_t handle);

#endif
