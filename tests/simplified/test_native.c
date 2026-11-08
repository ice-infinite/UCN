#include "ucn/ucn_native.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Real Core/Adapter fixture reused without replacing its implementation. */
#define main core_regression_main
#include "test_core_static.c"
#undef main

static ucn_native_storage_t ns_a, ns_b;
static ucn_native_storage_t ns_unused;
static ucn_native_t *na, *nb;
static unsigned received;
static uint8_t received_byte;
static ucn_native_handle_t subscription, publisher;
static bool close_in_callback;
static unsigned local_calls;
static ucn_native_handle_t local_subscription;
static void local_sample(void *ctx, const ucn_native_sample_t *s)
{
    (void)ctx;
    assert(s->provenance == UCN_NATIVE_LOCAL);
    ++local_calls;
    assert(ucn_native_step(na, s->receive_time_us, 1U) == UCN_ERR_STATE);
    if (local_calls == 1U) { assert(ucn_publish(na, publisher, s->payload, s->payload_bytes) == UCN_OK); }
    else { assert(ucn_subscription_close(na, local_subscription) == UCN_OK); }
}
static void sample(void *ctx, const ucn_native_sample_t *s)
{
    uint16_t n = 999U;
    (void)ctx;
    ++received;
    received_byte = s->payload[0];
    assert(s->source.address == 1U && s->origin_sequence > 0U);
    assert(s->provenance == UCN_NATIVE_STATIC_TRUSTED);
    assert(ucn_native_dispatch(nb, s->receive_time_us, 1U, &n) == UCN_ERR_STATE);
    assert(n == 999U);
    if (close_in_callback) {
        assert(ucn_subscription_close(nb, subscription) == UCN_OK);
    }
}

static ucn_native_channel_t channel(void)
{
    ucn_native_channel_t c;
    memset(&c, 0, sizeof(c));
    c.id = 1U; c.service_id = 0x1234U;
    c.schema_id = 0x4845414cU; c.schema_revision = 1U; c.payload_bytes = 1U;
    c.traffic_class = UCN_TRAFFIC_Q1; c.queue_policy = UCN_NATIVE_KEEP_LATEST;
    c.security_minimum = UCN_NATIVE_STATIC_TRUSTED;
    c.max_local_age_us = 100U;
    c.source_count = 1U;
    c.sources[0].address = 1U; c.sources[0].binding_generation = 10U;
    c.sources[0].principal_digest = 0x1111U;
    c.target_count = 1U; c.targets[0].address = 2U;
    c.targets[0].binding_generation = 20U;
    return c;
}

static ucn_native_config_t native_config(const ucn_native_channel_t *c)
{
    ucn_native_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.api_version = UCN_NATIVE_API_VERSION; cfg.storage_layout = UCN_NATIVE_LAYOUT;
    cfg.manifest_hash = UCN_NATIVE_MANIFEST_HASH; cfg.instance = 42U;
    cfg.channels = c; cfg.channel_count = 1U;
    cfg.lock.struct_size = sizeof(cfg.lock); cfg.lock.api_version = UCN_API_VERSION;
    cfg.lock.enter = lock_enter; cfg.lock.leave = lock_leave;
    return cfg;
}

static void unused_root_can_stop(void)
{
    fake_link_t link;
    ucn_node_t *core = NULL;
    ucn_native_t *root = NULL;
    ucn_native_channel_t c = channel();
    ucn_native_config_t cfg = native_config(&c);
    memset(&link, 0, sizeof(link)); memset(&storage_bad, 0, sizeof(storage_bad));
    link.submit_result = UCN_DRIVER_COMPLETE;
    assert(make_node(&storage_bad, 900U, 1U, &link, &core) == UCN_OK);
    assert(ucn_native_init(&ns_unused, sizeof(ns_unused), core, &cfg, &root) == UCN_OK);
    assert(ucn_native_stop(root) == UCN_OK);
    assert(ucn_native_step(root, 0U, 1U) == UCN_OK);
    assert(ucn_native_start(root, 1U) == UCN_ERR_STATE);
    assert(ucn_native_deinit(root) == UCN_OK);
}

static void initialization_lock_failures_never_publish_partial_callbacks(void)
{
    static ucn_native_storage_t storage;
    unsigned failure;
    for (failure = 1U; failure <= 24U; ++failure) {
        fake_link_t fake;
        ucn_node_t *core = NULL;
        ucn_native_t *out = (ucn_native_t *)(uintptr_t)0x1234U;
        ucn_native_channel_t channels[2];
        ucn_native_config_t cfg;
        ucn_result_t result;
        memset(&storage_bad, 0, sizeof(storage_bad)); memset(&storage, 0, sizeof(storage)); memset(&fake, 0, sizeof(fake));
        fake.submit_result = UCN_DRIVER_COMPLETE;
        lock_failure_at = 0U;
        assert(make_node(&storage_bad, 1000U + failure, 1U, &fake, &core) == UCN_OK);
        channels[0] = channel(); channels[1] = channels[0]; channels[1].id = 2U; channels[1].service_id = 0x1235U;
        cfg = native_config(channels); cfg.channel_count = 2U;
        lock_enter_count = 0U; lock_failure_at = failure;
        result = ucn_native_init(&storage, sizeof(storage), core, &cfg, &out);
        lock_failure_at = 0U;
        if (result != UCN_OK) {
            assert(out == (ucn_native_t *)(uintptr_t)0x1234U);
            assert(bytes_are_value(&storage, sizeof(storage), 0U));
            for (unsigned i = 0U; i < UCN_ENDPOINT_COUNT; ++i) {
                assert(!core->endpoints[i].valid && core->endpoints[i].generation == 0U);
            }
        } else {
            assert(ucn_native_stop(out) == UCN_OK);
            assert(ucn_native_step(out, 0U, 1U) == UCN_OK);
            assert(ucn_native_deinit(out) == UCN_OK);
        }
        /* Faulted initialization never enabled RX or entered a Driver. Recreate fixture. */
        assert(fake.submit_calls == 0U);
    }
}

static void unknown_requires_real_late_completion_before_deinit(void)
{
    static ucn_native_storage_t storage;
    fake_link_t fake;
    ucn_native_t *root = NULL; ucn_node_t *core = NULL;
    ucn_native_channel_t c = channel(); ucn_native_config_t cfg = native_config(&c);
    ucn_static_path_t pc = path_config(); ucn_path_handle_t path;
    ucn_link_handle_t link; ucn_link_event_meta_t physical_fence;
    ucn_native_handle_t pub; ucn_native_status_t status;
    uint8_t payload = 1U;
    memset(&fake, 0, sizeof(fake)); memset(&storage_bad, 0, sizeof(storage_bad));
    fake.submit_result = UCN_DRIVER_SUBMIT_UNKNOWN; fake.cancel_result = UCN_ERR_IN_DOUBT;
    assert(make_node(&storage_bad, 2000U, 1U, &fake, &core) == UCN_OK);
    assert(ucn_link_get(core, 0U, &link) == UCN_OK);
    assert(ucn_static_path_add(core, &pc, &path) == UCN_OK);
    assert(ucn_native_init(&storage, sizeof(storage), core, &cfg, &root) == UCN_OK);
    assert(ucn_publisher_create(root, 1U, &pub) == UCN_OK);
    assert(ucn_native_start(root, 1U) == UCN_OK);
    assert(ucn_publish(root, pub, &payload, 1U) == UCN_OK);
    assert(ucn_native_step(root, 2U, 8U) == UCN_OK);
    assert(fake.submit_calls == 1U);
    assert(ucn_native_stop(root) == UCN_OK);
    for (uint64_t t = 3U; t < 6U; ++t) { assert(ucn_native_step(root, t, 8U) == UCN_OK); }
    assert(ucn_native_status(root, &status) == UCN_OK && !status.core_quiescent);
    assert(ucn_native_deinit(root) == UCN_ERR_STATE);
    /* Reopening is not physical retirement of the exact old token. */
    memset(&physical_fence, 0, sizeof(physical_fence)); physical_fence.struct_size = sizeof(physical_fence);
    physical_fence.api_version = UCN_API_VERSION; physical_fence.new_link_instance = 2U;
    assert(ucn_driver_link_event(core, link, UCN_DRIVER_LINK_REOPENED, &physical_fence) == UCN_OK);
    assert(ucn_native_step(root, 6U, 8U) == UCN_OK);
    assert(ucn_native_deinit(root) == UCN_ERR_STATE);
    /* This fixture now supplies the real per-token Driver completion fact. */
    assert(ucn_driver_tx_complete(core, fake.last_token, UCN_OK, NULL) == UCN_OK);
    for (uint64_t t = 6U; t < 11U; ++t) { assert(ucn_native_step(root, t, 8U) == UCN_OK); }
    assert(ucn_native_deinit(root) == UCN_OK);
}

#ifndef UCN_NATIVE_TEST_MAIN
#define UCN_NATIVE_TEST_MAIN main
#endif
int UCN_NATIVE_TEST_MAIN(void)
{
    assert(ucn_native_step(NULL, 1U, 1U) == UCN_ERR_ARGUMENT);
    unused_root_can_stop();
    initialization_lock_failures_never_publish_partial_callbacks();
    unknown_requires_real_late_completion_before_deinit();
    fake_link_t la, lb;
    ucn_node_t *a = NULL, *b = NULL;
    ucn_native_channel_t c = channel();
    ucn_native_config_t cfg = native_config(&c);
    ucn_path_handle_t path;
    ucn_static_path_t pc = path_config();
    uint16_t done;
    uint8_t value = 7U;
    memset(&storage_a, 0, sizeof(storage_a)); memset(&storage_b, 0, sizeof(storage_b));
    memset(&la, 0, sizeof(la)); memset(&lb, 0, sizeof(lb));
    la.submit_result = lb.submit_result = UCN_DRIVER_COMPLETE;
    la.deliver_on_submit = 1U; la.receive_time_override = 2U;
    assert(make_node(&storage_a, 501U, 1U, &la, &a) == UCN_OK);
    assert(make_node(&storage_b, 502U, 2U, &lb, &b) == UCN_OK);
    assert(ucn_static_path_add(a, &pc, &path) == UCN_OK);
    la.peer = b; assert(ucn_link_get(b, 0U, &la.peer_link) == UCN_OK);
    cfg.manifest_hash ^= 1U;
    assert(ucn_native_init(&ns_a, sizeof(ns_a), a, &cfg, &na) == UCN_ERR_CONFIG);
    assert(bytes_are_value(&ns_a, sizeof(ns_a), 0U));
    cfg.manifest_hash = UCN_NATIVE_MANIFEST_HASH;
    cfg.lock.context = a;
    assert(ucn_native_init(&ns_a, sizeof(ns_a), a, &cfg, &na) == UCN_ERR_ARGUMENT);
    assert(bytes_are_value(&ns_a, sizeof(ns_a), 0U));
    cfg.lock.context = NULL;
    c.security_minimum = UCN_NATIVE_ORIGIN_AUTH;
    assert(ucn_native_init(&ns_a, sizeof(ns_a), a, &cfg, &na) == UCN_ERR_UNSUPPORTED);
    c.security_minimum = UCN_NATIVE_STATIC_TRUSTED;
    assert(ucn_native_init(&ns_a, sizeof(ns_a), a, &cfg, &na) == UCN_OK);
    c.target_count = 0U; /* Receiver has no outgoing target, local delivery is implicit. */
    cfg.instance = 43U;
    assert(ucn_native_init(&ns_b, sizeof(ns_b), b, &cfg, &nb) == UCN_OK);
    assert(ucn_publisher_create(na, 1U, &publisher) == UCN_OK);
    assert(ucn_subscription_create(nb, 1U, sample, NULL, &subscription) == UCN_OK);
    assert(ucn_native_start(na, 1U) == UCN_OK && ucn_native_start(nb, 1U) == UCN_OK);
    assert(ucn_publish(na, publisher, &value, 1U) == UCN_OK); value = 99U;
    for (uint64_t t = 2U; t < 8U; ++t) {
        assert(ucn_native_step(na, t, 8U) == UCN_OK);
        assert(ucn_native_step(nb, t, 8U) == UCN_OK);
    }
    assert(ucn_native_dispatch(nb, 8U, 1U, &done) == UCN_OK && done == 1U);
    assert(received == 1U && received_byte == 7U);
    value = 8U; assert(ucn_publish(na, publisher, &value, 1U) == UCN_OK);
    la.receive_time_override = 9U;
    for (uint64_t t = 9U; t < 15U; ++t) {
        assert(ucn_native_step(na, t, 8U) == UCN_OK);
        assert(ucn_native_step(nb, t, 8U) == UCN_OK);
    }
    close_in_callback = true;
    assert(ucn_native_dispatch(nb, 15U, 1U, &done) == UCN_OK && done == 1U);
    assert(received == 2U && received_byte == 8U);
    assert(ucn_subscription_close(nb, subscription) == UCN_ERR_STATE);
    la.deliver_on_submit = 0U;
    assert(ucn_subscription_create(na, 1U, local_sample, NULL, &local_subscription) == UCN_OK);
    value = 5U; assert(ucn_publish(na, publisher, &value, 1U) == UCN_OK);
    assert(ucn_native_dispatch(na, 15U, 1U, &done) == UCN_OK && done == 1U);
    assert(ucn_native_dispatch(na, 16U, 1U, &done) == UCN_OK && done == 1U);
    assert(local_calls == 2U);
    for (uint64_t t = 17U; t < 25U; ++t) { assert(ucn_native_step(na, t, 8U) == UCN_OK); }
    close_in_callback = false;
    assert(ucn_subscription_create(nb, 1U, sample, NULL, &subscription) == UCN_OK);
    {
        uint8_t frame[UCN_ADAPTER_FRAME_BYTES]; size_t length;
        ucn_rx_meta_t meta;
        memset(&meta, 0, sizeof(meta)); meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
        meta.timestamp_us = 30U;
        length = make_rx_frame(100U, 10U, frame);
        assert(ucn_driver_rx_publish(b, la.peer_link, frame, length, &meta) == UCN_OK);
        assert(ucn_native_step(nb, 30U, 8U) == UCN_OK);
        meta.timestamp_us = 31U; length = make_rx_frame(99U, 11U, frame);
        assert(ucn_driver_rx_publish(b, la.peer_link, frame, length, &meta) == UCN_OK);
        assert(ucn_native_step(nb, 31U, 8U) == UCN_OK);
        assert(ucn_native_dispatch(nb, 32U, 8U, &done) == UCN_OK && done == 1U);
        assert(received_byte == 10U); /* unseen old Replay-window packet must not replace Latest */
        meta.timestamp_us = 41U; length = make_rx_frame(101U, 12U, frame);
        assert(ucn_driver_rx_publish(b, la.peer_link, frame, length, &meta) == UCN_OK);
        assert(ucn_native_step(nb, 40U, 8U) == UCN_OK);
        assert(ucn_native_dispatch(nb, 40U, 8U, &done) == UCN_OK && done == 0U);
        assert(ucn_native_dispatch(nb, 41U, 8U, &done) == UCN_OK && done == 1U);
        meta.timestamp_us = 50U; length = make_rx_frame(102U, 13U, frame);
        assert(ucn_driver_rx_publish(b, la.peer_link, frame, length, &meta) == UCN_OK);
        assert(ucn_native_step(nb, 50U, 8U) == UCN_OK);
        assert(ucn_native_dispatch(nb, 150U, 8U, &done) == UCN_OK && done == 0U);
        meta.timestamp_us = 160U; length = make_rx_frame(103U, 13U, frame);
        assert(ucn_driver_rx_publish(b, la.peer_link, frame, length, &meta) == UCN_OK);
        assert(ucn_native_step(nb, 160U, 8U) == UCN_OK);
        assert(ucn_native_source_fence(nb, 1U, c.sources[0]) == UCN_OK);
        assert(ucn_native_dispatch(nb, 161U, 8U, &done) == UCN_OK && done == 0U);
        meta.timestamp_us = 162U; length = make_rx_frame(104U, 14U, frame);
        assert(ucn_driver_rx_publish(b, la.peer_link, frame, length, &meta) == UCN_OK);
        assert(ucn_native_step(nb, 162U, 8U) == UCN_OK);
        assert(ucn_native_dispatch(nb, 163U, 8U, &done) == UCN_OK && done == 0U);
    }
    {
        ucn_native_status_t status;
        uint32_t sends = la.submit_calls;
        for (unsigned i = 0; i < UCN_NATIVE_CAPACITY; ++i)
            assert(ucn_publish(na, publisher, &value, 1U) == UCN_OK);
        assert(ucn_publish(na, publisher, &value, 1U) == UCN_ERR_NO_SPACE);
        assert(ucn_publisher_close(na, publisher) == UCN_OK); /* full command pool cannot block close */
        assert(ucn_native_source_fence(na, 1U, c.sources[0]) == UCN_OK);
        assert(ucn_native_step(na, 154U, UCN_NATIVE_CAPACITY) == UCN_OK);
        assert(la.submit_calls == sends); /* queued local jobs cannot outlive their source fence */
        assert(ucn_native_status(na, &status) == UCN_OK && status.pending_commands == 0U);
        assert(ucn_native_status(na, (ucn_native_status_t *)a) == UCN_ERR_ARGUMENT);
        assert(ucn_native_dispatch(na, 154U, 1U, (uint16_t *)a) == UCN_ERR_ARGUMENT);
    }
    assert(ucn_native_stop(na) == UCN_OK && ucn_native_stop(nb) == UCN_OK);
    for (uint64_t t = 170U; t < 178U; ++t) {
        assert(ucn_native_step(na, t, 8U) == UCN_OK);
        assert(ucn_native_step(nb, t, 8U) == UCN_OK);
    }
    assert(ucn_native_deinit(na) == UCN_OK && ucn_native_deinit(nb) == UCN_OK);
    puts("native actual Core/Adapter pubsub passed");
    return 0;
}
