#ifndef UCN_PORT_H
#define UCN_PORT_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An ISR critical-section implementation may need to restore the exact
 * interrupt mask returned when it entered.  The value is opaque to UCN and is
 * only handed back to the matching ISR exit callback. */
typedef uintptr_t ucn_port_critical_token_t;

#define UCN_PORT_OPS_API_VERSION ((uint16_t)2U)

typedef struct ucn_port_ops {
    /* Pre-release Port API v2 is intentionally source/ABI breaking.  Every
     * product must rebuild all Port/Adapter objects with the same header.
     * struct_size allows a future implementation to reject a too-short
     * prefix before it reads optional tail callbacks; api_version changes
     * only when an existing field changes meaning or layout. */
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t (*now_ms)(void *context);
    ucn_result_t (*random_bytes)(void *context, uint8_t *output, size_t length);
    ucn_result_t (*load_counter)(void *context, uint32_t *counter);
    ucn_result_t (*store_counter)(void *context, uint32_t counter);
    /* Task/owner-context Queue critical section.  These two callbacks must
     * either both be present or both be absent. */
    void (*enter_critical)(void *context);
    void (*exit_critical)(void *context);
    /* ISR Queue critical section.  These callbacks are optional as a pair,
     * but are mandatory before Adapter RX accepts from_isr input.  They are
     * deliberately separate from the task callbacks: products such as
     * FreeRTOS can map taskENTER_CRITICAL_FROM_ISR()'s returned mask to token
     * and restore that exact mask on exit. */
    ucn_port_critical_token_t (*enter_critical_from_isr)(void *context);
    void (*exit_critical_from_isr)(void *context,
                                   ucn_port_critical_token_t token);
} ucn_port_ops_t;

static inline bool ucn_port_ops_is_compatible(const ucn_port_ops_t *ops)
{
    return ops != NULL &&
           ops->struct_size >= (uint16_t)sizeof(ucn_port_ops_t) &&
           ops->api_version == UCN_PORT_OPS_API_VERSION;
}

#ifdef __cplusplus
}
#endif

#endif
