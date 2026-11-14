/* Real JOIN and Foundation first; a merely constructed DTO cannot start the
 * Capability leaf. Break caught: missing Coordinator or bypass of ACTIVE. */
#define main native_join_regression_main
#include "test_native_join.c"
#undef main
#include "internal/ucn_native_capability.h"
#include "native_reauth_fixture.h"

#if UCN_PERSIST_DOMAIN_COUNT >= 3U
static ucn_i_native_capability_t cap;
static ucn_i_capability_owner_t cap_owner;
static native_join_fixture_lock_t cap_lock;
static ucn_i_native_capability_t peer_cap;
static ucn_i_capability_owner_t peer_owner;
static native_join_fixture_lock_t peer_lock;
static uint64_t next_c0 = 1U;
static uint64_t peer_counter = 1U, capability_ready_now;
static int capability_drop_ack;
static int capability_first_not_submitted;
static ucn_i_security_acl_rule_t device_rules[64], authority_rules[64];
static ucn_result_t fill_capability_policy(ucn_i_security_acl_rule_t *rules,
    const ucn_i_security_candidate_t *candidate, const ucn_i_security_acl_rule_t **out, uint16_t *count)
{
    uint16_t n = 0U, op, layer, direction;
    for (op = 0x101U; op <= 0x108U; ++op) for (layer = 0U; layer < 2U; ++layer)
        for (direction = 1U; direction <= 2U; ++direction) {
            ucn_i_security_acl_rule_t *r = &rules[n++];
            memset(r, 0, sizeof(*r)); memcpy(r->peer_principal, candidate->peer.principal, 16U);
            r->peer_binding_generation = candidate->peer.binding_generation; r->protocol_opcode = op;
            memcpy(r->context_fingerprint, layer ? candidate->hop_fingerprint : candidate->origin_fingerprint, 16U);
            r->direction = (uint8_t)direction;
        }
    for (layer = 0U; layer < 2U; ++layer) for (direction = 1U; direction <= 2U; ++direction) {
        ucn_i_security_acl_rule_t *r = &rules[n++];
        memset(r, 0, sizeof(*r)); memcpy(r->peer_principal, candidate->peer.principal, 16U);
        r->peer_binding_generation = candidate->peer.binding_generation; r->service_id = 1U;
        memcpy(r->context_fingerprint, layer ? candidate->hop_fingerprint : candidate->origin_fingerprint, 16U);
        r->direction = (uint8_t)direction;
    }
#ifdef UCN_NATIVE_DYNAMIC_CONTROL_FIXTURE
    { const uint16_t route_ops[4]={0x201U,0x202U,0x203U,0x206U}; unsigned index;
      for(index=0U;index<4U;++index) for(layer=0U;layer<2U;++layer) for(direction=1U;direction<=2U;++direction) {
          ucn_i_security_acl_rule_t *r=&rules[n++]; memset(r,0,sizeof(*r));
          memcpy(r->peer_principal,candidate->peer.principal,16U); r->peer_binding_generation=candidate->peer.binding_generation;
          r->protocol_opcode=route_ops[index]; r->direction=(uint8_t)direction;
          memcpy(r->context_fingerprint,layer?candidate->hop_fingerprint:candidate->origin_fingerprint,16U);
      }
    }
#endif
    *out = rules; *count = n; return UCN_OK;
}
static ucn_result_t capability_policy(void *ctx, const ucn_i_security_candidate_t *candidate,
    const ucn_i_security_acl_rule_t **out, uint16_t *count)
{
    return fill_capability_policy(ctx == &device ? device_rules : authority_rules, candidate, out, count);
}
static ucn_result_t cap_lock_enter(void *ctx)
{
    native_join_fixture_lock_t *lock = ctx;
    if (lock->held) return UCN_ERR_STATE;
    lock->held = 1U; return UCN_OK;
}
static void cap_lock_leave(void *ctx) { ((native_join_fixture_lock_t *)ctx)->held = 0U; }

static ucn_result_t reserve_c0(void *ctx, ucn_i_security_owner_t *security,
    ucn_i_security_handle_t session, uint64_t *out)
{
    uint64_t *counter = ctx;
    (void)security; (void)session;
    if (*counter == UINT64_MAX) return UCN_ERR_EXHAUSTED;
    *out = (*counter)++; return UCN_OK;
}

/* The baseline deliberately has no Coordinator. A RED run links this explicit
 * absent-feature shim, so failure is the requested behavioral assertion. */
#ifdef UCN_CAPABILITY_RED
ucn_result_t ucn_i_native_capability_init(ucn_i_native_capability_t *c,
    const ucn_i_native_capability_config_t *config, uint64_t now)
{ (void)c; (void)config; (void)now; return UCN_ERR_UNSUPPORTED; }
ucn_result_t ucn_i_native_capability_selected(ucn_i_native_capability_t *c, uint64_t now,
    ucn_i_capability_ref_t *r, ucn_i_cached_capability_t *v, ucn_i_profile_select_t *s)
{ (void)c; (void)now; (void)r; (void)v; (void)s; return UCN_ERR_UNSUPPORTED; }
#endif
#ifdef UCN_CAPABILITY_RECEIVE_RED
ucn_result_t ucn_i_native_capability_receive_claimed(ucn_i_native_capability_t *c,
    const ucn_i_adapter_rx_view_t *rx, uint64_t now)
{ (void)c; (void)rx; (void)now; return UCN_ERR_UNSUPPORTED; }
#endif
#if defined(UCN_CAPABILITY_RED) || defined(UCN_CAPABILITY_DRIVE_RED)
ucn_result_t ucn_i_native_capability_drive_one(ucn_i_native_capability_t *c, uint64_t now)
{ (void)c; (void)now; return UCN_ERR_UNSUPPORTED; }
#endif
#ifdef UCN_CAPABILITY_STOP_RED
ucn_result_t ucn_i_native_capability_stop(ucn_i_native_capability_t *c, uint64_t now)
{ (void)c; (void)now; return UCN_ERR_UNSUPPORTED; }
ucn_result_t ucn_i_native_capability_quiescent(ucn_i_native_capability_t *c, bool *quiet)
{ (void)c; (void)quiet; return UCN_ERR_UNSUPPORTED; }
#endif

static int capability_ready_pair(void)
{
    ucn_i_native_capability_config_t config = {0};
    ucn_i_lock_ops_t lock = {0};
    ucn_i_capability_ref_t ref;
    ucn_i_cached_capability_t value;
    ucn_i_profile_select_t selected;
    uint64_t now, selected_now;
    REQUIRE(reset_pair() == 0);
    for (now = 301U; now < 4000U; ++now) {
        REQUIRE(tick(now, true) == 0);
        if (device.join.active && authority.join.active) break;
    }
    REQUIRE(now < 4000U);
    REQUIRE(ucn_i_native_join_handoff(&device.join, now, &current_device) == UCN_OK);
    REQUIRE(ucn_i_native_join_handoff(&authority.join, now, &current_authority) == UCN_OK);
    lock.struct_size = sizeof(lock); lock.api_version = UCN_I_LOCK_OPS_VERSION;
    lock.context = &cap_lock; lock.enter = cap_lock_enter; lock.leave = cap_lock_leave;
    REQUIRE(ucn_i_capability_owner_init(&cap_owner, 10U, 42U, 30U, 23U,
        1000000U, 1000000U, &lock) == UCN_OK);
    config.adapter = &device.adapter; config.security = &device.security;
    config.capability_owner = &cap_owner; config.join_parents = &device.join;
    config.callback_gate = &device.callback_gate; config.session = current_device.session;
    config.candidate = current_device.candidate; config.reserve_c0 = reserve_c0; config.counter_context = &next_c0;
    config.deadline_us = now + 1000000U; config.retry_interval_us = 1000U;
    config.physical_peer = 77U; config.link_generation = device.config.link_generation; config.owner_instance = 31U;
    config.max_attempts = 100U; config.device = 1U; config.service = 1U;
    config.local_record.capability_generation = 1U;
    config.local_record.link.link_instance_generation = device.config.link_generation;
    config.local_record.link.carrier_mtu = 1500U; config.local_record.link.link_frame_mtu = UCN_ADAPTER_FRAME_BYTES;
    config.local_record.link.processing_frame_mtu = UCN_ADAPTER_FRAME_BYTES;
    config.local_record.link.carrier_max_fragments = 8U; config.local_record.link.nominal_rate_bps = 1000000U;
    config.local_record.link.hardware_priority_count = 4U; config.local_record.link.link_flags = 0x000cU;
    config.local_record.peer.feature_bits = UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES;
    config.local_record.peer.hop_suite_bits = 2U; config.local_record.peer.e2e_suite_bits = 2U;
    config.local_record.peer.max_message_class = UCN_I_MESSAGE_T512;
    config.local_record.peer.max_rx_window = 16U; config.local_record.peer.max_concurrent_transfers = 2U;
    config.requirements.minimum_rx_window = 1U; config.requirements.minimum_concurrent_transfers = 1U;
    REQUIRE(ucn_i_native_capability_init(NULL, &config, now) == UCN_ERR_ARGUMENT);
    REQUIRE(ucn_i_native_capability_init((ucn_i_native_capability_t *)(void *)device.persistence,
        &config, now) == UCN_ERR_ARGUMENT);
    cap.tx_ready = 1U;
    REQUIRE(ucn_i_native_capability_init(&cap, &config, now) == UCN_ERR_STATE && cap.tx_ready == 1U);
    cap.tx_ready = 0U;
    {
        ucn_i_native_capability_config_t invalid = config;
        invalid.max_attempts = 1U;
        REQUIRE(ucn_i_native_capability_init(&cap, &invalid, now) == UCN_ERR_CONFIG && cap.magic == 0U);
        invalid = config; invalid.deadline_us = now;
        REQUIRE(ucn_i_native_capability_init(&cap, &invalid, now) == UCN_ERR_CONFIG && cap.magic == 0U);
        invalid = config; ++invalid.candidate.origin_tx.key_id;
        REQUIRE(ucn_i_native_capability_init(&cap, &invalid, now) == UCN_ERR_SECURITY && cap.magic == 0U);
    }
    { ucn_result_t result = ucn_i_native_capability_init(&cap, &config, now);
      if (result != UCN_OK) fprintf(stderr, "cap init result %ld\n", (long)result);
      REQUIRE(result == UCN_OK); }
    memset(&ref, 0xa5, sizeof(ref)); memset(&value, 0xa5, sizeof(value)); memset(&selected, 0xa5, sizeof(selected));
    REQUIRE(ucn_i_native_capability_selected(&cap, now, &ref, &value, &selected) != UCN_OK);
    REQUIRE(ref.runtime_instance == UINT32_C(0xa5a5a5a5) && value.reference.runtime_instance == UINT32_C(0xa5a5a5a5) &&
        selected.local_generation == UINT32_C(0xa5a5a5a5));
    { uint32_t magic = cap_owner.magic;
      REQUIRE(ucn_i_native_capability_selected(&cap, now, (ucn_i_capability_ref_t *)(void *)&cap_owner,
          &value, &selected) == UCN_ERR_ARGUMENT && cap_owner.magic == magic); }
    REQUIRE(ucn_i_native_capability_selected(&cap, now,
        (ucn_i_capability_ref_t *)(void *)device.persistence, &value, &selected) == UCN_ERR_ARGUMENT);
    REQUIRE(ucn_i_native_capability_selected(&cap, now,
        (ucn_i_capability_ref_t *)(void *)device.identity_rules, &value, &selected) == UCN_ERR_ARGUMENT);
#ifndef UCN_CAPABILITY_RED
    /* Fixture's pre-ACL session must never emit a Capability HELLO. */
    REQUIRE(ucn_i_native_capability_drive_one(&cap, now) == UCN_ERR_ACCESS);
    REQUIRE(device.captured_bytes == 0U && next_c0 == 1U);
    { bool quiet = false;
      REQUIRE(ucn_i_native_capability_stop(&cap, now) == UCN_OK);
      REQUIRE(ucn_i_native_capability_quiescent(&cap, &quiet) == UCN_OK && quiet);
      REQUIRE(ucn_i_native_capability_drive_one(&cap, now) == UCN_ERR_STATE); }
    REQUIRE(ucn_i_capability_owner_destroy(&cap_owner) == UCN_OK);
    memset(&cap, 0, sizeof(cap)); next_c0 = 1U;
    device_sent = authority_sent = 0U; memset(opcodes, 0, sizeof(opcodes));
#ifdef UCN_NATIVE_SMALL_MTU_TEST
    REQUIRE(native_join_fixture_init_with_mtu(&device,true,127U)==UCN_OK);
    REQUIRE(native_join_fixture_init_with_mtu(&authority,false,128U)==UCN_OK);
#else
    REQUIRE(native_join_fixture_init(&device, true) == UCN_OK);
    REQUIRE(native_join_fixture_init(&authority, false) == UCN_OK);
#endif
    device.config.configure_session_policy = capability_policy;
    authority.config.configure_session_policy = capability_policy;
    REQUIRE(ucn_i_native_join_init(&device.join, &device.config, 300U) == UCN_OK);
    REQUIRE(ucn_i_native_join_init(&authority.join, &authority.config, 300U) == UCN_OK);
    for (now = 301U; now < 4000U; ++now) {
        REQUIRE(tick(now, true) == 0);
        if (device.join.active && authority.join.active) break;
    }
    REQUIRE(now < 4000U);
    REQUIRE(ucn_i_native_join_handoff(&device.join, now, &current_device) == UCN_OK);
    REQUIRE(ucn_i_native_join_handoff(&authority.join, now, &current_authority) == UCN_OK);
    REQUIRE(ucn_i_capability_owner_init(&cap_owner, 10U, 42U, 30U, 23U,
        1000000U, 1000000U, &lock) == UCN_OK);
    config.session = current_device.session; config.candidate = current_device.candidate;
    config.local_record.link.link_frame_mtu = UCN_ADAPTER_FRAME_BYTES;
    config.local_record.link.processing_frame_mtu = UCN_ADAPTER_FRAME_BYTES;
#ifdef UCN_NATIVE_SMALL_MTU_TEST
    config.local_record.link.link_frame_mtu=127U; config.local_record.link.processing_frame_mtu=127U;
#endif
    config.deadline_us = now + 1000000U;
    REQUIRE(ucn_i_native_capability_init(&cap, &config, now) == UCN_OK);
    REQUIRE(ucn_i_native_capability_drive_one(&cap, now) == UCN_OK);
    if (capability_first_not_submitted) {
        uint8_t saved[UCN_ADAPTER_FRAME_BYTES]; size_t saved_bytes;
        uint64_t counter_before = next_c0;
        device.driver_mode = 1U;
        REQUIRE(ucn_i_native_capability_drive_one(&cap, now + 1U) == UCN_OK);
        REQUIRE(cap.tx_live && device.captured_bytes == 0U);
        saved_bytes = cap.tx_refresh_bytes; memcpy(saved, cap.tx_refresh, saved_bytes);
        REQUIRE(ucn_i_native_capability_retire_one(&cap, now + 1U) == UCN_OK);
        REQUIRE(cap.tx_ready && !cap.tx_live && next_c0 == counter_before);
        device.driver_mode = 0U;
        REQUIRE(ucn_i_native_capability_drive_one(&cap, now + 1U) == UCN_OK);
        REQUIRE(device.captured_bytes == saved_bytes && memcmp(saved, device.captured, saved_bytes) == 0);
    }
    REQUIRE(ucn_i_native_capability_drive_one(&cap, now + 1U) == UCN_OK);
    REQUIRE(device.captured_bytes > 0U && cap.tx_live);
    {
        ucn_i_native_capability_config_t peer_config = config;
        ucn_i_adapter_rx_view_t rx;
        ucn_rx_meta_t meta = {0}; ucn_link_handle_t link;
        ucn_i_lock_ops_t peer_ops = lock;
        peer_counter = 1U;
        peer_ops.context = &peer_lock;
        REQUIRE(ucn_i_capability_owner_init(&peer_owner, 10U, 42U, 32U, 23U,
            1000000U, 1000000U, &peer_ops) == UCN_OK);
        peer_config.adapter = &authority.adapter; peer_config.security = &authority.security;
        peer_config.capability_owner = &peer_owner; peer_config.join_parents = &authority.join;
        peer_config.callback_gate = &authority.callback_gate; peer_config.session = current_authority.session;
        peer_config.candidate = current_authority.candidate; peer_config.counter_context = &peer_counter;
        peer_config.device = 0U; peer_config.link_generation = authority.config.link_generation;
        peer_config.local_record.link.link_instance_generation = authority.config.link_generation;
        peer_config.local_record.link.link_frame_mtu = UCN_ADAPTER_FRAME_BYTES;
        peer_config.local_record.link.processing_frame_mtu = UCN_ADAPTER_FRAME_BYTES;
#ifdef UCN_NATIVE_SMALL_MTU_TEST
        peer_config.local_record.link.link_frame_mtu=128U; peer_config.local_record.link.processing_frame_mtu=128U;
#endif
        REQUIRE(ucn_i_native_capability_init(&peer_cap, &peer_config, now + 1U) == UCN_OK);
        REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
        meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
        meta.timestamp_us = now + 2U; meta.sender_discriminator = authority.config.physical_peer;
        REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter, link, device.captured,
            device.captured_bytes, &meta) == UCN_OK);
        device.captured_bytes = 0U;
        REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter, &rx) == UCN_OK);
        REQUIRE(ucn_i_native_capability_receive_claimed(&peer_cap, &rx, now + 2U) == UCN_OK);
        REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter, &rx.token) == UCN_OK);
        REQUIRE(ucn_i_native_capability_drive_one(&peer_cap, now + 3U) == UCN_OK);
        REQUIRE(peer_cap.tx_opcode == UCN_I_OPCODE_CAPABILITY_QUERY);
        REQUIRE(ucn_i_native_capability_retire_one(&cap, now + 3U) == UCN_OK);
        REQUIRE(ucn_i_native_capability_drive_one(&peer_cap, now + 4U) == UCN_OK);
        REQUIRE(authority.captured_bytes > 0U);
        REQUIRE(ucn_i_adapter_link_handle(&device.adapter, 0U, &link) == UCN_OK);
        meta.timestamp_us = now + 5U; meta.sender_discriminator = device.config.physical_peer;
        REQUIRE(ucn_i_adapter_rx_publish(&device.adapter, link, authority.captured,
            authority.captured_bytes, &meta) == UCN_OK);
        authority.captured_bytes = 0U;
        REQUIRE(ucn_i_adapter_rx_claim(&device.adapter, &rx) == UCN_OK);
        REQUIRE(ucn_i_native_capability_receive_claimed(&cap, &rx, now + 5U) == UCN_OK);
        REQUIRE(ucn_i_adapter_rx_retire(&device.adapter, &rx.token) == UCN_OK);
        REQUIRE(ucn_i_native_capability_drive_one(&cap, now + 6U) == UCN_OK);
        REQUIRE(cap.tx_ready && cap.tx_count == 2U);
        REQUIRE(ucn_i_native_capability_retire_one(&peer_cap, now + 6U) == UCN_OK);
        {
            unsigned fragment_index;
            for (fragment_index = 0U; fragment_index < 2U; ++fragment_index) {
                uint64_t stamp = now + 7U + 3U * fragment_index;
                REQUIRE(ucn_i_native_capability_drive_one(&cap, stamp) == UCN_OK);
                REQUIRE(device.captured_bytes > 0U);
                REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
                meta.timestamp_us = stamp; meta.sender_discriminator = authority.config.physical_peer;
                REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter, link, device.captured,
                    device.captured_bytes, &meta) == UCN_OK);
                device.captured_bytes = 0U;
                REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter, &rx) == UCN_OK);
                REQUIRE(ucn_i_native_capability_receive_claimed(&peer_cap, &rx, stamp) == UCN_OK);
                REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter, &rx.token) == UCN_OK);
                REQUIRE(ucn_i_native_capability_retire_one(&cap, stamp + 1U) == UCN_OK);
                if (fragment_index == 0U) REQUIRE(!peer_cap.pending_control);
            }
        }
        REQUIRE(peer_cap.pending_control && peer_cap.tx_opcode == UCN_I_OPCODE_CAPABILITY_ADVERTISE);
        REQUIRE(ucn_i_native_capability_drive_one(&peer_cap, now + 13U) == UCN_OK);
        REQUIRE(peer_cap.tx_ready && peer_cap.tx_count == 2U);
        {
            unsigned fragment_index;
            for (fragment_index = 0U; fragment_index < 2U; ++fragment_index) {
                uint64_t stamp = now + 14U + 3U * fragment_index;
                REQUIRE(ucn_i_native_capability_drive_one(&peer_cap, stamp) == UCN_OK);
                REQUIRE(authority.captured_bytes > 0U);
                REQUIRE(ucn_i_adapter_link_handle(&device.adapter, 0U, &link) == UCN_OK);
                meta.timestamp_us = stamp; meta.sender_discriminator = device.config.physical_peer;
                REQUIRE(ucn_i_adapter_rx_publish(&device.adapter, link, authority.captured,
                    authority.captured_bytes, &meta) == UCN_OK);
                authority.captured_bytes = 0U;
                REQUIRE(ucn_i_adapter_rx_claim(&device.adapter, &rx) == UCN_OK);
                REQUIRE(ucn_i_native_capability_receive_claimed(&cap, &rx, stamp) == UCN_OK);
                REQUIRE(ucn_i_adapter_rx_retire(&device.adapter, &rx.token) == UCN_OK);
                REQUIRE(ucn_i_native_capability_retire_one(&peer_cap, stamp + 1U) == UCN_OK);
            }
        }
        REQUIRE(cap.pending_control && cap.tx_opcode == UCN_I_OPCODE_CAPABILITY_SELECT);
        REQUIRE(ucn_i_native_capability_drive_one(&cap, now + 20U) == UCN_OK);
        REQUIRE(ucn_i_native_capability_drive_one(&cap, now + 21U) == UCN_OK);
        REQUIRE(device.captured_bytes > 0U);
        REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
        meta.timestamp_us = now + 22U; meta.sender_discriminator = authority.config.physical_peer;
        REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter, link, device.captured,
            device.captured_bytes, &meta) == UCN_OK);
        device.captured_bytes = 0U;
        REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter, &rx) == UCN_OK);
        REQUIRE(ucn_i_native_capability_receive_claimed(&peer_cap, &rx, now + 22U) == UCN_OK);
        REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter, &rx.token) == UCN_OK);
        REQUIRE(peer_cap.pending_control && peer_cap.tx_opcode == UCN_I_OPCODE_CAPABILITY_ACK);
        REQUIRE(ucn_i_native_capability_retire_one(&cap, now + 23U) == UCN_OK);
        REQUIRE(ucn_i_native_capability_drive_one(&peer_cap, now + 23U) == UCN_OK);
        {
            unsigned fragment_index;
            for (fragment_index = 0U; fragment_index < 2U; ++fragment_index) {
                uint64_t stamp = now + 24U + 3U * fragment_index;
                REQUIRE(ucn_i_native_capability_drive_one(&peer_cap, stamp) == UCN_OK);
                REQUIRE(authority.captured_bytes > 0U);
                if (capability_drop_ack && fragment_index == 1U) {
                    authority.captured_bytes = 0U;
                    REQUIRE(ucn_i_native_capability_retire_one(&peer_cap, stamp + 1U) == UCN_OK);
                    continue;
                }
                REQUIRE(ucn_i_adapter_link_handle(&device.adapter, 0U, &link) == UCN_OK);
                meta.timestamp_us = stamp; meta.sender_discriminator = device.config.physical_peer;
                REQUIRE(ucn_i_adapter_rx_publish(&device.adapter, link, authority.captured,
                    authority.captured_bytes, &meta) == UCN_OK);
                authority.captured_bytes = 0U;
                REQUIRE(ucn_i_adapter_rx_claim(&device.adapter, &rx) == UCN_OK);
                REQUIRE(ucn_i_native_capability_receive_claimed(&cap, &rx, stamp) == UCN_OK);
                REQUIRE(ucn_i_adapter_rx_retire(&device.adapter, &rx.token) == UCN_OK);
                REQUIRE(ucn_i_native_capability_retire_one(&peer_cap, stamp + 1U) == UCN_OK);
            }
        }
        selected_now = now + 31U;
        if (capability_drop_ack) {
            unsigned retry_index;
            REQUIRE(ucn_i_native_capability_selected(&cap, selected_now, &ref, &value, &selected) != UCN_OK);
            REQUIRE(ucn_i_native_capability_drive_one(&cap, now + 1024U) == UCN_OK);
            REQUIRE(ucn_i_native_capability_drive_one(&cap, now + 1025U) == UCN_OK);
            REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
            meta.timestamp_us = now + 1026U; meta.sender_discriminator = authority.config.physical_peer;
            REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter, link, device.captured, device.captured_bytes, &meta) == UCN_OK);
            device.captured_bytes = 0U;
            REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter, &rx) == UCN_OK);
            REQUIRE(ucn_i_native_capability_receive_claimed(&peer_cap, &rx, now + 1026U) == UCN_OK);
            REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter, &rx.token) == UCN_OK);
            REQUIRE(ucn_i_native_capability_retire_one(&cap, now + 1027U) == UCN_OK);
            for (retry_index = 0U; retry_index < 2U; ++retry_index) {
                uint64_t stamp = now + 1028U + 3U * retry_index;
                REQUIRE(ucn_i_native_capability_drive_one(&peer_cap, stamp) == UCN_OK);
                REQUIRE(authority.captured_bytes > 0U);
                REQUIRE(ucn_i_adapter_link_handle(&device.adapter, 0U, &link) == UCN_OK);
                meta.timestamp_us = stamp; meta.sender_discriminator = device.config.physical_peer;
                REQUIRE(ucn_i_adapter_rx_publish(&device.adapter, link, authority.captured, authority.captured_bytes, &meta) == UCN_OK);
                authority.captured_bytes = 0U;
                REQUIRE(ucn_i_adapter_rx_claim(&device.adapter, &rx) == UCN_OK);
                REQUIRE(ucn_i_native_capability_receive_claimed(&cap, &rx, stamp) == UCN_OK);
                REQUIRE(ucn_i_adapter_rx_retire(&device.adapter, &rx.token) == UCN_OK);
                REQUIRE(ucn_i_native_capability_retire_one(&peer_cap, stamp + 1U) == UCN_OK);
            }
            selected_now = now + 1036U;
        }
        REQUIRE(ucn_i_native_capability_selected(&cap, selected_now, &ref, &value, &selected) == UCN_OK);
        REQUIRE(value.reference.link_generation == device.config.link_generation);
        REQUIRE(value.record.link.link_instance_generation == authority.config.link_generation);
        REQUIRE(ucn_i_native_capability_selected(&peer_cap, selected_now, &ref, &value, &selected) == UCN_OK);
        REQUIRE(!cap.tx_live && !peer_cap.tx_live && !cap.rx_bitmap && !peer_cap.rx_bitmap);
        capability_ready_now = selected_now;
    }
#endif
    puts("actual JOIN parents accepted; Capability not selected before exchange");
    return 0;
}
#ifndef UCN_CAPABILITY_FIXTURE_ONLY
static ucn_i_native_reauth_t reauth_device, reauth_authority;
static native_reauth_fixture_t reauth_device_product, reauth_authority_product;
static ucn_i_native_capability_t successor_cap, successor_peer;
static ucn_i_security_acl_rule_t successor_device_rules[64], successor_authority_rules[64];
static uint64_t successor_device_counter = 1U, successor_authority_counter = 1U;
static ucn_result_t successor_policy(void *ctx, const ucn_i_security_candidate_t *candidate,
    const ucn_i_security_acl_rule_t **out, uint16_t *count)
{
    native_reauth_fixture_t *f = ctx;
    return fill_capability_policy(f->prior == &device ? successor_device_rules : successor_authority_rules,
        candidate, out, count);
}

static int loss_pair(uint16_t lost_opcode, unsigned lost_occurrence)
{
    ucn_i_native_capability_config_t cfg = {0}, peer_cfg;
    ucn_i_lock_ops_t lock = {0}, peer_ops;
    uint64_t now; unsigned step, matching = 0U, transient_rejections = 0U; bool dropped = false;
    ucn_i_capability_ref_t ref; ucn_i_cached_capability_t cache; ucn_i_profile_select_t selected;
    REQUIRE(native_join_fixture_init_with_mtu(&device, true, 127U) == UCN_OK);
    REQUIRE(native_join_fixture_init_with_mtu(&authority, false, 128U) == UCN_OK);
    device.config.configure_session_policy = capability_policy;
    authority.config.configure_session_policy = capability_policy;
    REQUIRE(ucn_i_native_join_init(&device.join, &device.config, 300U) == UCN_OK);
    REQUIRE(ucn_i_native_join_init(&authority.join, &authority.config, 300U) == UCN_OK);
    for (now = 301U; now < 4000U; ++now) {
        REQUIRE(tick(now, true) == 0);
        if (device.join.active && authority.join.active) break;
    }
    REQUIRE(now < 4000U);
    REQUIRE(ucn_i_native_join_handoff(&device.join, now, &current_device) == UCN_OK);
    REQUIRE(ucn_i_native_join_handoff(&authority.join, now, &current_authority) == UCN_OK);
    lock.struct_size = sizeof(lock); lock.api_version = UCN_I_LOCK_OPS_VERSION;
    lock.context = &cap_lock; lock.enter = cap_lock_enter; lock.leave = cap_lock_leave;
    peer_ops = lock; peer_ops.context = &peer_lock;
    REQUIRE(ucn_i_capability_owner_init(&cap_owner, 10U, 42U, 30U, 23U, 1000000U, 1000000U, &lock) == UCN_OK);
    REQUIRE(ucn_i_capability_owner_init(&peer_owner, 10U, 42U, 32U, 23U, 1000000U, 1000000U, &peer_ops) == UCN_OK);
    cfg.adapter = &device.adapter; cfg.security = &device.security; cfg.capability_owner = &cap_owner;
    cfg.join_parents = &device.join; cfg.callback_gate = &device.callback_gate;
    cfg.session = current_device.session; cfg.candidate = current_device.candidate;
    cfg.reserve_c0 = reserve_c0; cfg.counter_context = &next_c0;
    cfg.deadline_us = now + 1000000U; cfg.retry_interval_us = 1000U;
    cfg.physical_peer = device.config.physical_peer; cfg.link_generation = device.config.link_generation;
    cfg.owner_instance = 31U; cfg.max_attempts = 100U; cfg.device = 1U; cfg.service = 1U;
    cfg.local_record.capability_generation = 1U; cfg.local_record.link.link_instance_generation = cfg.link_generation;
    cfg.local_record.link.carrier_mtu = 1500U; cfg.local_record.link.link_frame_mtu = 127U;
    cfg.local_record.link.processing_frame_mtu = 127U;
    cfg.local_record.link.carrier_max_fragments = 8U; cfg.local_record.link.nominal_rate_bps = 1000000U;
    cfg.local_record.link.hardware_priority_count = 4U; cfg.local_record.link.link_flags = 0x000cU;
    cfg.local_record.peer.feature_bits = UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES;
    cfg.local_record.peer.hop_suite_bits = 2U; cfg.local_record.peer.e2e_suite_bits = 2U;
    cfg.local_record.peer.max_message_class = UCN_I_MESSAGE_T512;
    cfg.local_record.peer.max_rx_window = 16U; cfg.local_record.peer.max_concurrent_transfers = 2U;
    cfg.requirements.minimum_rx_window = 1U; cfg.requirements.minimum_concurrent_transfers = 1U;
    peer_cfg = cfg; peer_cfg.adapter = &authority.adapter; peer_cfg.security = &authority.security;
    peer_cfg.capability_owner = &peer_owner; peer_cfg.join_parents = &authority.join;
    peer_cfg.callback_gate = &authority.callback_gate; peer_cfg.session = current_authority.session;
    peer_cfg.candidate = current_authority.candidate; peer_cfg.counter_context = &peer_counter;
    peer_cfg.device = 0U; peer_cfg.link_generation = authority.config.link_generation;
    peer_cfg.local_record.link.link_instance_generation = peer_cfg.link_generation;
    peer_cfg.local_record.link.link_frame_mtu = 128U; peer_cfg.local_record.link.processing_frame_mtu = 128U;
    REQUIRE(ucn_i_native_capability_init(&cap, &cfg, now) == UCN_OK);
    REQUIRE(ucn_i_native_capability_init(&peer_cap, &peer_cfg, now) == UCN_OK);
    for (step = 1U; step < 1000U; ++step) {
        native_join_fixture_t *nodes[2] = { &device, &authority };
        ucn_i_native_capability_t *leaves[2] = { &cap, &peer_cap };
        uint64_t stamp = now + (uint64_t)step * 100U; unsigned side;
        for (side = 0U; side < 2U; ++side) {
            ucn_result_t r = ucn_i_native_capability_retire_one(leaves[side], stamp);
            REQUIRE(r == UCN_OK);
            r = ucn_i_native_capability_drive_one(leaves[side], stamp);
            if (r != UCN_OK) {
                ucn_i_c0_frame_t dbg;
                ucn_result_t dr = ucn_i_c0_decode(leaves[side]->tx_frame[leaves[side]->tx_index], leaves[side]->tx_bytes[leaves[side]->tx_index], 2U, UCN_I_CONTROL_H1, &dbg);
                fprintf(stderr, "loss drive side=%u phase=%u op=%u r=%ld frame=%zu refresh=%zu body=%zu mtu=%u live=%u decode=%ld wireop=%u origin=%u payload=%zu\n", side, leaves[side]->phase, leaves[side]->tx_opcode, (long)r, leaves[side]->tx_bytes[leaves[side]->tx_index], leaves[side]->tx_refresh_bytes, leaves[side]->tx_body_bytes, nodes[side]->adapter.links[0].frame_mtu, leaves[side]->tx_live, (long)dr, dr == UCN_OK ? dbg.opcode : 0U, dr == UCN_OK ? dbg.origin_security : 0U, dr == UCN_OK ? dbg.payload_bytes : 0U);
            }
            REQUIRE(r == UCN_OK);
        }
        for (side = 0U; side < 2U; ++side) {
            native_join_fixture_t *from = nodes[side], *to = nodes[1U-side];
            ucn_i_c0_frame_t decoded; ucn_i_adapter_rx_view_t rx; ucn_link_handle_t link; ucn_rx_meta_t meta = {0};
            ucn_result_t r;
            if (!from->captured_bytes) continue;
            REQUIRE(ucn_i_c0_decode(from->captured, from->captured_bytes, 2U, UCN_I_CONTROL_H1, &decoded) == UCN_OK);
            if (decoded.opcode == UCN_I_OPCODE_NEIGHBOR_HELLO) REQUIRE(from->captured_bytes == 85U);
            if (decoded.opcode == UCN_I_OPCODE_CAPABILITY_QUERY) REQUIRE(from->captured_bytes == 81U);
            if (decoded.opcode == UCN_I_OPCODE_CAPABILITY_SELECT) REQUIRE(from->captured_bytes == 120U);
            if (decoded.opcode == UCN_I_OPCODE_CAPABILITY_FRAGMENT) {
                uint16_t inner;
                REQUIRE(decoded.payload_bytes >= 6U);
                inner = (uint16_t)(((uint16_t)decoded.payload[0] << 8U) | decoded.payload[1]);
                if (inner == UCN_I_OPCODE_CAPABILITY_ADVERTISE) REQUIRE(from->captured_bytes == 101U);
                else { REQUIRE(inner == UCN_I_OPCODE_CAPABILITY_ACK);
                    REQUIRE(from->captured_bytes == (decoded.payload[4] == 0U ? 105U : 104U)); }
            }
            if (decoded.opcode == lost_opcode && ++matching == lost_occurrence) { dropped = true; from->captured_bytes = 0U; continue; }
            REQUIRE(ucn_i_adapter_link_handle(&to->adapter, 0U, &link) == UCN_OK);
            meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
            meta.timestamp_us = stamp; meta.sender_discriminator = to->config.physical_peer;
            REQUIRE(ucn_i_adapter_rx_publish(&to->adapter, link, from->captured, from->captured_bytes, &meta) == UCN_OK);
            from->captured_bytes = 0U;
            REQUIRE(ucn_i_adapter_rx_claim(&to->adapter, &rx) == UCN_OK);
            r = ucn_i_native_capability_receive_claimed(leaves[1U-side], &rx, stamp);
            REQUIRE(ucn_i_adapter_rx_retire(&to->adapter, &rx.token) == UCN_OK);
            if (r != UCN_OK && r != UCN_ERR_STATE && r != UCN_ERR_REPLAY)
                fprintf(stderr, "loss receive side=%u phase=%u op=%u r=%ld\n", 1U-side, leaves[1U-side]->phase, decoded.opcode, (long)r);
            REQUIRE(r == UCN_OK || r == UCN_ERR_STATE || r == UCN_ERR_REPLAY);
            if (r != UCN_OK) ++transient_rejections;
        }
        if (ucn_i_native_capability_selected(&cap, stamp, &ref, &cache, &selected) == UCN_OK &&
            ucn_i_native_capability_selected(&peer_cap, stamp, &ref, &cache, &selected) == UCN_OK &&
            !cap.tx_live && !peer_cap.tx_live && !cap.tx_ready && !peer_cap.tx_ready) {
            REQUIRE(dropped); printf("loss opcode=%u occurrence=%u transient=%u converged\n", lost_opcode, lost_occurrence, transient_rejections); return 0;
        }
    }
    REQUIRE(false); return 1;
}

/* Real first JOIN + selected Capability, actual fence/drain, signed Reauth,
 * Foundation reload, then new leaves replace the same Owner's old references. */
static int successor_pair(void)
{
    ucn_i_capability_ref_t old_device, old_authority, ref;
    ucn_i_cached_capability_t cache; ucn_i_profile_select_t profile;
    ucn_i_native_capability_config_t a, b;
    uint64_t now, stamp; unsigned step, side;
    bool quiet;
    if (capability_ready_pair()) return 1;
    now = capability_ready_now;
    old_device = cap.reference; old_authority = peer_cap.reference;
    REQUIRE(ucn_i_native_capability_stop(&cap, now) == UCN_OK);
    REQUIRE(ucn_i_native_capability_stop(&peer_cap, now) == UCN_OK);
    REQUIRE(ucn_i_native_capability_quiescent(&cap, &quiet) == UCN_OK && quiet);
    REQUIRE(ucn_i_native_capability_quiescent(&peer_cap, &quiet) == UCN_OK && quiet);
    REQUIRE(ucn_i_security_fence(&device.security, current_device.session) == UCN_OK);
    REQUIRE(ucn_i_security_fence(&authority.security, current_authority.session) == UCN_OK);
    native_reauth_fixture_configure(&reauth_device_product, &device, &reauth_device, true, 0x1234U, 101U);
    native_reauth_fixture_configure(&reauth_authority_product, &authority, &reauth_authority, false, 0x5678U, 0U);
    reauth_device_product.config.configure_session_policy = successor_policy;
    reauth_authority_product.config.configure_session_policy = successor_policy;
    REQUIRE(ucn_i_native_reauth_init(&reauth_device, &reauth_device_product.config, ++now) == UCN_OK);
    REQUIRE(ucn_i_native_reauth_init(&reauth_authority, &reauth_authority_product.config, now) == UCN_OK);
    for (step = 1U; step < 10000U; ++step) {
        native_join_fixture_t *nodes[2] = { &device, &authority };
        ucn_i_native_reauth_t *roots[2] = { &reauth_device, &reauth_authority };
        stamp = now + step;
        for (side = 0U; side < 2U; ++side) {
            ucn_persistence_step_result_t io;
            REQUIRE(ucn_i_native_reauth_retire_one(roots[side], stamp) == UCN_OK);
            if (roots[side]->work) {
                REQUIRE(ucn_i_persistence_step(nodes[side]->persistence, stamp, 1U, &io) == UCN_OK);
                REQUIRE(ucn_i_native_reauth_finish_durability(roots[side], stamp) == UCN_OK);
            }
            REQUIRE(ucn_i_native_reauth_drive_one(roots[side], stamp) == UCN_OK);
        }
        for (side = 0U; side < 2U; ++side) {
            native_join_fixture_t *from = nodes[side], *to = nodes[1U-side];
            ucn_link_handle_t link; ucn_rx_meta_t meta = {0}; ucn_i_adapter_rx_view_t rx;
            if (!from->captured_bytes) continue;
            REQUIRE(ucn_i_adapter_link_handle(&to->adapter, 0U, &link) == UCN_OK);
            meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
            meta.timestamp_us = stamp; meta.sender_discriminator = to->config.physical_peer;
            REQUIRE(ucn_i_adapter_rx_publish(&to->adapter, link, from->captured, from->captured_bytes, &meta) == UCN_OK);
            from->captured_bytes = 0U;
            REQUIRE(ucn_i_adapter_rx_claim(&to->adapter, &rx) == UCN_OK);
            REQUIRE(ucn_i_native_reauth_receive_claimed(roots[1U-side], &rx, stamp) == UCN_OK);
            REQUIRE(ucn_i_adapter_rx_retire(&to->adapter, &rx.token) == UCN_OK);
        }
        if (ucn_i_native_reauth_session_current(&reauth_device, stamp, &current_device) == UCN_OK &&
            ucn_i_native_reauth_session_current(&reauth_authority, stamp, &current_authority) == UCN_OK) break;
    }
    REQUIRE(step < 10000U);
    REQUIRE(current_device.candidate.session_generation == 2U && current_authority.candidate.session_generation == 2U);
    REQUIRE(ucn_i_native_reauth_handoff(&reauth_device, stamp, &current_device) == UCN_OK);
    REQUIRE(ucn_i_native_reauth_handoff(&reauth_authority, stamp, &current_authority) == UCN_OK);
    a = cap.config; b = peer_cap.config;
    a.session = current_device.session; a.candidate = current_device.candidate; a.has_predecessor = 1U;
    a.predecessor = old_device; a.counter_context = &successor_device_counter; a.deadline_us = stamp + 500000U;
    b.session = current_authority.session; b.candidate = current_authority.candidate; b.has_predecessor = 1U;
    b.predecessor = old_authority; b.counter_context = &successor_authority_counter; b.deadline_us = stamp + 500000U;
    REQUIRE(ucn_i_native_capability_init(&successor_cap, &a, stamp) == UCN_OK);
    REQUIRE(ucn_i_native_capability_init(&successor_peer, &b, stamp) == UCN_OK);
    /* The predecessor slot exists until the new authenticated Advertise;
     * this Owner snapshot does not re-authorize its fenced old Session. */
    REQUIRE(ucn_i_capability_get(&cap_owner, &old_device, stamp, &cache) == UCN_OK);
    REQUIRE(ucn_i_capability_get(&peer_owner, &old_authority, stamp, &cache) == UCN_OK);
    now = stamp;
    for (step = 1U; step < 10000U; ++step) {
        native_join_fixture_t *nodes[2] = { &device, &authority };
        ucn_i_native_capability_t *roots[2] = { &successor_cap, &successor_peer };
        stamp = now + step;
        for (side = 0U; side < 2U; ++side) {
            REQUIRE(ucn_i_native_capability_retire_one(roots[side], stamp) == UCN_OK);
            REQUIRE(ucn_i_native_capability_drive_one(roots[side], stamp) == UCN_OK);
        }
        for (side = 0U; side < 2U; ++side) {
            native_join_fixture_t *from = nodes[side], *to = nodes[1U-side];
            ucn_link_handle_t link; ucn_rx_meta_t meta = {0}; ucn_i_adapter_rx_view_t rx;
            if (!from->captured_bytes) continue;
            REQUIRE(ucn_i_adapter_link_handle(&to->adapter, 0U, &link) == UCN_OK);
            meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
            meta.timestamp_us = stamp; meta.sender_discriminator = to->config.physical_peer;
            REQUIRE(ucn_i_adapter_rx_publish(&to->adapter, link, from->captured, from->captured_bytes, &meta) == UCN_OK);
            from->captured_bytes = 0U;
            REQUIRE(ucn_i_adapter_rx_claim(&to->adapter, &rx) == UCN_OK);
            REQUIRE(ucn_i_native_capability_receive_claimed(roots[1U-side], &rx, stamp) == UCN_OK);
            REQUIRE(ucn_i_adapter_rx_retire(&to->adapter, &rx.token) == UCN_OK);
        }
        if (ucn_i_native_capability_selected(&successor_cap, stamp, &ref, &cache, &profile) == UCN_OK &&
            ucn_i_native_capability_selected(&successor_peer, stamp, &ref, &cache, &profile) == UCN_OK &&
            !successor_cap.tx_live && !successor_peer.tx_live) break;
    }
    REQUIRE(step < 10000U);
    REQUIRE(ucn_i_capability_get(&cap_owner, &old_device, stamp, &cache) != UCN_OK);
    REQUIRE(ucn_i_capability_get(&peer_owner, &old_authority, stamp, &cache) != UCN_OK);
    REQUIRE(successor_cap.reference.session_generation == 2U && successor_peer.reference.session_generation == 2U);
    {
        ucn_i_security_acl_rule_t saved = successor_device_rules[0];
        uint8_t saved_output[sizeof(ucn_i_security_candidate_t)];
        memcpy(saved_output, successor_device_rules, sizeof(saved_output));
        REQUIRE(ucn_i_security_session_candidate_get(&device.security, successor_cap.config.session,
            (ucn_i_security_candidate_t *)(void *)successor_device_rules) == UCN_ERR_ARGUMENT);
        REQUIRE(memcmp(saved_output, successor_device_rules, sizeof(saved_output)) == 0);
        REQUIRE(ucn_i_native_capability_quiescent(&successor_cap,
            (bool *)(void *)successor_device_rules) == UCN_ERR_ARGUMENT);
        REQUIRE(memcmp(saved_output, successor_device_rules, sizeof(saved_output)) == 0);
        REQUIRE(ucn_i_native_capability_selected(&successor_cap, stamp,
            (ucn_i_capability_ref_t *)(void *)successor_device_rules, &cache, &profile) == UCN_ERR_ARGUMENT);
        REQUIRE(memcmp(&saved, &successor_device_rules[0], sizeof(saved)) == 0);
        REQUIRE(ucn_i_native_capability_init((ucn_i_native_capability_t *)(void *)successor_device_rules,
            &a, stamp) == UCN_ERR_ARGUMENT);
        REQUIRE(memcmp(&saved, &successor_device_rules[0], sizeof(saved)) == 0);
    }
    puts("successor real Reauth + Foundation + Capability atomic replacement passed");
    return 0;
}

static int retirement_query_guards(void)
{
    static ucn_i_native_capability_t before;
    ucn_i_callback_claim_t held = {81U, 1U, 1U, 1U};
    bool quiet = true;
    void *aliases[] = { &cap, &cap_owner, &device.security, &device.adapter,
        &device.callback_gate, &device.join, &device.config, &device.identity,
        device.persistence, device.persistence->callback_gate, device_rules, &next_c0 };
    size_t i;
    before = cap;
    REQUIRE(ucn_i_callback_gate_enter(&device.callback_gate, &held) == UCN_OK);
    REQUIRE(ucn_i_native_capability_retire_one(&cap, cap.now_us + 100U) == UCN_ERR_STATE);
    REQUIRE(memcmp(&cap, &before, sizeof(cap)) == 0);
    REQUIRE(ucn_i_native_capability_quiescent(&cap, &quiet) == UCN_ERR_STATE && quiet);
    REQUIRE(memcmp(&cap, &before, sizeof(cap)) == 0);
    REQUIRE(ucn_i_callback_gate_leave(&device.callback_gate, &held) == UCN_OK);
    for (i = 0U; i < sizeof(aliases)/sizeof(aliases[0]); ++i) {
        uint8_t first = *(uint8_t *)aliases[i];
        REQUIRE(ucn_i_native_capability_quiescent(&cap, (bool *)aliases[i]) == UCN_ERR_ARGUMENT);
        REQUIRE(*(uint8_t *)aliases[i] == first);
    }
    REQUIRE(memcmp(&cap, &before, sizeof(cap)) == 0);
    REQUIRE(ucn_i_native_capability_quiescent(&cap, &quiet) == UCN_OK && !quiet);
    return 0;
}

static int selected_revocation(void)
{
    static ucn_i_security_packet_workspace_t work;
    static ucn_i_security_c0_protect_request_t request;
    ucn_i_capability_query_t query = {0};
    ucn_i_capability_summary_t summary = {0};
    uint8_t body[UCN_I_CAPABILITY_SUMMARY_BYTES], frame[UCN_ADAPTER_FRAME_BYTES];
    size_t frame_bytes; ucn_i_adapter_rx_view_t rx; ucn_link_handle_t link;
    ucn_i_callback_claim_t claim = {0};
    ucn_rx_meta_t meta = {0}; ucn_i_capability_ref_t ref; ucn_i_cached_capability_t cache;
    ucn_i_profile_select_t selected;
    request.session = peer_cap.config.session; request.facts = peer_cap.facts;
    request.facts.now_us = capability_ready_now + 1U;
    claim.owner_instance = 31U; claim.operation_id = 999U;
    claim.operation_generation = 1U; claim.operation_kind = 1U;
    REQUIRE(ucn_i_callback_gate_enter(&authority.callback_gate, &claim) == UCN_OK);
    REQUIRE(reserve_c0(&peer_counter, &authority.security, request.session, &request.transaction_id) == UCN_OK);
    REQUIRE(ucn_i_callback_gate_leave(&authority.callback_gate, &claim) == UCN_OK);
    request.protocol_opcode = UCN_I_OPCODE_NEIGHBOR_KEEPALIVE;
    memcpy(request.context_fingerprint, peer_cap.config.candidate.origin_fingerprint, 16U);
    request.traffic_class = UCN_TRAFFIC_Q2; request.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    request.interaction_role = UCN_INTERACTION_ONE_WAY; request.payload_kind = UCN_I_PAYLOAD_CONTROL;
    request.hop_limit = 1U; request.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    summary.capability_generation = peer_cap.config.local_record.capability_generation;
    summary.link_instance_generation = peer_cap.config.local_record.link.link_instance_generation;
    memcpy(summary.digest, peer_cap.local_digest, 16U);
    REQUIRE(ucn_i_capability_summary_encode(&summary, body) == UCN_OK);
    REQUIRE(ucn_i_security_c0_protect(&authority.security, &request,
        body, sizeof(body), &work, frame, sizeof(frame), &frame_bytes) == UCN_OK);
    REQUIRE(ucn_i_adapter_link_handle(&device.adapter, 0U, &link) == UCN_OK);
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    meta.sender_discriminator = device.config.physical_peer; meta.timestamp_us = capability_ready_now + 1U;
    REQUIRE(ucn_i_adapter_rx_publish(&device.adapter, link, frame, frame_bytes, &meta) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_claim(&device.adapter, &rx) == UCN_OK);
    REQUIRE(ucn_i_native_capability_receive_claimed(&cap, &rx, capability_ready_now + 1U) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_retire(&device.adapter, &rx.token) == UCN_OK);
    REQUIRE(ucn_i_native_capability_selected(&cap, capability_ready_now + 1U, &ref, &cache, &selected) == UCN_OK);
    query.requested_generation = peer_cap.config.local_record.capability_generation;
    memcpy(query.known_digest, peer_cap.local_digest, 16U);
    REQUIRE(ucn_i_capability_query_encode(&query, body) == UCN_OK);
    request.facts.now_us++; claim.operation_id++;
    REQUIRE(ucn_i_callback_gate_enter(&authority.callback_gate, &claim) == UCN_OK);
    REQUIRE(reserve_c0(&peer_counter, &authority.security, request.session, &request.transaction_id) == UCN_OK);
    REQUIRE(ucn_i_callback_gate_leave(&authority.callback_gate, &claim) == UCN_OK);
    request.protocol_opcode = UCN_I_OPCODE_CAPABILITY_REVOKE;
    REQUIRE(ucn_i_security_c0_protect(&authority.security, &request,
        body, UCN_I_CAPABILITY_QUERY_BYTES, &work, frame, sizeof(frame), &frame_bytes) == UCN_OK);
    meta.timestamp_us++;
    frame[frame_bytes - 1U] ^= 1U;
    REQUIRE(ucn_i_adapter_rx_publish(&device.adapter, link, frame, frame_bytes, &meta) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_claim(&device.adapter, &rx) == UCN_OK);
    REQUIRE(ucn_i_native_capability_receive_claimed(&cap, &rx, capability_ready_now + 2U) != UCN_OK);
    REQUIRE(ucn_i_adapter_rx_retire(&device.adapter, &rx.token) == UCN_OK);
    REQUIRE(ucn_i_native_capability_selected(&cap, capability_ready_now + 2U, &ref, &cache, &selected) == UCN_OK);
    frame[frame_bytes - 1U] ^= 1U;
    REQUIRE(ucn_i_adapter_rx_publish(&device.adapter, link, frame, frame_bytes, &meta) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_claim(&device.adapter, &rx) == UCN_OK);
    REQUIRE(ucn_i_native_capability_receive_claimed(&cap, &rx, capability_ready_now + 2U) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_retire(&device.adapter, &rx.token) == UCN_OK);
    REQUIRE(ucn_i_native_capability_selected(&cap, capability_ready_now + 2U, &ref, &cache, &selected) != UCN_OK);
    return 0;
}
static int duplicate_select_replies(void)
{
    static ucn_i_security_packet_workspace_t work;
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES]; size_t n;
    ucn_i_adapter_rx_view_t rx; ucn_link_handle_t link; ucn_rx_meta_t meta = {0};
    ucn_i_security_current_facts_t facts = cap.facts;
    facts.now_us = capability_ready_now + 3U;
    REQUIRE(cap.tx_opcode == UCN_I_OPCODE_CAPABILITY_SELECT);
    REQUIRE(ucn_i_security_c0_refresh_hop(&device.security, cap.config.session, &facts,
        cap.tx_frame[0], cap.tx_bytes[0], &work, packet, sizeof(packet), &n) == UCN_OK);
    REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    meta.sender_discriminator = authority.config.physical_peer; meta.timestamp_us = facts.now_us;
    REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter, link, packet, n, &meta) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter, &rx) == UCN_OK);
    REQUIRE(ucn_i_native_capability_receive_claimed(&peer_cap, &rx, facts.now_us) == UCN_OK);
    REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter, &rx.token) == UCN_OK);
    REQUIRE(peer_cap.tx_ready && peer_cap.tx_opcode == UCN_I_OPCODE_CAPABILITY_ACK);
    authority.driver_mode = 2U; authority.cancel_result = UCN_ERR_IN_DOUBT;
    REQUIRE(ucn_i_native_capability_drive_one(&peer_cap, facts.now_us + 1U) == UCN_OK);
    REQUIRE(peer_cap.tx_live);
    REQUIRE(ucn_i_native_capability_stop(&peer_cap, facts.now_us + 2U) == UCN_OK);
    REQUIRE(ucn_i_native_capability_retire_one(&peer_cap, facts.now_us + 3U) == UCN_ERR_IN_DOUBT);
    {
        bool quiet = true;
        ucn_tx_meta_t complete = {0};
        ucn_link_event_meta_t reopen = {0};
        REQUIRE(ucn_i_native_capability_quiescent(&peer_cap, &quiet) == UCN_OK && !quiet);
        REQUIRE(ucn_i_adapter_link_handle(&authority.adapter, 0U, &link) == UCN_OK);
        reopen.struct_size = sizeof(reopen); reopen.api_version = UCN_API_VERSION;
        reopen.new_link_instance = authority.config.link_generation + 1U;
        REQUIRE(ucn_i_adapter_link_event(&authority.adapter, link, UCN_DRIVER_LINK_REOPENED, &reopen) == UCN_OK);
        complete.struct_size = sizeof(complete); complete.api_version = UCN_API_VERSION;
        complete.timestamp_us = peer_cap.config.deadline_us;
        REQUIRE(ucn_i_adapter_tx_complete(&authority.adapter, peer_cap.tx_token, UCN_ERR_IN_DOUBT, &complete) == UCN_OK);
        REQUIRE(ucn_i_native_capability_retire_one(&peer_cap, complete.timestamp_us) == UCN_OK);
        REQUIRE(ucn_i_native_capability_quiescent(&peer_cap, &quiet) == UCN_OK && quiet);
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2) {
        if (strcmp(argv[1], "successor") == 0) return successor_pair();
        if (strcmp(argv[1], "updated") == 0) {
            ucn_i_capability_ref_t ref; ucn_i_cached_capability_t cache; ucn_i_profile_select_t profile;
            ucn_i_capability_record_t changed;
            if (capability_ready_pair()) return 1;
            changed = cap.cached.record; ++changed.capability_generation;
            /* Regression mutation of the real Owner, not a Wire authorization. */
            REQUIRE(ucn_i_capability_advertise_ingest(&cap_owner, &cap.peer, &changed,
                capability_ready_now + 1U, &ref) == UCN_OK);
            REQUIRE(ucn_i_native_capability_selected(&cap, capability_ready_now + 1U, &ref, &cache, &profile) == UCN_ERR_SECURITY);
            return 0;
        }
        if (strcmp(argv[1], "expired") == 0) {
            ucn_i_capability_ref_t ref; ucn_i_cached_capability_t cache; ucn_i_profile_select_t profile;
            if (capability_ready_pair()) return 1;
            memset(&ref, 0xa5, sizeof(ref)); memset(&cache, 0xa5, sizeof(cache)); memset(&profile, 0xa5, sizeof(profile));
            REQUIRE(ucn_i_native_capability_selected(&cap, cap.config.deadline_us, &ref, &cache, &profile) == UCN_ERR_TIMEOUT);
            REQUIRE(ref.runtime_instance == UINT32_C(0xa5a5a5a5) && cache.reference.runtime_instance == UINT32_C(0xa5a5a5a5) &&
                profile.local_generation == UINT32_C(0xa5a5a5a5));
            return 0;
        }
        if (strcmp(argv[1], "hello") == 0) return loss_pair(UCN_I_OPCODE_NEIGHBOR_HELLO, 1U);
        if (strcmp(argv[1], "query") == 0) return loss_pair(UCN_I_OPCODE_CAPABILITY_QUERY, 1U);
        if (strcmp(argv[1], "fragment") == 0) return loss_pair(UCN_I_OPCODE_CAPABILITY_FRAGMENT, 1U);
        if (strcmp(argv[1], "fragment2") == 0) return loss_pair(UCN_I_OPCODE_CAPABILITY_FRAGMENT, 2U);
        if (strcmp(argv[1], "fragment3") == 0) return loss_pair(UCN_I_OPCODE_CAPABILITY_FRAGMENT, 3U);
        if (strcmp(argv[1], "fragment4") == 0) return loss_pair(UCN_I_OPCODE_CAPABILITY_FRAGMENT, 4U);
        if (strcmp(argv[1], "select") == 0) return loss_pair(UCN_I_OPCODE_CAPABILITY_SELECT, 1U);
        return 1;
    }
    capability_drop_ack = 1;
    capability_first_not_submitted = 1;
    if (capability_ready_pair() || retirement_query_guards() || selected_revocation()) return 1;
    return duplicate_select_replies();
}
#endif
#else
#ifndef UCN_CAPABILITY_FIXTURE_ONLY
int main(void) { return 0; }
#endif
#endif
