#ifndef UCN_COORDINATOR_H
#define UCN_COORDINATOR_H

#include "internal/ucn_owner.h"

#define UCN_I_COORDINATOR_SCHEMA UINT16_C(1)
#define UCN_I_COORDINATOR_MAGIC UINT32_C(0x5543434F)
#define UCN_I_DEPENDENCY_EXACT_BYTES 32U
#define UCN_I_DEPENDENCY_KIND_COUNT 9U
#define UCN_I_COORDINATOR_PENDING_LIMIT 4U

typedef uint8_t ucn_i_dependency_kind_t;
enum {
    UCN_I_DEP_IDENTITY_BINDING = 1,
    UCN_I_DEP_SECURITY_SESSION = 2,
    UCN_I_DEP_CAPABILITY_REFRESH = 3,
    UCN_I_DEP_SOFT_ROUTE = 4,
    UCN_I_DEP_FLOW = 5,
    UCN_I_DEP_TRANSFER = 6,
    UCN_I_DEP_GROUP = 7,
    UCN_I_DEP_TIME_DOMAIN = 8,
    UCN_I_DEP_PERSISTENCE = 9
};

typedef uint8_t ucn_i_owner_id_t;
enum {
    UCN_I_OWNER_COORDINATOR = 1,
    UCN_I_OWNER_IDENTITY = 2,
    UCN_I_OWNER_SECURITY = 3,
    UCN_I_OWNER_CAPABILITY = 4,
    UCN_I_OWNER_ROUTE = 5,
    UCN_I_OWNER_FLOW = 6,
    UCN_I_OWNER_TRANSPORT = 7,
    UCN_I_OWNER_GROUP = 8,
    UCN_I_OWNER_TIME = 9,
    UCN_I_OWNER_PERSISTENCE = 10
};

typedef struct ucn_i_dependency_requirement {
    uint64_t requirement_digest;
    uint64_t policy_digest;
    uint64_t absolute_deadline_us;
    uint32_t runtime_instance;
    uint16_t requester_owner_instance;
    uint8_t kind;
    uint8_t exact_length;
    uint8_t exact[UCN_I_DEPENDENCY_EXACT_BYTES];
} ucn_i_dependency_requirement_t;

/* exact[] is an already-canonical, kind-specific body produced by the target
 * module's typed builder. The Coordinator compares it but never interprets it. */

typedef uint64_t (*ucn_i_requirement_digest_fn)(const uint8_t *bytes,
                                                size_t length);

typedef ucn_result_t (*ucn_i_dependency_ensure_fn)(
    void *context,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out);

typedef ucn_result_t (*ucn_i_dependency_retire_fn)(
    void *context,
    const ucn_handle_t *handle);

typedef uint8_t ucn_i_dependency_outcome_t;
enum {
    UCN_I_DEPENDENCY_READY = 1,
    UCN_I_DEPENDENCY_FAILED = 2,
    UCN_I_DEPENDENCY_FENCED = 3
};

typedef struct ucn_i_dependency_event {
    uint64_t requirement_digest;
    uint32_t runtime_instance;
    ucn_result_t result;
    ucn_handle_t dependency_handle;
    uint16_t owner_instance;
    uint8_t dependency_kind;
    uint8_t outcome;
} ucn_i_dependency_event_t;

typedef ucn_result_t (*ucn_i_dependency_event_sink_fn)(
    void *context,
    uint16_t requester_owner_instance,
    const ucn_i_dependency_requirement_t *requirement,
    const ucn_i_dependency_event_t *event);

typedef struct ucn_i_owner_binding {
    void *context;
    ucn_i_dependency_ensure_fn ensure;
    ucn_i_dependency_retire_fn retire;
    uint16_t owner_instance;
    uint16_t slot_limit;
    uint8_t owner_id;
    uint8_t object_kind;
    uint16_t reserved_zero;
} ucn_i_owner_binding_t;

typedef struct ucn_i_coordinator_slot {
    ucn_i_dependency_requirement_t requirement;
    ucn_handle_t dependency_handle;
    ucn_i_dependency_event_t terminal_event;
    uint8_t valid;
    uint8_t terminal_state;
    uint8_t reserved_zero[2];
} ucn_i_coordinator_slot_t;

typedef struct ucn_i_coordinator {
    uint32_t magic;
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t schema;
    uint8_t route_active;
    uint8_t faulted;
    uint16_t reserved_zero;
    void *event_sink_context;
    ucn_i_requirement_digest_fn digest;
    ucn_i_dependency_event_sink_fn event_sink;
    ucn_i_lock_ops_t lock;
    ucn_i_owner_binding_t bindings[UCN_I_DEPENDENCY_KIND_COUNT];
    ucn_i_coordinator_slot_t slots[UCN_I_COORDINATOR_PENDING_LIMIT];
} ucn_i_coordinator_t;

uint64_t ucn_i_requirement_digest_default(const uint8_t *bytes,
                                          size_t length);
ucn_result_t ucn_i_dependency_requirement_build(
    ucn_i_requirement_digest_fn digest,
    ucn_i_dependency_kind_t kind,
    uint32_t runtime_instance,
    uint16_t requester_owner_instance,
    uint64_t absolute_deadline_us,
    uint64_t policy_digest,
    const uint8_t *exact,
    uint8_t exact_length,
    ucn_i_dependency_requirement_t *requirement_out);
bool ucn_i_dependency_requirement_equal(
    const ucn_i_dependency_requirement_t *left,
    const ucn_i_dependency_requirement_t *right);

ucn_result_t ucn_i_coordinator_init(
    ucn_i_coordinator_t *coordinator,
    uint32_t runtime_instance,
    uint16_t owner_instance,
    const ucn_i_lock_ops_t *lock,
    ucn_i_requirement_digest_fn digest,
    ucn_i_dependency_event_sink_fn event_sink,
    void *event_sink_context);
ucn_result_t ucn_i_coordinator_bind_owner(
    ucn_i_coordinator_t *coordinator,
    const ucn_i_owner_binding_t *binding);
ucn_result_t ucn_i_coordinator_route_requirement(
    ucn_i_coordinator_t *coordinator,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out);
ucn_result_t ucn_i_coordinator_route_event(
    ucn_i_coordinator_t *coordinator,
    const ucn_i_dependency_event_t *event,
    uint64_t now_us);
ucn_result_t ucn_i_coordinator_destroy(ucn_i_coordinator_t *coordinator);

#endif
