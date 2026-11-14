#include "native_join_fixture.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "JOIN check failed %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; } } while (0)

#if UCN_PERSIST_DOMAIN_COUNT < 3U
int main(void)
{
    static ucn_i_native_join_t join;
    static ucn_i_native_join_config_t config;
    REQUIRE(ucn_i_native_join_init(&join, &config, 300U) == UCN_ERR_UNSUPPORTED);
    REQUIRE(join.magic == 0U);
    puts("native JOIN insufficient profile domains rejected without publication");
    return 0;
}
#else

static native_join_fixture_t device, authority;
static ucn_i_native_join_current_t current_device, current_authority;
static ucn_i_security_owner_t security_before;
static ucn_i_identity_owner_t identity_before;
static ucn_i_native_join_t rejected_join, zero_join;
static unsigned device_sent, authority_sent;
static unsigned opcodes[9];
static uint16_t tamper_opcode;
static ucn_result_t received_rejection;
static uint64_t delayed_final_time;

static int reset_pair(void)
{
    device_sent = 0U; authority_sent = 0U; memset(opcodes, 0, sizeof(opcodes));
    tamper_opcode = 0U; received_rejection = UCN_OK; delayed_final_time = 0U;
    REQUIRE(native_join_fixture_init(&device, true) == UCN_OK);
    REQUIRE(native_join_fixture_init(&authority, false) == UCN_OK);
    REQUIRE(ucn_i_native_join_init(&device.join, &device.config, 300U) == UCN_OK);
    REQUIRE(ucn_i_native_join_init(&authority.join, &authority.config, 300U) == UCN_OK);
    return 0;
}

static int init_failure_is_atomic(void)
{
    REQUIRE(native_join_fixture_init(&device, true) == UCN_OK);
    device.authority.local_deadline_us = 300U;
    REQUIRE(ucn_i_native_join_init(&rejected_join, &device.config, 300U) != UCN_OK);
    REQUIRE(memcmp(&rejected_join, &zero_join, sizeof(zero_join)) == 0);
    return 0;
}

static int parent_equality_ignores_padding(void)
{
    size_t authority_end = offsetof(ucn_i_identity_authority_view_t, body_digest) +
        sizeof(device.join.frozen_authority.body_digest);
    size_t binding_end = offsetof(ucn_i_identity_binding_view_t, body_digest) +
        sizeof(device.join.frozen_authority_binding.body_digest);
    REQUIRE(reset_pair() == 0);
    /* Model two C objects with identical fields and distinct tail padding.
     * Touch the frozen side to avoid an optimizer dropping padding on copy. */
    memset((unsigned char *)&device.join.frozen_authority + authority_end, 0xA5,
        sizeof(device.join.frozen_authority) - authority_end);
    memset((unsigned char *)&device.join.frozen_authority_binding + binding_end, 0x5A,
        sizeof(device.join.frozen_authority_binding) - binding_end);
    REQUIRE(ucn_i_native_join_parents(&device.join, 301U, false) == UCN_OK);
    device.authority.body_digest[0] ^= 1U;
    REQUIRE(ucn_i_native_join_parents(&device.join, 302U, false) == UCN_ERR_SECURITY);
    return 0;
}

static int transfer(native_join_fixture_t *from, native_join_fixture_t *to,
    unsigned *seen, uint64_t now)
{
    ucn_link_handle_t link;
    ucn_rx_meta_t meta = {0};
    ucn_i_adapter_rx_view_t rx;
    ucn_i_c0_frame_t frame;
    ucn_result_t result;
    if (*seen == from->submit_calls) return 0;
    REQUIRE(from->submit_calls == *seen+1U); *seen = from->submit_calls;
    REQUIRE(ucn_i_c0_decode(from->captured, from->captured_bytes, 2U, 0U, &frame) == UCN_OK);
    REQUIRE(frame.opcode >= 1U && frame.opcode <= 8U); ++opcodes[frame.opcode];
    REQUIRE(ucn_i_adapter_link_handle(&to->adapter, 0U, &link) == UCN_OK);
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    if (frame.opcode == 8U && delayed_final_time != 0U && frame.payload[3]+1U == frame.payload[4])
        now = delayed_final_time;
    meta.timestamp_us = now; meta.sender_discriminator = to->config.physical_peer;
    if (frame.opcode == tamper_opcode && frame.opcode >= 4U && frame.payload[3]+1U == frame.payload[4])
        from->captured[from->captured_bytes-1U] ^= 1U;
    REQUIRE(ucn_i_adapter_rx_publish(&to->adapter, link, from->captured,
        from->captured_bytes, &meta) == UCN_OK);
    from->captured_bytes = 0U;
    REQUIRE(ucn_i_adapter_rx_claim(&to->adapter, &rx) == UCN_OK);
    result = ucn_i_native_join_receive_claimed(&to->join, &rx, now);
    REQUIRE(ucn_i_adapter_rx_retire(&to->adapter, &rx.token) == UCN_OK);
    if (result != UCN_OK && frame.opcode == tamper_opcode) {
        received_rejection = result; return 0;
    }
    if (result != UCN_OK) fprintf(stderr, "RX opcode %u phase %u returned %ld\n",
        (unsigned)frame.opcode, (unsigned)to->join.phase, (long)result);
    REQUIRE(result == UCN_OK);
    return 0;
}

static int media(native_join_fixture_t *f, uint64_t now, bool session_allowed)
{
    ucn_persistence_step_result_t step;
    if (!f->join.work_kind || (!session_allowed && f->join.work_kind == 2U)) return 0;
    REQUIRE(ucn_i_persistence_step(f->persistence, now, 1U, &step) == UCN_OK);
    REQUIRE(ucn_i_native_join_finish_durability(&f->join, now) == UCN_OK);
    return 0;
}

static int tick(uint64_t now, bool session_allowed)
{
    ucn_result_t result;
    REQUIRE(ucn_i_native_join_retire_one(&device.join, now) == UCN_OK);
    REQUIRE(ucn_i_native_join_retire_one(&authority.join, now) == UCN_OK);
    REQUIRE(media(&device, now, session_allowed) == 0);
    REQUIRE(media(&authority, now, session_allowed) == 0);
    result = ucn_i_native_join_drive_one(&device.join, now);
    if (result != UCN_OK) fprintf(stderr, "device drive phase %u result %ld\n", device.join.phase, (long)result);
    REQUIRE(result == UCN_OK);
    result = ucn_i_native_join_drive_one(&authority.join, now);
    if (result != UCN_OK) fprintf(stderr, "authority drive phase %u result %ld\n", authority.join.phase, (long)result);
    REQUIRE(result == UCN_OK);
    REQUIRE(transfer(&device, &authority, &device_sent, now) == 0);
    REQUIRE(transfer(&authority, &device, &authority_sent, now) == 0);
    return 0;
}

/* Catches discarding a pending durability holder, or publishing ACTIVE to make
 * a stopped candidate retire. Actual Foundation first reaches ProofReady. */
static int stop_pending_binding(void)
{
    uint64_t now;
    ucn_persistence_step_result_t step;
    ucn_persistence_request_view_t request;
    ucn_i_identity_binding_view_t view;
    ucn_link_handle_t link;
    ucn_i_adapter_rx_view_t rx;
    ucn_rx_meta_t duplicate = {0};
    bool quiet = true;
    REQUIRE(reset_pair() == 0);
    for (now = 301U; now < 2000U; ++now) {
        REQUIRE(tick(now, false) == 0);
        if (authority.join.work_kind == 1U) break;
    }
    REQUIRE(now < 2000U && authority.join.phase == 3U);
    /* Persisting the locally signed Offer must not overwrite the exact inbound
     * DeviceProof cache; a retransmitted fragment remains byte-identical. */
    duplicate.struct_size = sizeof(duplicate); duplicate.api_version = UCN_API_VERSION;
    duplicate.timestamp_us = now; duplicate.sender_discriminator = 77U;
    REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter, link, device.captured,
        device.join.tx_frame_bytes, &duplicate) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter, &rx) == UCN_OK);
    REQUIRE(ucn_i_native_join_receive_claimed(&authority.join, &rx, now) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter, &rx.token) == UCN_OK);
    REQUIRE(ucn_i_native_join_stop(&authority.join, now) == UCN_OK);
    REQUIRE(ucn_i_native_join_quiescent(&authority.join, &quiet) == UCN_OK && !quiet);
    REQUIRE(ucn_i_identity_binding_get(&authority.identity, authority.join.device_binding,
        now, &view) != UCN_OK);
    for (++now; now < 3000U; ++now) {
        REQUIRE(ucn_i_persistence_step(authority.persistence, now, 1U, &step) == UCN_OK);
        REQUIRE(ucn_i_persistence_request_get(authority.persistence, authority.join.persistence_handle,
            &request) == UCN_OK);
        if (request.state == UCN_PERSIST_REQUEST_PROOF_READY) break;
    }
    REQUIRE(now < 3000U);
    identity_before = authority.identity;
    REQUIRE(ucn_i_identity_discard_durable_binding(&authority.identity,
        authority.join.device_binding, &authority.identity_proof) == UCN_ERR_STATE);
    REQUIRE(memcmp(&identity_before, &authority.identity, sizeof(identity_before)) == 0);
    REQUIRE(ucn_i_native_join_finish_durability(&authority.join, now) == UCN_OK);
    REQUIRE(ucn_i_identity_binding_get(&authority.identity, authority.join.device_binding,
        now, &view) != UCN_OK);
    REQUIRE(ucn_i_native_join_quiescent(&authority.join, &quiet) == UCN_OK && !quiet);
    REQUIRE(ucn_i_native_join_session_current(&authority.join, now, &current_authority) != UCN_OK);
    for (now = 0U; now <= UCN_I_ADMISSION_PENDING_COUNT; ++now)
        REQUIRE(ucn_i_native_join_retire_one(&authority.join, authority.join.deadline_us) == UCN_OK);
    REQUIRE(authority.join.admission_handle.slot_generation == 0U);
    REQUIRE(ucn_i_native_join_quiescent(&authority.join, &quiet) == UCN_OK && quiet);
    return 0;
}

static int wrong_proof_and_delayed_final(void)
{
    uint64_t now;
    REQUIRE(reset_pair() == 0); tamper_opcode = 4U;
    for (now = 301U; now < 1000U && received_rejection == UCN_OK; ++now)
        REQUIRE(tick(now, false) == 0);
    REQUIRE(received_rejection == UCN_ERR_SECURITY);
    REQUIRE(device.join.phase == 1U && !device.join.binding_ready && !device.join.active);
    REQUIRE(opcodes[5] == 0U && opcodes[6] == 0U && opcodes[7] == 0U && opcodes[8] == 0U);
    REQUIRE(reset_pair() == 0); delayed_final_time = 2000300U;
    for (now = 301U; now < 1500U; ++now) {
        REQUIRE(tick(now, false) == 0);
        if (device.join.now_us == delayed_final_time) break;
    }
    REQUIRE(now < 1500U && device.join.started_us == 300U);
    REQUIRE(ucn_i_native_join_drive_one(&device.join, delayed_final_time) != UCN_OK);
    REQUIRE(ucn_i_native_join_session_current(&device.join, delayed_final_time,
        &current_device) != UCN_OK);
    REQUIRE(!device.join.active && device.join.work_kind == 0U);
    return 0;
}

static int backpressure_and_unknown(void)
{
    bool quiet;
    ucn_tx_meta_t completion = {0};
    ucn_link_handle_t link;
    ucn_link_event_meta_t reopen = {0};
    ucn_i_callback_claim_t reentry = {90U, 1U, 1U, 1U};
    REQUIRE(reset_pair() == 0);
    REQUIRE(ucn_i_callback_gate_enter(&device.callback_gate, &reentry) == UCN_OK);
    quiet = true;
    REQUIRE(ucn_i_native_join_quiescent(&device.join, &quiet) == UCN_ERR_STATE && quiet);
    REQUIRE(ucn_i_native_join_drive_one(&device.join, 301U) == UCN_ERR_STATE);
    REQUIRE(device.submit_calls == 0U);
    REQUIRE(ucn_i_callback_gate_leave(&device.callback_gate, &reentry) == UCN_OK);
    device.driver_mode = 1U;
    REQUIRE(ucn_i_native_join_drive_one(&device.join, 301U) == UCN_OK);
    REQUIRE(device.submit_calls == 1U && !device.join.hello_submitted && device.captured_bytes == 0U);
    REQUIRE(ucn_i_native_join_retire_one(&device.join, 302U) == UCN_OK);
    REQUIRE(!device.join.tx_live && device.join.started_us == 300U);
    device.driver_mode = 0U;
    REQUIRE(ucn_i_native_join_drive_one(&device.join, 303U) == UCN_OK);
    REQUIRE(device.submit_calls == 2U && device.join.hello_submitted && device.captured_bytes == 69U);
    REQUIRE(reset_pair() == 0);
    device.driver_mode = 2U; device.cancel_result = UCN_ERR_IN_DOUBT;
    REQUIRE(ucn_i_native_join_drive_one(&device.join, 301U) == UCN_OK);
    REQUIRE(device.join.tx_live && !device.join.hello_submitted);
    REQUIRE(ucn_i_native_join_stop(&device.join, 302U) == UCN_OK);
    REQUIRE(ucn_i_native_join_retire_one(&device.join, 303U) == UCN_ERR_IN_DOUBT);
    REQUIRE(device.join.tx_live);
    REQUIRE(ucn_i_native_join_quiescent(&device.join, &quiet) == UCN_OK && !quiet);
    REQUIRE(ucn_i_adapter_link_handle(&device.adapter, 0U, &link) == UCN_OK);
    reopen.struct_size = sizeof(reopen); reopen.api_version = UCN_API_VERSION;
    reopen.new_link_instance = 10U;
    REQUIRE(ucn_i_adapter_link_event(&device.adapter, link, UCN_DRIVER_LINK_REOPENED, &reopen) == UCN_OK);
    REQUIRE(ucn_i_native_join_stop(&device.join, 304U) == UCN_OK);
    completion.struct_size = sizeof(completion); completion.api_version = UCN_API_VERSION;
    completion.timestamp_us = 304U;
    REQUIRE(ucn_i_adapter_tx_complete(&device.adapter, device.join.tx_token,
        UCN_ERR_IN_DOUBT, &completion) == UCN_OK);
    REQUIRE(ucn_i_native_join_retire_one(&device.join, 304U) == UCN_OK);
    REQUIRE(ucn_i_native_join_quiescent(&device.join, &quiet) == UCN_OK && quiet);
    REQUIRE(ucn_i_native_join_drive_one(&device.join, 305U) != UCN_OK);
    REQUIRE(device.submit_calls == 1U && device.join.attempts == 1U);
    REQUIRE(ucn_i_native_join_quiescent(&device.join, (bool *)(void *)&device.join.config.device) == UCN_ERR_ARGUMENT);
    return 0;
}

static int corrupt_binding_readback(void)
{
    uint64_t now;
    unsigned which;
    native_join_fixture_t *target;
    ucn_persistence_step_result_t step;
    ucn_result_t result = UCN_OK;
    ucn_i_identity_binding_view_t binding;
    for (which = 0U; which < 2U; ++which) {
    REQUIRE(reset_pair() == 0); target = which ? &device : &authority;
    for (now = 301U; now < 2000U; ++now) {
        REQUIRE(tick(now, false) == 0);
        if (target->join.work_kind == 1U) break;
    }
    REQUIRE(now < 2000U && opcodes[which ? 7U : 6U] == 0U);
    target->fake.corrupt_readback_once = 1U;
    for (++now; now < 3000U; ++now) {
        (void)ucn_i_persistence_step(target->persistence, now, 1U, &step);
        result = ucn_i_native_join_finish_durability(&target->join, now);
        if (result != UCN_OK) break;
    }
    REQUIRE(now < 3000U && result != UCN_OK);
    REQUIRE(!target->join.binding_ready && !target->join.active && opcodes[which ? 7U : 6U] == 0U);
    REQUIRE(ucn_i_identity_binding_get(&target->identity, target->join.device_binding,
        now, &binding) != UCN_OK);
    REQUIRE(ucn_i_native_join_drive_one(&target->join, now) != UCN_OK);
    REQUIRE(ucn_i_native_join_stop(&target->join, now) == UCN_OK);
    (void)ucn_i_native_join_finish_durability(&target->join, now);
    REQUIRE(target->join.work_kind == 0U);
    }
    return 0;
}

static int session_reload_and_stop(void)
{
    uint64_t now; unsigned scenario;
    ucn_persistence_step_result_t step;
    ucn_persistence_request_view_t request;
    ucn_result_t result;
    bool quiet;
    for (scenario = 0U; scenario < 2U; ++scenario) {
        REQUIRE(reset_pair() == 0);
        for (now = 301U; now < 2000U; ++now) {
            REQUIRE(tick(now, false) == 0);
            if (device.join.work_kind == 2U && authority.join.work_kind == 2U) break;
        }
        REQUIRE(now < 2000U && !device.join.active);
        if (scenario == 1U) {
            REQUIRE(ucn_i_native_join_stop(&device.join, now) == UCN_OK);
            REQUIRE(ucn_i_native_join_quiescent(&device.join, &quiet) == UCN_OK && !quiet);
        }
        for (++now; now < 3000U; ++now) {
            REQUIRE(ucn_i_persistence_request_get(device.persistence, device.join.persistence_handle,
                &request) == UCN_OK);
            if (scenario == 0U && request.provider_phase == UCN_I_PERSIST_SUBMIT_RELOAD_SLOT0)
                device.fake.fail_once_phase = UCN_PERSIST_IO_LOAD_SLOT;
            (void)ucn_i_persistence_step(device.persistence, now, 1U, &step);
            result = ucn_i_native_join_finish_durability(&device.join, now);
            if (device.join.work_kind == 0U) {
                REQUIRE(scenario == 0U ? result != UCN_OK : result == UCN_OK);
                break;
            }
            REQUIRE(result == UCN_OK);
        }
        REQUIRE(now < 3000U && !device.join.active);
        REQUIRE(ucn_i_native_join_session_current(&device.join, now, &current_device) != UCN_OK);
        if (scenario == 1U) REQUIRE(ucn_i_native_join_quiescent(&device.join, &quiet) == UCN_OK && quiet);
    }
    return 0;
}

static int raw_identity_and_phase_rejection(void)
{
    ucn_i_c0_frame_t frame;
    uint8_t bytes[128], body[40]; size_t written;
    ucn_link_handle_t link; ucn_i_adapter_rx_view_t rx;
    ucn_rx_meta_t meta = {0};
    REQUIRE(reset_pair() == 0);
    REQUIRE(ucn_i_native_join_drive_one(&device.join, 301U) == UCN_OK);
    REQUIRE(ucn_i_c0_decode(device.captured, device.captured_bytes, 2U, 0U, &frame) == UCN_OK);
    memcpy(body, frame.payload, sizeof(body)); body[4] ^= 1U; frame.payload = body;
    REQUIRE(ucn_i_c0_encode(&frame, 2U, 0U, bytes, sizeof(bytes), &written) == UCN_OK);
    REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    meta.timestamp_us = 302U; meta.sender_discriminator = 77U;
    REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter, link, bytes, written, &meta) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter, &rx) == UCN_OK);
    REQUIRE(ucn_i_native_join_receive_claimed(&authority.join, &rx, 302U) == UCN_ERR_SECURITY);
    REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter, &rx.token) == UCN_OK);
    REQUIRE(authority.submit_calls == 0U && authority.join.phase == 0U);
    frame.opcode = 5U; frame.interaction_role = UCN_INTERACTION_RESULT;
    REQUIRE(ucn_i_c0_encode(&frame, 2U, 0U, bytes, sizeof(bytes), &written) == UCN_OK);
    REQUIRE(ucn_i_adapter_link_handle(&device.adapter, 0U, &link) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_publish(&device.adapter, link, bytes, written, &meta) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_claim(&device.adapter, &rx) == UCN_OK);
    REQUIRE(ucn_i_native_join_receive_claimed(&device.join, &rx, 302U) == UCN_ERR_STATE);
    REQUIRE(ucn_i_adapter_rx_retire(&device.adapter, &rx.token) == UCN_OK);
    REQUIRE(device.join.phase == 0U && !device.join.active);
    return 0;
}

static int cookie_verify_rate_limit(void)
{
    ucn_i_admission_hello_cookie_t echo = {0};
    ucn_i_c0_frame_t frame = {0};
    ucn_i_adapter_rx_view_t rx;
    ucn_link_handle_t link; ucn_rx_meta_t meta = {0};
    uint8_t body[98], bytes[128]; size_t n, written; unsigned i;
    REQUIRE(reset_pair() == 0);
    echo.key.link.id = 1U; echo.key.link.generation = 9U;
    echo.key.transaction_id = authority.config.transaction;
    echo.key.local_peer_discriminator = 77U;
    memcpy(echo.key.identity_digest, authority.config.identity_digest, 16U);
    echo.device_nonce = authority.config.device_nonce;
    echo.lease_freshness_challenge_nonce = authority.config.freshness_nonce;
    echo.cookie_bytes = 16U; memset(echo.cookie, 0x55, 16U);
    memset(echo.prior_messages_hash, 0x66, 32U);
    REQUIRE(ucn_i_admission_hello_cookie_encode(&echo, body, sizeof(body), &n) == UCN_OK);
    frame.payload = body; frame.payload_bytes = n; frame.transaction_id = authority.config.transaction;
    frame.realm_id = 42U; frame.destination_address = 65535U;
    frame.opcode = 3U; frame.interaction_role = UCN_INTERACTION_REQUEST;
    frame.payload_kind = UCN_I_PAYLOAD_CONTROL; frame.hop_limit = 1U;
    REQUIRE(ucn_i_c0_encode(&frame, 2U, 0U, bytes, sizeof(bytes), &written) == UCN_OK);
    REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    meta.sender_discriminator = 77U;
    for (i = 0U; i < 5U; ++i) {
        meta.timestamp_us = 301U+i;
        REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter, link, bytes, written, &meta) == UCN_OK);
        REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter, &rx) == UCN_OK);
        REQUIRE(ucn_i_native_join_receive_claimed(&authority.join, &rx, meta.timestamp_us) ==
            (i < 4U ? UCN_ERR_SECURITY : UCN_ERR_EXHAUSTED));
        REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter, &rx.token) == UCN_OK);
    }
    REQUIRE(authority.join.phase == 0U && authority.join.started_us == 300U &&
        authority.join.deadline_us == 3000300U && !authority.join.active);
    return 0;
}

/* Mutations caught: granting ACTIVE on Final, skipping Foundation reload,
 * keeping the old root's receive permission after successful handoff. */
int main(void)
{
    uint64_t now;
    unsigned i;
    ucn_link_handle_t authority_link;
    ucn_link_event_meta_t link_down = {0};
    ucn_result_t init_result = native_join_fixture_init(&device, true);
    if (init_result != UCN_OK) fprintf(stderr, "fixture init returned %ld\n", (long)init_result);
    REQUIRE(init_result == UCN_OK);
    REQUIRE(native_join_fixture_init(&authority, false) == 0);
    REQUIRE(ucn_i_native_join_init(&device.join, &device.config, 300U) == UCN_OK);
    REQUIRE(ucn_i_native_join_init(&authority.join, &authority.config, 300U) == UCN_OK);
    REQUIRE(ucn_i_native_join_session_current(&device.join, 301U,
        &current_device) != UCN_OK);
    REQUIRE(ucn_i_native_join_drive_one(&device.join, 301U) == UCN_OK);
    REQUIRE(device.submit_calls == 1U);
    REQUIRE(device.captured_bytes == 69U); /* A1 HELLO = 29 + 40. */
    REQUIRE(transfer(&device, &authority, &device_sent, 301U) == 0);
    for (now = 302U; now < 2000U; ++now) {
        REQUIRE(tick(now, false) == 0);
        if (device.join.work_kind == 2U && authority.join.work_kind == 2U) break;
    }
    REQUIRE(now < 2000U);
    for (i = 1U; i <= 8U; ++i) REQUIRE(opcodes[i] != 0U);
    REQUIRE(device.join.phase == 6U && authority.join.phase == 6U);
    REQUIRE(ucn_i_native_join_session_current(&device.join, now, &current_device) != UCN_OK);
    REQUIRE(ucn_i_native_join_session_current(&authority.join, now, &current_authority) != UCN_OK);
    for (++now; now < 4000U; ++now) {
        REQUIRE(tick(now, true) == 0);
        if (device.join.active && authority.join.active) break;
    }
    REQUIRE(now < 4000U);
    REQUIRE(ucn_i_native_join_session_current(&device.join, now, &current_device) == UCN_OK);
    REQUIRE(ucn_i_native_join_session_current(&authority.join, now, &current_authority) == UCN_OK);
    REQUIRE(current_device.session.session_generation == 1U);
    REQUIRE(current_device.candidate.link_generation == 3U && current_authority.candidate.link_generation == 9U);
    REQUIRE(current_device.candidate.expires_at_us == 2000300U);
    REQUIRE(memcmp(current_device.candidate.transcript_digest,
        current_authority.candidate.transcript_digest, 32U) == 0);
    REQUIRE(current_device.local.record_generation == 1U && current_device.peer.record_generation == 1U);
    REQUIRE(current_device.local.witness_generation == 1U && current_device.peer.witness_generation == 1U);
    security_before = device.security;
    REQUIRE(ucn_i_native_join_session_current(&device.join, now,
        (ucn_i_native_join_current_t *)(void *)&device.security) == UCN_ERR_ARGUMENT);
    REQUIRE(memcmp(&security_before, &device.security, sizeof(security_before)) == 0);
    REQUIRE(ucn_i_native_join_handoff(&device.join, now, &current_device) == UCN_OK);
    REQUIRE(ucn_i_native_join_handoff(&device.join, now, &current_device) != UCN_OK);
    REQUIRE(ucn_i_native_join_drive_one(&device.join, now) != UCN_OK);
    REQUIRE(ucn_i_native_join_stop(&device.join, now) != UCN_OK);
    REQUIRE(ucn_i_native_join_init(&device.join, &device.config, now) == UCN_ERR_STATE);
    REQUIRE(ucn_i_security_fence(&device.security, current_device.session) == UCN_OK);
    REQUIRE(ucn_i_native_join_session_current(&device.join, now, &current_device) != UCN_OK);
    REQUIRE(ucn_i_native_join_bindings_current(&device.join, now, &current_device) == UCN_OK);
    REQUIRE(current_device.session.session_generation == 0U && current_device.candidate.session_generation == 0U);
    REQUIRE(current_device.local.local_deadline_us == 2000300U);
    REQUIRE(ucn_i_native_join_handoff(&authority.join, now, &current_authority) == UCN_OK);
    REQUIRE(authority.join.admission_handle.slot_generation == 0U);
    REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &authority_link) == UCN_OK);
    link_down.struct_size = sizeof(link_down); link_down.api_version = UCN_API_VERSION;
    REQUIRE(ucn_i_adapter_link_event(&authority.adapter, authority_link, UCN_DRIVER_LINK_DOWN, &link_down) == UCN_OK);
    REQUIRE(ucn_i_native_join_session_current(&authority.join, now, &current_authority) == UCN_ERR_STATE);
    puts("native JOIN eight raw stages and actual Foundation Session visibility passed");
    REQUIRE(stop_pending_binding() == 0);
    REQUIRE(wrong_proof_and_delayed_final() == 0);
    REQUIRE(backpressure_and_unknown() == 0);
    REQUIRE(corrupt_binding_readback() == 0);
    REQUIRE(init_failure_is_atomic() == 0);
    REQUIRE(parent_equality_ignores_padding() == 0);
    REQUIRE(session_reload_and_stop() == 0);
    REQUIRE(raw_identity_and_phase_rejection() == 0);
    REQUIRE(cookie_verify_rate_limit() == 0);
    puts("native JOIN wrong proof, delayed Final, backpressure, UNKNOWN and corrupt readback passed");
    printf("native JOIN caller-owned workspace: %zu bytes\n", sizeof(ucn_i_native_join_t));
    return 0;
}
#endif
