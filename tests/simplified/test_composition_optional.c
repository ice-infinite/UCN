#include "internal/ucn_composition.h"
#include "internal/ucn_runtime.h"

#include <stdio.h>
#include <string.h>

#if UCN_V6S_FEATURE_REALTIME_ENABLED || UCN_V6S_FEATURE_GROUP_ENABLED || \
    (UCN_V6S_FEATURE_CLUSTER_ENABLED && UCN_FEATURE_PERSISTENCE_ENABLED)

#define TEST_STORAGE_BYTES 600000U

typedef union test_storage {
    uint64_t alignment;
    uint8_t bytes[TEST_STORAGE_BYTES];
} test_storage_t;

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

typedef struct test_fixture {
    test_storage_t composition;
    ucn_storage_t foundation_storage;
    ucn_static_binding_t binding;
    ucn_link_port_t link;
    ucn_ports_t ports;
    ucn_config_t foundation_config;
    test_lock_t composition_lock;
    test_lock_t foundation_lock;
    test_lock_t driver_lock;
    test_lock_t realtime_lock;
    test_lock_t group_lock;
    test_lock_t cluster_lock;
    ucn_i_composition_runtime_t *runtime;
} test_fixture_t;

static test_fixture_t fixture;
static int failures;

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                    __LINE__, #condition_);                                 \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;

    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = context;

    if (lock != NULL) {
        lock->held = 0U;
    }
}

static ucn_i_lock_ops_t internal_lock(test_lock_t *lock)
{
    ucn_i_lock_ops_t value;

    memset(&value, 0, sizeof(value));
    value.struct_size = (uint16_t)sizeof(value);
    value.api_version = UCN_I_LOCK_OPS_VERSION;
    value.context = lock;
    value.enter = lock_enter;
    value.leave = lock_leave;
    return value;
}

static ucn_lock_ops_t public_lock(test_lock_t *lock)
{
    ucn_lock_ops_t value;

    memset(&value, 0, sizeof(value));
    value.struct_size = (uint16_t)sizeof(value);
    value.api_version = UCN_API_VERSION;
    value.context = lock;
    value.enter = lock_enter;
    value.leave = lock_leave;
    return value;
}

static ucn_driver_submit_result_t driver_submit(
    void *context, ucn_link_handle_t link, const uint8_t *bytes,
    size_t length, ucn_driver_token_t token)
{
    (void)context;
    (void)link;
    (void)bytes;
    (void)length;
    (void)token;
    return UCN_DRIVER_COMPLETE;
}

static ucn_result_t driver_cancel(void *context,
                                  ucn_driver_token_t token)
{
    (void)context;
    (void)token;
    return UCN_OK;
}

static ucn_i_composition_owner_record_t *record_for(
    ucn_i_composition_runtime_t *runtime, uint8_t module_id)
{
    uint8_t index;

    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].ref.module_id == module_id) {
            return &runtime->registry[index];
        }
    }
    return NULL;
}

static void *owner_for(ucn_i_composition_runtime_t *runtime,
                       uint8_t module_id)
{
    ucn_i_composition_owner_record_t *record = record_for(runtime, module_id);

    if (record == NULL || record->storage_offset == 0U) {
        return NULL;
    }
    return (uint8_t *)runtime + record->storage_offset;
}

static void foundation_start(test_fixture_t *value,
                             uint32_t runtime_instance,
                             uint16_t owner_instance)
{
    ucn_node_t *node = NULL;

    memset(&value->foundation_storage, 0, sizeof(value->foundation_storage));
    memset(&value->binding, 0, sizeof(value->binding));
    value->binding.struct_size = (uint16_t)sizeof(value->binding);
    value->binding.api_version = UCN_API_VERSION;
    value->binding.address = 1U;
    value->binding.binding_generation = 1U;
    value->binding.principal_digest = UINT64_C(0x0102030405060708);

    memset(&value->link, 0, sizeof(value->link));
    value->link.struct_size = (uint16_t)sizeof(value->link);
    value->link.api_version = UCN_API_VERSION;
    value->link.link_instance = 1U;
    value->link.frame_mtu = 256U;
    value->link.tx.struct_size = (uint16_t)sizeof(value->link.tx);
    value->link.tx.api_version = UCN_API_VERSION;
    value->link.tx.submit = driver_submit;
    value->link.tx.cancel = driver_cancel;

    memset(&value->ports, 0, sizeof(value->ports));
    value->ports.struct_size = (uint16_t)sizeof(value->ports);
    value->ports.api_version = UCN_API_VERSION;
    value->ports.links = &value->link;
    value->ports.link_count = 1U;
    value->ports.state_lock = public_lock(&value->foundation_lock);
    value->ports.driver_callback_gate = public_lock(&value->driver_lock);

    memset(&value->foundation_config, 0,
           sizeof(value->foundation_config));
    value->foundation_config.struct_size =
        (uint16_t)sizeof(value->foundation_config);
    value->foundation_config.api_version = UCN_API_VERSION;
    value->foundation_config.storage_layout = UCN_STORAGE_LAYOUT;
    value->foundation_config.compiled_manifest_hash =
        UCN_COMPILED_MANIFEST_HASH;
    value->foundation_config.runtime_instance = runtime_instance;
    value->foundation_config.realm_id = 7U;
    value->foundation_config.local_address = 1U;
    value->foundation_config.local_binding_generation = 1U;
    value->foundation_config.local_principal_digest =
        value->binding.principal_digest;
    value->foundation_config.bindings = &value->binding;
    value->foundation_config.binding_count = 1U;
    value->foundation_config.address_width = 1U;
    value->foundation_config.trusted_o0_network = 1U;

    CHECK(ucn_i_node_init_with_owner_instance(
              &value->foundation_storage,
              sizeof(value->foundation_storage),
              &value->foundation_config, &value->ports,
              owner_instance, &node) == UCN_OK);
    CHECK(node != NULL);
    CHECK(ucn_start(node, 1U) == UCN_OK);
    value->runtime->foundation_node = node;
    value->runtime->foundation_storage_bytes =
        (uint32_t)sizeof(value->foundation_storage);
}

static ucn_i_composition_runtime_t *runtime_prepare(
    test_fixture_t *value,
    ucn_i_composition_module_mask_t modules,
    ucn_i_composition_capability_mask_t capabilities,
    uint32_t runtime_instance)
{
    ucn_i_composition_request_t request;
    ucn_i_composition_plan_t plan;
    ucn_i_composition_prepare_config_t prepare;
    size_t required = 0U;

    memset(value, 0, sizeof(*value));
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint16_t)sizeof(request);
    request.schema = UCN_I_COMPOSITION_SCHEMA;
    request.profile = UCN_PROFILE;
    request.available_module_mask = modules;
    request.capability_mask = capabilities;
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    CHECK(required <= sizeof(value->composition));

    memset(&prepare, 0, sizeof(prepare));
    prepare.struct_size = (uint16_t)sizeof(prepare);
    prepare.schema = UCN_I_COMPOSITION_RUNTIME_SCHEMA;
    prepare.runtime_instance = runtime_instance;
    prepare.owner_instance_base = 100U;
    prepare.coordinator_lock = internal_lock(&value->composition_lock);
    CHECK(ucn_i_composition_prepare(
              value->composition.bytes, required, &plan, &prepare,
              &value->runtime) == UCN_OK);
    CHECK(value->runtime != NULL);
    foundation_start(value, runtime_instance,
                     value->runtime->registry[0].ref.owner_instance);
    return value->runtime;
}

static void runtime_activate(test_fixture_t *value, uint8_t target_module)
{
    uint8_t index;

    for (index = 0U; index < value->runtime->plan.init_count; ++index) {
        const uint8_t module = value->runtime->plan.init_order[index];
        ucn_i_composition_owner_record_t *record =
            record_for(value->runtime, module);

        CHECK(record != NULL);
        if (record != NULL && module != target_module) {
            memset((uint8_t *)value->runtime + record->storage_offset,
                   0xA5, record->storage_bytes);
        }
        if (record != NULL) {
            record->state = UCN_I_COMPOSITION_OWNER_ACTIVE;
        }
    }
    value->runtime->initialized_count = value->runtime->plan.init_count;
    value->runtime->persistence_ready =
        (value->runtime->plan.selected_module_mask &
         UCN_I_COMPOSE_PERSISTENCE) != 0U ? 1U : 0U;
    value->runtime->registry[0].state =
        UCN_I_COMPOSITION_OWNER_ACTIVE;
    value->runtime->phase = UCN_I_COMPOSITION_RUNNING;
    CHECK(ucn_i_composition_runtime_valid(value->runtime));
}

#if UCN_V6S_FEATURE_GROUP_ENABLED || UCN_V6S_FEATURE_CLUSTER_ENABLED
static void fill_bytes(uint8_t *bytes, size_t count, uint8_t seed)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        bytes[index] = (uint8_t)(seed + index);
    }
}

static bool bytes_are_zero(const void *object, size_t count)
{
    const uint8_t *bytes = object;
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}
#endif

#if UCN_V6S_FEATURE_REALTIME_ENABLED
static void test_local_stamp_is_standalone(void)
{
    ucn_i_realtime_owner_t *owner;
    ucn_i_realtime_config_t config;
    ucn_i_realtime_policy_t policy;
    ucn_i_realtime_prepared_t prepared;
    ucn_i_realtime_prepared_t sentinel;
    ucn_i_composition_runtime_t *runtime = runtime_prepare(
        &fixture, UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_REALTIME,
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_LOCAL_STAMP, 101U);
    ucn_i_composition_owner_record_t *record = record_for(
        runtime, UCN_I_COMPOSITION_MODULE_REALTIME);

    CHECK(record != NULL);
    CHECK((runtime->plan.selected_module_mask &
           (UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_PERSISTENCE |
            UCN_I_COMPOSE_GROUP | UCN_I_COMPOSE_CLUSTER)) == 0U);
    owner = owner_for(runtime, UCN_I_COMPOSITION_MODULE_REALTIME);
    memset(&config, 0, sizeof(config));
    config.runtime_instance = runtime->runtime_instance;
    config.owner_instance = record->ref.owner_instance;
    config.state_lock = internal_lock(&fixture.realtime_lock);
    CHECK(ucn_i_realtime_owner_init(owner, &config) == UCN_OK);
    runtime_activate(&fixture, UCN_I_COMPOSITION_MODULE_REALTIME);

    memset(&policy, 0, sizeof(policy));
    policy.endpoint = 0x1001U;
    policy.mode = UCN_I_REALTIME_LOCAL_STAMP;
    policy.requirement = UCN_I_REALTIME_REQUIRED;
    CHECK(ucn_i_composition_realtime_local_policy_set(
              runtime, &policy) == UCN_OK);
    memset(&prepared, 0, sizeof(prepared));
    CHECK(ucn_i_composition_realtime_local_prepare(
              runtime, policy.endpoint, 1234U, 0U, false,
              &prepared) == UCN_OK);
    CHECK(prepared.metadata_present == 1U);
    CHECK(prepared.envelope.mode == UCN_I_REALTIME_LOCAL_STAMP);
    CHECK(prepared.envelope.capture_time_us == 1234U);
    CHECK(prepared.envelope.domain_generation == 0U);
    CHECK(prepared.envelope.domain_time_valid == 0U);
    CHECK(ucn_i_composition_realtime_local_prepare(
              runtime, policy.endpoint, 1235U, 0U, false,
              (ucn_i_realtime_prepared_t *)&fixture.foundation_storage) ==
          UCN_ERR_ARGUMENT);

    memset(&prepared, 0xA5, sizeof(prepared));
    sentinel = prepared;
    CHECK(ucn_i_composition_realtime_local_prepare(
              runtime, 0x2002U, 1235U, 0U, false,
              &prepared) == UCN_ERR_NOT_FOUND);
    CHECK(memcmp(&prepared, &sentinel, sizeof(prepared)) == 0);

    policy.mode = UCN_I_REALTIME_DEADLINE;
    memset(&prepared, 0xA5, sizeof(prepared));
    sentinel = prepared;
    CHECK(ucn_i_composition_realtime_local_policy_set(
              runtime, &policy) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&prepared, &sentinel, sizeof(prepared)) == 0);
}
#endif

#if UCN_V6S_FEATURE_GROUP_ENABLED
static void test_static_group_is_standalone(void)
{
    ucn_i_group_owner_t *owner;
    ucn_i_group_config_t config;
    ucn_i_group_context_config_t group_config;
    ucn_i_group_send_request_t send_request;
    ucn_i_group_attempt_view_t attempt;
    ucn_handle_t group;
    ucn_handle_t send;
    ucn_handle_t sentinel_handle;
    ucn_i_composition_runtime_t *runtime = runtime_prepare(
        &fixture, UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_GROUP,
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_STATIC_GROUP, 102U);
    ucn_i_composition_owner_record_t *record = record_for(
        runtime, UCN_I_COMPOSITION_MODULE_GROUP);

    CHECK(record != NULL);
    CHECK((runtime->plan.selected_module_mask &
           (UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_REALTIME |
            UCN_I_COMPOSE_CLUSTER)) == 0U);
    owner = owner_for(runtime, UCN_I_COMPOSITION_MODULE_GROUP);
    memset(&config, 0, sizeof(config));
    config.runtime_instance = runtime->runtime_instance;
    config.realm_id = 7U;
    config.owner_instance = record->ref.owner_instance;
    config.state_lock = internal_lock(&fixture.group_lock);
    CHECK(ucn_i_group_owner_init(owner, &config) == UCN_OK);
    runtime_activate(&fixture, UCN_I_COMPOSITION_MODULE_GROUP);

    memset(&group_config, 0, sizeof(group_config));
    group_config.realm_id = 7U;
    group_config.group_id = 51U;
    group_config.group_generation = 1U;
    group_config.policy_generation = 1U;
    group_config.member_generation = 1U;
    group_config.endpoint = 0x1001U;
    group_config.opcode = 0x2001U;
    group_config.mode = UCN_I_GROUP_STATIC;
    group_config.member_count = 1U;
    group_config.unicast_fanout_limit = 1U;
    group_config.quorum_weight = 1U;
    group_config.allowed_scope_mask =
        UCN_I_GROUP_SCOPE_BIT(UCN_I_GROUP_SCOPE_LOCAL_ONLY);
    group_config.public_static = 1U;
    group_config.members[0].address = 2U;
    group_config.members[0].binding_generation = 3U;
    group_config.members[0].sender_slot = 1U;
    group_config.members[0].weight = 1U;
    fill_bytes(group_config.members[0].principal,
               sizeof(group_config.members[0].principal), 0x20U);
    memset(&group, 0xA5, sizeof(group));
    sentinel_handle = group;
    CHECK(ucn_i_composition_group_install_static(
              runtime, 0U, &group_config, false, true,
              &group) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&group, &sentinel_handle, sizeof(group)) == 0);
    CHECK(owner->contexts[0].occupied == 0U);
    CHECK(ucn_i_composition_group_install_static(
              runtime, 0U, &group_config, true, true,
              &group) == UCN_OK);

    memset(&send_request, 0, sizeof(send_request));
    send_request.send_id = 1U;
    send_request.absolute_deadline_us = 1000U;
    send_request.expected_group_generation = 1U;
    send_request.expected_policy_generation = 1U;
    send_request.expected_member_generation = 1U;
    send_request.success_scope = UCN_I_GROUP_SCOPE_LOCAL_ONLY;
    fill_bytes(send_request.payload_digest,
               sizeof(send_request.payload_digest), 0x40U);
    send_request.expected_group_generation = 2U;
    memset(&send, 0xA5, sizeof(send));
    sentinel_handle = send;
    CHECK(ucn_i_composition_group_send_begin(
              runtime, group, &send_request, 10U,
              &send) == UCN_ERR_POLICY);
    CHECK(memcmp(&send, &sentinel_handle, sizeof(send)) == 0);
    CHECK(bytes_are_zero(owner->sends, sizeof(owner->sends)));
    CHECK(bytes_are_zero(owner->attempts, sizeof(owner->attempts)));
    send_request.expected_group_generation = 1U;
    CHECK(ucn_i_composition_group_send_begin(
              runtime, group, &send_request, 10U, &send) == UCN_OK);
    CHECK(send.object_kind == UCN_OBJECT_KIND_SEND);
    CHECK(ucn_i_composition_group_attempt_peek(
              runtime, &attempt) == UCN_OK);
    CHECK(attempt.group_id == group_config.group_id);
    CHECK(attempt.member_address == 2U);
    CHECK(attempt.plan == UCN_I_GROUP_PLAN_UNICAST);
}
#endif

#if UCN_V6S_FEATURE_CLUSTER_ENABLED && UCN_FEATURE_PERSISTENCE_ENABLED
static void cluster_principal(uint8_t principal[UCN_I_CLUSTER_PRINCIPAL_BYTES],
                              uint8_t seed)
{
    fill_bytes(principal, UCN_I_CLUSTER_PRINCIPAL_BYTES, seed);
}

static void test_cluster_base_is_standalone(void)
{
    const ucn_i_composition_module_mask_t modules =
        UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
        UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
        UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
        UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_CLUSTER;
    ucn_i_cluster_owner_t *owner;
    ucn_i_cluster_config_t config;
    ucn_i_cluster_member_fact_t fact;
    ucn_i_cluster_authority_view_t view;
    ucn_i_cluster_authority_view_t view_sentinel;
    ucn_i_composition_runtime_t *runtime = runtime_prepare(
        &fixture, modules,
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_CLUSTER, 103U);
    ucn_i_composition_owner_record_t *record = record_for(
        runtime, UCN_I_COMPOSITION_MODULE_CLUSTER);

    CHECK(record != NULL);
    CHECK((runtime->plan.selected_module_mask &
           (UCN_I_COMPOSE_REALTIME | UCN_I_COMPOSE_GROUP)) == 0U);
    owner = owner_for(runtime, UCN_I_COMPOSITION_MODULE_CLUSTER);
    memset(&config, 0, sizeof(config));
    config.runtime_instance = runtime->runtime_instance;
    config.owner_instance = record->ref.owner_instance;
    cluster_principal(config.local_principal, 0x10U);
    config.state_lock = internal_lock(&fixture.cluster_lock);
    CHECK(ucn_i_cluster_owner_init(owner, &config) == UCN_OK);

    owner->state.epoch.cluster_id = 9U;
    owner->state.epoch.term = 1U;
    owner->state.epoch.head_binding_generation = 7U;
    memcpy(owner->state.epoch.head_principal, config.local_principal,
           sizeof(owner->state.epoch.head_principal));
    owner->state.stable_config.config_id = 4U;
    owner->state.stable_config.generation = 1U;
    owner->state.stable_config.member_count = 1U;
    owner->state.stable_config.members[0].binding_generation = 7U;
    owner->state.stable_config.members[0].flags =
        UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
        UCN_I_CLUSTER_MEMBER_FLAG_VOTER;
    memcpy(owner->state.stable_config.members[0].principal,
           config.local_principal,
           sizeof(owner->state.stable_config.members[0].principal));
    owner->state.role = UCN_I_CLUSTER_HEAD;
    owner->state.phase = UCN_I_CLUSTER_STABLE;
    runtime_activate(&fixture, UCN_I_COMPOSITION_MODULE_CLUSTER);

    memset(&fact, 0, sizeof(fact));
    fact.lease_deadline_us = 1000U;
    fact.capability_deadline_us = 1000U;
    fact.binding_generation = 7U;
    fact.session_generation = 2U;
    fact.capability_generation = 3U;
    fact.route_generation = 4U;
    fact.link_generation = 5U;
    fact.link_id = 1U;
    fact.authenticated = 1U;
    fact.current = 1U;
    memcpy(fact.principal, config.local_principal,
           sizeof(fact.principal));
    memset(fact.capability_digest, 0x5A,
           sizeof(fact.capability_digest));
    fact.authenticated = 0U;
    CHECK(ucn_i_composition_cluster_member_observe(
              runtime, &fact, 10U) == UCN_ERR_ARGUMENT);
    CHECK(bytes_are_zero(owner->members, sizeof(owner->members)));
    fact.authenticated = 1U;
    CHECK(ucn_i_composition_cluster_member_observe(
              runtime, &fact, 10U) == UCN_OK);
    memset(&view, 0, sizeof(view));
    CHECK(ucn_i_composition_cluster_authority_preflight(
              runtime, 10U, &view) == UCN_OK);
    CHECK(view.authority_active == 1U);
    CHECK(view.live_old_voters == 1U);
    CHECK(view.epoch.cluster_id == 9U);

    fact.lease_deadline_us = 10U;
    memset(&view, 0xA5, sizeof(view));
    view_sentinel = view;
    CHECK(ucn_i_composition_cluster_authority_preflight(
              runtime, 1000U, &view) == UCN_ERR_ACCESS);
    CHECK(memcmp(&view, &view_sentinel, sizeof(view)) == 0);
}
#endif

int main(void)
{
#if UCN_V6S_FEATURE_REALTIME_ENABLED
    test_local_stamp_is_standalone();
#endif
#if UCN_V6S_FEATURE_GROUP_ENABLED
    test_static_group_is_standalone();
#endif
#if UCN_V6S_FEATURE_CLUSTER_ENABLED && UCN_FEATURE_PERSISTENCE_ENABLED
    test_cluster_base_is_standalone();
#endif
    if (failures != 0) {
        fprintf(stderr, "composition optional failures=%d\n", failures);
        return 1;
    }
    printf("composition_optional_tests=PASS realtime=%d group=%d cluster=%d\n",
           UCN_V6S_FEATURE_REALTIME_ENABLED,
           UCN_V6S_FEATURE_GROUP_ENABLED,
           UCN_V6S_FEATURE_CLUSTER_ENABLED &&
               UCN_FEATURE_PERSISTENCE_ENABLED);
    return 0;
}

#else

int main(void)
{
    printf("composition_optional_tests=PASS realtime=0 group=0 cluster=0\n");
    return 0;
}

#endif
