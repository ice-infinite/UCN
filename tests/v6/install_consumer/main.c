#include <ucn/ucn.h>

int main(void)
{
    const ucn_v6_feature_manifest_t *manifest = ucn_v6_compiled_manifest();

    return (manifest != NULL && manifest->api_version == UCN_V6_API_VERSION &&
            sizeof(ucn_result_t) == 4U && UCN_ERR_IN_DOUBT == -15) ? 0 : 1;
}
