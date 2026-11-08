#define main security_regression_main
#include "test_security_session.c"
#undef main
#include "internal/ucn_native_session.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#ifdef UCN_NATIVE_ENCRYPTED_TEST
#define TEST_PROTECTION UCN_NATIVE_ENCRYPTED
#else
#define TEST_PROTECTION UCN_NATIVE_ORIGIN_AUTH
#endif

static session_fixture_t tx_fixture, rx_fixture;
static ucn_i_native_session_backend_t sender_backend, receiver_backend;
static ucn_i_adapter_t tx_adapter, rx_adapter;
static ucn_native_storage_t native_tx_storage, native_rx_storage;
static ucn_native_storage_t zero_native_storage;
static ucn_i_native_session_backend_t rejected_backend, zero_backend;
static ucn_native_t *native_tx, *native_rx;
static test_lock_t native_tx_lock, native_rx_lock;
static test_lock_t adapter_tx_lock, adapter_rx_lock, driver_tx_gate, driver_rx_gate;
static uint8_t captured[UCN_ADAPTER_FRAME_BYTES];
static size_t captured_bytes;
static unsigned backpressure = 2U, submit_calls;
static bool unknown;
static uint8_t first_frame[UCN_ADAPTER_FRAME_BYTES];
static ucn_driver_submit_result_t submit(void *ctx, ucn_link_handle_t link,
    const uint8_t *bytes, size_t count, ucn_driver_token_t token)
{
    (void)ctx; (void)link; (void)token;
    if (submit_calls == 0U) { memcpy(first_frame, bytes, count); }
    ++submit_calls;
    memcpy(captured, bytes, count); captured_bytes = count;
    if (backpressure != 0U) { --backpressure; return UCN_DRIVER_NOT_SUBMITTED; }
    if (unknown) { return UCN_DRIVER_SUBMIT_UNKNOWN; }
    return UCN_DRIVER_COMPLETE;
}
static ucn_result_t cancel(void *ctx, ucn_driver_token_t token)
{ (void)ctx; (void)token; return unknown ? UCN_ERR_STATE : UCN_OK; }
static ucn_result_t init_adapter(ucn_i_adapter_t *a, uint32_t instance)
{
    ucn_ports_t ports; ucn_link_port_t link;
    memset(&ports, 0, sizeof(ports)); memset(&link, 0, sizeof(link));
    ports.struct_size = sizeof(ports); ports.api_version = UCN_API_VERSION;
    ports.link_count = 1U; ports.links = &link;
    ports.state_lock.struct_size = sizeof(ports.state_lock); ports.state_lock.api_version = UCN_API_VERSION;
    ports.state_lock.enter = lock_enter; ports.state_lock.leave = lock_leave;
    ports.state_lock.context = instance == 51U ? (void *)&adapter_tx_lock : (void *)&adapter_rx_lock;
    ports.driver_callback_gate = ports.state_lock;
    ports.driver_callback_gate.context = instance == 51U ? (void *)&driver_tx_gate : (void *)&driver_rx_gate;
    link.struct_size = sizeof(link); link.api_version = UCN_API_VERSION;
    link.link_instance = 1U; link.frame_mtu = UCN_ADAPTER_FRAME_BYTES;
    link.tx.struct_size = sizeof(link.tx); link.tx.api_version = UCN_API_VERSION;
    link.tx.submit = submit; link.tx.cancel = cancel;
    return ucn_i_adapter_init(a, instance, 9U, &ports);
}
static unsigned samples;
static void authenticated_sample(void *ctx, const ucn_native_sample_t *s)
{
    (void)ctx; assert(s->payload_bytes == 4U && s->payload[0] == 0x11U);
    assert(s->provenance == TEST_PROTECTION);
    assert(memcmp(s->principal, tx_fixture.candidate.local.principal, 16U) == 0);
    ++samples;
}
int main(void)
{
    ucn_i_security_candidate_t tx = candidate_make(0x20U, 1U), rx;
    ucn_native_channel_t c;
    ucn_native_config_t cfg;
    ucn_native_handle_t pub, sub;
    ucn_link_handle_t link;
    ucn_rx_meta_t meta;
    uint16_t delivered;
    uint8_t payload[4] = {0x11U, 0x22U, 0x33U, 0x44U};
    tx.hop_profile = UCN_I_HOP_PROFILE_H0; tx.link_generation = 1U;
#ifdef UCN_NATIVE_ENCRYPTED_TEST
    tx.origin_level = UCN_I_SECURITY_CONFIDENTIAL;
    tx.origin_tx.suite_id = 2U; tx.origin_rx.suite_id = 2U;
#endif
    rx = tx; rx.local = tx.peer; rx.peer = tx.local;
    rx.origin_tx = tx.origin_rx; rx.origin_rx = tx.origin_tx;
    assert(fixture_activate(&tx_fixture, &tx, UCN_I_SECURITY_ACCESS_OUTBOUND, 51U, 81U, 0x1201U, 0U) == 0);
    assert(fixture_activate(&rx_fixture, &rx, UCN_I_SECURITY_ACCESS_INBOUND, 52U, 82U, 0x1201U, 0U) == 0);
    assert(init_adapter(&tx_adapter, 51U) == UCN_OK);
    assert(init_adapter(&rx_adapter, 52U) == UCN_OK);
    {
        const uint8_t wrong_fingerprint[16] = {0U};
        assert(ucn_i_native_session_init(&rejected_backend, &rx_fixture.owner, &rx_adapter,
            rx_fixture.handle, &rx_fixture.facts, wrong_fingerprint, 1U, 0x1201U, 22U, 11U) == UCN_ERR_STATE);
        assert(memcmp(&rejected_backend, &zero_backend, sizeof(zero_backend)) == 0);
    }
    {
        ucn_i_security_acl_rule_t swapped = rx_fixture.acl;
        memcpy(swapped.context_fingerprint, rx.hop_fingerprint, 16U);
        rx_fixture.owner.acl_rules = &swapped;
        assert(ucn_i_native_session_init(&rejected_backend, &rx_fixture.owner, &rx_adapter,
            rx_fixture.handle, &rx_fixture.facts, rx.hop_fingerprint, 1U, 0x1201U, 22U, 11U) == UCN_ERR_STATE);
        assert(memcmp(&rejected_backend, &zero_backend, sizeof(zero_backend)) == 0);
        rx_fixture.owner.acl_rules = &rx_fixture.acl;
    }
    {
        ucn_driver_token_t existing;
        assert(ucn_i_adapter_tx_reserve(&rx_adapter, 0U, 1U, 0U, &existing) == UCN_OK);
        assert(ucn_i_native_session_init(&rejected_backend, &rx_fixture.owner, &rx_adapter,
            rx_fixture.handle, &rx_fixture.facts, rx.origin_fingerprint, 1U, 0x1201U, 22U, 11U) == UCN_ERR_STATE);
        assert(memcmp(&rejected_backend, &zero_backend, sizeof(zero_backend)) == 0);
        assert(ucn_i_adapter_tx_cancel(&rx_adapter, &existing) == UCN_OK);
        assert(ucn_i_adapter_tx_retire(&rx_adapter, &existing) == UCN_OK);
    }
    assert(ucn_i_native_session_init(&sender_backend, &tx_fixture.owner, &tx_adapter,
        tx_fixture.handle, &tx_fixture.facts, tx.origin_fingerprint, 1U, 0x1201U, 11U, 22U) == UCN_OK);
    assert(ucn_i_native_session_init(&receiver_backend, &rx_fixture.owner, &rx_adapter,
        rx_fixture.handle, &rx_fixture.facts, rx.origin_fingerprint, 1U, 0x1201U, 22U, 11U) == UCN_OK);
    memset(&c, 0, sizeof(c)); c.id = 1U; c.service_id = 0x1201U; c.schema_id = 1U;
    c.schema_revision = 1U; c.payload_bytes = 4U; c.traffic_class = UCN_TRAFFIC_Q1;
    c.queue_policy = UCN_NATIVE_FIFO; c.security_minimum = TEST_PROTECTION;
    c.max_local_age_us = 1000U; c.source_count = 1U;
    c.sources[0].address = tx.local.address; c.sources[0].binding_generation = tx.local.binding_generation;
    c.sources[0].principal_digest = 11U; c.target_count = 1U;
    c.targets[0].address = tx.peer.address; c.targets[0].binding_generation = tx.peer.binding_generation;
    memset(&cfg, 0, sizeof(cfg)); cfg.api_version = UCN_NATIVE_API_VERSION; cfg.storage_layout = UCN_NATIVE_LAYOUT;
    cfg.instance = 41U; cfg.manifest_hash = UCN_NATIVE_MANIFEST_HASH; cfg.channels = &c; cfg.channel_count = 1U;
    cfg.lock.struct_size = sizeof(cfg.lock); cfg.lock.api_version = UCN_API_VERSION;
    cfg.lock.enter = lock_enter; cfg.lock.leave = lock_leave; cfg.lock.context = &native_tx_lock;
    c.security_minimum = TEST_PROTECTION == UCN_NATIVE_ORIGIN_AUTH ? UCN_NATIVE_ENCRYPTED : UCN_NATIVE_STATIC_TRUSTED;
    native_tx = (ucn_native_t *)(uintptr_t)1U;
    assert(ucn_native_init_backend(&native_tx_storage, sizeof(native_tx_storage), &sender_backend.port, &cfg, &native_tx) == UCN_ERR_UNSUPPORTED);
    assert(native_tx == (ucn_native_t *)(uintptr_t)1U && memcmp(&native_tx_storage, &zero_native_storage, sizeof(native_tx_storage)) == 0);
    c.security_minimum = TEST_PROTECTION;
    assert(ucn_native_init_backend(&native_tx_storage, sizeof(native_tx_storage), &sender_backend.port, &cfg, &native_tx) == UCN_OK);
    c.target_count = 0U; cfg.instance = 42U; cfg.lock.context = &native_rx_lock;
    assert(ucn_native_init_backend(&native_rx_storage, sizeof(native_rx_storage), &receiver_backend.port, &cfg, &native_rx) == UCN_OK);
    assert(ucn_publisher_create(native_tx, 1U, &pub) == UCN_OK);
    assert(ucn_subscription_create(native_rx, 1U, authenticated_sample, NULL, &sub) == UCN_OK);
    assert(ucn_native_start(native_tx, 1500U) == UCN_OK);
    assert(ucn_native_start(native_rx, 1500U) == UCN_OK);
    assert(ucn_i_adapter_link_handle(&rx_adapter, 0U, &link) == UCN_OK);
    memset(&meta, 0, sizeof(meta)); meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION; meta.timestamp_us = 1500U;
    {
        uint16_t i; const uint8_t bad = 0U;
        for (i = 0U; i < UCN_ADAPTER_RX_SLOT_COUNT; ++i) {
            assert(ucn_i_adapter_rx_publish(&rx_adapter, link, &bad, 1U, &meta) == UCN_OK);
        }
        assert(ucn_native_step(native_rx, 1501U, UCN_ADAPTER_RX_SLOT_COUNT) == UCN_OK);
        assert(ucn_i_adapter_rx_publish(&rx_adapter, link, &bad, 1U, &meta) == UCN_OK);
        assert(ucn_native_step(native_rx, 1501U, 1U) == UCN_OK);
    }
    assert(ucn_publish(native_tx, pub, payload, sizeof(payload)) == UCN_OK);
    assert(ucn_native_step(native_tx, 1501U, 2U) == UCN_OK && captured_bytes > sizeof(payload));
    /* Command admission no longer performs Driver I/O. The actor makes one
     * bounded attempt per step, still with the original sealed bytes.
     * 接纳不执行Driver，同轮step仅一次尝试，后续重试仍用原帧。 */
    assert(submit_calls == 1U);
    {
        ucn_i_callback_claim_t claim = {123U, 321U, 1U, 1U};
        assert(ucn_i_callback_gate_enter(tx_fixture.owner.provider_gate, &claim) == UCN_OK);
        assert(ucn_native_step(native_tx, 1501U, 2U) == UCN_OK);
        assert(submit_calls == 1U);
        assert(ucn_i_callback_gate_leave(tx_fixture.owner.provider_gate, &claim) == UCN_OK);
    }
    assert(ucn_native_step(native_tx, 1501U, 2U) == UCN_OK);
    assert(submit_calls == 2U && memcmp(first_frame, captured, captured_bytes) == 0);
    assert(ucn_native_step(native_tx, 1501U, 2U) == UCN_OK);
    assert(submit_calls == 3U && memcmp(first_frame, captured, captured_bytes) == 0);
    assert(ucn_i_adapter_link_handle(&rx_adapter, 0U, &link) == UCN_OK);
    memset(&meta, 0, sizeof(meta)); meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION; meta.timestamp_us = 1502U;
    assert(ucn_i_adapter_rx_publish(&rx_adapter, link, captured, captured_bytes, &meta) == UCN_OK);
    assert(ucn_native_step(native_rx, 1503U, 2U) == UCN_OK);
    assert(ucn_native_dispatch(native_rx, 1504U, 2U, &delivered) == UCN_OK && delivered == 1U && samples == 1U);
    assert(ucn_i_adapter_rx_publish(&rx_adapter, link, captured, captured_bytes, &meta) == UCN_OK);
    assert(ucn_native_step(native_rx, 1505U, 2U) == UCN_OK);
    assert(ucn_native_dispatch(native_rx, 1506U, 2U, &delivered) == UCN_OK && delivered == 0U);
    assert(ucn_publish(native_tx, pub, payload, sizeof(payload)) == UCN_OK);
    assert(ucn_native_step(native_tx, 1507U, 2U) == UCN_OK);
    meta.timestamp_us = 1508U;
    captured[captured_bytes - 1U] ^= 1U;
    assert(ucn_i_adapter_rx_publish(&rx_adapter, link, captured, captured_bytes, &meta) == UCN_OK);
    assert(ucn_native_step(native_rx, 1508U, 2U) == UCN_OK);
    assert(ucn_native_dispatch(native_rx, 1508U, 2U, &delivered) == UCN_OK && delivered == 0U);
    captured[captured_bytes - 1U] ^= 1U;
    assert(ucn_i_adapter_rx_publish(&rx_adapter, link, captured, captured_bytes, &meta) == UCN_OK);
    assert(ucn_native_step(native_rx, 1509U, 2U) == UCN_OK);
    assert(ucn_i_security_fence(&rx_fixture.owner, rx_fixture.handle) == UCN_OK);
    assert(ucn_native_dispatch(native_rx, 1510U, 2U, &delivered) == UCN_OK && delivered == 0U);
    assert(ucn_i_adapter_rx_publish(&rx_adapter, link, captured, captured_bytes, &meta) == UCN_OK);
    assert(ucn_native_stop(native_rx) == UCN_OK);
    assert(ucn_native_step(native_rx, 1511U, 1U) == UCN_OK);
    assert(ucn_i_adapter_rx_publish(&rx_adapter, link, captured, captured_bytes, &meta) == UCN_ERR_STATE);
    assert(ucn_native_step(native_rx, 1512U, 2U) == UCN_OK);
    assert(ucn_i_adapter_rx_claim(&rx_adapter, &receiver_backend.rx) == UCN_ERR_NOT_FOUND);
    unknown = true;
    assert(ucn_publish(native_tx, pub, payload, sizeof(payload)) == UCN_OK);
    assert(ucn_native_step(native_tx, 1513U, 16U) == UCN_OK);
    assert(ucn_native_stop(native_tx) == UCN_OK);
    assert(ucn_native_step(native_tx, 1514U, 16U) == UCN_OK);
    assert(sender_backend.tx_live != 0U && sender_backend.lifecycle != UCN_LIFECYCLE_QUIESCENT);
    assert(ucn_native_deinit(native_tx) == UCN_ERR_STATE);
    puts("native real Security/Adapter roundtrip, replay and queued revocation passed");
    return 0;
}
