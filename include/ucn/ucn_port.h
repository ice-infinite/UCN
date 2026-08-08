#ifndef UCN_PORT_H
#define UCN_PORT_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_port_ops {
    uint32_t (*now_ms)(void *context);
    ucn_result_t (*random_bytes)(void *context, uint8_t *output, size_t length);
    ucn_result_t (*load_counter)(void *context, uint32_t *counter);
    ucn_result_t (*store_counter)(void *context, uint32_t counter);
    void (*enter_critical)(void *context);
    void (*exit_critical)(void *context);
} ucn_port_ops_t;

#ifdef __cplusplus
}
#endif

#endif
