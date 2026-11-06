#ifndef UCN_NATIVE_HPP
#define UCN_NATIVE_HPP
#include "ucn/ucn_native.h"
#include <array>
#include <exception>

namespace ucn {
// EN: Non-owning, allocation-free typed facade; Root/context outlive all handles.
// 中文：不拥有运行期内存的零分配薄外观；Root/context必须比句柄活得久。
// Codec supplies message_type, wire_size, encode(value, bytes, size), decode(...).
template<class Codec> class Publisher {
    ucn_native_t *root_ = nullptr;
    ucn_native_handle_t handle_{};
public:
    Publisher() = default;
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    ~Publisher() { if (root_ && close() != UCN_OK) std::terminate(); }
    ucn_result_t open(ucn_native_t *root, uint16_t channel) {
        if (root_) return UCN_ERR_STATE;
        static_assert(Codec::wire_size <= UCN_NATIVE_PAYLOAD_BYTES, "bounded schema");
        auto r = ucn_native_check_schema(root, channel, Codec::schema_id, Codec::revision, Codec::wire_size);
        if (r != UCN_OK) return r;
        r = ucn_publisher_create(root, channel, &handle_);
        if (r == UCN_OK) root_ = root;
        return r;
    }
    ucn_result_t publish(const typename Codec::message_type& value) const {
        std::array<uint8_t, Codec::wire_size> bytes{};
        if (!root_) return UCN_ERR_STATE;
        if (!Codec::encode(value, bytes.data(), bytes.size())) return UCN_ERR_ARGUMENT;
        return ucn_publish(root_, handle_, bytes.data(), bytes.size());
    }
    ucn_result_t close() {
        if (!root_) return UCN_ERR_STATE;
        auto r = ucn_publisher_close(root_, handle_);
        if (r == UCN_OK) root_ = nullptr;
        return r;
    }
};

// Stable-address object: explicitly close/drain before destruction, no hidden wait.
// 固定地址对象：析构前显式close/drain，不在析构中阻塞或伪造排空。
template<class Codec> class Subscription {
public:
    using Callback = void (*)(void *, const typename Codec::message_type&, const ucn_native_sample_t&);
private:
    ucn_native_t *root_ = nullptr;
    ucn_native_handle_t handle_{};
    Callback callback_ = nullptr;
    void *context_ = nullptr;
    static void receive(void *ctx, const ucn_native_sample_t *sample) {
        auto& self = *static_cast<Subscription *>(ctx);
        typename Codec::message_type value{};
        if (Codec::decode(sample->payload, sample->payload_bytes, &value))
            self.callback_(self.context_, value, *sample);
    }
public:
    Subscription() = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription() {
        if (root_) {
            (void)ucn_subscription_close(root_, handle_);
            bool quiet = false;
            // Never return from destruction with a dangling registered this.
            if (ucn_subscription_quiescent(root_, handle_, &quiet) != UCN_OK || !quiet)
                std::terminate();
        }
    }
    ucn_result_t open(ucn_native_t *root, uint16_t channel, Callback callback, void *context) {
        if (root_ || !callback) return UCN_ERR_STATE;
        static_assert(Codec::wire_size <= UCN_NATIVE_PAYLOAD_BYTES, "bounded schema");
        auto r = ucn_native_check_schema(root, channel, Codec::schema_id, Codec::revision, Codec::wire_size);
        if (r != UCN_OK) return r;
        // Publish a fully initialized callback object before registration unlocks.
        root_ = root; callback_ = callback; context_ = context;
        r = ucn_subscription_create(root, channel, receive, this, &handle_);
        if (r != UCN_OK) { root_ = nullptr; callback_ = nullptr; context_ = nullptr; }
        return r;
    }
    ucn_result_t close() { return root_ ? ucn_subscription_close(root_, handle_) : UCN_ERR_STATE; }
    ucn_result_t release() {
        bool quiet = false;
        if (!root_) return UCN_ERR_STATE;
        auto r = ucn_subscription_quiescent(root_, handle_, &quiet);
        if (r != UCN_OK || !quiet) return r != UCN_OK ? r : UCN_ERR_STATE;
        root_ = nullptr; return UCN_OK;
    }
};
}
#endif
