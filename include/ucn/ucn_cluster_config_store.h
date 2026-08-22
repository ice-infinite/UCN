#ifndef UCN_CLUSTER_CONFIG_STORE_H
#define UCN_CLUSTER_CONFIG_STORE_H

/* CLV2-07-11: caller-owned, fixed-size Config body double-slot format.
 *
 * The bytes of this object are the two independent persistence slots a BSP
 * must place in separate erase/program units.  This module gives Host tests a
 * deterministic torn-write model and verifies CRC/canonical body binding; it
 * does not perform Flash I/O, hide flash atomicity, or make a real-device
 * power-loss claim. */

#include "ucn/ucn_cluster_config_state.h"
#include "ucn/ucn_cluster_persist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT ((size_t)2U)
#define UCN_CLUSTER_CONFIG_STORE_RECORD_SCHEMA_VERSION ((uint16_t)1U)
#define UCN_CLUSTER_CONFIG_STORE_RECORD_HEADER_BYTES ((size_t)37U)
#define UCN_CLUSTER_CONFIG_STORE_RECORD_BYTES \
    (UCN_CLUSTER_CONFIG_STORE_RECORD_HEADER_BYTES + \
     UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES + (size_t)4U)

typedef struct ucn_cluster_config_store {
    uint8_t slots[UCN_CLUSTER_CONFIG_STORE_SLOT_COUNT]
                 [UCN_CLUSTER_CONFIG_STORE_RECORD_BYTES];
} ucn_cluster_config_store_t;

typedef struct ucn_cluster_config_store_recovery {
    ucn_cluster_config_state_t active_config;
    ucn_cluster_config_state_t staged_config;
    uint32_t active_generation;
    uint32_t staged_generation;
    bool has_staged_config;
} ucn_cluster_config_store_recovery_t;

void ucn_cluster_config_store_init_empty(ucn_cluster_config_store_t *store);

/* Writes/reloads one exact Stable Config body. expected_ref is checked
 * against the canonical body before any slot is replaced. Repeating the same
 * body/reference is a no-write success. */
ucn_result_t ucn_cluster_config_store_write_stable(
    ucn_cluster_config_store_t *store,
    const ucn_cluster_config_state_t *stable_config,
    const ucn_cluster_persist_config_ref_t *expected_ref);

/* Selects exactly the body named by M04 committed_config as Active. When the
 * M04 record is CONFIG_PREPARED, C_new is returned only as a non-active
 * staged body and both C_old/C_new must be present. Missing/corrupt matching
 * bodies fail closed without writing output. */
ucn_result_t ucn_cluster_config_store_recover(
    const ucn_cluster_config_store_t *store,
    const ucn_cluster_persist_state_t *durable_state,
    ucn_cluster_config_store_recovery_t *output);

#ifdef __cplusplus
}
#endif

#endif
