#include "ucn/v6/ucn_v6_config.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static ucn_v6_feature_manifest_t header_manifest(void)
{
    ucn_v6_feature_manifest_t value;
    memset(&value, 0, sizeof(value));
    value.api_version = UCN_V6_API_VERSION;
    value.storage_layout = UCN_V6_STORAGE_LAYOUT;
    value.feature_bits = UCN_V6_COMPILED_FEATURE_BITS;
    value.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    value.max_bindings = UCN_V6_CONFIG_MAX_BINDINGS;
    value.max_active_groups = UCN_V6_CONFIG_MAX_ACTIVE_GROUPS;
    value.bootstrap_pending = UCN_V6_CONFIG_BOOTSTRAP_PENDING;
    value.bootstrap_links = UCN_V6_CONFIG_BOOTSTRAP_LINKS;
    value.operation_slots = UCN_V6_CONFIG_OPERATION_SLOTS;
    value.operation_high_waters = UCN_V6_CONFIG_OPERATION_HIGH_WATERS;
    value.active_group_slots = UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS;
    value.static_group_slots = UCN_V6_CONFIG_STATIC_GROUP_SLOTS;
    value.group_key_slots = UCN_V6_CONFIG_GROUP_KEY_SLOTS;
    value.owner_event_depth = UCN_V6_CONFIG_OWNER_EVENT_DEPTH;
    return value;
}

int main(void)
{
    union aligned_bytes {
        uint64_t align;
        uint8_t bytes[32];
    } storage;
    ucn_v6_feature_manifest_t manifest = header_manifest();

#if defined(UCN_V6_EXPECT_MANIFEST_MISMATCH)
    CHECK(ucn_v6_manifest_validate_exact(&manifest) == UCN_V6_ERR_CONFIG);
#else
    CHECK(ucn_v6_manifest_validate_exact(&manifest) == UCN_V6_OK);
    CHECK(memcmp(&manifest, ucn_v6_compiled_manifest(),
                 sizeof(manifest)) == 0);
#endif
    CHECK(ucn_v6_storage_validate(
              storage.bytes, sizeof(storage), sizeof(storage),
              UCN_V6_STORAGE_ALIGNMENT) == UCN_V6_OK);
    CHECK(ucn_v6_storage_validate(
              storage.bytes + 1U, sizeof(storage) - 1U, 8U,
              UCN_V6_STORAGE_ALIGNMENT) == UCN_V6_ERR_CONFIG);
    CHECK(ucn_v6_storage_validate(
              storage.bytes, 7U, 8U,
              UCN_V6_STORAGE_ALIGNMENT) == UCN_V6_ERR_CONFIG);
    puts("ucn v6 config contract tests passed");
    return 0;
}
