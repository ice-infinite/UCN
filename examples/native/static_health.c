#include "ucn/ucn_native.h"
#include "health_schema.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Entirely software loopback. No COM, radio, actuator, or device API. */
static ucn_storage_t core_storage[3];
static ucn_native_storage_t native_storage[3];
static ucn_node_t *nodes[3];
static ucn_native_t *roots[3];
#ifndef UCN_NATIVE_EXAMPLE_SUBSCRIBE
#define UCN_NATIVE_EXAMPLE_SUBSCRIBE(root_, ctx_, out_) ucn_subscription_create((root_), 1U, health_received, (ctx_), (out_))
#endif
#ifndef UCN_NATIVE_EXAMPLE_PUBLISH
#define UCN_NATIVE_EXAMPLE_PUBLISH(root_, handle_, payload_, bytes_) ucn_publish((root_), (handle_), (payload_), (bytes_))
#endif
#ifndef UCN_NATIVE_EXAMPLE_UNSUBSCRIBE
#define UCN_NATIVE_EXAMPLE_UNSUBSCRIBE(root_, handle_) ucn_subscription_close((root_), (handle_))
#endif
static ucn_link_handle_t links[3];
static uint64_t clock_us;
static unsigned samples[3], frames;
static uint8_t driver_gates[3];
static ucn_result_t lock(void *ctx) { (void)ctx; return UCN_OK; }
static void unlock(void *ctx) { (void)ctx; }
static ucn_result_t gate(void *ctx) { uint8_t *b = (uint8_t *)ctx; if (*b) { return UCN_ERR_STATE; } *b = 1U; return UCN_OK; }
static void ungate(void *ctx) { *(uint8_t *)ctx = 0U; }
static ucn_driver_submit_result_t send_frame(void *ctx, ucn_link_handle_t link,
                                             const uint8_t *data, size_t len, ucn_driver_token_t token)
{
    ucn_rx_meta_t meta;
    (void)ctx; (void)link; (void)token;
    assert(len == 27U && data[0] == 0x61U && data[4] >= 1U && data[4] <= 3U);
    {
        uint8_t expected[11] = {0x61U,0x40U,4U,1U,0U,0U,0x40U,0U,0U,0U,0U};
        expected[4] = data[4]; expected[10] = (uint8_t)(frames + 1U);
        assert(memcmp(data, expected, sizeof(expected)) == 0);
        printf("NATIVE_FRAME_HEX=");
        for (size_t b = 0U; b < len; ++b) { printf("%02X", data[b]); }
        puts("");
    }
    memset(&meta, 0, sizeof(meta)); meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION; meta.timestamp_us = clock_us;
    assert(ucn_driver_rx_publish(nodes[data[4] - 1U], links[data[4] - 1U], data, len, &meta) == UCN_OK);
    ++frames; return UCN_DRIVER_COMPLETE;
}
static ucn_result_t cancel_frame(void *ctx, ucn_driver_token_t token) { (void)ctx; (void)token; return UCN_OK; }
static void health_received(void *ctx, const ucn_native_sample_t *sample)
{
    ucn_example_health_t h;
    unsigned *count = (unsigned *)ctx;
    assert(sample->provenance == UCN_NATIVE_STATIC_TRUSTED);
    assert(ucn_health_decode(sample->payload, sample->payload_bytes, &h));
    assert(h.voltage_mv == 24000U && h.current_ma == -1200 && h.temperature_centi_c == 2534 && h.fault_bits == UINT32_C(0x80000001));
    ++*count;
}
int main(void)
{
    unsigned i, j;
    ucn_native_handle_t publisher, subscriptions[3];
    ucn_native_channel_t channel;
    uint8_t payload[16];
    const uint8_t golden[16] = {0,0,0x5d,0xc0,0xff,0xff,0xfb,0x50,9,0xe6,2,1,0x80,0,0,1};
    ucn_example_health_t health = {24000U, -1200, 2534, 2U, 1U, UINT32_C(0x80000001)};
    assert(ucn_health_encode(&health, payload, sizeof(payload)));
    assert(memcmp(golden, payload, sizeof(payload)) == 0);
    for (i = 0U; i < 3U; ++i) {
        ucn_static_binding_t bindings[3]; ucn_config_t cfg; ucn_ports_t ports; ucn_link_port_t port;
        memset(&cfg, 0, sizeof(cfg)); memset(&ports, 0, sizeof(ports)); memset(&port, 0, sizeof(port)); memset(bindings, 0, sizeof(bindings));
        for (j = 0U; j < 3U; ++j) { bindings[j].struct_size = sizeof(bindings[j]); bindings[j].api_version = UCN_API_VERSION;
            bindings[j].address = j + 1U; bindings[j].binding_generation = (j + 1U) * 10U; bindings[j].principal_digest = j + 1U; }
        cfg.struct_size = sizeof(cfg); cfg.api_version = UCN_API_VERSION; cfg.storage_layout = UCN_STORAGE_LAYOUT;
        cfg.compiled_manifest_hash = UCN_COMPILED_MANIFEST_HASH; cfg.runtime_instance = i + 100U; cfg.realm_id = 7U;
        cfg.local_address = i + 1U; cfg.local_binding_generation = (i + 1U) * 10U; cfg.local_principal_digest = i + 1U;
        cfg.bindings = bindings; cfg.binding_count = 3U; cfg.address_width = 1U; cfg.trusted_o0_network = 1U;
        port.struct_size = sizeof(port); port.api_version = UCN_API_VERSION; port.link_instance = 1U; port.frame_mtu = 64U;
        port.tx.struct_size = sizeof(port.tx); port.tx.api_version = UCN_API_VERSION; port.tx.submit = send_frame; port.tx.cancel = cancel_frame;
        ports.struct_size = sizeof(ports); ports.api_version = UCN_API_VERSION; ports.links = &port; ports.link_count = 1U;
        ports.state_lock.struct_size = sizeof(ports.state_lock); ports.state_lock.api_version = UCN_API_VERSION; ports.state_lock.enter = lock; ports.state_lock.leave = unlock;
        ports.driver_callback_gate = ports.state_lock; ports.driver_callback_gate.context = &driver_gates[i]; ports.driver_callback_gate.enter = gate; ports.driver_callback_gate.leave = ungate;
        assert(ucn_init(&core_storage[i], sizeof(core_storage[i]), &cfg, &ports, &nodes[i]) == UCN_OK);
        assert(ucn_link_get(nodes[i], 0U, &links[i]) == UCN_OK);
    }
    memset(&channel, 0, sizeof(channel)); channel.id = 1U; channel.service_id = 0x40U; channel.schema_id = 0x4845414cU;
    channel.schema_revision = 1U; channel.payload_bytes = 16U; channel.traffic_class = UCN_TRAFFIC_Q1;
    channel.queue_policy = UCN_NATIVE_KEEP_LATEST; channel.security_minimum = UCN_NATIVE_STATIC_TRUSTED;
    channel.max_local_age_us = 1000U; channel.source_count = 1U;
    channel.sources[0].address = 1U; channel.sources[0].binding_generation = 10U; channel.sources[0].principal_digest = 1U;
    for (i = 0U; i < 3U; ++i) {
        ucn_native_config_t cfg;
        memset(&cfg, 0, sizeof(cfg)); cfg.api_version = UCN_NATIVE_API_VERSION; cfg.storage_layout = UCN_NATIVE_LAYOUT;
        cfg.instance = i + 200U; cfg.manifest_hash = UCN_NATIVE_MANIFEST_HASH; cfg.channels = &channel; cfg.channel_count = 1U;
        cfg.lock.struct_size = sizeof(cfg.lock); cfg.lock.api_version = UCN_API_VERSION; cfg.lock.enter = lock; cfg.lock.leave = unlock;
        channel.target_count = i == 0U ? 2U : 0U;
        if (i == 0U) {
            for (j = 0U; j < 2U; ++j) {
                ucn_static_path_t path; ucn_path_handle_t h;
                memset(&path, 0, sizeof(path)); path.struct_size = sizeof(path); path.api_version = UCN_API_VERSION;
                path.destination_address = j + 2U; path.destination_binding_generation = (j + 2U) * 10U; path.path_frame_mtu = 64U;
                assert(ucn_static_path_add(nodes[i], &path, &h) == UCN_OK);
                channel.targets[j].address = j + 2U; channel.targets[j].binding_generation = (j + 2U) * 10U;
            }
        }
        assert(ucn_native_init(&native_storage[i], sizeof(native_storage[i]), nodes[i], &cfg, &roots[i]) == UCN_OK);
        if (i > 0U) { assert(UCN_NATIVE_EXAMPLE_SUBSCRIBE(roots[i], &samples[i], &subscriptions[i]) == UCN_OK); }
        assert(ucn_native_start(roots[i], 1U) == UCN_OK);
    }
    assert(ucn_publisher_create(roots[0], 1U, &publisher) == UCN_OK);
    assert(UCN_NATIVE_EXAMPLE_PUBLISH(roots[0], publisher, payload, sizeof(payload)) == UCN_OK);
    memset(payload, 0xee, sizeof(payload));
    for (clock_us = 2U; clock_us < 20U; ++clock_us) {
        for (i = 0U; i < 3U; ++i) { uint16_t done;
            assert(ucn_native_step(roots[i], clock_us, 8U) == UCN_OK); assert(ucn_native_dispatch(roots[i], clock_us, 8U, &done) == UCN_OK); }
    }
    assert(samples[1] == 1U && samples[2] == 1U && frames == 2U);
    for (i = 1U; i < 3U; ++i) { assert(UCN_NATIVE_EXAMPLE_UNSUBSCRIBE(roots[i], subscriptions[i]) == UCN_OK); }
    for (i = 0U; i < 3U; ++i) { assert(ucn_native_stop(roots[i]) == UCN_OK); }
    for (; clock_us < 24U; ++clock_us) { for (i = 0U; i < 3U; ++i) { assert(ucn_native_step(roots[i], clock_us, 8U) == UCN_OK); } }
    for (i = 0U; i < 3U; ++i) { assert(ucn_native_deinit(roots[i]) == UCN_OK); }
    printf("health: B=%u C=%u frames=%u header=11 payload=16 native_storage=%zu\n", samples[1], samples[2], frames, sizeof(ucn_native_storage_t));
    return 0;
}
