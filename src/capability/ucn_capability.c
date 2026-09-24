#include "internal/ucn_capability.h"

#include "internal/ucn_checked.h"
#include "internal/ucn_digest.h"

#include <string.h>

#define UCN_I_CAPABILITY_MAGIC UINT32_C(0x55434341)
#define UCN_I_CAPABILITY_MAX_FRAME_BYTES UINT32_C(65678)
#define UCN_I_CAPABILITY_KNOWN_LINK_FLAGS UINT16_C(0x001F)
#define UCN_I_CAPABILITY_KNOWN_TIMESTAMP_BITS UINT16_C(0x000F)
#define UCN_I_CAPABILITY_KNOWN_REALTIME_BITS UINT16_C(0x0007)

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool bytes_zero(const uint8_t *bytes, size_t length)
{
    return !bytes_nonzero(bytes, length);
}

static bool object_zero(const void *object, size_t bytes)
{
    return bytes_zero((const uint8_t *)object, bytes);
}

static bool lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

static ucn_result_t owner_lock(ucn_i_capability_owner_t *owner)
{
    if (owner == NULL || owner->magic != UCN_I_CAPABILITY_MAGIC ||
        owner->schema != UCN_I_CAPABILITY_SCHEMA ||
        !lock_valid(&owner->state_lock)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

static void owner_unlock(ucn_i_capability_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static void put16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (uint8_t)(value >> 8U);
    bytes[offset + 1U] = (uint8_t)value;
}

static void put32(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (uint8_t)(value >> 24U);
    bytes[offset + 1U] = (uint8_t)(value >> 16U);
    bytes[offset + 2U] = (uint8_t)(value >> 8U);
    bytes[offset + 3U] = (uint8_t)value;
}

static uint16_t get16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t)(((uint16_t)bytes[offset] << 8U) |
                      bytes[offset + 1U]);
}

static uint32_t get32(const uint8_t *bytes, size_t offset)
{
    return ((uint32_t)bytes[offset] << 24U) |
           ((uint32_t)bytes[offset + 1U] << 16U) |
           ((uint32_t)bytes[offset + 2U] << 8U) |
           bytes[offset + 3U];
}

static bool record_valid(const ucn_i_capability_record_t *record)
{
    uint32_t overhead;

    if (record == NULL || record->link.reserved_zero != 0U ||
        !bytes_zero(record->peer.reserved_zero,
                    sizeof(record->peer.reserved_zero))) {
        return false;
    }
    overhead = record->link.carrier_header_bytes;
    if (UINT32_MAX - overhead < record->link.carrier_padding_bytes) {
        return false;
    }
    overhead += record->link.carrier_padding_bytes;
    if (UINT32_MAX - overhead < record->link.carrier_crc_bytes) {
        return false;
    }
    overhead += record->link.carrier_crc_bytes;
    if (UINT32_MAX - overhead < record->link.carrier_tag_bytes) {
        return false;
    }
    overhead += record->link.carrier_tag_bytes;
    return record->capability_generation != 0U &&
           record->link.link_instance_generation != 0U &&
           record->link.carrier_mtu != 0U &&
           record->link.link_frame_mtu != 0U &&
           record->link.processing_frame_mtu != 0U &&
           record->link.link_frame_mtu <=
               UCN_I_CAPABILITY_MAX_FRAME_BYTES &&
           record->link.processing_frame_mtu <=
               UCN_I_CAPABILITY_MAX_FRAME_BYTES &&
           record->link.carrier_max_fragments != 0U &&
           record->link.nominal_rate_bps != 0U &&
           record->link.hardware_priority_count != 0U &&
           (record->link.link_flags &
            (uint16_t)~UCN_I_CAPABILITY_KNOWN_LINK_FLAGS) == 0U &&
           (record->link.link_flags & UINT16_C(0x000C)) != 0U &&
           (record->link.timestamp_capability_bits &
            (uint16_t)~UCN_I_CAPABILITY_KNOWN_TIMESTAMP_BITS) == 0U &&
           ((record->link.timestamp_capability_bits == 0U) ==
            (record->link.timestamp_uncertainty_us == 0U)) &&
           overhead < record->link.carrier_mtu &&
           (record->peer.feature_bits &
            ~UCN_I_CAPABILITY_KNOWN_FEATURES) == 0U &&
           (record->peer.feature_bits &
            UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES) ==
               UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES &&
           record->peer.hop_suite_bits != 0U &&
           (record->peer.hop_suite_bits &
            ~UCN_I_CAPABILITY_KNOWN_HOP_SUITES) == 0U &&
           record->peer.e2e_suite_bits != 0U &&
           (record->peer.e2e_suite_bits &
            ~UCN_I_CAPABILITY_KNOWN_E2E_SUITES) == 0U &&
           record->peer.max_message_class <= UCN_I_MESSAGE_T8K &&
           record->peer.max_rx_window != 0U &&
           record->peer.max_concurrent_transfers != 0U &&
           (record->peer.realtime_mode_bits &
            (uint16_t)~UCN_I_CAPABILITY_KNOWN_REALTIME_BITS) == 0U &&
           (((record->peer.feature_bits & (UINT32_C(1) << 6U)) == 0U) ==
            (record->peer.realtime_mode_bits == 0U)) &&
           (((record->peer.realtime_mode_bits & UINT16_C(0x0006)) != 0U) ?
                (record->peer.clock_domain_id != 0U &&
                 record->peer.clock_domain_generation != 0U) :
                (record->peer.clock_domain_id == 0U &&
                 record->peer.clock_domain_generation == 0U));
}

ucn_result_t ucn_i_capability_record_encode(
    const ucn_i_capability_record_t *record,
    uint8_t output[UCN_I_CAPABILITY_RECORD_BYTES])
{
    uint8_t bytes[UCN_I_CAPABILITY_RECORD_BYTES];

    if (!record_valid(record) || output == NULL ||
        ucn_i_ranges_overlap(record, sizeof(*record), output,
                             UCN_I_CAPABILITY_RECORD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(bytes, 0, sizeof(bytes));
    put32(bytes, 0U, record->capability_generation);
    put32(bytes, 4U, record->link.link_instance_generation);
    put32(bytes, 8U, record->link.carrier_mtu);
    put32(bytes, 12U, record->link.link_frame_mtu);
    put32(bytes, 16U, record->link.processing_frame_mtu);
    put16(bytes, 20U, record->link.carrier_header_bytes);
    put16(bytes, 22U, record->link.carrier_padding_bytes);
    put16(bytes, 24U, record->link.carrier_crc_bytes);
    put16(bytes, 26U, record->link.carrier_tag_bytes);
    put16(bytes, 28U, record->link.carrier_max_fragments);
    put16(bytes, 30U, record->link.link_flags);
    put32(bytes, 32U, record->link.nominal_rate_bps);
    put32(bytes, 36U, record->link.timestamp_uncertainty_us);
    put32(bytes, 40U, record->peer.feature_bits);
    put32(bytes, 44U, record->peer.hop_suite_bits);
    put32(bytes, 48U, record->peer.e2e_suite_bits);
    put16(bytes, 52U, record->peer.max_rx_window);
    put16(bytes, 54U, record->peer.max_concurrent_transfers);
    bytes[56] = record->peer.max_message_class;
    bytes[57] = record->link.hardware_priority_count;
    put16(bytes, 58U, record->link.timestamp_capability_bits);
    put16(bytes, 60U, record->peer.realtime_mode_bits);
    put16(bytes, 62U, record->peer.clock_domain_id);
    put32(bytes, 64U, record->peer.clock_domain_generation);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_capability_record_decode(
    const uint8_t input[UCN_I_CAPABILITY_RECORD_BYTES],
    ucn_i_capability_record_t *record_out)
{
    ucn_i_capability_record_t record;

    if (input == NULL || record_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_CAPABILITY_RECORD_BYTES,
                             record_out, sizeof(*record_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&record, 0, sizeof(record));
    record.capability_generation = get32(input, 0U);
    record.link.link_instance_generation = get32(input, 4U);
    record.link.carrier_mtu = get32(input, 8U);
    record.link.link_frame_mtu = get32(input, 12U);
    record.link.processing_frame_mtu = get32(input, 16U);
    record.link.carrier_header_bytes = get16(input, 20U);
    record.link.carrier_padding_bytes = get16(input, 22U);
    record.link.carrier_crc_bytes = get16(input, 24U);
    record.link.carrier_tag_bytes = get16(input, 26U);
    record.link.carrier_max_fragments = get16(input, 28U);
    record.link.link_flags = get16(input, 30U);
    record.link.nominal_rate_bps = get32(input, 32U);
    record.link.timestamp_uncertainty_us = get32(input, 36U);
    record.peer.feature_bits = get32(input, 40U);
    record.peer.hop_suite_bits = get32(input, 44U);
    record.peer.e2e_suite_bits = get32(input, 48U);
    record.peer.max_rx_window = get16(input, 52U);
    record.peer.max_concurrent_transfers = get16(input, 54U);
    record.peer.max_message_class = input[56];
    record.link.hardware_priority_count = input[57];
    record.link.timestamp_capability_bits = get16(input, 58U);
    record.peer.realtime_mode_bits = get16(input, 60U);
    record.peer.clock_domain_id = get16(input, 62U);
    record.peer.clock_domain_generation = get32(input, 64U);
    if (!record_valid(&record)) {
        return UCN_ERR_MALFORMED;
    }
    *record_out = record;
    return UCN_OK;
}

ucn_result_t ucn_i_capability_digest(
    const ucn_i_capability_record_t *record,
    uint8_t digest_out[UCN_I_CAPABILITY_DIGEST_BYTES])
{
    uint8_t encoded[UCN_I_CAPABILITY_RECORD_BYTES];
    ucn_i_sha256_workspace_t workspace;

    if (!record_valid(record) || digest_out == NULL ||
        ucn_i_ranges_overlap(record, sizeof(*record), digest_out,
                             UCN_I_CAPABILITY_DIGEST_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_i_capability_record_encode(record, encoded) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (ucn_i_sha256_128(encoded, sizeof(encoded), digest_out, &workspace) !=
        UCN_OK) {
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

static bool summary_valid(const ucn_i_capability_summary_t *summary)
{
    return summary != NULL && summary->capability_generation != 0U &&
           summary->link_instance_generation != 0U &&
           bytes_nonzero(summary->digest, sizeof(summary->digest));
}

static bool query_valid(const ucn_i_capability_query_t *query)
{
    return query != NULL &&
           ((query->requested_generation == 0U) ==
            bytes_zero(query->known_digest, sizeof(query->known_digest)));
}

ucn_result_t ucn_i_capability_summary_encode(
    const ucn_i_capability_summary_t *summary,
    uint8_t output[UCN_I_CAPABILITY_SUMMARY_BYTES])
{
    uint8_t bytes[UCN_I_CAPABILITY_SUMMARY_BYTES];

    if (!summary_valid(summary) || output == NULL ||
        ucn_i_ranges_overlap(summary, sizeof(*summary), output,
                             UCN_I_CAPABILITY_SUMMARY_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    put32(bytes, 0U, summary->capability_generation);
    put32(bytes, 4U, summary->link_instance_generation);
    memcpy(&bytes[8], summary->digest, 16U);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_capability_summary_decode(
    const uint8_t input[UCN_I_CAPABILITY_SUMMARY_BYTES],
    ucn_i_capability_summary_t *summary_out)
{
    ucn_i_capability_summary_t summary;

    if (input == NULL || summary_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_CAPABILITY_SUMMARY_BYTES,
                             summary_out, sizeof(*summary_out))) {
        return UCN_ERR_ARGUMENT;
    }
    summary.capability_generation = get32(input, 0U);
    summary.link_instance_generation = get32(input, 4U);
    memcpy(summary.digest, &input[8], 16U);
    if (!summary_valid(&summary)) {
        return UCN_ERR_MALFORMED;
    }
    *summary_out = summary;
    return UCN_OK;
}

ucn_result_t ucn_i_capability_query_encode(
    const ucn_i_capability_query_t *query,
    uint8_t output[UCN_I_CAPABILITY_QUERY_BYTES])
{
    uint8_t bytes[UCN_I_CAPABILITY_QUERY_BYTES];

    if (!query_valid(query) || output == NULL ||
        ucn_i_ranges_overlap(query, sizeof(*query), output,
                             UCN_I_CAPABILITY_QUERY_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    put32(bytes, 0U, query->requested_generation);
    memcpy(&bytes[4], query->known_digest, 16U);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_capability_query_decode(
    const uint8_t input[UCN_I_CAPABILITY_QUERY_BYTES],
    ucn_i_capability_query_t *query_out)
{
    ucn_i_capability_query_t query;

    if (input == NULL || query_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_CAPABILITY_QUERY_BYTES,
                             query_out, sizeof(*query_out))) {
        return UCN_ERR_ARGUMENT;
    }
    query.requested_generation = get32(input, 0U);
    memcpy(query.known_digest, &input[4], 16U);
    if (!query_valid(&query)) {
        return UCN_ERR_MALFORMED;
    }
    *query_out = query;
    return UCN_OK;
}

static bool authenticated_valid(
    const ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *view,
    uint64_t now_us)
{
    return view != NULL && view->runtime_instance == owner->runtime_instance &&
           view->realm_id == owner->realm_id &&
           view->security_owner_instance == owner->security_owner_instance &&
           view->address != 0U && view->address != UINT32_MAX &&
           view->binding_generation != 0U &&
           view->session_generation != 0U && view->link_id != 0U &&
           view->link_generation != 0U && view->reserved_zero == 0U &&
           bytes_nonzero(view->principal, sizeof(view->principal)) &&
           now_us != 0U && now_us < view->expires_at_us;
}

static ucn_i_capability_ref_t make_ref(
    const ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *view)
{
    ucn_i_capability_ref_t reference;

    memset(&reference, 0, sizeof(reference));
    reference.runtime_instance = owner->runtime_instance;
    reference.realm_id = owner->realm_id;
    reference.address = view->address;
    reference.binding_generation = view->binding_generation;
    reference.session_generation = view->session_generation;
    reference.link_id = view->link_id;
    reference.link_generation = view->link_generation;
    reference.security_owner_instance = owner->security_owner_instance;
    memcpy(reference.principal, view->principal, sizeof(reference.principal));
    return reference;
}

static bool ref_equal(const ucn_i_capability_ref_t *left,
                      const ucn_i_capability_ref_t *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

ucn_result_t ucn_i_capability_owner_init(
    ucn_i_capability_owner_t *owner,
    uint32_t runtime_instance,
    uint32_t realm_id,
    uint16_t owner_instance,
    uint16_t security_owner_instance,
    uint64_t discovery_lease_us,
    uint64_t capability_lease_us,
    const ucn_i_lock_ops_t *state_lock)
{
    ucn_result_t result;

    if (owner == NULL || runtime_instance == 0U || realm_id == 0U ||
        realm_id == UINT32_MAX || owner_instance == 0U ||
        security_owner_instance == 0U || discovery_lease_us == 0U ||
        capability_lease_us == 0U || !lock_valid(state_lock) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), state_lock,
                             sizeof(*state_lock)) ||
        (state_lock->context != NULL &&
         ucn_i_ranges_overlap(owner, sizeof(*owner),
                              state_lock->context, 1U))) {
        return UCN_ERR_CONFIG;
    }
    result = state_lock->enter(state_lock->context);
    if (result != UCN_OK) {
        return result;
    }
    if (!object_zero(owner, sizeof(*owner))) {
        state_lock->leave(state_lock->context);
        return UCN_ERR_STATE;
    }
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_I_CAPABILITY_MAGIC;
    owner->schema = UCN_I_CAPABILITY_SCHEMA;
    owner->runtime_instance = runtime_instance;
    owner->realm_id = realm_id;
    owner->owner_instance = owner_instance;
    owner->security_owner_instance = security_owner_instance;
    owner->discovery_lease_us = discovery_lease_us;
    owner->capability_lease_us = capability_lease_us;
    owner->state_lock = *state_lock;
    state_lock->leave(state_lock->context);
    return UCN_OK;
}

ucn_result_t ucn_i_capability_summary_ingest(
    ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *authenticated,
    const ucn_i_capability_summary_t *summary,
    uint64_t now_us,
    ucn_i_capability_summary_result_t *result_out)
{
    uint16_t index;
    ucn_i_capability_ref_t reference;
    ucn_i_capability_summary_result_t disposition =
        UCN_I_CAPABILITY_SUMMARY_QUERY_REQUIRED;
    ucn_result_t result;

    if (owner == NULL || authenticated == NULL ||
        !summary_valid(summary) || result_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), authenticated,
                             sizeof(*authenticated)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), summary,
                             sizeof(*summary)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(authenticated, sizeof(*authenticated),
                             result_out, sizeof(*result_out)) ||
        ucn_i_ranges_overlap(summary, sizeof(*summary),
                             result_out, sizeof(*result_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!authenticated_valid(owner, authenticated, now_us) ||
        summary->link_instance_generation != authenticated->link_generation) {
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    reference = make_ref(owner, authenticated);
    for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
        const ucn_i_cached_capability_t *value = &owner->peers[index].value;

        if (owner->peers[index].occupied != 0U &&
            ref_equal(&value->reference, &reference) &&
            value->record.capability_generation ==
                summary->capability_generation &&
            memcmp(value->digest, summary->digest, 16U) == 0 &&
            now_us < value->discovery_deadline_us &&
            now_us < value->capability_deadline_us) {
            disposition = UCN_I_CAPABILITY_SUMMARY_MATCHED;
            break;
        }
    }
    *result_out = disposition;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_capability_advertise_ingest(
    ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *authenticated,
    const ucn_i_capability_record_t *record,
    uint64_t now_us,
    ucn_i_capability_ref_t *reference_out)
{
    uint16_t index;
    uint16_t empty = UINT16_MAX;
    ucn_i_capability_ref_t reference;
    uint8_t digest[16];
    uint64_t discovery_deadline = 0U;
    uint64_t capability_deadline = 0U;
    ucn_result_t result;

    if (owner == NULL || authenticated == NULL ||
        !record_valid(record) || reference_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), authenticated,
                             sizeof(*authenticated)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), record,
                             sizeof(*record)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), reference_out,
                             sizeof(*reference_out)) ||
        ucn_i_ranges_overlap(authenticated, sizeof(*authenticated),
                             reference_out, sizeof(*reference_out)) ||
        ucn_i_ranges_overlap(record, sizeof(*record),
                             reference_out, sizeof(*reference_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_capability_digest(record, digest);
    if (result != UCN_OK) {
        return result;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (!authenticated_valid(owner, authenticated, now_us) ||
        record->link.link_instance_generation !=
            authenticated->link_generation) {
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    reference = make_ref(owner, authenticated);
    for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
        if (owner->peers[index].occupied == 0U) {
            if (empty == UINT16_MAX) {
                empty = index;
            }
            continue;
        }
        if (ref_equal(&owner->peers[index].value.reference, &reference)) {
            const ucn_i_cached_capability_t *previous =
                &owner->peers[index].value;

            if (record->capability_generation ==
                previous->record.capability_generation) {
                if (memcmp(digest, previous->digest, 16U) != 0) {
                    owner_unlock(owner);
                    return UCN_ERR_SECURITY;
                }
                *reference_out = reference;
                owner_unlock(owner);
                return UCN_OK;
            }
            if (previous->record.capability_generation == UINT32_MAX ||
                record->capability_generation !=
                    previous->record.capability_generation + 1U) {
                owner_unlock(owner);
                return UCN_ERR_REPLAY;
            }
            empty = index;
            break;
        }
        if (memcmp(owner->peers[index].value.reference.principal,
                   reference.principal, sizeof(reference.principal)) == 0) {
            owner_unlock(owner);
            return UCN_ERR_REPLAY;
        }
    }
    if (empty == UINT16_MAX) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    result = ucn_i_deadline_from_duration_us(
        now_us, owner->discovery_lease_us, &discovery_deadline);
    if (result == UCN_OK) {
        result = ucn_i_deadline_from_duration_us(
            now_us, owner->capability_lease_us, &capability_deadline);
    }
    if (result != UCN_OK) {
        owner_unlock(owner);
        return result;
    }
    if (discovery_deadline > authenticated->expires_at_us) {
        discovery_deadline = authenticated->expires_at_us;
    }
    if (capability_deadline > authenticated->expires_at_us) {
        capability_deadline = authenticated->expires_at_us;
    }
    if (discovery_deadline <= now_us || capability_deadline <= now_us) {
        owner_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    /* All failure points are complete before this fixed Owner-slot commit.
     * Avoid a full cached-record automatic object on the MCU task stack. */
    memset(&owner->peers[empty].value, 0,
           sizeof(owner->peers[empty].value));
    owner->peers[empty].value.reference = reference;
    owner->peers[empty].value.record = *record;
    owner->peers[empty].value.discovery_deadline_us = discovery_deadline;
    owner->peers[empty].value.capability_deadline_us = capability_deadline;
    memcpy(owner->peers[empty].value.digest, digest,
           sizeof(owner->peers[empty].value.digest));
    owner->peers[empty].occupied = 1U;
    *reference_out = reference;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_capability_get(
    ucn_i_capability_owner_t *owner,
    const ucn_i_capability_ref_t *reference,
    uint64_t now_us,
    ucn_i_cached_capability_t *value_out)
{
    uint16_t index;
    ucn_result_t result;

    if (owner == NULL || reference == NULL || value_out == NULL ||
        now_us == 0U || reference->reserved_zero != 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), reference,
                             sizeof(*reference)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), value_out,
                             sizeof(*value_out)) ||
        ucn_i_ranges_overlap(reference, sizeof(*reference), value_out,
                             sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
        const ucn_i_cached_capability_t *value = &owner->peers[index].value;

        if (owner->peers[index].occupied != 0U &&
            ref_equal(&value->reference, reference)) {
            if (now_us >= value->discovery_deadline_us ||
                now_us >= value->capability_deadline_us) {
                owner_unlock(owner);
                return UCN_ERR_TIMEOUT;
            }
            *value_out = *value;
            owner_unlock(owner);
            return UCN_OK;
        }
    }
    owner_unlock(owner);
    return UCN_ERR_NOT_FOUND;
}

ucn_result_t ucn_i_capability_invalidate_session(
    ucn_i_capability_owner_t *owner,
    const ucn_i_capability_ref_t *reference)
{
    uint16_t index;
    ucn_result_t result;

    if (owner == NULL || reference == NULL || reference->reserved_zero != 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), reference,
                             sizeof(*reference))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
        if (owner->peers[index].occupied != 0U &&
            ref_equal(&owner->peers[index].value.reference, reference)) {
            memset(&owner->peers[index], 0, sizeof(owner->peers[index]));
            owner_unlock(owner);
            return UCN_OK;
        }
    }
    owner_unlock(owner);
    return UCN_ERR_NOT_FOUND;
}

ucn_result_t ucn_i_capability_maintain(
    ucn_i_capability_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    ucn_i_capability_ref_t *expired_reference_out,
    uint16_t *inspected_out,
    uint8_t *expired_valid_out)
{
    ucn_i_capability_ref_t expired_reference;
    uint16_t inspected = 0U;
    uint8_t expired_valid = 0U;
    ucn_result_t result;

    if (owner == NULL || now_us == 0U || budget == 0U ||
        expired_reference_out == NULL || inspected_out == NULL ||
        expired_valid_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), expired_reference_out,
                             sizeof(*expired_reference_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), expired_valid_out,
                             sizeof(*expired_valid_out)) ||
        ucn_i_ranges_overlap(expired_reference_out,
                             sizeof(*expired_reference_out), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(expired_reference_out,
                             sizeof(*expired_reference_out),
                             expired_valid_out,
                             sizeof(*expired_valid_out)) ||
        ucn_i_ranges_overlap(inspected_out, sizeof(*inspected_out),
                             expired_valid_out,
                             sizeof(*expired_valid_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&expired_reference, 0, sizeof(expired_reference));
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    while (inspected < budget &&
           inspected < UCN_I_CAPABILITY_PEER_COUNT) {
        uint16_t index = owner->maintenance_cursor;

        owner->maintenance_cursor = (uint16_t)(
            (index + 1U) % UCN_I_CAPABILITY_PEER_COUNT);
        inspected++;
        if (owner->peers[index].occupied != 0U &&
            (now_us >= owner->peers[index].value.discovery_deadline_us ||
             now_us >= owner->peers[index].value.capability_deadline_us)) {
            expired_reference = owner->peers[index].value.reference;
            memset(&owner->peers[index], 0, sizeof(owner->peers[index]));
            expired_valid = 1U;
            break;
        }
    }
    *expired_reference_out = expired_reference;
    *inspected_out = inspected;
    *expired_valid_out = expired_valid;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_capability_owner_destroy(
    ucn_i_capability_owner_t *owner)
{
    uint16_t index;
    ucn_i_lock_ops_t lock;
    ucn_result_t result;

    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
        if (owner->peers[index].occupied != 0U) {
            owner_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    lock = owner->state_lock;
    memset(owner, 0, sizeof(*owner));
    lock.leave(lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_profile_select_build(
    const ucn_i_capability_record_t *local,
    const uint8_t local_digest[16],
    const ucn_i_capability_record_t *peer,
    const uint8_t peer_digest[16],
    const ucn_i_profile_requirements_t *requirements,
    ucn_i_profile_select_t *select_out)
{
    ucn_i_profile_select_t selected;
    uint8_t expected_local[16];
    uint8_t expected_peer[16];

    if (!record_valid(local) || !record_valid(peer) || local_digest == NULL ||
        peer_digest == NULL || requirements == NULL || select_out == NULL ||
        requirements->reserved_zero != 0U ||
        requirements->minimum_message_class > UCN_I_MESSAGE_T8K ||
        requirements->minimum_rx_window == 0U ||
        requirements->minimum_concurrent_transfers == 0U ||
        ucn_i_ranges_overlap(local, sizeof(*local), select_out,
                             sizeof(*select_out)) ||
        ucn_i_ranges_overlap(peer, sizeof(*peer), select_out,
                             sizeof(*select_out)) ||
        ucn_i_ranges_overlap(local_digest, 16U, select_out,
                             sizeof(*select_out)) ||
        ucn_i_ranges_overlap(peer_digest, 16U, select_out,
                             sizeof(*select_out)) ||
        ucn_i_ranges_overlap(requirements, sizeof(*requirements), select_out,
                             sizeof(*select_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (ucn_i_capability_digest(local, expected_local) != UCN_OK ||
        ucn_i_capability_digest(peer, expected_peer) != UCN_OK ||
        memcmp(expected_local, local_digest, 16U) != 0 ||
        memcmp(expected_peer, peer_digest, 16U) != 0) {
        return UCN_ERR_SECURITY;
    }
    memset(&selected, 0, sizeof(selected));
    selected.local_generation = local->capability_generation;
    selected.peer_generation = peer->capability_generation;
    selected.feature_bits = local->peer.feature_bits & peer->peer.feature_bits;
    selected.hop_suite_bits =
        local->peer.hop_suite_bits & peer->peer.hop_suite_bits;
    selected.e2e_suite_bits =
        local->peer.e2e_suite_bits & peer->peer.e2e_suite_bits;
    selected.realtime_mode_bits =
        local->peer.realtime_mode_bits & peer->peer.realtime_mode_bits;
    selected.max_message_class =
        local->peer.max_message_class < peer->peer.max_message_class ?
            local->peer.max_message_class : peer->peer.max_message_class;
    selected.max_rx_window =
        local->peer.max_rx_window < peer->peer.max_rx_window ?
            local->peer.max_rx_window : peer->peer.max_rx_window;
    selected.max_concurrent_transfers =
        local->peer.max_concurrent_transfers <
                peer->peer.max_concurrent_transfers ?
            local->peer.max_concurrent_transfers :
            peer->peer.max_concurrent_transfers;
    if ((selected.feature_bits & requirements->required_feature_bits) !=
            requirements->required_feature_bits ||
        (selected.hop_suite_bits & requirements->required_hop_suite_bits) !=
            requirements->required_hop_suite_bits ||
        (selected.e2e_suite_bits & requirements->required_e2e_suite_bits) !=
            requirements->required_e2e_suite_bits ||
        selected.max_message_class < requirements->minimum_message_class ||
        selected.max_rx_window < requirements->minimum_rx_window ||
        selected.max_concurrent_transfers <
            requirements->minimum_concurrent_transfers ||
        (selected.realtime_mode_bits &
         requirements->required_realtime_mode_bits) !=
            requirements->required_realtime_mode_bits) {
        return UCN_ERR_UNSUPPORTED;
    }
    memcpy(selected.local_digest, local_digest, 16U);
    memcpy(selected.peer_digest, peer_digest, 16U);
    *select_out = selected;
    return UCN_OK;
}

ucn_result_t ucn_i_profile_ack_verify_exact(
    const ucn_i_profile_ack_t *ack,
    const ucn_i_profile_select_t *expected,
    const uint8_t expected_transcript_digest[16])
{
    if (ack == NULL || expected == NULL ||
        expected_transcript_digest == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!bytes_nonzero(expected_transcript_digest, 16U) ||
        memcmp(&ack->selected, expected, sizeof(*expected)) != 0 ||
        memcmp(ack->transcript_digest, expected_transcript_digest, 16U) != 0) {
        return UCN_ERR_SECURITY;
    }
    return UCN_OK;
}

static bool candidate_supported(const ucn_i_effective_intent_t *intent,
                                const ucn_i_contract_candidate_t *candidate)
{
    return candidate->reserved_zero == 0U && candidate->contract <= 5U &&
           candidate->origin_security <= 2U && candidate->hop_profile <= 3U &&
           candidate->reliable <= 1U && candidate->realtime <= 1U &&
           candidate->pinned_path <= 1U &&
           candidate->missing_dependency <= UCN_I_DEPENDENCY_TRANSFER &&
           candidate->expected_reuse_count != 0U &&
           (candidate->feature_bits & intent->required_feature_bits) ==
               intent->required_feature_bits &&
           (candidate->feature_bits & intent->forbidden_feature_bits) == 0U &&
           candidate->payload_budget >= intent->payload_bytes &&
           (!intent->reliable_required || candidate->reliable != 0U) &&
           (!intent->realtime_required || candidate->realtime != 0U) &&
           (!intent->pinned_path_required || candidate->pinned_path != 0U) &&
           candidate->origin_security >= intent->security_floor;
}

static bool candidate_better(const ucn_i_contract_candidate_t *candidate,
                             const ucn_i_contract_candidate_t *current)
{
    uint32_t candidate_setup =
        candidate->setup_cost_bytes / candidate->expected_reuse_count;
    uint32_t current_setup =
        current->setup_cost_bytes / current->expected_reuse_count;

    if (candidate->exact_frame_bytes != current->exact_frame_bytes) {
        return candidate->exact_frame_bytes < current->exact_frame_bytes;
    }
    if (candidate_setup != current_setup) {
        return candidate_setup < current_setup;
    }
    if (candidate->contract != current->contract) {
        return candidate->contract < current->contract;
    }
    return candidate->stable_order < current->stable_order;
}

ucn_result_t ucn_i_contract_resolve(
    const ucn_i_effective_intent_t *intent,
    const ucn_i_resource_view_t *resources,
    const ucn_i_contract_candidate_t *candidates,
    size_t candidate_count,
    ucn_i_resolve_result_t *result_out)
{
    ucn_i_resolve_result_t result;
    ucn_i_contract_candidate_t best;
    bool have_best = false;
    bool resource_blocked = false;
    uint8_t first_dependency = 0U;
    size_t index;

    if (intent == NULL || resources == NULL || result_out == NULL ||
        (candidate_count != 0U && candidates == NULL) ||
        (candidate_count > SIZE_MAX / sizeof(*candidates)) ||
        ucn_i_ranges_overlap(intent, sizeof(*intent), result_out,
                             sizeof(*result_out)) ||
        ucn_i_ranges_overlap(resources, sizeof(*resources), result_out,
                             sizeof(*result_out)) ||
        (candidate_count != 0U &&
         ucn_i_ranges_overlap(candidates,
                              candidate_count * sizeof(*candidates),
                              result_out, sizeof(*result_out))) ||
        intent->security_floor > 2U || intent->reliable_required > 1U ||
        intent->realtime_required > 1U ||
        intent->pinned_path_required > 1U || resources->tx_available > 1U ||
        resources->reliable_available > 1U ||
        resources->transfer_available > 1U || resources->reserved_zero != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&result, 0, sizeof(result));
    memset(&best, 0, sizeof(best));
    if ((intent->required_feature_bits & intent->forbidden_feature_bits) != 0U) {
        result.status = UCN_I_RESOLVE_REJECT_POLICY;
        *result_out = result;
        return UCN_OK;
    }
    for (index = 0U; index < candidate_count; ++index) {
        const ucn_i_contract_candidate_t *candidate = &candidates[index];

        if (!candidate_supported(intent, candidate)) {
            continue;
        }
        if (candidate->missing_dependency != 0U) {
            if (first_dependency == 0U ||
                candidate->missing_dependency < first_dependency) {
                first_dependency = candidate->missing_dependency;
            }
            continue;
        }
        if (resources->tx_available == 0U ||
            (intent->reliable_required &&
             resources->reliable_available == 0U)) {
            resource_blocked = true;
            continue;
        }
        if (!have_best || candidate_better(candidate, &best)) {
            best = *candidate;
            have_best = true;
        }
    }
    if (have_best) {
        result.status = UCN_I_RESOLVE_READY;
        result.candidate = best;
    } else if (first_dependency != 0U) {
        result.status = UCN_I_RESOLVE_NEED_DEPENDENCY;
        result.dependency = first_dependency;
    } else if (resource_blocked) {
        result.status = UCN_I_RESOLVE_REJECT_RESOURCE;
    } else {
        result.status = UCN_I_RESOLVE_REJECT_UNSUPPORTED;
    }
    *result_out = result;
    return UCN_OK;
}
