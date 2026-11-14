#define UCN_NATIVE_TEST_MAIN static_native_regression_main
#include "test_native.c"
#include "ucn/ucn_native_backend.h"

/* This port fixture tests facade authorization/lifetime, not cryptography.
 * 此端口夹具只测Facade授权和生命周期；真实密码链另有集成测试。 */
static ucn_native_source_t peer = {1U, 10U, UINT64_C(0x1111)};
static uint8_t live = 1U, pending = 1U;
static uint32_t sequence = 9U;
static uint32_t scope_generation = 3U, live_generation = 3U;
static bool replace_on_validate;
static uint8_t lifecycle = UCN_LIFECYCLE_INITIALIZED;
static unsigned replacement_calls;
static uint32_t sent_mask;
static bool check_step_reentry;
static void replacement_sample(void *ctx, const ucn_native_sample_t *s)
{ (void)ctx; (void)s; ++replacement_calls; }
static ucn_result_t binding(void *ctx, uint32_t address, ucn_native_source_t *out)
{
    (void)ctx;
    if (address == 1U) { *out = peer; return UCN_OK; }
    out->address = 2U; out->binding_generation = 20U; out->principal_digest = 0x2222U;
    if (address == 3U) { out->address = 3U; out->binding_generation = 30U; out->principal_digest = 0x3333U; }
    return address == 2U || address == 3U ? UCN_OK : UCN_ERR_NOT_FOUND;
}
static ucn_result_t view(void *ctx, ucn_node_view_t *out)
{
    (void)ctx; memset(out, 0, sizeof(*out));
    out->local_address = 2U; out->local_binding_generation = 20U;
    out->lifecycle = lifecycle; return UCN_OK;
}
static ucn_result_t start_backend(void *ctx, uint64_t now)
{ (void)ctx; (void)now; return UCN_OK; }
static ucn_result_t preflight(void *ctx, const ucn_native_channel_t *c)
{ (void)ctx; return c->security_minimum == UCN_NATIVE_ORIGIN_AUTH ? UCN_OK : UCN_ERR_UNSUPPORTED; }
static ucn_result_t stop_backend(void *ctx) { (void)ctx; lifecycle = UCN_LIFECYCLE_QUIESCENT; return UCN_OK; }
static ucn_result_t step_backend(void *ctx, uint64_t now, uint16_t budget)
{
    const ucn_native_scope_t scope = {7U, 3U, 1U, 0U}; bool quiet = true;
    (void)ctx; (void)now; (void)budget;
    if (check_step_reentry) {
        ucn_native_borrowed_range_t range, before;
        memset(&range, 0xA5, sizeof(range)); before = range;
        assert(ucn_native_backend_lifecycle_range(na, &range) == UCN_ERR_STATE);
        assert(memcmp(&range, &before, sizeof(range)) == 0);
        assert(ucn_native_backend_retire_peer(na, peer, scope) == UCN_ERR_STATE);
        assert(ucn_native_backend_peer_quiescent(na, peer, scope, &quiet) == UCN_ERR_STATE && quiet);
    }
    return UCN_OK;
}
static ucn_result_t send_backend(void *ctx, const ucn_native_channel_t *c,
    ucn_native_target_t t, const uint8_t *p, uint64_t now, uint64_t deadline)
{ (void)ctx; (void)c; (void)p; (void)now; (void)deadline; sent_mask |= UINT32_C(1) << t.address; return UCN_OK; }
static ucn_result_t validate(void *ctx, const ucn_native_scope_t *scope,
    const ucn_native_source_t *source, uint16_t service, uint8_t minimum,
    uint64_t now, uint8_t principal[16])
{
    (void)ctx;
    if (scope->instance == 0U && source->address == 2U) { memset(principal, 0, 16U); return UCN_OK; }
    if (replace_on_validate) {
        ucn_native_handle_t replacement;
        replace_on_validate = false;
        assert(ucn_subscription_close(na, subscription) == UCN_OK);
        assert(ucn_subscription_create(na, 1U, replacement_sample, NULL, &replacement) == UCN_OK);
    }
    if (!live || now >= 100U || scope->instance != 7U || scope->generation != live_generation ||
        scope->slot != 1U || source->address != peer.address || source->binding_generation != peer.binding_generation || service != 0x1234U ||
        minimum != UCN_NATIVE_ORIGIN_AUTH) { return UCN_ERR_ACCESS; }
    memset(principal, 0x51, 16U); return UCN_OK;
}
static ucn_result_t poll_backend(void *ctx, ucn_native_backend_message_t *m,
    uint8_t *p, size_t cap)
{
    (void)ctx;
    if (!pending) { return UCN_ERR_NOT_FOUND; }
    assert(cap >= 1U); memset(m, 0, sizeof(*m)); m->source = peer;
    m->scope.instance = 7U; m->scope.generation = scope_generation; m->scope.slot = 1U;
    m->receive_time_us = 10U; m->origin_sequence = sequence;
    m->service_id = 0x1234U; m->payload_bytes = 1U;
    m->traffic_class = UCN_TRAFFIC_Q1; m->protection = UCN_NATIVE_ORIGIN_AUTH;
    p[0] = 0xA7U; pending = 0U; return UCN_OK;
}
static unsigned auth_received;
static void auth_sample(void *ctx, const ucn_native_sample_t *s)
{
    const ucn_native_scope_t scope = {7U, 3U, 1U, 0U}; bool quiet = true;
    ucn_native_borrowed_range_t range, before;
    memset(&range, 0xA5, sizeof(range)); before = range;
    assert(ucn_native_backend_lifecycle_range(na, &range) == UCN_ERR_STATE);
    assert(memcmp(&range, &before, sizeof(range)) == 0);
    (void)ctx; ++auth_received;
    assert(ucn_native_backend_retire_peer(na, peer, scope) == UCN_ERR_STATE);
    assert(ucn_native_backend_peer_quiescent(na, peer, scope, &quiet) == UCN_ERR_STATE && quiet);
    if (s->provenance == UCN_NATIVE_LOCAL) { return; }
    assert(s->provenance == UCN_NATIVE_ORIGIN_AUTH && s->payload[0] == 0xA7U);
    assert(s->principal[0] == 0x51U && s->principal[15] == 0x51U);
    assert(s->receive_time_us == 10U && s->origin_sequence > 0U);
}
int main(void)
{
    ucn_native_channel_t c = channel();
    ucn_native_config_t cfg = native_config(&c);
    ucn_native_backend_t backend;
    static ucn_native_lifecycle_storage_t lifecycle_storage;
    ucn_native_borrowed_range_t range, range_before;
    uint16_t count = 99U;
    ucn_native_status_t status, before;
    memset(&status, 0x5A, sizeof(status)); before = status;
    memset(&ns_unused, 0xA5, sizeof(ns_unused));
    assert(ucn_native_status((ucn_native_t *)&ns_unused, &status) == UCN_ERR_ARGUMENT);
    assert(memcmp(&status, &before, sizeof(status)) == 0);
    memset(&backend, 0, sizeof(backend));
    backend.struct_size = sizeof(backend); backend.api_version = UCN_NATIVE_API_VERSION;
    backend.preflight = preflight;
    backend.local_view = view; backend.binding = binding; backend.start = start_backend;
    backend.stop = stop_backend; backend.step = step_backend; backend.send = send_backend;
    backend.poll = poll_backend; backend.validate = validate;
    c.target_count = 2U; c.targets[0].address = 1U; c.targets[0].binding_generation = 10U;
    c.targets[1].address = 3U; c.targets[1].binding_generation = 30U;
    c.source_count = 2U; c.sources[1].address = 2U; c.sources[1].binding_generation = 20U; c.sources[1].principal_digest = 0x2222U;
    c.security_minimum = UCN_NATIVE_ORIGIN_AUTH;
    assert(ucn_native_init_backend(&ns_a, sizeof(ns_a), &backend, &cfg, &na) == UCN_OK);
    memset(&range, 0xA5, sizeof(range)); range_before = range;
    assert(ucn_native_backend_lifecycle_range(na, &range) == UCN_ERR_STATE);
    assert(memcmp(&range, &range_before, sizeof(range)) == 0);
    assert(ucn_native_backend_enable_lifecycle(na, &lifecycle_storage, sizeof(lifecycle_storage) - 1U) == UCN_ERR_ARGUMENT);
    lifecycle_storage.bytes[0] = 1U;
    assert(ucn_native_backend_enable_lifecycle(na, &lifecycle_storage, sizeof(lifecycle_storage)) == UCN_ERR_ARGUMENT);
    lifecycle_storage.bytes[0] = 0U;
    assert(ucn_native_backend_enable_lifecycle(na, &lifecycle_storage, sizeof(lifecycle_storage)) == UCN_OK);
    assert(ucn_native_backend_lifecycle_range(na, &range) == UCN_OK);
    assert(range.address == &lifecycle_storage && range.bytes == UCN_NATIVE_LIFECYCLE_STORAGE_BYTES);
    assert(ucn_native_backend_lifecycle_range(na, (ucn_native_borrowed_range_t *)&lifecycle_storage.bytes[8]) == UCN_ERR_ARGUMENT);
    assert(ucn_native_backend_lifecycle_range(na, (ucn_native_borrowed_range_t *)&ns_a) == UCN_ERR_ARGUMENT);
    assert(ucn_native_backend_lifecycle_range(na, (ucn_native_borrowed_range_t *)&backend) == UCN_ERR_ARGUMENT);
    assert(ucn_native_backend_lifecycle_range(na, NULL) == UCN_ERR_ARGUMENT);
    assert(ucn_subscription_create(na, 1U, auth_sample, NULL, &subscription) == UCN_OK);
    assert(ucn_native_start(na, 0U) == UCN_OK);
    check_step_reentry = true;
    assert(ucn_native_step(na, 11U, 1U) == UCN_OK);
    live = 0U; /* Authorization lost after queue admission. */
    assert(ucn_native_dispatch(na, 12U, 1U, &count) == UCN_OK);
    assert(count == 0U && auth_received == 0U);
    live = 1U; pending = 1U;
    assert(ucn_native_step(na, 13U, 1U) == UCN_OK);
    /* KEEP_LATEST does not resurrect the already consumed sequence. */
    assert(ucn_native_dispatch(na, 14U, 1U, &count) == UCN_OK && count == 0U);
    {
        ucn_native_handle_t other;
        assert(ucn_subscription_create(na, 1U, auth_sample, NULL, &other) == UCN_OK);
        pending = 1U; sequence = 10U;
        assert(ucn_native_step(na, 15U, 1U) == UCN_OK);
        replace_on_validate = true;
        assert(ucn_native_dispatch(na, 16U, 2U, &count) == UCN_OK);
        assert(replacement_calls == 0U && auth_received == 1U);
    }
    pending = 1U; sequence = 100U;
    assert(ucn_native_step(na, 17U, 1U) == UCN_OK);
    assert(ucn_native_dispatch(na, 18U, 2U, &count) == UCN_OK && count == 2U);
    pending = 1U; sequence = 1U; scope_generation = live_generation = 4U;
    assert(ucn_native_step(na, 19U, 1U) == UCN_OK);
    assert(ucn_native_dispatch(na, 20U, 2U, &count) == UCN_OK && count == 2U);
    {
        const ucn_native_scope_t old_scope = {7U, 4U, 1U, 0U};
        const uint8_t payload = 0xA7U;
        bool quiet = true;
        ucn_native_source_t unknown = {9U, 90U, 0x9999U}, alias = peer;
        ucn_native_scope_t invalid = old_scope;
        static ucn_native_storage_t snapshot;
        static ucn_native_lifecycle_storage_t lifecycle_snapshot;
        snapshot = ns_a; lifecycle_snapshot = lifecycle_storage;
        alias.principal_digest++;
        assert(ucn_native_backend_retire_peer(na, unknown, old_scope) == UCN_ERR_NOT_FOUND);
        assert(ucn_native_backend_retire_peer(na, alias, old_scope) == UCN_ERR_NOT_FOUND);
        invalid.reserved_zero = 1U;
        assert(ucn_native_backend_retire_peer(na, peer, invalid) == UCN_ERR_ARGUMENT);
        assert(ucn_native_backend_peer_quiescent(na, peer, old_scope, (bool *)&lifecycle_storage) == UCN_ERR_ARGUMENT);
        assert(ucn_native_backend_peer_quiescent(na, peer, old_scope, (bool *)&ns_a) == UCN_ERR_ARGUMENT);
        assert(memcmp(&snapshot, &ns_a, sizeof(snapshot)) == 0);
        assert(memcmp(&lifecycle_snapshot, &lifecycle_storage, sizeof(lifecycle_snapshot)) == 0);
        pending = 1U; sequence = 2U;
        assert(ucn_native_step(na, 21U, 1U) == UCN_OK);
        assert(ucn_publisher_create(na, 1U, &publisher) == UCN_OK);
        assert(ucn_publish(na, publisher, &payload, 1U) == UCN_OK);
        assert(ucn_native_send(na, 1U, c.targets[0], &payload, 1U) == UCN_OK);
        assert(ucn_native_backend_peer_quiescent(na, peer, old_scope, &quiet) == UCN_OK && !quiet);
        assert(ucn_native_backend_retire_peer(na, peer, old_scope) == UCN_OK);
        assert(ucn_native_backend_peer_quiescent(na, peer, old_scope, &quiet) == UCN_OK && quiet);
        assert(ucn_native_status(na, &status) == UCN_OK && status.failed == 2U);
        status = before;
        assert(ucn_native_step(na, 22U, 32U) == UCN_OK);
        assert(sent_mask == (UINT32_C(1) << 3));
        assert(ucn_native_dispatch(na, 23U, 4U, &count) == UCN_OK && count == 2U); /* local only */
        sent_mask = 0U;
        assert(ucn_publish(na, publisher, &payload, 1U) == UCN_OK);
        assert(ucn_native_backend_retire_peer(na, peer, old_scope) == UCN_OK); /* idempotent */
        assert(ucn_native_step(na, 24U, 32U) == UCN_OK);
        assert(sent_mask == ((UINT32_C(1) << 1) | (UINT32_C(1) << 3)));
        {
            const ucn_native_source_t target_only = {3U, 30U, 0x3333U};
            const ucn_native_scope_t target_scope = {7U, 8U, 2U, 0U};
            assert(ucn_publish(na, publisher, &payload, 1U) == UCN_OK);
            assert(ucn_native_backend_peer_quiescent(na, target_only, target_scope, &quiet) == UCN_OK && !quiet);
            assert(ucn_native_backend_retire_peer(na, target_only, target_scope) == UCN_OK);
            assert(ucn_native_backend_peer_quiescent(na, target_only, target_scope, &quiet) == UCN_OK && quiet);
            sent_mask = 0U;
            assert(ucn_native_step(na, 25U, 64U) == UCN_OK);
            assert(sent_mask == (UINT32_C(1) << 1));
        }
        assert(ucn_publisher_close(na, publisher) == UCN_OK);
    }
    pending = 1U; sequence = 1000U; scope_generation = 6U; /* validation rejects */
    assert(ucn_native_step(na, 26U, 1U) == UCN_OK);
    pending = 1U; sequence = 1U; scope_generation = live_generation = 5U;
    assert(ucn_native_step(na, 27U, 1U) == UCN_OK);
    {
        const ucn_native_scope_t old_scope = {7U, 4U, 1U, 0U};
        assert(ucn_native_backend_retire_peer(na, peer, old_scope) == UCN_OK);
    }
    assert(ucn_native_dispatch(na, 28U, 8U, &count) == UCN_OK && count >= 2U);
    pending = 1U; sequence = 1000U; scope_generation = live_generation = 3U; /* even a backend accepting an old scope cannot reset history */
    assert(ucn_native_step(na, 29U, 1U) == UCN_OK);
    assert(ucn_native_dispatch(na, 30U, 4U, &count) == UCN_OK && count == 0U);
    assert(ucn_native_source_fence(na, 1U, peer) == UCN_OK);
    pending = 1U; sequence = 1U; scope_generation = live_generation = 7U;
    assert(ucn_native_step(na, 31U, 1U) == UCN_OK);
    assert(ucn_native_dispatch(na, 32U, 4U, &count) == UCN_OK && count == 0U);
    assert(ucn_native_stop(na) == UCN_OK);
    assert(ucn_native_step(na, 33U, 64U) == UCN_OK);
    assert(ucn_native_deinit(na) == UCN_OK);
    assert(ucn_native_status(na, &status) == UCN_ERR_STATE);
    assert(memcmp(&status, &before, sizeof(status)) == 0);
    {
        static ucn_native_lifecycle_storage_t fifo_lifecycle;
        lifecycle = UCN_LIFECYCLE_INITIALIZED;
        c.queue_policy = UCN_NATIVE_FIFO;
        assert(ucn_native_init_backend(&ns_b, sizeof(ns_b), &backend, &cfg, &na) == UCN_OK);
        assert(ucn_native_backend_enable_lifecycle(na, &lifecycle_storage, sizeof(lifecycle_storage)) == UCN_ERR_ARGUMENT); /* permanent ownership */
        assert(ucn_native_backend_enable_lifecycle(na, &fifo_lifecycle, sizeof(fifo_lifecycle)) == UCN_OK);
        assert(ucn_subscription_create(na, 1U, auth_sample, NULL, &subscription) == UCN_OK);
        assert(ucn_native_start(na, 0U) == UCN_OK);
        pending = 1U; sequence = 100U; scope_generation = live_generation = 3U;
        assert(ucn_native_step(na, 11U, 1U) == UCN_OK);
        pending = 1U; sequence = 1U; scope_generation = live_generation = 4U;
        assert(ucn_native_step(na, 12U, 1U) == UCN_OK);
        live_generation = 3U; /* stale backend validation cannot revive queued FIFO scope */
        assert(ucn_native_dispatch(na, 13U, 4U, &count) == UCN_OK && count == 0U);
        assert(ucn_native_stop(na) == UCN_OK);
        assert(ucn_native_step(na, 14U, 64U) == UCN_OK);
        assert(ucn_native_deinit(na) == UCN_OK);
    }
    puts("native backend authorization and original-time checks passed");
    return 0;
}
