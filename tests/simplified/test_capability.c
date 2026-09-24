#include "internal/ucn_capability.h"

#include "rust/tests/conformance/v6s_admission_capability_v1.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr_)                                                         \
    do {                                                                     \
        if (!(expr_)) {                                                      \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #expr_);                                       \
            return 1;                                                        \
        }                                                                    \
    } while (0)

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = context;
    lock->held = 0U;
}

static ucn_i_lock_ops_t lock_ops(test_lock_t *lock)
{
    ucn_i_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = lock_enter;
    ops.leave = lock_leave;
    return ops;
}

static ucn_i_capability_record_t make_record(uint32_t generation)
{
    ucn_i_capability_record_t record;
    memset(&record, 0, sizeof(record));
    record.capability_generation = generation;
    record.link.link_instance_generation = UINT32_C(0x11121314);
    record.link.carrier_mtu = 1500U;
    record.link.link_frame_mtu = 1400U;
    record.link.processing_frame_mtu = 1300U;
    record.link.carrier_header_bytes = 14U;
    record.link.carrier_padding_bytes = 2U;
    record.link.carrier_crc_bytes = 4U;
    record.link.carrier_tag_bytes = 12U;
    record.link.carrier_max_fragments = 8U;
    record.link.link_flags = 0x000CU;
    record.link.nominal_rate_bps = 1000000U;
    record.link.hardware_priority_count = 4U;
    record.peer.feature_bits = UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES;
    record.peer.hop_suite_bits = 2U;
    record.peer.e2e_suite_bits = 6U;
    record.peer.max_message_class = UCN_I_MESSAGE_T512;
    record.peer.max_rx_window = 16U;
    record.peer.max_concurrent_transfers = 2U;
    return record;
}

static ucn_i_authenticated_peer_view_t make_auth(uint8_t principal,
                                                  uint32_t session)
{
    ucn_i_authenticated_peer_view_t view;
    memset(&view, 0, sizeof(view));
    view.runtime_instance = 10U;
    view.realm_id = 42U;
    view.address = (uint32_t)principal + 1U;
    view.binding_generation = 7U;
    view.session_generation = session;
    view.link_id = 2U;
    view.link_generation = UINT32_C(0x11121314);
    view.expires_at_us = 10000U;
    view.security_owner_instance = 21U;
    memset(view.principal, principal, sizeof(view.principal));
    return view;
}

int main(void)
{
    static const uint8_t record_golden[68] = {
        UCN_V6S_CAPABILITY_RECORD_BYTES};
    static const uint8_t digest_golden[16] = {
        UCN_V6S_CAPABILITY_DIGEST_BYTES};
    static const uint8_t summary_golden[24] = {
        UCN_V6S_CAPABILITY_SUMMARY_BYTES};
    static const uint8_t query_golden[20] = {
        UCN_V6S_CAPABILITY_QUERY_BYTES};
    ucn_i_capability_owner_t owner;
    ucn_i_capability_record_t record = make_record(UINT32_C(0x01020304));
    ucn_i_capability_record_t decoded;
    ucn_i_capability_summary_t summary;
    ucn_i_capability_summary_t decoded_summary;
    ucn_i_capability_query_t query;
    ucn_i_capability_query_t decoded_query;
    ucn_i_authenticated_peer_view_t auth = make_auth(0x41U, 3U);
    ucn_i_capability_ref_t reference;
    ucn_i_cached_capability_t cached;
    ucn_i_profile_requirements_t requirements;
    ucn_i_profile_select_t selected;
    ucn_i_profile_ack_t ack;
    ucn_i_effective_intent_t intent;
    ucn_i_resource_view_t resources;
    ucn_i_contract_candidate_t candidates[3];
    ucn_i_resolve_result_t resolved;
    test_lock_t state_lock = {0};
    ucn_i_lock_ops_t ops = lock_ops(&state_lock);
    uint8_t bytes[68];
    uint8_t before[68];
    uint8_t digest[16];
    uint8_t disposition;

    memset(&owner, 0, sizeof(owner));
    memset(&decoded, 0, sizeof(decoded));
    memset(&summary, 0, sizeof(summary));
    memset(&decoded_summary, 0, sizeof(decoded_summary));
    memset(&query, 0, sizeof(query));
    memset(&decoded_query, 0, sizeof(decoded_query));
    memset(&reference, 0, sizeof(reference));
    memset(&cached, 0, sizeof(cached));
    memset(&requirements, 0, sizeof(requirements));
    memset(&selected, 0, sizeof(selected));
    memset(&ack, 0, sizeof(ack));
    memset(&intent, 0, sizeof(intent));
    memset(&resources, 0, sizeof(resources));
    memset(candidates, 0, sizeof(candidates));
    memset(&resolved, 0, sizeof(resolved));

    CHECK(ucn_i_capability_record_encode(&record, bytes) == UCN_OK);
    CHECK(memcmp(bytes, record_golden, sizeof(record_golden)) == 0);
    CHECK(ucn_i_capability_record_decode(record_golden, &decoded) == UCN_OK);
    CHECK(memcmp(&record, &decoded, sizeof(record)) == 0);
    CHECK(ucn_i_capability_digest(&record, digest) == UCN_OK);
    CHECK(memcmp(digest, digest_golden, sizeof(digest)) == 0);

    summary.capability_generation = record.capability_generation;
    summary.link_instance_generation = record.link.link_instance_generation;
    memcpy(summary.digest, digest, sizeof(summary.digest));
    CHECK(ucn_i_capability_summary_encode(&summary, bytes) == UCN_OK);
    CHECK(memcmp(bytes, summary_golden, sizeof(summary_golden)) == 0);
    CHECK(ucn_i_capability_summary_decode(
              summary_golden, &decoded_summary) == UCN_OK);
    CHECK(memcmp(&summary, &decoded_summary, sizeof(summary)) == 0);

    query.requested_generation = record.capability_generation;
    memcpy(query.known_digest, digest, sizeof(query.known_digest));
    CHECK(ucn_i_capability_query_encode(&query, bytes) == UCN_OK);
    CHECK(memcmp(bytes, query_golden, sizeof(query_golden)) == 0);
    CHECK(ucn_i_capability_query_decode(query_golden, &decoded_query) == UCN_OK);
    CHECK(memcmp(&query, &decoded_query, sizeof(query)) == 0);

    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    record.link.carrier_max_fragments = 0U;
    CHECK(ucn_i_capability_record_encode(&record, bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    record = make_record(UINT32_C(0x01020304));

    CHECK(ucn_i_capability_owner_init(
              &owner, 10U, 42U, 30U, 21U, 1000U, 2000U, &ops) == UCN_OK);
    CHECK(ucn_i_capability_summary_ingest(
              &owner, &auth, &summary, 100U, &disposition) == UCN_OK);
    CHECK(disposition == UCN_I_CAPABILITY_SUMMARY_QUERY_REQUIRED);
    {
        ucn_i_capability_summary_t alias = summary;
        ucn_i_capability_summary_t alias_before = alias;
        CHECK(ucn_i_capability_summary_ingest(
                  &owner, &auth, &alias, 100U,
                  (ucn_i_capability_summary_result_t *)(void *)&alias) ==
              UCN_ERR_ARGUMENT);
        CHECK(memcmp(&alias, &alias_before, sizeof(alias)) == 0);
    }
    CHECK(ucn_i_capability_advertise_ingest(
              &owner, &auth, &record, 100U, &reference) == UCN_OK);
    CHECK(ucn_i_capability_summary_ingest(
              &owner, &auth, &summary, 101U, &disposition) == UCN_OK);
    CHECK(disposition == UCN_I_CAPABILITY_SUMMARY_MATCHED);
    CHECK(ucn_i_capability_get(
              &owner, &reference, 1099U, &cached) == UCN_OK);
    CHECK(ucn_i_capability_get(
              &owner, &reference, 1100U, &cached) == UCN_ERR_TIMEOUT);

    {
        ucn_i_authenticated_peer_view_t moved = auth;
        ucn_i_capability_ref_t sentinel;
        ucn_i_capability_ref_t sentinel_before;
        moved.session_generation++;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        sentinel_before = sentinel;
        CHECK(ucn_i_capability_advertise_ingest(
                  &owner, &moved, &record, 200U,
                  &sentinel) == UCN_ERR_REPLAY);
        CHECK(memcmp(&sentinel, &sentinel_before, sizeof(sentinel)) == 0);
    }
    record.capability_generation++;
    CHECK(ucn_i_capability_advertise_ingest(
              &owner, &auth, &record, 200U, &reference) == UCN_OK);
    CHECK(ucn_i_capability_invalidate_session(
              &owner, &reference) == UCN_OK);
    auth.session_generation++;
    record.capability_generation = 1U;
    CHECK(ucn_i_capability_advertise_ingest(
              &owner, &auth, &record, 201U, &reference) == UCN_OK);

    CHECK(ucn_i_capability_digest(&record, digest) == UCN_OK);
    requirements.required_feature_bits =
        UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES;
    requirements.required_hop_suite_bits = 2U;
    requirements.required_e2e_suite_bits = 2U;
    requirements.minimum_message_class = UCN_I_MESSAGE_T128;
    requirements.minimum_rx_window = 2U;
    requirements.minimum_concurrent_transfers = 1U;
    CHECK(ucn_i_profile_select_build(
              &record, digest, &record, digest,
              &requirements, &selected) == UCN_OK);
    ack.selected = selected;
    memset(ack.transcript_digest, 0x77, sizeof(ack.transcript_digest));
    CHECK(ucn_i_profile_ack_verify_exact(
              &ack, &selected, ack.transcript_digest) == UCN_OK);
    CHECK(ucn_i_profile_ack_verify_exact(
              NULL, &selected, ack.transcript_digest) == UCN_ERR_ARGUMENT);
    ack.selected.max_rx_window--;
    CHECK(ucn_i_profile_ack_verify_exact(
              &ack, &selected, ack.transcript_digest) == UCN_ERR_SECURITY);
    ack.selected = selected;
    {
        uint8_t wrong_transcript[16];
        memset(wrong_transcript, 0x76, sizeof(wrong_transcript));
        CHECK(ucn_i_profile_ack_verify_exact(
                  &ack, &selected, wrong_transcript) == UCN_ERR_SECURITY);
        memset(wrong_transcript, 0, sizeof(wrong_transcript));
        CHECK(ucn_i_profile_ack_verify_exact(
                  &ack, &selected, wrong_transcript) == UCN_ERR_SECURITY);
    }
    requirements.required_realtime_mode_bits = 1U;
    CHECK(ucn_i_profile_select_build(
              &record, digest, &record, digest,
              &requirements, &selected) == UCN_ERR_UNSUPPORTED);

    intent.required_feature_bits = 1U;
    intent.security_floor = 1U;
    intent.payload_bytes = 16U;
    resources.tx_available = 1U;
    resources.reliable_available = 1U;
    resources.transfer_available = 1U;
    candidates[0].feature_bits = 1U;
    candidates[0].payload_budget = 32U;
    candidates[0].exact_frame_bytes = 40U;
    candidates[0].setup_cost_bytes = 100U;
    candidates[0].expected_reuse_count = 2U;
    candidates[0].stable_order = 2U;
    candidates[0].contract = 1U;
    candidates[0].origin_security = 1U;
    candidates[0].missing_dependency = UCN_I_DEPENDENCY_ROUTE;
    candidates[1] = candidates[0];
    candidates[1].missing_dependency = UCN_I_DEPENDENCY_SECURITY;
    candidates[2] = candidates[0];
    candidates[2].missing_dependency = 0U;
    candidates[2].exact_frame_bytes = 30U;
    candidates[2].stable_order = 1U;
    CHECK(ucn_i_contract_resolve(
              &intent, &resources, candidates, 3U, &resolved) == UCN_OK);
    CHECK(resolved.status == UCN_I_RESOLVE_READY);
    CHECK(resolved.candidate.exact_frame_bytes == 30U);
    {
        ucn_i_effective_intent_t alias = intent;
        ucn_i_effective_intent_t alias_before = alias;
        CHECK(ucn_i_contract_resolve(
                  &alias, &resources, candidates, 3U,
                  (ucn_i_resolve_result_t *)(void *)&alias) ==
              UCN_ERR_ARGUMENT);
        CHECK(memcmp(&alias, &alias_before, sizeof(alias)) == 0);
    }
    resources.tx_available = 0U;
    CHECK(ucn_i_contract_resolve(
              &intent, &resources, candidates, 2U, &resolved) == UCN_OK);
    CHECK(resolved.status == UCN_I_RESOLVE_NEED_DEPENDENCY);
    CHECK(resolved.dependency == UCN_I_DEPENDENCY_SECURITY);

    CHECK(ucn_i_capability_invalidate_session(&owner, &reference) == UCN_OK);
    CHECK(ucn_i_capability_owner_destroy(&owner) == UCN_OK);

    CHECK(ucn_i_capability_owner_init(
              &owner, 10U, 42U, 30U, 21U, 10U, 10U, &ops) == UCN_OK);
    {
        ucn_i_capability_owner_t owner_before = owner;
        ucn_i_capability_ref_t expired_ref;
        uint8_t expired_valid = UINT8_C(0xA5);

        memset(&expired_ref, 0xA5, sizeof(expired_ref));
        CHECK(ucn_i_capability_maintain(
                  &owner, 100U, 1U, &expired_ref,
                  &owner.maintenance_cursor,
                  &expired_valid) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&owner, &owner_before, sizeof(owner)) == 0);
        CHECK(expired_valid == UINT8_C(0xA5));
        CHECK(((const uint8_t *)&expired_ref)[0] == UINT8_C(0xA5));
    }
    {
        ucn_i_capability_ref_t refs[UCN_I_CAPABILITY_PEER_COUNT];
        uint16_t index;
        for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
            ucn_i_authenticated_peer_view_t peer =
                make_auth((uint8_t)(index + 1U), 1U);
            ucn_i_capability_record_t value = make_record(1U);
            CHECK(ucn_i_capability_advertise_ingest(
                      &owner, &peer, &value, 100U, &refs[index]) == UCN_OK);
        }
        {
            ucn_i_authenticated_peer_view_t extra = make_auth(0x70U, 1U);
            ucn_i_capability_record_t value = make_record(1U);
            ucn_i_capability_ref_t sentinel;
            ucn_i_capability_ref_t sentinel_before;
            memset(&sentinel, 0xA5, sizeof(sentinel));
            sentinel_before = sentinel;
            CHECK(ucn_i_capability_advertise_ingest(
                      &owner, &extra, &value, 200U,
                      &sentinel) == UCN_ERR_NO_SPACE);
            CHECK(memcmp(&sentinel, &sentinel_before,
                         sizeof(sentinel)) == 0);
            {
                ucn_i_capability_ref_t expired_ref;
                uint16_t inspected;
                uint8_t expired_valid;
                CHECK(ucn_i_capability_maintain(
                          &owner, 110U, 1U, &expired_ref, &inspected,
                          &expired_valid) == UCN_OK);
                CHECK(inspected == 1U && expired_valid == 1U);
                CHECK(memcmp(&expired_ref, &refs[0], sizeof(expired_ref)) == 0);
                CHECK(ucn_i_capability_advertise_ingest(
                          &owner, &extra, &value, 110U,
                          &sentinel) == UCN_OK);
                CHECK(ucn_i_capability_invalidate_session(
                          &owner, &sentinel) == UCN_OK);
            }
        }
        for (index = 1U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
            CHECK(ucn_i_capability_invalidate_session(
                      &owner, &refs[index]) == UCN_OK);
        }
    }
    CHECK(ucn_i_capability_owner_destroy(&owner) == UCN_OK);
    puts("capability tests passed");
    return 0;
}
