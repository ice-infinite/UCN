#ifndef UCN_LINK_H
#define UCN_LINK_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_link_status {
    bool is_up;
    size_t mtu;
    uint32_t tx_errors;
    uint32_t rx_errors;
} ucn_link_status_t;

#define UCN_LINK_METRIC_PER_MILLE_MAX ((uint16_t)1000U)
#define UCN_LINK_ROUTE_COST_UNKNOWN UINT16_MAX
#define UCN_LINK_ROUTE_COST_MAX ((uint16_t)(UINT16_MAX - 1U))

/*
 * Link Metrics contract:
 *
 * - route_cost is a positive, additive base route cost in UCN cost units.
 *   Smaller is preferred by AODV-Lite.  It may encode stable media/product
 *   preference (for example a preferred wired Bearer), but MUST NOT include
 *   the instantaneous RTT, failure-rate or queue-pressure samples reported
 *   below.  A future multi-metric Policy can therefore combine each signal
 *   exactly once.  Valid values are 1..UCN_LINK_ROUTE_COST_MAX.
 *   route_cost_valid=false, cost 0, or the reserved
 *   UCN_LINK_ROUTE_COST_UNKNOWN value means unknown.  A known Cost always
 *   sorts before an unknown Cost; UINT16_MAX is never a valid product Cost.
 * - rtt_ms is the direct Link round-trip sample, in whole milliseconds.
 *   It is not an end-to-end routed-path RTT; zero is permitted for a
 *   sub-millisecond/virtual Link sample.
 * - tx_failure_per_mille is the Adapter's outbound Link failure ratio for
 *   its declared sampling window, in [0, UCN_LINK_METRIC_PER_MILLE_MAX].
 *   It is not an application or end-to-end delivery loss ratio.
 * - queue_pressure_per_mille is the Adapter's own outbound queue occupancy
 *   ratio in the same range.  It MUST NOT report UCN Core Q0/Q1 occupancy.
 *
 * Each valid bit controls only its own metric.  For route_cost, zero and the
 * reserved UINT16_MAX sentinel are also rejected even if its valid bit was
 * set.  Callers zero-initialize this structure before invoking an older
 * Adapter, preserving the original route-cost-only contract.
 */
typedef struct ucn_link_metrics {
    bool route_cost_valid;
    uint16_t route_cost;
    bool rtt_valid;
    uint16_t rtt_ms;
    bool tx_failure_rate_valid;
    uint16_t tx_failure_per_mille;
    bool queue_pressure_valid;
    uint16_t queue_pressure_per_mille;
} ucn_link_metrics_t;

typedef struct ucn_link ucn_link_t;

typedef struct ucn_link_ops {
    ucn_result_t (*open)(ucn_link_t *link);
    /* Production contract: perform only bounded copy/enqueue work and return;
     * do not wait for physical transmission completion.  A full Adapter TX
     * queue returns UCN_ERR_NO_SPACE and a down Driver returns
     * UCN_ERR_LINK_DOWN.  Synchronous delivery is permitted only for bounded
     * virtual/test Links. */
    ucn_result_t (*send)(ucn_link_t *link, const uint8_t *frame, size_t length);
    ucn_result_t (*poll_rx)(ucn_link_t *link);
    ucn_result_t (*get_status)(const ucn_link_t *link, ucn_link_status_t *status);
    void (*close)(ucn_link_t *link);
    ucn_result_t (*get_metrics)(const ucn_link_t *link, ucn_link_metrics_t *metrics);
} ucn_link_ops_t;

struct ucn_link {
    const ucn_link_ops_t *ops;
    void *context;
    uint8_t link_id;
    size_t mtu;
    ucn_node_id_t peer_node_id;
};

#ifdef __cplusplus
}
#endif

#endif
