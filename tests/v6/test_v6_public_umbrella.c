#include "ucn/ucn.h"

#include <stdio.h>

int main(void)
{
    const ucn_v6_feature_manifest_t *manifest = ucn_v6_compiled_manifest();
    if (manifest == NULL || manifest->api_version != UCN_V6_API_VERSION ||
        manifest->storage_layout != UCN_V6_STORAGE_LAYOUT ||
        manifest->profile != UCN_V6_PROFILE ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK) {
        return 1;
    }
    puts("ucn v6 public umbrella test passed");
    return 0;
}
