#include "ucn/ucn_native.h"
static bool inject_visibility;
static unsigned visibility_calls;
static ucn_result_t visibility_create(ucn_native_t *, uint16_t, ucn_native_receive_fn, void *, ucn_native_handle_t *);
#define ucn_subscription_create visibility_create
#include "ucn/ucn.hpp"
#undef ucn_subscription_create
#include "../../examples/native/health_schema.h"
#include <type_traits>
struct HealthCodec {
    using message_type = ucn_example_health_t;
    static constexpr uint16_t wire_size = 16;
    static constexpr uint32_t schema_id = 0x4845414cU;
    static constexpr uint16_t revision = 1;
    static bool encode(const message_type& h, uint8_t *p, size_t n) { return ucn_health_encode(&h, p, n) != 0; }
    static bool decode(const uint8_t *p, size_t n, message_type *h) { return ucn_health_decode(p, n, h) != 0; }
};
struct WrongCodec : HealthCodec { static constexpr uint32_t schema_id = 123U; };
static ucn::Subscription<HealthCodec> typed_subscriptions[2];
static unsigned subscription_index;
static ucn_result_t cpp_subscribe(ucn_native_t *, void *, ucn_native_handle_t *);
static ucn_result_t cpp_unsubscribe(ucn_native_t *);
static ucn_result_t cpp_publish(ucn_native_t *, const uint8_t *, size_t);
#define UCN_NATIVE_EXAMPLE_SUBSCRIBE(root_, ctx_, out_) cpp_subscribe((root_), (ctx_), (out_))
#define UCN_NATIVE_EXAMPLE_PUBLISH(root_, handle_, payload_, bytes_) cpp_publish((root_), (payload_), (bytes_))
#define UCN_NATIVE_EXAMPLE_UNSUBSCRIBE(root_, handle_) cpp_unsubscribe((root_))
#define main health_example_main
#include "../../examples/native/static_health.c"
#undef main
static void cpp_received(void *ctx, const ucn_example_health_t& h, const ucn_native_sample_t& sample)
{
    assert(h.current_ma == -1200 && sample.source.address == 1U);
    ++*static_cast<unsigned *>(ctx);
}
static ucn_result_t cpp_subscribe(ucn_native_t *root, void *ctx, ucn_native_handle_t *out)
{
    (void)out;
    return typed_subscriptions[subscription_index++].open(root, 1U, cpp_received, ctx);
}
static ucn_result_t cpp_unsubscribe(ucn_native_t *root)
{
    unsigned i = root == roots[1] ? 0U : 1U;
    auto r = typed_subscriptions[i].close();
    if (r != UCN_OK) return r;
    return typed_subscriptions[i].release();
}
static ucn_result_t cpp_publish(ucn_native_t *root, const uint8_t *p, size_t n)
{
    inject_visibility = true;
    {
        ucn::Subscription<HealthCodec> visibility;
        assert(visibility.open(root, 1U, [](void *, const ucn_example_health_t& h, const ucn_native_sample_t&) {
            assert(h.voltage_mv == 24000U); ++visibility_calls;
        }, nullptr) == UCN_OK);
        assert(visibility_calls == 1U);
    }
    inject_visibility = false;
    ucn::Publisher<WrongCodec> wrong;
    assert(wrong.open(root, 1U) == UCN_ERR_CONFIG);
    // Repeated automatic destruction closes each publisher, never leaking a slot.
    for (unsigned i = 0; i < UCN_NATIVE_CAPACITY * 2U; ++i) {
        ucn::Publisher<HealthCodec> temporary;
        assert(temporary.open(root, 1U) == UCN_OK);
    }
    ucn::Publisher<HealthCodec> publisher;
    HealthCodec::message_type h{};
    assert(HealthCodec::decode(p, n, &h));
    assert(publisher.open(root, 1U) == UCN_OK);
    return publisher.publish(h);
}
static ucn_result_t visibility_create(ucn_native_t *root, uint16_t id, ucn_native_receive_fn fn, void *ctx, ucn_native_handle_t *out)
{
    auto r = ucn_subscription_create(root, id, fn, ctx, out);
    if (r == UCN_OK && inject_visibility) {
        // Unit-only scheduling point: registered callback visible before open returns.
        // This synthetic sample is NOT network/authentication evidence.
        const ucn_example_health_t h = {24000U, -1200, 2534, 2U, 1U, UINT32_C(0x80000001)};
        uint8_t bytes[16]; ucn_native_sample_t sample{};
        assert(ucn_health_encode(&h, bytes, sizeof(bytes)));
        sample.payload = bytes; sample.payload_bytes = sizeof(bytes); sample.provenance = UCN_NATIVE_LOCAL;
        fn(ctx, &sample);
    }
    return r;
}
int main()
{
    static_assert(!std::is_copy_constructible_v<ucn::Publisher<HealthCodec>>);
    static_assert(!std::is_move_constructible_v<ucn::Subscription<HealthCodec>>);
    // Keep the C example receiver compiled too, without using it as our callback.
    (void)&health_received;
    return health_example_main();
}
