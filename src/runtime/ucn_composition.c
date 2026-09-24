#include "internal/ucn_composition.h"

#include "internal/ucn_admission.h"
#include "internal/ucn_capability.h"
#include "internal/ucn_checked.h"
#include "internal/ucn_cluster.h"
#include "internal/ucn_coordinator.h"
#include "internal/ucn_flow.h"
#include "internal/ucn_group.h"
#include "internal/ucn_identity.h"
#include "internal/ucn_persistence.h"
#include "internal/ucn_realtime.h"
#include "internal/ucn_route.h"
#include "internal/ucn_runtime.h"
#include "internal/ucn_security.h"
#include "internal/ucn_service.h"
#include "internal/ucn_transport.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define UCN_I_COMPOSITION_RUNTIME_MAGIC UINT32_C(0x55434352)
#define UCN_I_COMPOSITION_OPERATION_LIFECYCLE UINT16_C(1)

typedef struct ucn_i_composition_rule {
    ucn_i_composition_capability_mask_t capability;
    ucn_i_composition_module_mask_t required_modules;
    ucn_i_composition_module_mask_t durable_modules;
} ucn_i_composition_rule_t;

static const ucn_i_composition_rule_t composition_rules[] = {
    {UCN_I_CAP_STATIC_C1, UCN_I_COMPOSE_FOUNDATION, 0U},
    {UCN_I_CAP_PROTECTED_C1,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_SECURITY,
     UCN_I_COMPOSE_SECURITY},
    {UCN_I_CAP_DYNAMIC_ADMISSION,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_ADMISSION | UCN_I_COMPOSE_CAPABILITY,
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY},
    {UCN_I_CAP_AUTO_ROUTE,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_ROUTE, 0U},
    {UCN_I_CAP_ADVANCED_FLOW,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
         UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW,
     UCN_I_COMPOSE_SECURITY},
    {UCN_I_CAP_RELIABLE,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_TRANSPORT, 0U},
    {UCN_I_CAP_TRANSFER,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_TRANSPORT,
     UCN_I_COMPOSE_TRANSPORT},
    {UCN_I_CAP_SERVICE,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_SERVICE, 0U},
    {UCN_I_CAP_DURABLE_OPERATION,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_SERVICE,
     UCN_I_COMPOSE_SERVICE},
    {UCN_I_CAP_LOCAL_STAMP,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_REALTIME, 0U},
    {UCN_I_CAP_NETWORK_TIME,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
         UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW |
         UCN_I_COMPOSE_REALTIME,
     UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_REALTIME},
    {UCN_I_CAP_STATIC_GROUP,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_GROUP, 0U},
    {UCN_I_CAP_DYNAMIC_GROUP,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_GROUP,
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_GROUP},
    {UCN_I_CAP_CLUSTER,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
         UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_CLUSTER,
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_CLUSTER},
    {UCN_I_CAP_CLUSTER_GROUP_ACCELERATION,
     UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
         UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE |
         UCN_I_COMPOSE_FLOW | UCN_I_COMPOSE_GROUP |
         UCN_I_COMPOSE_CLUSTER,
     UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
         UCN_I_COMPOSE_CLUSTER}
};

static const ucn_i_composition_module_mask_t module_bits[] = {
    UCN_I_COMPOSE_PERSISTENCE, UCN_I_COMPOSE_IDENTITY,
    UCN_I_COMPOSE_SECURITY,    UCN_I_COMPOSE_ADMISSION,
    UCN_I_COMPOSE_CAPABILITY,  UCN_I_COMPOSE_ROUTE,
    UCN_I_COMPOSE_FLOW,        UCN_I_COMPOSE_TRANSPORT,
    UCN_I_COMPOSE_SERVICE,     UCN_I_COMPOSE_REALTIME,
    UCN_I_COMPOSE_GROUP,       UCN_I_COMPOSE_CLUSTER
};

static bool bytes_are_zero(const void *object, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool lock_ops_are_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->enter != NULL && lock->leave != NULL;
}

static bool pointer_is_aligned(const void *pointer, size_t alignment)
{
    return pointer != NULL && alignment != 0U &&
           ((uintptr_t)pointer % alignment) == 0U;
}

static ucn_result_t size_align_up(size_t value,
                                  size_t alignment,
                                  size_t *aligned_out)
{
    size_t remainder;
    size_t padding;

    if (alignment == 0U || aligned_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    remainder = value % alignment;
    padding = remainder == 0U ? 0U : alignment - remainder;
    return ucn_i_size_add(value, padding, aligned_out);
}

static ucn_i_composition_module_mask_t module_bit_from_id(
    ucn_i_composition_module_id_t module_id)
{
    if (module_id <= UCN_I_COMPOSITION_MODULE_CLUSTER) {
        return UINT32_C(1) << module_id;
    }
    return 0U;
}

static ucn_i_composition_module_id_t dependency_target_module(
    ucn_i_dependency_kind_t dependency_kind)
{
    static const uint8_t targets[UCN_I_DEPENDENCY_KIND_COUNT] = {
        UCN_I_COMPOSITION_MODULE_IDENTITY,
        UCN_I_COMPOSITION_MODULE_SECURITY,
        UCN_I_COMPOSITION_MODULE_CAPABILITY,
        UCN_I_COMPOSITION_MODULE_ROUTE,
        UCN_I_COMPOSITION_MODULE_FLOW,
        UCN_I_COMPOSITION_MODULE_TRANSPORT,
        UCN_I_COMPOSITION_MODULE_GROUP,
        UCN_I_COMPOSITION_MODULE_REALTIME,
        UCN_I_COMPOSITION_MODULE_PERSISTENCE
    };

    if (dependency_kind < UCN_I_DEP_IDENTITY_BINDING ||
        dependency_kind > UCN_I_DEP_PERSISTENCE) {
        return UINT8_MAX;
    }
    return targets[dependency_kind - 1U];
}

static bool owner_ref_equal(const ucn_i_composition_owner_ref_t *left,
                            const ucn_i_composition_owner_ref_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static const ucn_i_composition_owner_record_t *owner_record_find(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id)
{
    uint8_t index;

    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].ref.module_id == module_id) {
            return &runtime->registry[index];
        }
    }
    return NULL;
}

static ucn_i_composition_owner_record_t *owner_record_find_mutable(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id)
{
    uint8_t index;

    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].ref.module_id == module_id) {
            return &runtime->registry[index];
        }
    }
    return NULL;
}

static ucn_result_t composition_event_sink(
    void *context,
    uint16_t requester_owner_instance,
    const ucn_i_dependency_requirement_t *requirement,
    const ucn_i_dependency_event_t *event)
{
    (void)context;
    (void)requester_owner_instance;
    (void)requirement;
    (void)event;
    /* IMPL-09-01 deliberately has no active business Owner.  IMPL-09-02
     * replaces this fail-closed sink with typed Owner event dispatch only
     * after durable reload and Owner publication complete. */
    return UCN_ERR_STATE;
}

static bool profile_is_valid(uint32_t profile)
{
    return profile == UCN_PROFILE_NANO || profile == UCN_PROFILE_LITE ||
           profile == UCN_PROFILE_FULL;
}

static ucn_i_composition_module_mask_t intrinsic_requirements(
    ucn_i_composition_module_mask_t modules)
{
    ucn_i_composition_module_mask_t required = UCN_I_COMPOSE_FOUNDATION;

    if ((modules & UCN_I_COMPOSE_IDENTITY) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE;
    }
    if ((modules & UCN_I_COMPOSE_SECURITY) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE;
    }
    if ((modules & UCN_I_COMPOSE_ADMISSION) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
                    UCN_I_COMPOSE_SECURITY;
    }
    if ((modules & UCN_I_COMPOSE_CAPABILITY) != 0U) {
        required |= UCN_I_COMPOSE_SECURITY;
    }
    if ((modules & UCN_I_COMPOSE_FLOW) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_SECURITY |
                    UCN_I_COMPOSE_CAPABILITY | UCN_I_COMPOSE_ROUTE;
    }
    if ((modules & UCN_I_COMPOSE_CLUSTER) != 0U) {
        required |= UCN_I_COMPOSE_PERSISTENCE | UCN_I_COMPOSE_IDENTITY |
                    UCN_I_COMPOSE_SECURITY | UCN_I_COMPOSE_CAPABILITY |
                    UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW;
    }
    return required;
}

static size_t module_owner_bytes(ucn_i_composition_module_mask_t module)
{
    switch (module) {
        case UCN_I_COMPOSE_PERSISTENCE:
            return sizeof(ucn_persistence_storage_t);
        case UCN_I_COMPOSE_IDENTITY:
            return sizeof(ucn_i_identity_owner_t);
        case UCN_I_COMPOSE_SECURITY:
            return sizeof(ucn_i_security_owner_t);
        case UCN_I_COMPOSE_ADMISSION:
            return sizeof(ucn_i_admission_owner_t);
        case UCN_I_COMPOSE_CAPABILITY:
            return sizeof(ucn_i_capability_owner_t);
        case UCN_I_COMPOSE_ROUTE:
            return sizeof(ucn_i_route_owner_t);
        case UCN_I_COMPOSE_FLOW:
            return sizeof(ucn_i_flow_owner_t);
        case UCN_I_COMPOSE_TRANSPORT:
            return sizeof(ucn_i_transport_owner_t);
        case UCN_I_COMPOSE_SERVICE:
            return sizeof(ucn_i_service_owner_t);
        case UCN_I_COMPOSE_REALTIME:
            return sizeof(ucn_i_realtime_owner_t);
        case UCN_I_COMPOSE_GROUP:
            return sizeof(ucn_i_group_owner_t);
        case UCN_I_COMPOSE_CLUSTER:
            return sizeof(ucn_i_cluster_owner_t);
        default:
            return 0U;
    }
}

static uint64_t digest_u32(uint64_t digest, uint32_t value)
{
    uint8_t index;

    for (index = 0U; index < 4U; ++index) {
        const uint8_t shift = (uint8_t)(24U - (index * 8U));
        digest ^= (uint64_t)((value >> shift) & UINT32_C(0xFF));
        digest *= UINT64_C(0x00000100000001B3);
    }
    return digest;
}

static uint64_t composition_digest(const ucn_i_composition_plan_t *plan)
{
    uint64_t digest = UINT64_C(0xCBF29CE484222325);
    uint8_t index;

    digest = digest_u32(digest, UINT32_C(0x55434E43));
    digest = digest_u32(digest, UCN_I_COMPOSITION_SCHEMA);
    digest = digest_u32(digest, plan->profile);
    digest = digest_u32(digest, plan->available_module_mask);
    digest = digest_u32(digest, plan->compiled_module_mask);
    digest = digest_u32(digest, plan->selected_module_mask);
    digest = digest_u32(digest, plan->durable_module_mask);
    digest = digest_u32(digest, plan->capability_mask);
    digest = digest_u32(digest, plan->private_owner_bytes);
    digest = digest_u32(digest, plan->init_count);
    for (index = 0U; index < plan->init_count; ++index) {
        digest = digest_u32(digest, plan->init_order[index]);
    }
    return digest;
}

static bool request_is_well_formed(
    const ucn_i_composition_request_t *request)
{
    return request != NULL && request->struct_size == sizeof(*request) &&
           request->schema == UCN_I_COMPOSITION_SCHEMA &&
           profile_is_valid(request->profile) && request->reserved_zero == 0U &&
           (request->available_module_mask & ~UCN_I_COMPOSE_KNOWN_MODULES) ==
               0U &&
           (request->available_module_mask & UCN_I_COMPOSE_FOUNDATION) != 0U &&
           (request->capability_mask & ~UCN_I_COMPOSE_KNOWN_CAPABILITIES) ==
               0U &&
           (request->capability_mask & UCN_I_CAP_STATIC_C1) != 0U;
}

ucn_result_t ucn_i_composition_plan_build(
    const ucn_i_composition_request_t *request,
    ucn_i_composition_plan_t *plan_out)
{
    ucn_i_composition_plan_t plan;
    ucn_i_composition_module_mask_t selected = UCN_I_COMPOSE_FOUNDATION;
    ucn_i_composition_module_mask_t durable = 0U;
    size_t private_owner_bytes = sizeof(ucn_i_coordinator_t);
    size_t index;

    if (!request_is_well_formed(request) || plan_out == NULL ||
        ucn_i_ranges_overlap(request, sizeof(*request), plan_out,
                             sizeof(*plan_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if ((request->available_module_mask &
         ~UCN_I_COMPOSITION_COMPILED_MODULE_MASK) != 0U) {
        return UCN_ERR_UNSUPPORTED;
    }

    for (index = 0U;
         index < sizeof(composition_rules) / sizeof(composition_rules[0]);
         ++index) {
        if ((request->capability_mask & composition_rules[index].capability) !=
            0U) {
            selected |= composition_rules[index].required_modules;
            durable |= composition_rules[index].durable_modules;
        }
    }
    selected |= intrinsic_requirements(selected);
    if ((selected & ~request->available_module_mask) != 0U) {
        return UCN_ERR_CONFIG;
    }

    memset(&plan, 0, sizeof(plan));
    plan.struct_size = (uint16_t)sizeof(plan);
    plan.schema = UCN_I_COMPOSITION_SCHEMA;
    plan.profile = request->profile;
    plan.available_module_mask = request->available_module_mask;
    plan.compiled_module_mask = UCN_I_COMPOSITION_COMPILED_MODULE_MASK;
    plan.selected_module_mask = selected;
    plan.durable_module_mask = durable;
    plan.capability_mask = request->capability_mask;

    for (index = 0U; index < sizeof(module_bits) / sizeof(module_bits[0]);
         ++index) {
        if ((selected & module_bits[index]) != 0U) {
            const size_t owner_bytes = module_owner_bytes(module_bits[index]);
            if (owner_bytes == 0U ||
                private_owner_bytes > UINT32_MAX - owner_bytes ||
                plan.init_count >= UCN_I_COMPOSITION_INIT_ORDER_CAPACITY) {
                return UCN_ERR_NO_SPACE;
            }
            private_owner_bytes += owner_bytes;
            plan.init_order[plan.init_count] = (uint8_t)(index + 1U);
            ++plan.init_count;
        }
    }
    plan.private_owner_bytes = (uint32_t)private_owner_bytes;
    plan.composition_digest = composition_digest(&plan);
    if (plan.composition_digest == 0U) {
        return UCN_ERR_STATE;
    }
    *plan_out = plan;
    return UCN_OK;
}

bool ucn_i_composition_plan_valid(const ucn_i_composition_plan_t *plan)
{
    ucn_i_composition_request_t request;
    ucn_i_composition_plan_t rebuilt;

    if (plan == NULL || plan->struct_size != sizeof(*plan) ||
        plan->schema != UCN_I_COMPOSITION_SCHEMA ||
        plan->reserved_zero[0] != 0U || plan->reserved_zero[1] != 0U ||
        plan->reserved_zero[2] != 0U) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint16_t)sizeof(request);
    request.schema = UCN_I_COMPOSITION_SCHEMA;
    request.profile = plan->profile;
    request.available_module_mask = plan->available_module_mask;
    request.capability_mask = plan->capability_mask;
    return ucn_i_composition_plan_build(&request, &rebuilt) == UCN_OK &&
           memcmp(&rebuilt, plan, sizeof(rebuilt)) == 0;
}

static ucn_result_t composition_layout_required(
    const ucn_i_composition_plan_t *plan,
    size_t *storage_bytes_out)
{
    size_t offset;
    uint8_t index;

    if (!ucn_i_composition_plan_valid(plan) || storage_bytes_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (size_align_up(sizeof(ucn_i_composition_runtime_t),
                      UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                      &offset) != UCN_OK) {
        return UCN_ERR_NO_SPACE;
    }
    for (index = 0U; index < plan->init_count; ++index) {
        const ucn_i_composition_module_mask_t module =
            module_bit_from_id(plan->init_order[index]);
        const size_t owner_bytes = module_owner_bytes(module);

        if (module == 0U || owner_bytes == 0U ||
            size_align_up(offset, UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                          &offset) != UCN_OK ||
            ucn_i_size_add(offset, owner_bytes, &offset) != UCN_OK) {
            return UCN_ERR_NO_SPACE;
        }
    }
    if (offset > UINT32_MAX) {
        return UCN_ERR_NO_SPACE;
    }
    *storage_bytes_out = offset;
    return UCN_OK;
}

ucn_result_t ucn_i_composition_storage_required(
    const ucn_i_composition_plan_t *plan,
    size_t *storage_bytes_out)
{
    size_t required;
    ucn_result_t result;

    if (storage_bytes_out == NULL ||
        (plan != NULL &&
         ucn_i_ranges_overlap(plan, sizeof(*plan), storage_bytes_out,
                              sizeof(*storage_bytes_out)))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_layout_required(plan, &required);
    if (result != UCN_OK) {
        return result;
    }
    *storage_bytes_out = required;
    return UCN_OK;
}

static void owner_record_fill(ucn_i_composition_owner_record_t *record,
                              uint32_t runtime_instance,
                              uint16_t owner_instance_base,
                              ucn_i_composition_module_id_t module_id,
                              ucn_i_composition_module_mask_t module_mask,
                              uint32_t storage_offset,
                              uint32_t storage_bytes,
                              ucn_i_composition_owner_state_t state)
{
    memset(record, 0, sizeof(*record));
    record->ref.runtime_instance = runtime_instance;
    record->ref.owner_instance =
        (uint16_t)(owner_instance_base + (uint16_t)module_id);
    record->ref.generation = UINT16_C(1);
    record->ref.module_id = module_id;
    record->module_mask = module_mask;
    record->storage_offset = storage_offset;
    record->storage_bytes = storage_bytes;
    record->state = state;
}

static bool prepare_config_is_valid(
    const ucn_i_composition_prepare_config_t *config)
{
    return config != NULL && config->struct_size == sizeof(*config) &&
           config->schema == UCN_I_COMPOSITION_RUNTIME_SCHEMA &&
           config->runtime_instance != 0U &&
           config->owner_instance_base != 0U &&
           config->owner_instance_base <=
               UINT16_MAX - UCN_I_COMPOSITION_MODULE_COORDINATOR &&
           config->reserved_zero == 0U &&
           lock_ops_are_valid(&config->coordinator_lock);
}

ucn_result_t ucn_i_composition_prepare(
    void *storage,
    size_t storage_bytes,
    const ucn_i_composition_plan_t *plan,
    const ucn_i_composition_prepare_config_t *config,
    ucn_i_composition_runtime_t **runtime_out)
{
    ucn_i_composition_runtime_t *runtime;
    size_t required;
    size_t offset;
    uint8_t index;
    ucn_result_t result;

    if (runtime_out == NULL ||
        composition_layout_required(plan, &required) != UCN_OK ||
        !prepare_config_is_valid(config) || storage == NULL ||
        !pointer_is_aligned(storage, UCN_I_COMPOSITION_STORAGE_ALIGNMENT) ||
        storage_bytes != required || required > UINT32_MAX ||
        ucn_i_ranges_overlap(storage, required, plan, sizeof(*plan)) ||
        ucn_i_ranges_overlap(storage, required, config, sizeof(*config)) ||
        ucn_i_ranges_overlap(storage, required, runtime_out,
                             sizeof(*runtime_out)) ||
        ucn_i_ranges_overlap(plan, sizeof(*plan), runtime_out,
                             sizeof(*runtime_out)) ||
        ucn_i_ranges_overlap(config, sizeof(*config), runtime_out,
                             sizeof(*runtime_out)) ||
        ucn_i_ranges_overlap(config->coordinator_lock.context, 1U, storage,
                             required) ||
        !bytes_are_zero(storage, required)) {
        return UCN_ERR_ARGUMENT;
    }

    runtime = (ucn_i_composition_runtime_t *)storage;
    runtime->runtime_instance = config->runtime_instance;
    runtime->storage_bytes = (uint32_t)required;
    runtime->schema = UCN_I_COMPOSITION_RUNTIME_SCHEMA;
    runtime->owner_instance_base = config->owner_instance_base;
    runtime->phase = UCN_I_COMPOSITION_PREPARED;
    runtime->plan = *plan;

    owner_record_fill(&runtime->registry[runtime->registry_count++],
                      config->runtime_instance,
                      config->owner_instance_base,
                      UCN_I_COMPOSITION_MODULE_FOUNDATION,
                      UCN_I_COMPOSE_FOUNDATION, 0U, 0U,
                      UCN_I_COMPOSITION_OWNER_REGISTERED);
    owner_record_fill(&runtime->registry[runtime->registry_count++],
                      config->runtime_instance,
                      config->owner_instance_base,
                      UCN_I_COMPOSITION_MODULE_COORDINATOR, 0U,
                      (uint32_t)offsetof(ucn_i_composition_runtime_t,
                                         coordinator),
                      (uint32_t)sizeof(runtime->coordinator),
                      UCN_I_COMPOSITION_OWNER_ACTIVE);

    result = size_align_up(sizeof(*runtime),
                           UCN_I_COMPOSITION_STORAGE_ALIGNMENT, &offset);
    if (result != UCN_OK) {
        memset(storage, 0, required);
        return UCN_ERR_NO_SPACE;
    }
    for (index = 0U; index < plan->init_count; ++index) {
        const ucn_i_composition_module_id_t module_id =
            plan->init_order[index];
        const ucn_i_composition_module_mask_t module =
            module_bit_from_id(module_id);
        const size_t owner_bytes = module_owner_bytes(module);

        result = size_align_up(offset, UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                               &offset);
        if (result != UCN_OK || offset > UINT32_MAX ||
            owner_bytes > UINT32_MAX) {
            memset(storage, 0, required);
            return UCN_ERR_NO_SPACE;
        }
        owner_record_fill(&runtime->registry[runtime->registry_count++],
                          config->runtime_instance,
                          config->owner_instance_base, module_id, module,
                          (uint32_t)offset, (uint32_t)owner_bytes,
                          UCN_I_COMPOSITION_OWNER_RESERVED);
        offset += owner_bytes;
    }

    result = ucn_i_coordinator_init(
        &runtime->coordinator, config->runtime_instance,
        (uint16_t)(config->owner_instance_base +
                   UCN_I_COMPOSITION_MODULE_COORDINATOR),
        &config->coordinator_lock, ucn_i_requirement_digest_default,
        composition_event_sink, runtime);
    if (result != UCN_OK) {
        memset(storage, 0, required);
        return result;
    }
    result = ucn_i_callback_gate_init(
        &runtime->lifecycle_gate,
        config->runtime_instance,
        &config->coordinator_lock);
    if (result != UCN_OK) {
        (void)ucn_i_coordinator_destroy(&runtime->coordinator);
        memset(storage, 0, required);
        return result;
    }
    runtime->magic = UCN_I_COMPOSITION_RUNTIME_MAGIC;
    *runtime_out = runtime;
    return UCN_OK;
}

static bool owner_record_is_exact(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_owner_record_t *record,
    ucn_i_composition_module_id_t module_id,
    ucn_i_composition_module_mask_t module_mask,
    uint32_t storage_offset,
    uint32_t storage_bytes,
    ucn_i_composition_owner_state_t state)
{
    return record->ref.runtime_instance == runtime->runtime_instance &&
           record->ref.owner_instance ==
               (uint16_t)(runtime->owner_instance_base + module_id) &&
           record->ref.generation == UINT16_C(1) &&
           record->ref.module_id == module_id &&
           bytes_are_zero(record->ref.reserved_zero,
                          sizeof(record->ref.reserved_zero)) &&
           record->module_mask == module_mask &&
           record->storage_offset == storage_offset &&
           record->storage_bytes == storage_bytes && record->state == state &&
           bytes_are_zero(record->reserved_zero,
                          sizeof(record->reserved_zero));
}

static bool coordinator_is_pristine(
    const ucn_i_composition_runtime_t *runtime)
{
    const ucn_i_coordinator_t *coordinator = &runtime->coordinator;

    return coordinator->magic == UCN_I_COORDINATOR_MAGIC &&
           coordinator->runtime_instance == runtime->runtime_instance &&
           coordinator->owner_instance ==
               (uint16_t)(runtime->owner_instance_base +
                          UCN_I_COMPOSITION_MODULE_COORDINATOR) &&
           coordinator->schema == UCN_I_COORDINATOR_SCHEMA &&
           coordinator->route_active == 0U && coordinator->faulted == 0U &&
           coordinator->reserved_zero == 0U &&
           coordinator->event_sink_context == runtime &&
           coordinator->digest == ucn_i_requirement_digest_default &&
           coordinator->event_sink == composition_event_sink &&
           lock_ops_are_valid(&coordinator->lock) &&
           !ucn_i_ranges_overlap(coordinator->lock.context, 1U, runtime,
                                 runtime->storage_bytes) &&
           bytes_are_zero(coordinator->bindings,
                          sizeof(coordinator->bindings)) &&
           bytes_are_zero(coordinator->slots, sizeof(coordinator->slots));
}

static bool lifecycle_gate_is_idle(
    const ucn_i_composition_runtime_t *runtime)
{
    const ucn_i_callback_gate_t *gate = &runtime->lifecycle_gate;

    return gate->magic == UCN_I_CALLBACK_GATE_MAGIC &&
           gate->gate_instance == runtime->runtime_instance &&
           gate->schema == UCN_I_CALLBACK_GATE_SCHEMA &&
           gate->reserved_zero == 0U &&
           bytes_are_zero(&gate->active_claim, sizeof(gate->active_claim)) &&
           lock_ops_are_valid(&gate->lock) &&
           !ucn_i_ranges_overlap(gate->lock.context, 1U, runtime,
                                 runtime->storage_bytes);
}

static ucn_i_composition_owner_state_t expected_foundation_state(
    ucn_i_composition_runtime_phase_t phase)
{
    if (phase == UCN_I_COMPOSITION_RUNNING) {
        return UCN_I_COMPOSITION_OWNER_ACTIVE;
    }
    if (phase == UCN_I_COMPOSITION_RELOADING ||
        phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED ||
        phase == UCN_I_COMPOSITION_STOPPING ||
        phase == UCN_I_COMPOSITION_FAULT) {
        return UCN_I_COMPOSITION_OWNER_INITIALIZED;
    }
    return UCN_I_COMPOSITION_OWNER_REGISTERED;
}

static ucn_i_composition_owner_state_t expected_business_state(
    const ucn_i_composition_runtime_t *runtime,
    uint8_t plan_index)
{
    if (plan_index >= runtime->initialized_count) {
        return UCN_I_COMPOSITION_OWNER_RESERVED;
    }
    if (runtime->phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED ||
        runtime->phase == UCN_I_COMPOSITION_RUNNING) {
        return UCN_I_COMPOSITION_OWNER_ACTIVE;
    }
    return UCN_I_COMPOSITION_OWNER_INITIALIZED;
}

bool ucn_i_composition_runtime_valid(
    const ucn_i_composition_runtime_t *runtime)
{
    size_t required;
    size_t offset;
    uint8_t record_index;
    uint8_t plan_index;

    if (runtime == NULL ||
        !pointer_is_aligned(runtime, UCN_I_COMPOSITION_STORAGE_ALIGNMENT) ||
        runtime->magic != UCN_I_COMPOSITION_RUNTIME_MAGIC ||
        runtime->runtime_instance == 0U ||
        runtime->schema != UCN_I_COMPOSITION_RUNTIME_SCHEMA ||
        runtime->owner_instance_base == 0U ||
        runtime->owner_instance_base >
            UINT16_MAX - UCN_I_COMPOSITION_MODULE_COORDINATOR ||
        (runtime->phase < UCN_I_COMPOSITION_PREPARED ||
         runtime->phase > UCN_I_COMPOSITION_FAULT) ||
        runtime->registry_count != (uint8_t)(runtime->plan.init_count + 2U) ||
        runtime->registry_count > UCN_I_COMPOSITION_REGISTRY_CAPACITY ||
        runtime->initialized_count > runtime->plan.init_count ||
        !bytes_are_zero(runtime->reserved_zero,
                        sizeof(runtime->reserved_zero)) ||
        composition_layout_required(&runtime->plan, &required) != UCN_OK ||
        runtime->storage_bytes != required) {
        return false;
    }
    if (!owner_record_is_exact(
            runtime, &runtime->registry[0],
            UCN_I_COMPOSITION_MODULE_FOUNDATION, UCN_I_COMPOSE_FOUNDATION,
            0U, 0U, expected_foundation_state(runtime->phase)) ||
        !owner_record_is_exact(
            runtime, &runtime->registry[1],
            UCN_I_COMPOSITION_MODULE_COORDINATOR, 0U,
            (uint32_t)offsetof(ucn_i_composition_runtime_t, coordinator),
            (uint32_t)sizeof(runtime->coordinator),
            UCN_I_COMPOSITION_OWNER_ACTIVE) ||
        !coordinator_is_pristine(runtime) || !lifecycle_gate_is_idle(runtime) ||
        size_align_up(sizeof(*runtime), UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                      &offset) != UCN_OK) {
        return false;
    }
    if ((runtime->phase == UCN_I_COMPOSITION_PREPARED ||
         runtime->phase == UCN_I_COMPOSITION_QUIESCENT) !=
            (runtime->foundation_node == NULL) ||
        (runtime->foundation_node == NULL) !=
            (runtime->foundation_storage_bytes == 0U) ||
        (runtime->foundation_node != NULL &&
         (runtime->foundation_storage_bytes != ucn_storage_required() ||
          ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                               runtime->foundation_node,
                               runtime->foundation_storage_bytes))) ||
        ((runtime->phase == UCN_I_COMPOSITION_PREPARED ||
          runtime->phase == UCN_I_COMPOSITION_QUIESCENT) &&
         (runtime->initialized_count != 0U ||
          runtime->persistence_ready != 0U)) ||
        (runtime->phase == UCN_I_COMPOSITION_RELOADING &&
         (runtime->initialized_count != runtime->plan.init_count ||
          runtime->persistence_ready != 0U)) ||
        ((runtime->phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED ||
          runtime->phase == UCN_I_COMPOSITION_RUNNING) &&
         runtime->initialized_count != runtime->plan.init_count) ||
        runtime->persistence_ready > 1U) {
        return false;
    }
    record_index = 2U;
    for (plan_index = 0U; plan_index < runtime->plan.init_count;
         ++plan_index, ++record_index) {
        const ucn_i_composition_module_id_t module_id =
            runtime->plan.init_order[plan_index];
        const ucn_i_composition_module_mask_t module =
            module_bit_from_id(module_id);
        const size_t owner_bytes = module_owner_bytes(module);

        if (size_align_up(offset, UCN_I_COMPOSITION_STORAGE_ALIGNMENT,
                          &offset) != UCN_OK ||
            offset > UINT32_MAX || owner_bytes > UINT32_MAX ||
            !owner_record_is_exact(
                runtime, &runtime->registry[record_index], module_id, module,
                (uint32_t)offset, (uint32_t)owner_bytes,
                expected_business_state(runtime, plan_index)) ||
            (plan_index >= runtime->initialized_count &&
             !bytes_are_zero((const uint8_t *)runtime + offset,
                             owner_bytes)) ||
            (plan_index < runtime->initialized_count &&
             bytes_are_zero((const uint8_t *)runtime + offset,
                            owner_bytes))) {
            return false;
        }
        offset += owner_bytes;
    }
    for (; record_index < UCN_I_COMPOSITION_REGISTRY_CAPACITY;
         ++record_index) {
        if (!bytes_are_zero(&runtime->registry[record_index],
                            sizeof(runtime->registry[record_index]))) {
            return false;
        }
    }
    return offset == required;
}

ucn_result_t ucn_i_composition_owner_view(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id,
    ucn_i_composition_owner_view_t *view_out)
{
    const ucn_i_composition_owner_record_t *record;
    ucn_i_composition_owner_view_t view;

    if (!ucn_i_composition_runtime_valid(runtime) || view_out == NULL ||
        module_id > UCN_I_COMPOSITION_MODULE_COORDINATOR ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    record = owner_record_find(runtime, module_id);
    if (record == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    memset(&view, 0, sizeof(view));
    view.ref = record->ref;
    view.module_mask = record->module_mask;
    view.storage_offset = record->storage_offset;
    view.storage_bytes = record->storage_bytes;
    view.state = record->state;
    *view_out = view;
    return UCN_OK;
}

static bool typed_adapter_is_exact(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_typed_adapter_t *adapter)
{
    const ucn_i_composition_owner_record_t *requester;
    const ucn_i_composition_owner_record_t *target;
    const ucn_i_composition_module_id_t target_module =
        adapter == NULL ? UINT8_MAX :
                          dependency_target_module(adapter->dependency_kind);

    if (adapter == NULL || adapter->struct_size != sizeof(*adapter) ||
        adapter->schema != UCN_I_COMPOSITION_ADAPTER_SCHEMA ||
        adapter->composition_digest != runtime->plan.composition_digest ||
        target_module == UINT8_MAX ||
        !bytes_are_zero(adapter->reserved_zero,
                        sizeof(adapter->reserved_zero))) {
        return false;
    }
    requester = owner_record_find(runtime, adapter->requester.module_id);
    target = owner_record_find(runtime, target_module);
    return requester != NULL && target != NULL &&
           owner_ref_equal(&requester->ref, &adapter->requester) &&
           owner_ref_equal(&target->ref, &adapter->target);
}

ucn_result_t ucn_i_composition_typed_adapter_build(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_owner_ref_t *requester,
    ucn_i_dependency_kind_t dependency_kind,
    ucn_i_composition_typed_adapter_t *adapter_out)
{
    const ucn_i_composition_owner_record_t *requester_record;
    const ucn_i_composition_owner_record_t *target_record;
    const ucn_i_composition_module_id_t target_module =
        dependency_target_module(dependency_kind);
    ucn_i_composition_typed_adapter_t adapter;

    if (!ucn_i_composition_runtime_valid(runtime) || requester == NULL ||
        adapter_out == NULL || target_module == UINT8_MAX ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, requester,
                             sizeof(*requester)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, adapter_out,
                             sizeof(*adapter_out)) ||
        ucn_i_ranges_overlap(requester, sizeof(*requester), adapter_out,
                             sizeof(*adapter_out))) {
        return UCN_ERR_ARGUMENT;
    }
    requester_record = owner_record_find(runtime, requester->module_id);
    if (requester_record == NULL ||
        !owner_ref_equal(&requester_record->ref, requester)) {
        return UCN_ERR_STATE;
    }
    target_record = owner_record_find(runtime, target_module);
    if (target_record == NULL) {
        return UCN_ERR_UNSUPPORTED;
    }
    memset(&adapter, 0, sizeof(adapter));
    adapter.struct_size = (uint16_t)sizeof(adapter);
    adapter.schema = UCN_I_COMPOSITION_ADAPTER_SCHEMA;
    adapter.composition_digest = runtime->plan.composition_digest;
    adapter.requester = *requester;
    adapter.target = target_record->ref;
    adapter.dependency_kind = dependency_kind;
    *adapter_out = adapter;
    return UCN_OK;
}

ucn_result_t ucn_i_composition_route_requirement(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_typed_adapter_t *adapter,
    const ucn_i_dependency_requirement_t *requirement,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    const ucn_i_composition_owner_record_t *requester;
    const ucn_i_composition_owner_record_t *target;

    if (!ucn_i_composition_runtime_valid(runtime) || adapter == NULL ||
        requirement == NULL || handle_out == NULL ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, adapter,
                             sizeof(*adapter)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, requirement,
                             sizeof(*requirement)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), requirement,
                             sizeof(*requirement)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(requirement, sizeof(*requirement), handle_out,
                             sizeof(*handle_out)) ||
        !typed_adapter_is_exact(runtime, adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    requester = owner_record_find(runtime, adapter->requester.module_id);
    target = owner_record_find(runtime, adapter->target.module_id);
    if (requester == NULL || target == NULL ||
        requester->state != UCN_I_COMPOSITION_OWNER_ACTIVE ||
        target->state != UCN_I_COMPOSITION_OWNER_ACTIVE ||
        requirement->runtime_instance != runtime->runtime_instance ||
        requirement->requester_owner_instance !=
            adapter->requester.owner_instance ||
        requirement->kind != adapter->dependency_kind) {
        return UCN_ERR_STATE;
    }
    return ucn_i_coordinator_route_requirement(
        &runtime->coordinator, requirement, now_us, handle_out);
}

ucn_result_t ucn_i_composition_route_event(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_typed_adapter_t *adapter,
    const ucn_i_dependency_event_t *event,
    uint64_t now_us)
{
    const ucn_i_composition_owner_record_t *requester;
    const ucn_i_composition_owner_record_t *target;

    if (!ucn_i_composition_runtime_valid(runtime) || adapter == NULL ||
        event == NULL ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, adapter,
                             sizeof(*adapter)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes, event,
                             sizeof(*event)) ||
        ucn_i_ranges_overlap(adapter, sizeof(*adapter), event,
                             sizeof(*event)) ||
        !typed_adapter_is_exact(runtime, adapter)) {
        return UCN_ERR_ARGUMENT;
    }
    requester = owner_record_find(runtime, adapter->requester.module_id);
    target = owner_record_find(runtime, adapter->target.module_id);
    if (requester == NULL || target == NULL ||
        requester->state != UCN_I_COMPOSITION_OWNER_ACTIVE ||
        target->state != UCN_I_COMPOSITION_OWNER_ACTIVE ||
        event->runtime_instance != runtime->runtime_instance ||
        event->owner_instance != adapter->target.owner_instance ||
        event->dependency_kind != adapter->dependency_kind) {
        return UCN_ERR_STATE;
    }
    return ucn_i_coordinator_route_event(&runtime->coordinator, event,
                                         now_us);
}

static void *owner_storage(ucn_i_composition_runtime_t *runtime,
                           ucn_i_composition_module_id_t module_id)
{
    ucn_i_composition_owner_record_t *record =
        owner_record_find_mutable(runtime, module_id);

    if (record == NULL || record->storage_bytes == 0U ||
        record->storage_offset >= runtime->storage_bytes) {
        return NULL;
    }
    return (uint8_t *)runtime + record->storage_offset;
}

static const void *module_config_pointer(
    const ucn_i_composition_start_config_t *config,
    ucn_i_composition_module_id_t module_id)
{
    switch (module_id) {
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE:
            return config->persistence;
        case UCN_I_COMPOSITION_MODULE_IDENTITY:
            return config->identity;
        case UCN_I_COMPOSITION_MODULE_SECURITY:
            return config->security;
        case UCN_I_COMPOSITION_MODULE_ADMISSION:
            return config->admission;
        case UCN_I_COMPOSITION_MODULE_CAPABILITY:
            return config->capability;
        case UCN_I_COMPOSITION_MODULE_ROUTE:
            return config->route;
        case UCN_I_COMPOSITION_MODULE_FLOW:
            return config->flow;
        case UCN_I_COMPOSITION_MODULE_TRANSPORT:
            return config->transport;
        case UCN_I_COMPOSITION_MODULE_SERVICE:
            return config->service;
        case UCN_I_COMPOSITION_MODULE_REALTIME:
            return config->realtime;
        case UCN_I_COMPOSITION_MODULE_GROUP:
            return config->group;
        case UCN_I_COMPOSITION_MODULE_CLUSTER:
            return config->cluster;
        default:
            return NULL;
    }
}

static size_t module_config_bytes(
    ucn_i_composition_module_id_t module_id)
{
    switch (module_id) {
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE:
            return sizeof(ucn_persistence_config_t);
        case UCN_I_COMPOSITION_MODULE_IDENTITY:
            return sizeof(ucn_i_identity_config_t);
        case UCN_I_COMPOSITION_MODULE_SECURITY:
            return sizeof(ucn_i_security_config_t);
        case UCN_I_COMPOSITION_MODULE_ADMISSION:
            return sizeof(ucn_i_admission_config_t);
        case UCN_I_COMPOSITION_MODULE_CAPABILITY:
            return sizeof(ucn_i_composition_capability_config_t);
        case UCN_I_COMPOSITION_MODULE_ROUTE:
            return sizeof(ucn_i_route_config_t);
        case UCN_I_COMPOSITION_MODULE_FLOW:
            return sizeof(ucn_i_flow_config_t);
        case UCN_I_COMPOSITION_MODULE_TRANSPORT:
            return sizeof(ucn_i_transport_config_t);
        case UCN_I_COMPOSITION_MODULE_SERVICE:
            return sizeof(ucn_i_service_config_t);
        case UCN_I_COMPOSITION_MODULE_REALTIME:
            return sizeof(ucn_i_realtime_config_t);
        case UCN_I_COMPOSITION_MODULE_GROUP:
            return sizeof(ucn_i_group_config_t);
        case UCN_I_COMPOSITION_MODULE_CLUSTER:
            return sizeof(ucn_i_cluster_config_t);
        default:
            return 0U;
    }
}

static bool module_config_identity_is_exact(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id,
    const void *module_config)
{
    const ucn_i_composition_owner_record_t *record =
        owner_record_find(runtime, module_id);
    uint32_t runtime_instance = 0U;
    uint16_t owner_instance = 0U;

    if (record == NULL || module_config == NULL) {
        return false;
    }
    switch (module_id) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE: {
            const ucn_persistence_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#endif
        case UCN_I_COMPOSITION_MODULE_IDENTITY: {
            const ucn_i_identity_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_SECURITY: {
            const ucn_i_security_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_ADMISSION: {
            const ucn_i_admission_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_CAPABILITY: {
            const ucn_i_composition_capability_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_ROUTE: {
            const ucn_i_route_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_FLOW: {
            const ucn_i_flow_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_TRANSPORT: {
            const ucn_i_transport_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
        case UCN_I_COMPOSITION_MODULE_SERVICE: {
            const ucn_i_service_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#if UCN_V6S_FEATURE_REALTIME_ENABLED
        case UCN_I_COMPOSITION_MODULE_REALTIME: {
            const ucn_i_realtime_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#endif
#if UCN_V6S_FEATURE_GROUP_ENABLED
        case UCN_I_COMPOSITION_MODULE_GROUP: {
            const ucn_i_group_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#endif
#if UCN_V6S_FEATURE_CLUSTER_ENABLED
        case UCN_I_COMPOSITION_MODULE_CLUSTER: {
            const ucn_i_cluster_config_t *value = module_config;
            runtime_instance = value->runtime_instance;
            owner_instance = value->owner_instance;
            break;
        }
#endif
        default:
            return false;
    }
    return runtime_instance == runtime->runtime_instance &&
           owner_instance == record->ref.owner_instance;
}

static bool start_config_is_valid(
    const ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_start_config_t *config)
{
    uint8_t module_id;

    if (config == NULL || config->struct_size != sizeof(*config) ||
        config->schema != UCN_I_COMPOSITION_LIFECYCLE_SCHEMA ||
        config->reserved_zero != 0U || config->foundation_storage == NULL ||
        config->foundation_storage_bytes != ucn_storage_required() ||
        config->foundation_config == NULL ||
        config->foundation_ports == NULL ||
        config->foundation_config->runtime_instance !=
            runtime->runtime_instance ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             config, sizeof(*config)) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             config->foundation_storage,
                             config->foundation_storage_bytes) ||
        ucn_i_ranges_overlap(config->foundation_storage,
                             config->foundation_storage_bytes,
                             config, sizeof(*config)) ||
        ucn_i_ranges_overlap(config->foundation_storage,
                             config->foundation_storage_bytes,
                             config->foundation_config,
                             sizeof(*config->foundation_config)) ||
        ucn_i_ranges_overlap(config->foundation_storage,
                             config->foundation_storage_bytes,
                             config->foundation_ports,
                             sizeof(*config->foundation_ports)) ||
        !bytes_are_zero(config->foundation_storage,
                        config->foundation_storage_bytes)) {
        return false;
    }
    for (module_id = UCN_I_COMPOSITION_MODULE_PERSISTENCE;
         module_id <= UCN_I_COMPOSITION_MODULE_CLUSTER; ++module_id) {
        const bool selected =
            (runtime->plan.selected_module_mask &
             module_bit_from_id(module_id)) != 0U;
        const void *module_config =
            module_config_pointer(config, module_id);
        const size_t config_bytes = module_config_bytes(module_id);

        if (selected != (module_config != NULL) || config_bytes == 0U ||
            (module_config != NULL &&
             (!module_config_identity_is_exact(runtime, module_id,
                                               module_config) ||
              ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                                   module_config, config_bytes) ||
              ucn_i_ranges_overlap(config->foundation_storage,
                                   config->foundation_storage_bytes,
                                   module_config, config_bytes)))) {
            return false;
        }
    }
    if (((runtime->plan.selected_module_mask & UCN_I_COMPOSE_ROUTE) != 0U) !=
            (config->route_state_lock != NULL) ||
        ((runtime->plan.selected_module_mask & UCN_I_COMPOSE_FLOW) != 0U) !=
            (config->flow_state_lock != NULL)) {
        return false;
    }
    return true;
}

static ucn_result_t composition_enter(
    ucn_i_composition_runtime_t *runtime,
    uint32_t operation_id,
    ucn_i_callback_claim_t *claim_out)
{
    ucn_i_callback_claim_t claim;
    ucn_result_t result;

    memset(&claim, 0, sizeof(claim));
    claim.owner_instance = (uint32_t)(runtime->owner_instance_base +
                                     UCN_I_COMPOSITION_MODULE_COORDINATOR);
    claim.operation_id = operation_id;
    claim.operation_generation = UINT16_C(1);
    claim.operation_kind = UCN_I_COMPOSITION_OPERATION_LIFECYCLE;
    result = ucn_i_callback_gate_enter(&runtime->lifecycle_gate, &claim);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    *claim_out = claim;
    return UCN_OK;
}

static ucn_result_t composition_leave(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_callback_claim_t *claim,
    ucn_result_t result)
{
    if (ucn_i_callback_gate_leave(&runtime->lifecycle_gate, claim) != UCN_OK) {
        runtime->phase = UCN_I_COMPOSITION_FAULT;
        return UCN_ERR_STATE;
    }
    return result;
}

static ucn_result_t module_init(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id,
    const ucn_i_composition_start_config_t *config)
{
    void *storage = owner_storage(runtime, module_id);
    const ucn_i_composition_owner_record_t *record =
        owner_record_find(runtime, module_id);

    if (storage == NULL || record == NULL) {
        return UCN_ERR_STATE;
    }
    switch (module_id) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE: {
            ucn_persistence_owner_t *owner = NULL;
            ucn_result_t result = ucn_persistence_init_in_place(
                storage, record->storage_bytes, config->persistence, &owner);
            return result == UCN_OK && owner != storage ? UCN_ERR_STATE : result;
        }
#endif
        case UCN_I_COMPOSITION_MODULE_IDENTITY:
            return ucn_i_identity_owner_init(storage, config->identity);
        case UCN_I_COMPOSITION_MODULE_SECURITY:
            return ucn_i_security_owner_init(storage, config->security);
        case UCN_I_COMPOSITION_MODULE_ADMISSION:
            return ucn_i_admission_owner_init(storage, config->admission);
        case UCN_I_COMPOSITION_MODULE_CAPABILITY:
            return ucn_i_capability_owner_init(
                storage, config->capability->runtime_instance,
                config->capability->realm_id,
                config->capability->owner_instance,
                config->capability->security_owner_instance,
                config->capability->discovery_lease_us,
                config->capability->capability_lease_us,
                &config->capability->state_lock);
        case UCN_I_COMPOSITION_MODULE_ROUTE:
            return ucn_i_route_owner_init(storage, config->route,
                                          config->route_state_lock);
        case UCN_I_COMPOSITION_MODULE_FLOW:
            return ucn_i_flow_owner_init(storage, config->flow,
                                         config->flow_state_lock);
        case UCN_I_COMPOSITION_MODULE_TRANSPORT:
            return ucn_i_transport_owner_init(storage, config->transport);
        case UCN_I_COMPOSITION_MODULE_SERVICE:
            return ucn_i_service_owner_init(storage, config->service);
#if UCN_V6S_FEATURE_REALTIME_ENABLED
        case UCN_I_COMPOSITION_MODULE_REALTIME:
            return ucn_i_realtime_owner_init(storage, config->realtime);
#endif
#if UCN_V6S_FEATURE_GROUP_ENABLED
        case UCN_I_COMPOSITION_MODULE_GROUP:
            return ucn_i_group_owner_init(storage, config->group);
#endif
#if UCN_V6S_FEATURE_CLUSTER_ENABLED
        case UCN_I_COMPOSITION_MODULE_CLUSTER:
            return ucn_i_cluster_owner_init(storage, config->cluster);
#endif
        default:
            return UCN_ERR_UNSUPPORTED;
    }
}

static ucn_result_t module_destroy(
    ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_module_id_t module_id)
{
    void *storage = owner_storage(runtime, module_id);

    if (storage == NULL) {
        return UCN_ERR_STATE;
    }
    switch (module_id) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        case UCN_I_COMPOSITION_MODULE_PERSISTENCE:
            return ucn_persistence_deinit(storage);
#endif
        case UCN_I_COMPOSITION_MODULE_IDENTITY:
            return ucn_i_identity_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_SECURITY:
            return ucn_i_security_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_ADMISSION:
            return ucn_i_admission_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_CAPABILITY:
            return ucn_i_capability_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_ROUTE:
            return ucn_i_route_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_FLOW:
            return ucn_i_flow_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_TRANSPORT:
            return ucn_i_transport_owner_destroy(storage);
        case UCN_I_COMPOSITION_MODULE_SERVICE:
            return ucn_i_service_owner_destroy(storage);
#if UCN_V6S_FEATURE_REALTIME_ENABLED
        case UCN_I_COMPOSITION_MODULE_REALTIME:
            return ucn_i_realtime_owner_destroy(storage);
#endif
#if UCN_V6S_FEATURE_GROUP_ENABLED
        case UCN_I_COMPOSITION_MODULE_GROUP:
            return ucn_i_group_owner_destroy(storage);
#endif
#if UCN_V6S_FEATURE_CLUSTER_ENABLED
        case UCN_I_COMPOSITION_MODULE_CLUSTER:
            return ucn_i_cluster_owner_reset(storage);
#endif
        default:
            return UCN_ERR_UNSUPPORTED;
    }
}

static ucn_result_t rollback_initialized(
    ucn_i_composition_runtime_t *runtime)
{
    while (runtime->initialized_count != 0U) {
        const uint8_t index = (uint8_t)(runtime->initialized_count - 1U);
        const ucn_i_composition_module_id_t module_id =
            runtime->plan.init_order[index];
        ucn_i_composition_owner_record_t *record =
            owner_record_find_mutable(runtime, module_id);
        ucn_result_t result = module_destroy(runtime, module_id);

        if (result != UCN_OK || record == NULL) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return UCN_ERR_STATE;
        }
        record->state = UCN_I_COMPOSITION_OWNER_RESERVED;
        --runtime->initialized_count;
    }
    if (runtime->foundation_node != NULL) {
        ucn_i_composition_owner_record_t *foundation =
            owner_record_find_mutable(
                runtime, UCN_I_COMPOSITION_MODULE_FOUNDATION);
        if (ucn_stop(runtime->foundation_node) != UCN_OK ||
            ucn_deinit(runtime->foundation_node) != UCN_OK ||
            foundation == NULL) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return UCN_ERR_STATE;
        }
        runtime->foundation_node = NULL;
        runtime->foundation_storage_bytes = 0U;
        foundation->state = UCN_I_COMPOSITION_OWNER_REGISTERED;
    }
    runtime->persistence_ready = 0U;
    runtime->phase = UCN_I_COMPOSITION_PREPARED;
    return UCN_OK;
}

static void publish_initialized_owners(
    ucn_i_composition_runtime_t *runtime)
{
    uint8_t index;

    for (index = 0U; index < runtime->initialized_count; ++index) {
        ucn_i_composition_owner_record_t *record = owner_record_find_mutable(
            runtime, runtime->plan.init_order[index]);
        record->state = UCN_I_COMPOSITION_OWNER_ACTIVE;
    }
    runtime->phase = UCN_I_COMPOSITION_OWNERS_PUBLISHED;
}

static void lifecycle_view_fill(
    const ucn_i_composition_runtime_t *runtime,
    uint16_t operations,
    bool made_progress,
    ucn_i_composition_lifecycle_view_t *view)
{
    uint8_t index;

    memset(view, 0, sizeof(*view));
    view->struct_size = (uint16_t)sizeof(*view);
    view->schema = UCN_I_COMPOSITION_LIFECYCLE_SCHEMA;
    view->operations = operations;
    view->phase = runtime->phase;
    view->initialized_count = runtime->initialized_count;
    view->persistence_ready = runtime->persistence_ready;
    view->foundation_ready =
        runtime->registry[0].state == UCN_I_COMPOSITION_OWNER_ACTIVE ? 1U : 0U;
    view->made_progress = made_progress ? 1U : 0U;
    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].state == UCN_I_COMPOSITION_OWNER_ACTIVE) {
            ++view->active_owner_count;
        }
    }
}

ucn_result_t ucn_i_composition_start_begin(
    ucn_i_composition_runtime_t *runtime,
    const ucn_i_composition_start_config_t *config)
{
    ucn_i_callback_claim_t claim;
    ucn_node_t *foundation = NULL;
    uint8_t index;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) ||
        runtime->phase != UCN_I_COMPOSITION_PREPARED ||
        !start_config_is_valid(runtime, config)) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_enter(runtime, UINT32_C(1), &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_init(config->foundation_storage,
                      config->foundation_storage_bytes,
                      config->foundation_config,
                      config->foundation_ports,
                      &foundation);
    if (result != UCN_OK) {
        return composition_leave(runtime, &claim, result);
    }
    runtime->foundation_node = foundation;
    runtime->foundation_storage_bytes =
        (uint32_t)config->foundation_storage_bytes;
    runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_INITIALIZED;

    for (index = 0U; index < runtime->plan.init_count; ++index) {
        const ucn_i_composition_module_id_t module_id =
            runtime->plan.init_order[index];
        ucn_i_composition_owner_record_t *record =
            owner_record_find_mutable(runtime, module_id);

        result = module_init(runtime, module_id, config);
        if (result != UCN_OK || record == NULL) {
            ucn_result_t rollback = rollback_initialized(runtime);
            return composition_leave(
                runtime, &claim,
                rollback == UCN_OK ? result : UCN_ERR_STATE);
        }
        record->state = UCN_I_COMPOSITION_OWNER_INITIALIZED;
        ++runtime->initialized_count;
    }

    if ((runtime->plan.selected_module_mask & UCN_I_COMPOSE_PERSISTENCE) !=
        0U) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        result = ucn_i_persistence_start_recovery(owner_storage(
            runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE));
#else
        result = UCN_ERR_UNSUPPORTED;
#endif
        if (result != UCN_OK) {
            ucn_result_t rollback = rollback_initialized(runtime);
            return composition_leave(
                runtime, &claim,
                rollback == UCN_OK ? result : UCN_ERR_STATE);
        }
        runtime->phase = UCN_I_COMPOSITION_RELOADING;
    } else {
        publish_initialized_owners(runtime);
    }
    return composition_leave(runtime, &claim, UCN_OK);
}

ucn_result_t ucn_i_composition_start_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_i_composition_lifecycle_view_t *view_out)
{
    ucn_i_composition_lifecycle_view_t view;
    ucn_i_callback_claim_t claim;
    uint16_t operations = 0U;
    bool progress = false;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) || view_out == NULL ||
        operation_budget == 0U ||
        (runtime->phase != UCN_I_COMPOSITION_RELOADING &&
         runtime->phase != UCN_I_COMPOSITION_OWNERS_PUBLISHED) ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             view_out, sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_enter(runtime, UINT32_C(2), &claim);
    if (result != UCN_OK) {
        return result;
    }
    if (runtime->phase == UCN_I_COMPOSITION_RELOADING) {
#if UCN_FEATURE_PERSISTENCE_ENABLED
        ucn_persistence_step_result_t persist_result;
        ucn_persistence_owner_t *persistence = owner_storage(
            runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE);

        result = ucn_i_persistence_step(persistence, now_us,
                                        operation_budget, &persist_result);
        if (result != UCN_OK || persist_result.domains_faulted != 0U) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return composition_leave(runtime, &claim,
                                     result == UCN_OK ? UCN_ERR_STATE : result);
        }
        operations = persist_result.operations_performed;
        progress = persist_result.made_progress != 0U;
        if (persist_result.owner_ready != 0U) {
            runtime->persistence_ready = 1U;
            publish_initialized_owners(runtime);
            progress = true;
        }
#else
        runtime->phase = UCN_I_COMPOSITION_FAULT;
        return composition_leave(runtime, &claim, UCN_ERR_UNSUPPORTED);
#endif
    }
    if (runtime->phase == UCN_I_COMPOSITION_OWNERS_PUBLISHED &&
        operations < operation_budget) {
        result = ucn_start(runtime->foundation_node, now_us);
        ++operations;
        if (result != UCN_OK) {
            if (ucn_stop(runtime->foundation_node) == UCN_OK) {
                uint8_t index;
                for (index = 0U; index < runtime->initialized_count; ++index) {
                    owner_record_find_mutable(
                        runtime, runtime->plan.init_order[index])->state =
                        UCN_I_COMPOSITION_OWNER_INITIALIZED;
                }
                runtime->phase = UCN_I_COMPOSITION_STOPPING;
            } else {
                runtime->phase = UCN_I_COMPOSITION_FAULT;
            }
            return composition_leave(runtime, &claim, result);
        }
        runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_ACTIVE;
        runtime->phase = UCN_I_COMPOSITION_RUNNING;
        progress = true;
    }
    lifecycle_view_fill(runtime, operations, progress, &view);
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result == UCN_OK) {
        *view_out = view;
    }
    return result;
}

ucn_result_t ucn_i_composition_stop_begin(
    ucn_i_composition_runtime_t *runtime)
{
    ucn_i_callback_claim_t claim;
    uint8_t index;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) ||
        (runtime->phase != UCN_I_COMPOSITION_RELOADING &&
         runtime->phase != UCN_I_COMPOSITION_OWNERS_PUBLISHED &&
         runtime->phase != UCN_I_COMPOSITION_RUNNING &&
         runtime->phase != UCN_I_COMPOSITION_FAULT)) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_enter(runtime, UINT32_C(3), &claim);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_stop(runtime->foundation_node);
    if (result != UCN_OK) {
        return composition_leave(runtime, &claim, result);
    }
    runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_INITIALIZED;
    for (index = 0U; index < runtime->initialized_count; ++index) {
        owner_record_find_mutable(
            runtime, runtime->plan.init_order[index])->state =
            UCN_I_COMPOSITION_OWNER_INITIALIZED;
    }
    runtime->phase = UCN_I_COMPOSITION_STOPPING;
    return composition_leave(runtime, &claim, UCN_OK);
}

static ucn_result_t stop_recovery_until_idle(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    uint16_t operation_budget,
    uint16_t *operations_out,
    bool *progress_out,
    bool *recovery_idle_out)
{
#if UCN_FEATURE_PERSISTENCE_ENABLED
    ucn_persistence_owner_t *persistence;
    ucn_persistence_step_result_t step_result;
    ucn_result_t result;

    if ((runtime->plan.selected_module_mask & UCN_I_COMPOSE_PERSISTENCE) ==
        0U) {
        *recovery_idle_out = true;
        return UCN_OK;
    }
    persistence = owner_storage(runtime,
                                UCN_I_COMPOSITION_MODULE_PERSISTENCE);
    if (persistence == NULL ||
        persistence->owner_phase != UCN_I_PERSIST_OWNER_RECOVERING) {
        *recovery_idle_out = true;
        return UCN_OK;
    }
    result = ucn_i_persistence_step(persistence, now_us, operation_budget,
                                    &step_result);
    if (result != UCN_OK) {
        return result;
    }
    *operations_out = step_result.operations_performed;
    *progress_out = step_result.made_progress != 0U;
    *recovery_idle_out =
        persistence->owner_phase != UCN_I_PERSIST_OWNER_RECOVERING;
    return UCN_OK;
#else
    (void)runtime;
    (void)now_us;
    (void)operation_budget;
    (void)operations_out;
    (void)progress_out;
    *recovery_idle_out = true;
    return UCN_OK;
#endif
}

ucn_result_t ucn_i_composition_stop_step(
    ucn_i_composition_runtime_t *runtime,
    uint64_t now_us,
    uint16_t operation_budget,
    ucn_i_composition_lifecycle_view_t *view_out)
{
    ucn_i_composition_lifecycle_view_t view;
    ucn_i_callback_claim_t claim;
    uint16_t operations = 0U;
    bool progress = false;
    bool recovery_idle = true;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) || view_out == NULL ||
        operation_budget == 0U ||
        runtime->phase != UCN_I_COMPOSITION_STOPPING ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             view_out, sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = composition_enter(runtime, UINT32_C(4), &claim);
    if (result != UCN_OK) {
        return result;
    }
    if (runtime->foundation_node->lifecycle == UCN_LIFECYCLE_STOPPING) {
        ucn_step_budget_t budget;
        ucn_step_result_t step_result;

        memset(&budget, 0, sizeof(budget));
        budget.struct_size = (uint16_t)sizeof(budget);
        budget.api_version = UCN_API_VERSION;
        budget.max_work = operation_budget;
        result = ucn_step(runtime->foundation_node, now_us, &budget,
                          &step_result);
        if (result != UCN_OK) {
            return composition_leave(runtime, &claim, result);
        }
        operations = step_result.work_done;
        progress = step_result.work_done != 0U;
    }
    if (runtime->foundation_node->lifecycle == UCN_LIFECYCLE_QUIESCENT &&
        operations < operation_budget) {
        uint16_t recovery_operations = 0U;
        bool recovery_progress = false;

        result = stop_recovery_until_idle(
            runtime, now_us, (uint16_t)(operation_budget - operations),
            &recovery_operations, &recovery_progress, &recovery_idle);
        if (result != UCN_OK) {
            return composition_leave(runtime, &claim, result);
        }
        operations = (uint16_t)(operations + recovery_operations);
        progress = progress || recovery_progress;
    }
    while (runtime->foundation_node->lifecycle == UCN_LIFECYCLE_QUIESCENT &&
           recovery_idle &&
           operations < operation_budget &&
           runtime->initialized_count != 0U) {
        const uint8_t index = (uint8_t)(runtime->initialized_count - 1U);
        const ucn_i_composition_module_id_t module_id =
            runtime->plan.init_order[index];
        ucn_i_composition_owner_record_t *record =
            owner_record_find_mutable(runtime, module_id);

        result = module_destroy(runtime, module_id);
        ++operations;
        if (result == UCN_ERR_STATE) {
            break;
        }
        if (result != UCN_OK || record == NULL) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return composition_leave(runtime, &claim,
                                     result == UCN_OK ? UCN_ERR_STATE : result);
        }
        record->state = UCN_I_COMPOSITION_OWNER_RESERVED;
        --runtime->initialized_count;
        progress = true;
    }
    if (runtime->initialized_count == 0U &&
        runtime->foundation_node->lifecycle == UCN_LIFECYCLE_QUIESCENT &&
        recovery_idle &&
        operations < operation_budget) {
        result = ucn_deinit(runtime->foundation_node);
        ++operations;
        if (result != UCN_OK) {
            runtime->phase = UCN_I_COMPOSITION_FAULT;
            return composition_leave(runtime, &claim, result);
        }
        runtime->foundation_node = NULL;
        runtime->foundation_storage_bytes = 0U;
        runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_REGISTERED;
        runtime->persistence_ready = 0U;
        runtime->phase = UCN_I_COMPOSITION_QUIESCENT;
        progress = true;
    }
    lifecycle_view_fill(runtime, operations, progress, &view);
    result = composition_leave(runtime, &claim, UCN_OK);
    if (result == UCN_OK) {
        *view_out = view;
    }
    return result;
}

ucn_result_t ucn_i_composition_lifecycle_view(
    const ucn_i_composition_runtime_t *runtime,
    ucn_i_composition_lifecycle_view_t *view_out)
{
    ucn_i_composition_lifecycle_view_t view;

    if (!ucn_i_composition_runtime_valid(runtime) || view_out == NULL ||
        ucn_i_ranges_overlap(runtime, runtime->storage_bytes,
                             view_out, sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    lifecycle_view_fill(runtime, 0U, false, &view);
    *view_out = view;
    return UCN_OK;
}

ucn_result_t ucn_i_composition_destroy(
    ucn_i_composition_runtime_t *runtime)
{
    const uint32_t storage_bytes =
        runtime == NULL ? 0U : runtime->storage_bytes;
    ucn_result_t result;

    if (!ucn_i_composition_runtime_valid(runtime) ||
        (runtime->phase != UCN_I_COMPOSITION_PREPARED &&
         runtime->phase != UCN_I_COMPOSITION_QUIESCENT)) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_callback_gate_destroy(&runtime->lifecycle_gate);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_coordinator_destroy(&runtime->coordinator);
    if (result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    memset(runtime, 0, storage_bytes);
    return UCN_OK;
}
