#include <string.h>

#include "test_support.h"
#include "ucn/ucn_standard_adapter.h"

static void standard_config_init(ucn_standard_link_config_t *config,
                                 ucn_standard_preset_t preset)
{
    (void)memset(config, 0, sizeof(*config));
    config->local_link_id = 1U;
    config->peer_node_id = UINT32_C(2);
    config->preset = preset;
}

static int test_all_standard_presets(void)
{
    ucn_standard_preset_t preset;

    for (preset = (ucn_standard_preset_t)(UCN_STANDARD_PRESET_UNSPECIFIED + 1U);
         preset < UCN_STANDARD_PRESET_COUNT;
         preset = (ucn_standard_preset_t)(preset + 1U)) {
        ucn_standard_preset_profile_t profile;

        TEST_ASSERT(ucn_standard_preset_resolve(preset, &profile) == UCN_OK);
        TEST_ASSERT(profile.preset == preset);
        TEST_ASSERT(profile.bearer_kind != UCN_BEARER_UNSPECIFIED);
        TEST_ASSERT(profile.base_cost != 0U &&
                    profile.base_cost != UCN_LINK_ROUTE_COST_UNKNOWN);
        TEST_ASSERT(profile.rtt_reference_ms != 0U);
        TEST_ASSERT(profile.maximum_logical_mtu >= UCN_STANDARD_LOGICAL_MTU_MIN);
    }

    return 0;
}

static int test_standard_preset_values(void)
{
    ucn_standard_preset_profile_t profile;

    TEST_ASSERT(ucn_standard_preset_resolve(
                    UCN_STANDARD_PRESET_UART_115200_8N1, &profile) == UCN_OK);
    TEST_ASSERT(profile.bearer_kind == UCN_BEARER_UART &&
                profile.configured_rate_bps == UINT32_C(115200) &&
                profile.base_cost == 34U && profile.rtt_reference_ms == 5U &&
                profile.maximum_logical_mtu == UCN_MAX_FRAME_BYTES &&
                profile.flags == UCN_STANDARD_PRESET_FLAG_NONE);

    TEST_ASSERT(ucn_standard_preset_resolve(
                    UCN_STANDARD_PRESET_RS485_115200_8N1, &profile) == UCN_OK);
    TEST_ASSERT(profile.bearer_kind == UCN_BEARER_RS485 &&
                profile.base_cost == 46U && profile.rtt_reference_ms == 5U);

    TEST_ASSERT(ucn_standard_preset_resolve(
                    UCN_STANDARD_PRESET_CAN_FD_1M_4M, &profile) == UCN_OK);
    TEST_ASSERT(profile.bearer_kind == UCN_BEARER_CAN_FD &&
                profile.configured_rate_bps == UINT32_C(4000000) &&
                profile.base_cost == 12U && profile.rtt_reference_ms == 3U &&
                profile.maximum_logical_mtu == 64U);

    TEST_ASSERT(ucn_standard_preset_resolve(
                    UCN_STANDARD_PRESET_ESPNOW_DEFAULT_1M, &profile) == UCN_OK);
    TEST_ASSERT(profile.bearer_kind == UCN_BEARER_WIFI &&
                profile.configured_rate_bps == UINT32_C(1000000) &&
                profile.base_cost == 45U && profile.rtt_reference_ms == 12U &&
                profile.maximum_logical_mtu == 250U);

    TEST_ASSERT(ucn_standard_preset_resolve(
                    UCN_STANDARD_PRESET_WIFI_UNKNOWN_RATE, &profile) == UCN_OK);
    TEST_ASSERT(profile.configured_rate_bps == 0U && profile.base_cost == 45U);

    TEST_ASSERT(ucn_standard_preset_resolve(
                    UCN_STANDARD_PRESET_USB_CDC_HS, &profile) == UCN_OK);
    TEST_ASSERT(profile.bearer_kind == UCN_BEARER_USB_CDC &&
                profile.configured_rate_bps == UINT32_C(480000000) &&
                profile.base_cost == 6U && profile.rtt_reference_ms == 4U);

    TEST_ASSERT(ucn_standard_preset_resolve(
                    UCN_STANDARD_PRESET_LORA_SF12_BW125K, &profile) == UCN_OK);
    TEST_ASSERT(profile.bearer_kind == UCN_BEARER_LORA_P2P &&
                profile.configured_rate_bps == 0U && profile.base_cost == 820U &&
                (profile.flags & UCN_STANDARD_PRESET_FLAG_CONDITIONAL) != 0U);

    return 0;
}

static int test_standard_link_config_priority(void)
{
    ucn_standard_link_config_t config;
    ucn_standard_resolved_link_config_t resolved;

    standard_config_init(&config, UCN_STANDARD_PRESET_UART_115200_8N1);
    config.peer_address.length = 6U;
    config.peer_address.bytes[0] = 0x10U;
    config.peer_address.bytes[1] = 0x11U;
    config.peer_address.bytes[2] = 0x12U;
    config.peer_address.bytes[3] = 0x13U;
    config.peer_address.bytes[4] = 0x14U;
    config.peer_address.bytes[5] = 0x15U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_OK);
    TEST_ASSERT(resolved.local_link_id == 1U && resolved.peer_node_id == UINT32_C(2) &&
                resolved.logical_mtu == UCN_MAX_FRAME_BYTES &&
                resolved.base_cost == 34U && resolved.rtt_reference_ms == 5U &&
                resolved.administrative_bias == 0);

    config.required_logical_mtu = 64U;
    config.override_base_cost = true;
    config.base_cost_override = 77U;
    config.override_rtt_reference = true;
    config.rtt_reference_ms_override = 21U;
    config.administrative_bias = -10;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_OK);
    TEST_ASSERT(resolved.logical_mtu == 64U && resolved.base_cost == 77U &&
                resolved.rtt_reference_ms == 21U &&
                resolved.administrative_bias == -10 &&
                resolved.profile.base_cost == 34U &&
                resolved.profile.rtt_reference_ms == 5U);

    return 0;
}

static int test_standard_link_config_mtu_and_carrier(void)
{
    ucn_standard_link_config_t config;
    ucn_standard_resolved_link_config_t resolved;

    standard_config_init(&config, UCN_STANDARD_PRESET_CAN_FD_1M_4M);
    config.required_logical_mtu = 64U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_OK);
    TEST_ASSERT(resolved.logical_mtu == 64U && resolved.base_cost == 12U);
    config.required_logical_mtu = 65U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);

    standard_config_init(&config, UCN_STANDARD_PRESET_ESPNOW_DEFAULT_1M);
    config.required_logical_mtu = 250U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_OK);
    config.required_logical_mtu = 251U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);

    standard_config_init(&config, UCN_STANDARD_PRESET_CAN_CLASSIC_500K);
    config.required_logical_mtu = 64U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.carrier_enabled = true;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_OK);
    TEST_ASSERT((resolved.profile.flags & UCN_STANDARD_PRESET_FLAG_REQUIRES_CARRIER) != 0U);

    return 0;
}

static int test_standard_link_config_rejections(void)
{
    ucn_standard_link_config_t config;
    ucn_standard_resolved_link_config_t resolved;

    TEST_ASSERT(ucn_standard_preset_resolve(UCN_STANDARD_PRESET_UART_9600_8N1,
                                            NULL) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_standard_preset_resolve(UCN_STANDARD_PRESET_UNSPECIFIED,
                                            &resolved.profile) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_standard_preset_resolve(UCN_STANDARD_PRESET_COUNT,
                                            &resolved.profile) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_standard_link_config_resolve(NULL, &resolved) == UCN_ERR_ARGUMENT);

    standard_config_init(&config, UCN_STANDARD_PRESET_UART_115200_8N1);
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, NULL) == UCN_ERR_ARGUMENT);
    config.local_link_id = 0U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.local_link_id = 1U;
    config.peer_node_id = UCN_NODE_BROADCAST;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.peer_node_id = UINT32_C(2);
    config.peer_address.length = UCN_ADAPTER_PHYSICAL_ADDRESS_MAX + 1U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.peer_address.length = 0U;
    config.required_logical_mtu = UCN_STANDARD_LOGICAL_MTU_MIN - 1U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.required_logical_mtu = 0U;
    config.override_base_cost = true;
    config.base_cost_override = 0U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.base_cost_override = UCN_LINK_ROUTE_COST_UNKNOWN;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.override_base_cost = false;
    config.override_rtt_reference = true;
    config.rtt_reference_ms_override = 0U;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.override_rtt_reference = false;
    config.administrative_bias = UCN_STANDARD_ADMINISTRATIVE_BIAS_MIN - 1;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);
    config.administrative_bias = UCN_STANDARD_ADMINISTRATIVE_BIAS_MAX + 1;
    TEST_ASSERT(ucn_standard_link_config_resolve(&config, &resolved) == UCN_ERR_CONFIG);

    return 0;
}

int test_standard_adapter(void)
{
    int result = 0;

    result |= test_all_standard_presets();
    result |= test_standard_preset_values();
    result |= test_standard_link_config_priority();
    result |= test_standard_link_config_mtu_and_carrier();
    result |= test_standard_link_config_rejections();
    return result;
}
