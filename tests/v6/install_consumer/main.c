#include <ucn/ucn.h>

int main(void)
{
    const ucn_v6_feature_manifest_t *manifest = ucn_v6_compiled_manifest();

    return (manifest != NULL && manifest->api_version == UCN_V6_API_VERSION) ? 0 : 1;
}
