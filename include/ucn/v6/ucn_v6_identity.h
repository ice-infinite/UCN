#ifndef UCN_V6_IDENTITY_H
#define UCN_V6_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_PROTOCOL_VERSION ((uint8_t)6U)
#define UCN_V6_SERIAL_ROTATION_THRESHOLD UINT32_C(0xFFFFFFFE)
#define UCN_V6_MAX_BINDING_SLOTS ((size_t)16U)
#define UCN_V6_MAX_ACTIVE_GROUPS ((size_t)8U)

typedef enum ucn_v6_result {
    UCN_V6_OK = 0,
    UCN_V6_ERR_ARGUMENT = -1,
    UCN_V6_ERR_CONFIG = -2,
    UCN_V6_ERR_NO_SPACE = -3,
    UCN_V6_ERR_MALFORMED = -4,
    UCN_V6_ERR_SECURITY = -5,
    UCN_V6_ERR_REPLAY = -6,
    UCN_V6_ERR_ACCESS = -7,
    UCN_V6_ERR_STATE = -8,
    UCN_V6_ERR_EXHAUSTED = -9,
    UCN_V6_ERR_NOT_FOUND = -10,
    UCN_V6_ERR_TIMEOUT = -11
} ucn_v6_result_t;

typedef struct ucn_v6_principal {
    uint8_t bytes[16];
} ucn_v6_principal_t;

typedef struct ucn_v6_binding_key {
    uint32_t realm_id;
    uint32_t node_address;
    uint32_t binding_generation;
} ucn_v6_binding_key_t;

typedef struct ucn_v6_session_key {
    ucn_v6_binding_key_t binding;
    ucn_v6_principal_t principal;
    uint32_t session_generation;
} ucn_v6_session_key_t;

typedef enum ucn_v6_address_mode {
    UCN_V6_ADDRESS_STATIC = 1,
    UCN_V6_ADDRESS_LEASED = 2,
    UCN_V6_ADDRESS_SELF_PROPOSED = 3
} ucn_v6_address_mode_t;

typedef struct ucn_v6_authority_epoch {
    ucn_v6_principal_t authority_principal;
    uint32_t authority_generation;
    uint8_t durable_fence_token[16];
    uint64_t lease_sequence;
    uint64_t lease_duration_us;
    uint8_t allocation_high_water_digest[16];
    bool quorum_proven;
    bool durable;
} ucn_v6_authority_epoch_t;

typedef struct ucn_v6_lease_verifier_policy {
    uint32_t local_timer_max_slow_ppm;
    uint64_t local_timer_resolution_us;
    uint64_t local_timer_read_uncertainty_us;
    bool timer_read_uncertainty_known;
    uint64_t local_policy_max_lease_us;
} ucn_v6_lease_verifier_policy_t;

typedef struct ucn_v6_binding_certificate {
    ucn_v6_principal_t device_principal;
    ucn_v6_principal_t authority_principal;
    ucn_v6_binding_key_t binding;
    uint32_t authority_generation;
    uint8_t lease_id[16];
    uint64_t lease_duration_us;
    uint64_t authority_lease_sequence;
    ucn_v6_address_mode_t mode;
} ucn_v6_binding_certificate_t;

typedef struct ucn_v6_binding_slot {
    bool occupied;
    bool active;
    uint32_t node_address;
    uint32_t generation_high_water;
    ucn_v6_binding_certificate_t certificate;
} ucn_v6_binding_slot_t;

typedef struct ucn_v6_group_allocator {
    uint32_t dynamic_group_id_high_water;
    uint32_t active_group_ids[UCN_V6_MAX_ACTIVE_GROUPS];
} ucn_v6_group_allocator_t;

typedef struct ucn_v6_callback_gate {
    void *context;
    void (*lock)(void *context);
    void (*unlock)(void *context);
    const void *active_owner;
    bool initialized;
    bool active;
} ucn_v6_callback_gate_t;

typedef struct ucn_v6_identity_store_ops {
    void *context;
    ucn_v6_result_t (*persist_authority_epoch)(
        void *context,
        const ucn_v6_authority_epoch_t *epoch);
    ucn_v6_result_t (*persist_binding_slot)(
        void *context,
        const ucn_v6_binding_slot_t *slot);
    ucn_v6_result_t (*persist_group_high_water)(
        void *context,
        uint32_t group_id_high_water);
} ucn_v6_identity_store_ops_t;

typedef struct ucn_v6_identity_authority {
    uint32_t realm_id;
    ucn_v6_authority_epoch_t epoch;
    uint64_t local_lease_deadline_us;
    ucn_v6_binding_slot_t bindings[UCN_V6_MAX_BINDING_SLOTS];
    ucn_v6_group_allocator_t groups;
    ucn_v6_identity_store_ops_t store;
    ucn_v6_callback_gate_t *callback_gate;
    bool epoch_valid;
    bool faulted;
} ucn_v6_identity_authority_t;

bool ucn_v6_principal_is_valid(const ucn_v6_principal_t *principal);
bool ucn_v6_binding_key_is_valid(const ucn_v6_binding_key_t *binding);
bool ucn_v6_binding_key_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right);
ucn_v6_result_t ucn_v6_serial_checked_next(
    uint32_t current,
    uint32_t *next);
ucn_v6_result_t ucn_v6_lease_deadline_build(
    uint64_t challenge_started_local_us,
    uint64_t max_remaining_lease_us,
    const ucn_v6_lease_verifier_policy_t *policy,
    uint64_t *local_deadline_us);
bool ucn_v6_lease_deadline_is_live(uint64_t now_us, uint64_t deadline_us);

ucn_v6_result_t ucn_v6_callback_gate_init(
    ucn_v6_callback_gate_t *gate,
    void *context,
    void (*lock)(void *context),
    void (*unlock)(void *context));

ucn_v6_result_t ucn_v6_identity_authority_init(
    ucn_v6_identity_authority_t *authority,
    uint32_t realm_id,
    const ucn_v6_identity_store_ops_t *store,
    ucn_v6_callback_gate_t *callback_gate);
ucn_v6_result_t ucn_v6_identity_authority_install_epoch(
    ucn_v6_identity_authority_t *authority,
    const ucn_v6_authority_epoch_t *epoch,
    uint64_t local_lease_deadline_us);
ucn_v6_result_t ucn_v6_identity_authority_allocate_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    const ucn_v6_principal_t *device_principal,
    ucn_v6_address_mode_t mode,
    const uint8_t lease_id[16],
    uint64_t lease_duration_us,
    ucn_v6_binding_certificate_t *certificate);
ucn_v6_result_t ucn_v6_identity_authority_retire_binding(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t node_address,
    uint32_t binding_generation);
ucn_v6_result_t ucn_v6_identity_authority_allocate_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint64_t now_us,
    uint32_t *group_id);
ucn_v6_result_t ucn_v6_identity_authority_retire_dynamic_group(
    ucn_v6_identity_authority_t *authority,
    uint32_t group_id);

#ifdef __cplusplus
}
#endif

#endif
