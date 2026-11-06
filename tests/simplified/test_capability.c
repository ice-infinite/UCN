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

/* EN: Independent metadata fixtures, exact chunks and zero-write rejection.
 * 中文：独立元数据向量、精确分块及拒绝时零写。 */
static int test_fragment_codec(void)
{
    ucn_i_capability_fragment_t v;
    ucn_i_capability_fragment_t decoded;
    ucn_i_capability_fragment_t sentinel;
    uint8_t raw[44];
    uint8_t before[44];
    size_t length;
    size_t total;
    uint8_t index;
    size_t i;
    for (total = 68U; total <= 75U; total += 7U) {
        for (index = 0U; index < 2U; ++index) {
            size_t half = (total + 1U) / 2U;
            memset(&v, 0, sizeof(v));
            v.opcode = total == 68U ? 0x0103U : 0x0105U;
            v.total_bytes = (uint16_t)total;
            v.index = index;
            v.count = 2U;
            v.chunk_bytes = (uint8_t)(index == 0U ? half : total - half);
            for (i = 0U; i < v.chunk_bytes; ++i) { v.chunk[i] = (uint8_t)(index * half + i); }
            memset(raw, 0xA5, sizeof(raw));
            CHECK(ucn_i_capability_fragment_encode(&v, raw, sizeof(raw), &length) == UCN_OK);
            CHECK(length == 6U + v.chunk_bytes && raw[0] == 1U && raw[1] == (total == 68U ? 3U : 5U));
            CHECK(raw[2] == 0U && raw[3] == total && raw[4] == index && raw[5] == 2U);
            CHECK(memcmp(raw + 6U, v.chunk, v.chunk_bytes) == 0);
            CHECK(ucn_i_capability_fragment_decode(raw, length, &decoded) == UCN_OK);
            CHECK(decoded.opcode == v.opcode && decoded.total_bytes == v.total_bytes && decoded.index == index);
            CHECK(decoded.chunk_bytes == v.chunk_bytes && memcmp(decoded.chunk, v.chunk, v.chunk_bytes) == 0);
            memset(&decoded, 0xA5, sizeof(decoded));
            sentinel = decoded;
            CHECK(ucn_i_capability_fragment_decode(raw, length - 1U, &decoded) == UCN_ERR_MALFORMED);
            CHECK(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
            raw[5] = 1U;
            CHECK(ucn_i_capability_fragment_decode(raw, length, &decoded) == UCN_ERR_MALFORMED);
            CHECK(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
            memset(raw, 0xA5, sizeof(raw));
            memcpy(before, raw, sizeof(before));
            length = 0xA5U;
            CHECK(ucn_i_capability_fragment_encode(&v, raw, 6U + v.chunk_bytes - 1U, &length) == UCN_ERR_NO_SPACE);
            CHECK(memcmp(raw, before, sizeof(raw)) == 0 && length == 0xA5U);
            v.index = 2U;
            CHECK(ucn_i_capability_fragment_encode(&v, raw, sizeof(raw), &length) == UCN_ERR_ARGUMENT);
            CHECK(memcmp(raw, before, sizeof(raw)) == 0 && length == 0xA5U);
        }
    }
    return 0;
}

/* EN: Exact retained-parent replacement, bounded reuse and zero-write failures.
 * ZH: 精确保留父域替换、固定槽复用与失败零写；认证输入是Coordinator单测SPI向量。 */
static int test_capability_successor(void)
{
    ucn_i_capability_owner_t owner;
    ucn_i_capability_owner_t snapshot;
    ucn_i_capability_record_t record = make_record(5U);
    ucn_i_authenticated_peer_view_t auth = make_auth(0x41U, 3U);
    ucn_i_authenticated_peer_view_t changed;
    ucn_i_capability_ref_t old;
    ucn_i_capability_ref_t next;
    ucn_i_capability_ref_t sentinel;
    ucn_i_cached_capability_t cached;
    test_lock_t lock = {0};
    ucn_i_lock_ops_t ops = lock_ops(&lock);
    uint32_t generation;
    memset(&owner,0,sizeof(owner));
    CHECK(ucn_i_capability_owner_init(&owner,10U,42U,30U,21U,
                                      1000U,2000U,&ops) == UCN_OK);
    CHECK(ucn_i_capability_advertise_ingest(&owner,&auth,&record,100U,&old) == UCN_OK);
    snapshot = owner;
    memset(&next,0xA5,sizeof(next)); sentinel = next;
    changed = auth;
    CHECK(ucn_i_capability_replace_session(&owner,&old,&changed,&record,101U,&next) == UCN_ERR_REPLAY);
    CHECK(memcmp(&owner,&snapshot,sizeof(owner)) == 0);
    CHECK(memcmp(&next,&sentinel,sizeof(next)) == 0);
    changed.session_generation = 4U;
    changed.binding_generation++;
    CHECK(ucn_i_capability_replace_session(&owner,&old,&changed,&record,101U,&next) == UCN_ERR_REPLAY);
    CHECK(memcmp(&owner,&snapshot,sizeof(owner)) == 0);
    CHECK(memcmp(&next,&sentinel,sizeof(next)) == 0);
    changed = auth; changed.session_generation = 4U; changed.expires_at_us = 101U;
    CHECK(ucn_i_capability_replace_session(&owner,&old,&changed,&record,101U,&next) == UCN_ERR_SECURITY);
    CHECK(memcmp(&owner,&snapshot,sizeof(owner)) == 0);
    CHECK(memcmp(&next,&sentinel,sizeof(next)) == 0);
    changed = auth; changed.session_generation = 4U;
    CHECK(ucn_i_capability_replace_session(&owner,&old,&changed,&record,101U,&old) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&owner,&snapshot,sizeof(owner)) == 0);
    record = make_record(1U); record.peer.max_rx_window = 4U;
    for (generation = 4U; generation <= 8U; ++generation) {
        uint16_t i;
        uint16_t occupied = 0U;
        changed.session_generation = generation;
        CHECK(ucn_i_capability_replace_session(&owner,&old,&changed,&record,
                  100U + generation,&next) == UCN_OK);
        CHECK(next.session_generation == generation);
        CHECK(ucn_i_capability_get(&owner,&old,100U + generation,&cached) == UCN_ERR_NOT_FOUND);
        CHECK(ucn_i_capability_get(&owner,&next,100U + generation,&cached) == UCN_OK);
        CHECK(cached.record.capability_generation == 1U);
        CHECK(cached.record.peer.max_rx_window == 4U);
        CHECK(cached.capability_deadline_us == 2100U + generation);
        for (i=0U; i<UCN_I_CAPABILITY_PEER_COUNT; ++i) {
            occupied = (uint16_t)(occupied + (owner.peers[i].occupied != 0U ? 1U : 0U));
        }
        CHECK(occupied == 1U);
        old = next;
    }
    CHECK(ucn_i_capability_advertise_ingest(&owner,&auth,&record,110U,&next) == UCN_ERR_REPLAY);
    CHECK(ucn_i_capability_invalidate_session(&owner,&old) == UCN_OK);
    CHECK(ucn_i_capability_owner_destroy(&owner) == UCN_OK);
    return 0;
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
    static const uint8_t profile_select_golden[59] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x11U, 0x12U, 0x13U, 0x14U,
        0x00U, 0x00U, 0x01U, 0x0BU, 0x00U, 0x00U, 0x00U, 0x02U,
        0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x10U, 0x00U, 0x02U,
        0x00U, 0x00U, 0x04U,
        0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U,
        0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U,
        0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U,
        0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U};
    static const uint8_t profile_ack_tail[16] = {
        0x77U, 0x77U, 0x77U, 0x77U, 0x77U, 0x77U, 0x77U, 0x77U,
        0x77U, 0x77U, 0x77U, 0x77U, 0x77U, 0x77U, 0x77U, 0x77U};
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
    ucn_i_profile_select_t decoded_selected;
    ucn_i_profile_ack_t ack;
    ucn_i_profile_ack_t decoded_ack;
    ucn_i_effective_intent_t intent;
    ucn_i_resource_view_t resources;
    ucn_i_contract_candidate_t candidates[3];
    ucn_i_resolve_result_t resolved;
    test_lock_t state_lock = {0};
    ucn_i_lock_ops_t ops = lock_ops(&state_lock);
    uint8_t bytes[UCN_I_PROFILE_ACK_BYTES];
    uint8_t before[UCN_I_PROFILE_ACK_BYTES];
    uint8_t digest[16];
    uint8_t disposition;

    memset(&owner, 0, sizeof(owner));
    memset(&decoded, 0, sizeof(decoded));
    CHECK(test_fragment_codec() == 0);
    CHECK(test_capability_successor() == 0);
    memset(&summary, 0, sizeof(summary));
    memset(&decoded_summary, 0, sizeof(decoded_summary));
    memset(&query, 0, sizeof(query));
    memset(&decoded_query, 0, sizeof(decoded_query));
    memset(&reference, 0, sizeof(reference));
    memset(&cached, 0, sizeof(cached));
    memset(&requirements, 0, sizeof(requirements));
    memset(&selected, 0, sizeof(selected));
    memset(&decoded_selected, 0, sizeof(decoded_selected));
    memset(&ack, 0, sizeof(ack));
    memset(&decoded_ack, 0, sizeof(decoded_ack));
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

    selected.local_generation = UINT32_C(0x01020304);
    selected.peer_generation = UINT32_C(0x11121314);
    selected.feature_bits = UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES;
    selected.hop_suite_bits = 2U;
    selected.e2e_suite_bits = 6U;
    selected.max_rx_window = 16U;
    selected.max_concurrent_transfers = 2U;
    selected.max_message_class = UCN_I_MESSAGE_T512;
    memset(selected.local_digest, 0x11, sizeof(selected.local_digest));
    memset(selected.peer_digest, 0x22, sizeof(selected.peer_digest));
    CHECK(ucn_i_profile_select_encode(&selected, bytes) == UCN_OK);
    CHECK(memcmp(bytes, profile_select_golden,
                 sizeof(profile_select_golden)) == 0);
    CHECK(ucn_i_profile_select_decode(profile_select_golden,
                                      &decoded_selected) == UCN_OK);
    CHECK(memcmp(&selected, &decoded_selected, sizeof(selected)) == 0);
    ack.selected = selected;
    memcpy(ack.transcript_digest, profile_ack_tail,
           sizeof(ack.transcript_digest));
    CHECK(ucn_i_profile_ack_encode(&ack, bytes) == UCN_OK);
    CHECK(memcmp(bytes, profile_select_golden,
                 sizeof(profile_select_golden)) == 0);
    CHECK(memcmp(&bytes[UCN_I_PROFILE_SELECT_BYTES], profile_ack_tail,
                 sizeof(profile_ack_tail)) == 0);
    CHECK(ucn_i_profile_ack_decode(bytes, &decoded_ack) == UCN_OK);
    CHECK(memcmp(&ack, &decoded_ack, sizeof(ack)) == 0);
    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    selected.reserved_zero = 1U;
    CHECK(ucn_i_profile_select_encode(&selected, bytes) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    selected.reserved_zero = 0U;
    memcpy(bytes, profile_select_golden, sizeof(profile_select_golden));
    bytes[26] = UINT8_MAX;
    memset(&decoded_selected, 0xA5, sizeof(decoded_selected));
    {
        ucn_i_profile_select_t decoded_before = decoded_selected;
        CHECK(ucn_i_profile_select_decode(bytes, &decoded_selected) ==
              UCN_ERR_MALFORMED);
        CHECK(memcmp(&decoded_selected, &decoded_before,
                     sizeof(decoded_selected)) == 0);
    }

    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    record.link.carrier_max_fragments = 0U;
    CHECK(ucn_i_capability_record_encode(&record, bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    record = make_record(UINT32_C(0x01020304));

    CHECK(ucn_i_capability_owner_init(
              &owner, 10U, 42U, 30U, 21U, 1000U, 2000U, &ops) == UCN_OK);
    /* The receiving Link's generation belongs to this node, not the sender.
     * 接收Link的代际属于本端，不要求与发送方Record代际相等。 */
    auth.link_generation = 9U;
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
    {
        ucn_i_capability_summary_t remote_wrong = summary;
        remote_wrong.link_instance_generation = auth.link_generation;
        CHECK(ucn_i_capability_summary_ingest(
                  &owner, &auth, &remote_wrong, 101U, &disposition) == UCN_OK);
        CHECK(disposition == UCN_I_CAPABILITY_SUMMARY_QUERY_REQUIRED);
        CHECK(ucn_i_capability_get(
                  &owner, &reference, 101U, &cached) == UCN_OK);
        CHECK(cached.record.link.link_instance_generation ==
              record.link.link_instance_generation);
        CHECK(cached.reference.link_generation == 9U);
    }
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

    {
        ucn_i_capability_query_t revoke = {0};
        ucn_i_capability_record_t changed = record;
        ucn_i_capability_owner_t revoke_snapshot;
        revoke.requested_generation = record.capability_generation;
        CHECK(ucn_i_capability_digest(&record, revoke.known_digest) == UCN_OK);
        CHECK(ucn_i_capability_revoke(&owner, &reference, &revoke) == UCN_OK);
        CHECK(ucn_i_capability_get(&owner,&reference,202U,&cached)==UCN_ERR_STATE);
        revoke_snapshot = owner;
        CHECK(ucn_i_capability_advertise_ingest(&owner,&auth,&record,202U,&reference)==UCN_ERR_REPLAY);
        CHECK(memcmp(&revoke_snapshot,&owner,sizeof(owner))==0);
        changed.capability_generation += 2U;
        CHECK(ucn_i_capability_advertise_ingest(&owner,&auth,&changed,202U,&reference)==UCN_ERR_REPLAY);
        record.capability_generation++;
        CHECK(ucn_i_capability_advertise_ingest(&owner,&auth,&record,203U,&reference)==UCN_OK);
        changed = record; changed.peer.max_rx_window++;
        revoke_snapshot = owner;
        CHECK(ucn_i_capability_advertise_ingest(&owner,&auth,&changed,204U,&reference)==UCN_ERR_SECURITY);
        CHECK(memcmp(&revoke_snapshot,&owner,sizeof(owner))==0);
    }

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
                          &sentinel) == UCN_ERR_NO_SPACE);
                CHECK(ucn_i_capability_get(&owner,&refs[0],110U,&cached)==UCN_ERR_TIMEOUT);
                CHECK(ucn_i_capability_invalidate_session(&owner,&refs[0])==UCN_OK);
                CHECK(ucn_i_capability_advertise_ingest(
                          &owner,&extra,&value,110U,&sentinel)==UCN_OK);
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
