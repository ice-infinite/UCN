#include "ucn/ucn.h"

/*
 * EN: Returns the immutable UCN library version string.
 * 中文：返回不可变的 UCN 库版本字符串。
 */
const char *ucn_version(void)
{
    return "5.0.0";
}

/*
 * EN: Validates the immutable Node identity and hop-limit configuration.
 * 中文：验证不可变的 Node 身份与跳数上限配置。
 */
ucn_result_t ucn_validate_config(const ucn_config_t *config)
{
    if (config == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    if (config->network_id == 0U || config->node_id == 0U ||
        config->node_id == UCN_NODE_BROADCAST) {
        return UCN_ERR_CONFIG;
    }

    if (config->default_hop_limit == 0U ||
        config->default_hop_limit > UCN_MAX_HOPS) {
        return UCN_ERR_CONFIG;
    }

    return UCN_OK;
}
