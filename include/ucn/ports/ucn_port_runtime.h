#ifndef UCN_PORTS_RUNTIME_H
#define UCN_PORTS_RUNTIME_H

#include "ucn/ports/ucn_protocol_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared shape only; each platform header declares its own named callback
 * contract.  These counters are maintained by the selected platform wrapper,
 * never by the Protocol Owner. */
typedef struct ucn_port_runtime_stats {
    uint32_t notifications;
    uint32_t notifications_from_isr;
    uint32_t waits;
    uint32_t last_wait_ms;
} ucn_port_runtime_stats_t;

#ifdef __cplusplus
}
#endif

#endif
