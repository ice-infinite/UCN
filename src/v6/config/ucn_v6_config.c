#include "ucn/v6/ucn_v6_config.h"

#include <stdint.h>

static const ucn_v6_feature_manifest_t compiled_manifest = {
    UCN_V6_API_VERSION,
    UCN_V6_STORAGE_LAYOUT,
    UCN_V6_COMPILED_FEATURE_BITS,
    UCN_V6_COMPILED_LAYOUT_HASH,
    UCN_V6_CONFIG_MAX_BINDINGS,
    UCN_V6_CONFIG_MAX_ACTIVE_GROUPS,
    UCN_V6_CONFIG_BOOTSTRAP_PENDING,
    UCN_V6_CONFIG_BOOTSTRAP_LINKS,
    UCN_V6_CONFIG_OPERATION_SLOTS,
    UCN_V6_CONFIG_OPERATION_HIGH_WATERS,
    UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS,
    UCN_V6_CONFIG_STATIC_GROUP_SLOTS,
    UCN_V6_CONFIG_GROUP_KEY_SLOTS,
    UCN_V6_CONFIG_OWNER_EVENT_DEPTH
};

const ucn_v6_feature_manifest_t *ucn_v6_compiled_manifest(void)
{
    return &compiled_manifest;
}

ucn_v6_result_t ucn_v6_manifest_validate_exact(
    const ucn_v6_feature_manifest_t *manifest)
{
    const ucn_v6_feature_manifest_t *expected = &compiled_manifest;

    if (manifest == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    return manifest->api_version == expected->api_version &&
           manifest->storage_layout == expected->storage_layout &&
           manifest->feature_bits == expected->feature_bits &&
           manifest->layout_hash == expected->layout_hash &&
           manifest->max_bindings == expected->max_bindings &&
           manifest->max_active_groups == expected->max_active_groups &&
           manifest->bootstrap_pending == expected->bootstrap_pending &&
           manifest->bootstrap_links == expected->bootstrap_links &&
           manifest->operation_slots == expected->operation_slots &&
           manifest->operation_high_waters ==
               expected->operation_high_waters &&
           manifest->active_group_slots == expected->active_group_slots &&
           manifest->static_group_slots == expected->static_group_slots &&
           manifest->group_key_slots == expected->group_key_slots &&
           manifest->owner_event_depth == expected->owner_event_depth ?
               UCN_V6_OK : UCN_V6_ERR_CONFIG;
}

ucn_v6_result_t ucn_v6_storage_validate(
    const void *storage,
    size_t storage_bytes,
    size_t required_bytes,
    size_t required_alignment)
{
    if (storage == NULL || required_bytes == 0U ||
        required_alignment == 0U ||
        (required_alignment & (required_alignment - 1U)) != 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (storage_bytes < required_bytes ||
        ((uintptr_t)storage & (required_alignment - 1U)) != 0U) {
        return UCN_V6_ERR_CONFIG;
    }
    return UCN_V6_OK;
}
