#ifndef UCN_DRIVER_H
#define UCN_DRIVER_H

#include "ucn/ucn_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t ucn_driver_submit_result_t;
enum {
    UCN_DRIVER_NOT_SUBMITTED = 0,
    UCN_DRIVER_SUBMITTED = 1,
    UCN_DRIVER_COMPLETE = 2,
    UCN_DRIVER_SUBMIT_UNKNOWN = 3
};

typedef uint8_t ucn_driver_link_event_t;
enum {
    UCN_DRIVER_LINK_UP = 1,
    UCN_DRIVER_LINK_DOWN = 2,
    UCN_DRIVER_LINK_REOPENED = 3
};

typedef struct ucn_rx_meta {
    uint16_t struct_size;
    uint16_t api_version;
    uint64_t timestamp_us;
    uint32_t sender_discriminator;
    uint32_t reserved_zero;
} ucn_rx_meta_t;

typedef struct ucn_tx_meta {
    uint16_t struct_size;
    uint16_t api_version;
    uint64_t timestamp_us;
    uint32_t reserved_zero;
} ucn_tx_meta_t;

typedef struct ucn_link_event_meta {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t new_link_instance;
    uint32_t reserved_zero;
} ucn_link_event_meta_t;

typedef ucn_driver_submit_result_t (*ucn_tx_submit_fn)(
    void *context,
    ucn_link_handle_t link,
    const uint8_t *bytes,
    size_t length,
    ucn_driver_token_t token);
typedef ucn_result_t (*ucn_tx_cancel_fn)(void *context,
                                        ucn_driver_token_t token);

typedef struct ucn_tx_port_vtable {
    uint16_t struct_size;
    uint16_t api_version;
    ucn_tx_submit_fn submit;
    ucn_tx_cancel_fn cancel;
} ucn_tx_port_vtable_t;

struct ucn_node;
typedef struct ucn_node ucn_node_t;

ucn_result_t ucn_driver_rx_publish(ucn_node_t *node,
                                   ucn_link_handle_t link,
                                   const uint8_t *bytes,
                                   size_t length,
                                   const ucn_rx_meta_t *meta);
ucn_result_t ucn_driver_tx_complete(ucn_node_t *node,
                                    ucn_driver_token_t token,
                                    ucn_result_t result,
                                    const ucn_tx_meta_t *meta);
ucn_result_t ucn_driver_link_event(ucn_node_t *node,
                                   ucn_link_handle_t link,
                                   ucn_driver_link_event_t event,
                                   const ucn_link_event_meta_t *meta);

#ifdef __cplusplus
}
#endif

#endif
