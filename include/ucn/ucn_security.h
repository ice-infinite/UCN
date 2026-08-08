#ifndef UCN_SECURITY_H
#define UCN_SECURITY_H

#include "ucn/ucn_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ucn_security_tx_mode {
    UCN_SECURITY_TX_PLAIN = 0,
    UCN_SECURITY_TX_E2E_PROTECTED = 1,
    UCN_SECURITY_TX_AUTO = 2
} ucn_security_tx_mode_t;

typedef enum ucn_security_rx_mode {
    UCN_SECURITY_RX_PLAIN_ONLY = 0,
    UCN_SECURITY_RX_ENCRYPTED_ONLY = 1,
    UCN_SECURITY_RX_BOTH = 2
} ucn_security_rx_mode_t;

typedef enum ucn_security_forward_mode {
    UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E = 0,
    UCN_SECURITY_FORWARD_OPAQUE_E2E_ONLY = 1,
    UCN_SECURITY_FORWARD_TERMINAL_ONLY = 2
} ucn_security_forward_mode_t;

typedef struct ucn_security_policy {
    ucn_security_tx_mode_t tx_mode;
    ucn_security_rx_mode_t rx_mode;
    ucn_security_forward_mode_t forward_mode;
} ucn_security_policy_t;

/*
 * 安全 Provider 由产品接入。Core 不实现密码算法：生产 Provider 必须使用
 * 经审计的 AEAD 实现。seal/open 的 AAD 由 ucn_frame_write_e2e_aad() 生成；
 * 它仅含不可变路由身份字段，故中继可修改 Hop Limit/Route Epoch 后透明转发。
 */
typedef struct ucn_security_ops {
    ucn_result_t (*load_next_sequence)(void *context, ucn_sequence_t *next_sequence);
    ucn_result_t (*store_next_sequence)(void *context, ucn_sequence_t next_sequence);
    ucn_result_t (*get_session_id)(void *context, ucn_session_id_t *session_id);
    ucn_result_t (*authorize_tx)(void *context, const ucn_frame_t *frame);
    ucn_result_t (*authorize_rx)(void *context,
                                 const ucn_link_t *ingress_link,
                                 const ucn_frame_t *frame);
    ucn_result_t (*select_tx_protection)(void *context,
                                         const ucn_frame_t *frame,
                                         bool *protected_frame);
    ucn_result_t (*seal)(void *context,
                         const ucn_frame_t *frame,
                         const uint8_t *plaintext,
                         uint16_t plaintext_length,
                         uint8_t *ciphertext,
                         uint8_t auth_tag[UCN_E2E_TAG_SIZE]);
    ucn_result_t (*open)(void *context,
                         const ucn_link_t *ingress_link,
                         const ucn_frame_t *frame,
                         const uint8_t *ciphertext,
                         uint16_t ciphertext_length,
                         const uint8_t auth_tag[UCN_E2E_TAG_SIZE],
                         uint8_t *plaintext);
} ucn_security_ops_t;

#ifdef __cplusplus
}
#endif

#endif
