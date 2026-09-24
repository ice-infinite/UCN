#include "internal/ucn_module_scaffold.h"

static uint8_t expected_stage(ucn_i_module_id_t module_id)
{
    static const uint8_t stages[UCN_I_MODULE_COUNT] = {
        3U, 4U, 5U, 6U, 7U, 8U, 8U, 8U
    };

    return module_id < UCN_I_MODULE_COUNT ? stages[module_id] : 0U;
}

static ucn_i_module_dependency_mask_t module_dependency_bit(
    ucn_i_module_id_t module_id)
{
    static const ucn_i_module_dependency_mask_t bits[UCN_I_MODULE_COUNT] = {
        UCN_I_DEP_SECURITY,
        UCN_I_DEP_ADMISSION,
        UCN_I_DEP_ROUTING,
        UCN_I_DEP_TRANSPORT,
        UCN_I_DEP_SERVICE,
        UCN_I_DEP_REALTIME,
        UCN_I_DEP_GROUP,
        UCN_I_DEP_CLUSTER
    };

    return module_id < UCN_I_MODULE_COUNT ? bits[module_id] : 0U;
}

ucn_result_t ucn_i_module_scaffold_validate(
    const ucn_i_module_scaffold_descriptor_t *descriptor)
{
    const ucn_i_module_dependency_mask_t infrastructure =
        UCN_I_DEP_COMMON | UCN_I_DEP_COORDINATOR;
    const ucn_i_module_dependency_mask_t known_dependencies =
        UCN_I_DEP_COMMON | UCN_I_DEP_COORDINATOR | UCN_I_DEP_PERSISTENCE |
        UCN_I_DEP_SECURITY | UCN_I_DEP_ADMISSION | UCN_I_DEP_ROUTING |
        UCN_I_DEP_TRANSPORT | UCN_I_DEP_SERVICE | UCN_I_DEP_REALTIME |
        UCN_I_DEP_GROUP | UCN_I_DEP_CLUSTER;
    const ucn_i_module_scaffold_flags_t required_flags =
        UCN_I_MODULE_SCAFFOLD_ONLY | UCN_I_MODULE_FAILS_CLOSED |
        UCN_I_MODULE_PRIVATE;

    if (descriptor == NULL ||
        descriptor->struct_size != sizeof(*descriptor) ||
        descriptor->schema != UCN_I_MODULE_SCAFFOLD_SCHEMA ||
        descriptor->module_id >= UCN_I_MODULE_COUNT ||
        descriptor->implementation_stage !=
            expected_stage(descriptor->module_id) ||
        descriptor->direct_dependency_mask != infrastructure ||
        descriptor->coordinator_visible_mask == 0U ||
        (descriptor->coordinator_visible_mask & ~known_dependencies) != 0U ||
        (descriptor->coordinator_visible_mask & infrastructure) != 0U ||
        (descriptor->coordinator_visible_mask &
         module_dependency_bit(descriptor->module_id)) != 0U ||
        descriptor->forbidden_direct_owner_mask !=
            descriptor->coordinator_visible_mask ||
        descriptor->flags != required_flags ||
        descriptor->reserved_zero != 0U) {
        return UCN_ERR_CONFIG;
    }
    return UCN_OK;
}

ucn_result_t ucn_i_module_scaffold_business_probe(
    const ucn_i_module_scaffold_descriptor_t *descriptor,
    uint32_t *output_unchanged)
{
    ucn_result_t result;

    if (output_unchanged == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_module_scaffold_validate(descriptor);
    if (result != UCN_OK) {
        return result;
    }
    return UCN_ERR_UNSUPPORTED;
}
