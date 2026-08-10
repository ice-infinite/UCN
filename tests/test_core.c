#include <string.h>

#include "test_support.h"
#include "ucn/ucn.h"

int test_core(void)
{
    ucn_config_t config;

    (void)memset(&config, 0, sizeof(config));
    TEST_ASSERT(strcmp(ucn_version(), "5.0.0") == 0);
    TEST_ASSERT(ucn_validate_config(NULL) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_validate_config(&config) == UCN_ERR_CONFIG);

    config.network_id = UINT32_C(0xAABBCCDD);
    config.node_id = UINT32_C(0x00000001);
    config.default_hop_limit = 3U;
    TEST_ASSERT(ucn_validate_config(&config) == UCN_OK);

    config.default_hop_limit = 0U;
    TEST_ASSERT(ucn_validate_config(&config) == UCN_ERR_CONFIG);
    config.default_hop_limit = (uint8_t)(UCN_MAX_HOPS + 1U);
    TEST_ASSERT(ucn_validate_config(&config) == UCN_ERR_CONFIG);
    return 0;
}
