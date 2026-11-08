#define UCN_NATIVE_TEST_MAIN static_native_regression_main
#include "test_native.c"
#include "ucn/ucn_native_backend.h"

/* This port fixture tests facade authorization/lifetime, not cryptography.
 * 此端口夹具只测Facade授权和生命周期；真实密码链另有集成测试。 */
static ucn_native_source_t peer = {1U, 10U, UINT64_C(0x1111)};
static uint8_t live = 1U, pending = 1U;
static uint32_t sequence = 9U;
static bool replace_on_validate;
static uint8_t lifecycle = UCN_LIFECYCLE_INITIALIZED;
static unsigned replacement_calls;
static void replacement_sample(void *ctx, const ucn_native_sample_t *s)
{ (void)ctx; (void)s; ++replacement_calls; }
static ucn_result_t binding(void *ctx, uint32_t address, ucn_native_source_t *out)
{
    (void)ctx;
    if (address == 1U) { *out = peer; return UCN_OK; }
    out->address = 2U; out->binding_generation = 20U; out->principal_digest = 0x2222U;
    return address == 2U ? UCN_OK : UCN_ERR_NOT_FOUND;
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
{ (void)ctx; (void)now; (void)budget; return UCN_OK; }
static ucn_result_t send_backend(void *ctx, const ucn_native_channel_t *c,
    ucn_native_target_t t, const uint8_t *p, uint64_t now, uint64_t deadline)
{ (void)ctx; (void)c; (void)t; (void)p; (void)now; (void)deadline; return UCN_ERR_UNSUPPORTED; }
static ucn_result_t validate(void *ctx, const ucn_native_scope_t *scope,
    const ucn_native_source_t *source, uint16_t service, uint8_t minimum,
    uint64_t now, uint8_t principal[16])
{
    (void)ctx;
    if (replace_on_validate) {
        ucn_native_handle_t replacement;
        replace_on_validate = false;
        assert(ucn_subscription_close(na, subscription) == UCN_OK);
        assert(ucn_subscription_create(na, 1U, replacement_sample, NULL, &replacement) == UCN_OK);
    }
    if (!live || now >= 100U || scope->instance != 7U || scope->generation != 3U ||
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
    m->scope.instance = 7U; m->scope.generation = 3U; m->scope.slot = 1U;
    m->receive_time_us = 10U; m->origin_sequence = sequence;
    m->service_id = 0x1234U; m->payload_bytes = 1U;
    m->traffic_class = UCN_TRAFFIC_Q1; m->protection = UCN_NATIVE_ORIGIN_AUTH;
    p[0] = 0xA7U; pending = 0U; return UCN_OK;
}
static unsigned auth_received;
static void auth_sample(void *ctx, const ucn_native_sample_t *s)
{
    (void)ctx; ++auth_received;
    assert(s->provenance == UCN_NATIVE_ORIGIN_AUTH && s->payload[0] == 0xA7U);
    assert(s->principal[0] == 0x51U && s->principal[15] == 0x51U);
    assert(s->receive_time_us == 10U && s->origin_sequence >= 9U);
}
int main(void)
{
    ucn_native_channel_t c = channel();
    ucn_native_config_t cfg = native_config(&c);
    ucn_native_backend_t backend;
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
    c.target_count = 0U; c.security_minimum = UCN_NATIVE_ORIGIN_AUTH;
    assert(ucn_native_init_backend(&ns_a, sizeof(ns_a), &backend, &cfg, &na) == UCN_OK);
    assert(ucn_subscription_create(na, 1U, auth_sample, NULL, &subscription) == UCN_OK);
    assert(ucn_native_start(na, 0U) == UCN_OK);
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
    assert(ucn_native_stop(na) == UCN_OK);
    assert(ucn_native_step(na, 17U, 2U) == UCN_OK);
    assert(ucn_native_deinit(na) == UCN_OK);
    assert(ucn_native_status(na, &status) == UCN_ERR_STATE);
    assert(memcmp(&status, &before, sizeof(status)) == 0);
    puts("native backend authorization and original-time checks passed");
    return 0;
}
