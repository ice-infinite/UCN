#include "../internal/ucn_v6_capability_private.h"

#include <limits.h>
#include <string.h>

#define UCN_V6_CAPABILITY_SCHEMA UINT16_C(1)
#define UCN_V6_CAPABILITY_KNOWN_FEATURES                                 \
    ((uint32_t)(UCN_V6_FEATURE_IDENTITY | UCN_V6_FEATURE_WIRE |         \
                UCN_V6_FEATURE_MESSAGE | UCN_V6_FEATURE_SECURITY |      \
                UCN_V6_FEATURE_ROUTE | UCN_V6_FEATURE_TRANSFER |        \
                UCN_V6_FEATURE_REALTIME | UCN_V6_FEATURE_CLUSTER |      \
                UCN_V6_FEATURE_CAPABILITY))
#define UCN_V6_CAPABILITY_KNOWN_LINK_FLAGS UINT16_C(0x001F)
#define UCN_V6_CAPABILITY_KNOWN_TIMESTAMP_BITS UINT16_C(0x000F)
#define UCN_V6_CAPABILITY_HOP_SUITE_BITS UINT32_C(0x00000002)
#define UCN_V6_CAPABILITY_E2E_SUITE_BITS UINT32_C(0x0000000E)

typedef char ucn_v6_capability_owner_storage_must_fit[
    sizeof(struct ucn_v6_capability_owner) <=
            UCN_V6_CAPABILITY_OWNER_STORAGE_BYTES ? 1 : -1];

static uint16_t read_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t read_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) |
           (uint32_t)input[3];
}

static void write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool digest_is_nonzero(const uint8_t *digest)
{
    return digest != NULL &&
           !bytes_are_zero(digest, UCN_V6_CAPABILITY_DIGEST_BYTES);
}

static bool principal_equal(const ucn_v6_principal_t *left,
                            const ucn_v6_principal_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static bool record_is_valid(const ucn_v6_capability_record_t *record)
{
    uint32_t carrier_overhead;
    if (record == NULL || record->capability_generation == 0U ||
        record->capability_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        record->link.link_instance_generation == 0U ||
        record->link.link_instance_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        record->link.carrier_mtu == 0U ||
        record->link.link_frame_mtu == 0U ||
        record->link.processing_frame_mtu == 0U ||
        record->link.link_frame_mtu > UCN_V6_WIRE_MAX_FRAME_BYTES ||
        record->link.processing_frame_mtu > UCN_V6_WIRE_MAX_FRAME_BYTES ||
        record->link.carrier_max_fragments == 0U ||
        record->link.nominal_rate_bps == 0U ||
        record->link.hardware_priority_count == 0U ||
        (record->link.link_flags &
         (uint16_t)~UCN_V6_CAPABILITY_KNOWN_LINK_FLAGS) != 0U ||
        (record->link.link_flags &
         (UCN_V6_LINK_BROADCAST | UCN_V6_LINK_UNICAST)) == 0U ||
        (record->link.timestamp_capability_bits &
         (uint16_t)~UCN_V6_CAPABILITY_KNOWN_TIMESTAMP_BITS) != 0U ||
        ((record->link.timestamp_capability_bits == 0U) !=
         (record->link.timestamp_uncertainty_us == 0U)) ||
        record->peer.feature_bits == 0U ||
        (record->peer.feature_bits & ~UCN_V6_CAPABILITY_KNOWN_FEATURES) != 0U ||
        (record->peer.feature_bits &
         (UCN_V6_FEATURE_IDENTITY | UCN_V6_FEATURE_WIRE |
          UCN_V6_FEATURE_SECURITY | UCN_V6_FEATURE_CAPABILITY)) !=
            (UCN_V6_FEATURE_IDENTITY | UCN_V6_FEATURE_WIRE |
             UCN_V6_FEATURE_SECURITY | UCN_V6_FEATURE_CAPABILITY) ||
        record->peer.hop_suite_bits == 0U ||
        (record->peer.hop_suite_bits & ~UCN_V6_CAPABILITY_HOP_SUITE_BITS) != 0U ||
        record->peer.e2e_suite_bits == 0U ||
        (record->peer.e2e_suite_bits & ~UCN_V6_CAPABILITY_E2E_SUITE_BITS) != 0U ||
        (uint32_t)record->peer.max_message_class >
            (uint32_t)UCN_V6_MESSAGE_T8K ||
        record->peer.max_rx_window == 0U ||
        record->peer.max_concurrent_transfers == 0U) {
        return false;
    }
    carrier_overhead = (uint32_t)record->link.carrier_header_bytes +
                       record->link.carrier_padding_bytes +
                       record->link.carrier_crc_bytes +
                       record->link.carrier_tag_bytes;
    return carrier_overhead < record->link.carrier_mtu;
}

static bool owner_is_valid(const ucn_v6_capability_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_CAPABILITY_OWNER_MAGIC &&
           owner->schema == UCN_V6_CAPABILITY_SCHEMA && owner->initialized &&
           owner->canary == UCN_V6_CAPABILITY_OWNER_CANARY &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH;
}

static ucn_v6_result_t checked_deadline(uint64_t now_us,
                                        uint64_t duration_us,
                                        uint64_t *deadline_us)
{
    if (duration_us == 0U || deadline_us == NULL ||
        now_us > UINT64_MAX - duration_us) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *deadline_us = now_us + duration_us;
    return *deadline_us != 0U ? UCN_V6_OK : UCN_V6_ERR_EXHAUSTED;
}

ucn_v6_result_t ucn_v6_capability_record_encode(
    const ucn_v6_capability_record_t *record,
    uint8_t output[UCN_V6_CAPABILITY_RECORD_BYTES])
{
    uint8_t encoded[UCN_V6_CAPABILITY_RECORD_BYTES];
    if (!record_is_valid(record) || output == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    write_u32(&encoded[0], record->capability_generation);
    write_u32(&encoded[4], record->link.link_instance_generation);
    write_u32(&encoded[8], record->link.carrier_mtu);
    write_u32(&encoded[12], record->link.link_frame_mtu);
    write_u32(&encoded[16], record->link.processing_frame_mtu);
    write_u16(&encoded[20], record->link.carrier_header_bytes);
    write_u16(&encoded[22], record->link.carrier_padding_bytes);
    write_u16(&encoded[24], record->link.carrier_crc_bytes);
    write_u16(&encoded[26], record->link.carrier_tag_bytes);
    write_u16(&encoded[28], record->link.carrier_max_fragments);
    write_u16(&encoded[30], record->link.link_flags);
    write_u32(&encoded[32], record->link.nominal_rate_bps);
    write_u32(&encoded[36], record->link.timestamp_uncertainty_us);
    write_u32(&encoded[40], record->peer.feature_bits);
    write_u32(&encoded[44], record->peer.hop_suite_bits);
    write_u32(&encoded[48], record->peer.e2e_suite_bits);
    write_u16(&encoded[52], record->peer.max_rx_window);
    write_u16(&encoded[54], record->peer.max_concurrent_transfers);
    encoded[56] = (uint8_t)record->peer.max_message_class;
    encoded[57] = record->link.hardware_priority_count;
    write_u16(&encoded[58], record->link.timestamp_capability_bits);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_record_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_capability_record_t *record)
{
    ucn_v6_capability_record_t decoded;
    if (input == NULL || record == NULL ||
        input_length != UCN_V6_CAPABILITY_RECORD_BYTES ||
        !bytes_are_zero(&input[60], 4U)) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.capability_generation = read_u32(&input[0]);
    decoded.link.link_instance_generation = read_u32(&input[4]);
    decoded.link.carrier_mtu = read_u32(&input[8]);
    decoded.link.link_frame_mtu = read_u32(&input[12]);
    decoded.link.processing_frame_mtu = read_u32(&input[16]);
    decoded.link.carrier_header_bytes = read_u16(&input[20]);
    decoded.link.carrier_padding_bytes = read_u16(&input[22]);
    decoded.link.carrier_crc_bytes = read_u16(&input[24]);
    decoded.link.carrier_tag_bytes = read_u16(&input[26]);
    decoded.link.carrier_max_fragments = read_u16(&input[28]);
    decoded.link.link_flags = read_u16(&input[30]);
    decoded.link.nominal_rate_bps = read_u32(&input[32]);
    decoded.link.timestamp_uncertainty_us = read_u32(&input[36]);
    decoded.peer.feature_bits = read_u32(&input[40]);
    decoded.peer.hop_suite_bits = read_u32(&input[44]);
    decoded.peer.e2e_suite_bits = read_u32(&input[48]);
    decoded.peer.max_rx_window = read_u16(&input[52]);
    decoded.peer.max_concurrent_transfers = read_u16(&input[54]);
    decoded.peer.max_message_class = (ucn_v6_message_class_t)input[56];
    decoded.link.hardware_priority_count = input[57];
    decoded.link.timestamp_capability_bits = read_u16(&input[58]);
    if (!record_is_valid(&decoded)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *record = decoded;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_digest(
    const ucn_v6_capability_record_t *record,
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES])
{
    uint8_t encoded[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint8_t salted[UCN_V6_CAPABILITY_RECORD_BYTES + 1U];
    uint8_t computed[UCN_V6_CAPABILITY_DIGEST_BYTES];
    size_t index;
    if (digest == NULL ||
        ucn_v6_capability_record_encode(record, encoded) != UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memcpy(&salted[1], encoded, sizeof(encoded));
    for (index = 0U; index < 4U; ++index) {
        uint32_t crc;
        salted[0] = (uint8_t)(0xA5U + index * 0x17U);
        crc = ucn_v6_crc32c(salted, sizeof(salted));
        write_u32(&computed[index * 4U], crc);
    }
    if (!digest_is_nonzero(computed)) {
        return UCN_V6_ERR_STATE;
    }
    memcpy(digest, computed, sizeof(computed));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_summary_encode(
    const ucn_v6_capability_summary_t *summary,
    uint8_t output[UCN_V6_CAPABILITY_HELLO_BYTES])
{
    uint8_t encoded[UCN_V6_CAPABILITY_HELLO_BYTES];
    if (summary == NULL || output == NULL ||
        summary->capability_generation == 0U ||
        summary->capability_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        summary->link_instance_generation == 0U ||
        summary->link_instance_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        !digest_is_nonzero(summary->digest)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    write_u32(&encoded[0], summary->capability_generation);
    write_u32(&encoded[4], summary->link_instance_generation);
    memcpy(&encoded[8], summary->digest, UCN_V6_CAPABILITY_DIGEST_BYTES);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_summary_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_capability_summary_t *summary)
{
    ucn_v6_capability_summary_t decoded;
    if (input == NULL || summary == NULL ||
        input_length != UCN_V6_CAPABILITY_HELLO_BYTES) {
        return UCN_V6_ERR_MALFORMED;
    }
    decoded.capability_generation = read_u32(&input[0]);
    decoded.link_instance_generation = read_u32(&input[4]);
    memcpy(decoded.digest, &input[8], sizeof(decoded.digest));
    if (decoded.capability_generation == 0U ||
        decoded.capability_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        decoded.link_instance_generation == 0U ||
        decoded.link_instance_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        !digest_is_nonzero(decoded.digest)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *summary = decoded;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_query_encode(
    const ucn_v6_capability_query_t *query,
    uint8_t output[UCN_V6_CAPABILITY_QUERY_BYTES])
{
    uint8_t encoded[UCN_V6_CAPABILITY_QUERY_BYTES];
    if (query == NULL || output == NULL ||
        query->requested_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        ((query->requested_generation == 0U) !=
         bytes_are_zero(query->known_digest, sizeof(query->known_digest)))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    write_u32(&encoded[0], query->requested_generation);
    memcpy(&encoded[4], query->known_digest, sizeof(query->known_digest));
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_query_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_capability_query_t *query)
{
    ucn_v6_capability_query_t decoded;
    if (input == NULL || query == NULL ||
        input_length != UCN_V6_CAPABILITY_QUERY_BYTES) {
        return UCN_V6_ERR_MALFORMED;
    }
    decoded.requested_generation = read_u32(&input[0]);
    memcpy(decoded.known_digest, &input[4], sizeof(decoded.known_digest));
    if (decoded.requested_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        ((decoded.requested_generation == 0U) !=
         bytes_are_zero(decoded.known_digest,
                        sizeof(decoded.known_digest)))) {
        return UCN_V6_ERR_MALFORMED;
    }
    *query = decoded;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_capability_record_t *local_record,
    uint64_t local_capability_lease_us,
    uint64_t discovery_lease_us,
    ucn_v6_capability_owner_t **owner_out)
{
    ucn_v6_capability_owner_t *owner;
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    if (owner_out == NULL || local_capability_lease_us == 0U ||
        discovery_lease_us == 0U ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_storage_validate(storage, storage_bytes, sizeof(*owner),
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK ||
        ucn_v6_capability_digest(local_record, digest) != UCN_V6_OK) {
        return UCN_V6_ERR_CONFIG;
    }
    owner = (ucn_v6_capability_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_CAPABILITY_OWNER_MAGIC;
    owner->schema = UCN_V6_CAPABILITY_SCHEMA;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->local_record = *local_record;
    memcpy(owner->local_digest, digest, sizeof(digest));
    owner->capability_lease_us = local_capability_lease_us;
    owner->discovery_lease_us = discovery_lease_us;
    owner->initialized = true;
    owner->canary = UCN_V6_CAPABILITY_OWNER_CANARY;
    *owner_out = owner;
    return UCN_V6_OK;
}

static bool frame_matches_payload(const ucn_v6_security_open_result_t *opened,
                                  uint16_t opcode,
                                  const uint8_t *payload,
                                  size_t payload_length)
{
    return opened != NULL && opened->hop_authenticated &&
           !opened->group_discovery_only &&
           ucn_v6_principal_is_valid(&opened->authenticated_principal) &&
           opened->frame.frame_type == UCN_V6_FRAME_CONTROL &&
           opened->frame.protocol_opcode == opcode &&
           opened->frame.flags == (UCN_V6_FLAG_PEER_HOP_CONTEXT |
                                   UCN_V6_FLAG_PROTOCOL_CONTEXT) &&
           opened->frame.payload_length == payload_length &&
           (payload_length == 0U ||
            (opened->frame.payload != NULL && payload != NULL &&
             memcmp(opened->frame.payload, payload, payload_length) == 0));
}

static ucn_v6_cached_peer_capability_t *find_peer(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_principal_t *principal,
    bool allow_empty)
{
    ucn_v6_cached_peer_capability_t *empty = NULL;
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        ucn_v6_cached_peer_capability_t *peer = &owner->peers[index];
        if (peer->valid && principal_equal(&peer->principal, principal)) {
            return peer;
        }
        if (!peer->valid && empty == NULL) {
            empty = peer;
        }
    }
    return allow_empty ? empty : NULL;
}

static void invalidate_paths_for_principal(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_principal_t *principal)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid &&
            principal_equal(&owner->paths[index].destination_principal,
                            principal)) {
            memset(&owner->paths[index], 0, sizeof(owner->paths[index]));
        }
    }
}

ucn_v6_result_t ucn_v6_capability_ingest_peer_hello(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_summary_t *summary,
    ucn_v6_hello_disposition_t *disposition)
{
    uint8_t payload[UCN_V6_CAPABILITY_HELLO_BYTES];
    ucn_v6_cached_peer_capability_t *peer;
    uint64_t deadline;
    if (!owner_is_valid(owner) || owner->faulted || summary == NULL ||
        disposition == NULL || ingress_link_id == 0U ||
        ingress_link_generation == 0U ||
        ingress_link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        ucn_v6_capability_summary_encode(summary, payload) != UCN_V6_OK ||
        !frame_matches_payload(opened, UCN_V6_PROTOCOL_OPCODE_PEER_HELLO,
                               payload, sizeof(payload)) ||
        opened->frame.session_generation == 0U ||
        opened->frame.source_binding_generation == 0U ||
        summary->link_instance_generation != ingress_link_generation ||
        checked_deadline(now_us, owner->discovery_lease_us,
                         &deadline) != UCN_V6_OK) {
        if (owner_is_valid(owner)) {
            increment_saturated(&owner->stats.rejected_authentication);
        }
        return UCN_V6_ERR_SECURITY;
    }
    peer = find_peer(owner, &opened->authenticated_principal, false);
    if (peer == NULL ||
        peer->binding.realm_id != opened->frame.realm_id ||
        peer->binding.node_address != opened->frame.source_address ||
        peer->binding.binding_generation !=
            opened->frame.source_binding_generation ||
        peer->session_generation != opened->frame.session_generation ||
        peer->record.link.link_instance_generation !=
            ingress_link_generation ||
        peer->record.capability_generation !=
            summary->capability_generation ||
        memcmp(peer->digest, summary->digest, sizeof(peer->digest)) != 0 ||
        now_us >= peer->capability_deadline_us) {
        *disposition = UCN_V6_HELLO_QUERY_REQUIRED;
        return UCN_V6_OK;
    }
    peer->ingress_link_id = ingress_link_id;
    peer->discovery_deadline_us = deadline;
    *disposition = UCN_V6_HELLO_MATCHED;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_ingest_advertise(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_record_t *record)
{
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    ucn_v6_cached_peer_capability_t *peer;
    uint64_t discovery_deadline;
    uint64_t capability_deadline;
    uint32_t expected_generation;
    if (!owner_is_valid(owner) || owner->faulted || ingress_link_id == 0U ||
        ingress_link_generation == 0U ||
        ingress_link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        ucn_v6_capability_record_encode(record, payload) != UCN_V6_OK ||
        ucn_v6_capability_digest(record, digest) != UCN_V6_OK ||
        !frame_matches_payload(
            opened, UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
            payload, sizeof(payload)) ||
        record->link.link_instance_generation != ingress_link_generation ||
        checked_deadline(now_us, owner->discovery_lease_us,
                         &discovery_deadline) != UCN_V6_OK ||
        checked_deadline(now_us, owner->capability_lease_us,
                         &capability_deadline) != UCN_V6_OK) {
        if (owner_is_valid(owner)) {
            increment_saturated(&owner->stats.rejected_authentication);
        }
        return UCN_V6_ERR_SECURITY;
    }
    peer = find_peer(owner, &opened->authenticated_principal, false);
    if (peer != NULL) {
        bool same_domain = peer->binding.realm_id == opened->frame.realm_id &&
            peer->binding.node_address == opened->frame.source_address &&
            peer->binding.binding_generation ==
                opened->frame.source_binding_generation &&
            peer->session_generation == opened->frame.session_generation &&
            peer->record.link.link_instance_generation ==
                ingress_link_generation;
        if (!same_domain) {
            memset(peer, 0, sizeof(*peer));
            peer = NULL;
        } else if (peer->record.capability_generation ==
                   record->capability_generation) {
            if (memcmp(peer->digest, digest, sizeof(peer->digest)) != 0) {
                return UCN_V6_ERR_REPLAY;
            }
            peer->ingress_link_id = ingress_link_id;
            peer->discovery_deadline_us = discovery_deadline;
            peer->capability_deadline_us = capability_deadline;
            return UCN_V6_OK;
        } else {
            if (ucn_v6_serial_checked_next(
                    peer->record.capability_generation,
                    &expected_generation) != UCN_V6_OK ||
                expected_generation != record->capability_generation) {
                return UCN_V6_ERR_REPLAY;
            }
        }
    }
    if (peer == NULL) {
        peer = find_peer(owner, &opened->authenticated_principal, true);
    }
    if (peer == NULL) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    invalidate_paths_for_principal(owner, &opened->authenticated_principal);
    memset(peer, 0, sizeof(*peer));
    peer->valid = true;
    peer->principal = opened->authenticated_principal;
    peer->binding.realm_id = opened->frame.realm_id;
    peer->binding.node_address = opened->frame.source_address;
    peer->binding.binding_generation =
        opened->frame.source_binding_generation;
    peer->session_generation = opened->frame.session_generation;
    peer->ingress_link_id = ingress_link_id;
    peer->record = *record;
    memcpy(peer->digest, digest, sizeof(peer->digest));
    peer->discovery_deadline_us = discovery_deadline;
    peer->capability_deadline_us = capability_deadline;
    return UCN_V6_OK;
}

static ucn_v6_group_discovery_hint_t *find_hint(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_binding_key_t *binding,
    uint32_t session_generation,
    bool allow_empty)
{
    ucn_v6_group_discovery_hint_t *empty = NULL;
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS; ++index) {
        ucn_v6_group_discovery_hint_t *hint = &owner->hints[index];
        if (hint->occupied &&
            ucn_v6_binding_key_equal(&hint->claimed_binding, binding) &&
            hint->claimed_session_generation == session_generation) {
            return hint;
        }
        if (!hint->occupied && empty == NULL) {
            empty = hint;
        }
    }
    return allow_empty ? empty : NULL;
}

static ucn_v6_group_hint_link_budget_t *find_hint_link(
    ucn_v6_capability_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation,
    bool allow_empty)
{
    ucn_v6_group_hint_link_budget_t *empty = NULL;
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS; ++index) {
        ucn_v6_group_hint_link_budget_t *link = &owner->hint_links[index];
        if (link->occupied && link->ingress_link_id == link_id &&
            link->ingress_link_generation == link_generation) {
            return link;
        }
        if (!link->occupied && empty == NULL) {
            empty = link;
        }
    }
    return allow_empty ? empty : NULL;
}

static ucn_v6_group_hint_group_budget_t *find_hint_group(
    ucn_v6_capability_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation,
    uint32_t group_id,
    uint32_t group_generation,
    bool allow_empty)
{
    ucn_v6_group_hint_group_budget_t *empty = NULL;
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS; ++index) {
        ucn_v6_group_hint_group_budget_t *group = &owner->hint_groups[index];
        if (group->occupied && group->ingress_link_id == link_id &&
            group->ingress_link_generation == link_generation &&
            group->group_id == group_id &&
            group->group_generation == group_generation) {
            return group;
        }
        if (!group->occupied && empty == NULL) {
            empty = group;
        }
    }
    return allow_empty ? empty : NULL;
}

static void preview_link_budget(
    const ucn_v6_group_hint_link_budget_t *current,
    uint16_t link_id,
    uint32_t link_generation,
    uint64_t now_us,
    ucn_v6_group_hint_link_budget_t *next)
{
    uint64_t elapsed;
    uint64_t refill;
    if (!current->occupied) {
        memset(next, 0, sizeof(*next));
        next->occupied = true;
        next->ingress_link_id = link_id;
        next->ingress_link_generation = link_generation;
        next->tokens = UCN_V6_GROUP_HINTS_PER_LINK;
        next->last_refill_us = now_us;
        return;
    }
    *next = *current;
    if (now_us <= next->last_refill_us) {
        return;
    }
    elapsed = now_us - next->last_refill_us;
    refill = elapsed / UCN_V6_GROUP_HINT_TIMEOUT_US;
    if (refill != 0U) {
        uint64_t total = (uint64_t)next->tokens + refill;
        next->tokens = (uint8_t)(total > UCN_V6_GROUP_HINTS_PER_LINK ?
                                     UCN_V6_GROUP_HINTS_PER_LINK : total);
        next->last_refill_us += refill * UCN_V6_GROUP_HINT_TIMEOUT_US;
    }
}

static void preview_group_budget(
    const ucn_v6_group_hint_group_budget_t *current,
    uint16_t link_id,
    uint32_t link_generation,
    uint32_t group_id,
    uint32_t group_generation,
    uint64_t now_us,
    ucn_v6_group_hint_group_budget_t *next)
{
    uint64_t elapsed;
    uint64_t refill;
    if (!current->occupied) {
        memset(next, 0, sizeof(*next));
        next->occupied = true;
        next->ingress_link_id = link_id;
        next->ingress_link_generation = link_generation;
        next->group_id = group_id;
        next->group_generation = group_generation;
        next->tokens = UCN_V6_GROUP_HINTS_PER_LINK;
        next->last_refill_us = now_us;
        return;
    }
    *next = *current;
    if (now_us <= next->last_refill_us) {
        return;
    }
    elapsed = now_us - next->last_refill_us;
    refill = elapsed / UCN_V6_GROUP_HINT_TIMEOUT_US;
    if (refill != 0U) {
        uint64_t total = (uint64_t)next->tokens + refill;
        next->tokens = (uint8_t)(total > UCN_V6_GROUP_HINTS_PER_LINK ?
                                     UCN_V6_GROUP_HINTS_PER_LINK : total);
        next->last_refill_us += refill * UCN_V6_GROUP_HINT_TIMEOUT_US;
    }
}

ucn_v6_result_t ucn_v6_capability_ingest_group_hello_hint(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    const ucn_v6_security_open_result_t *opened)
{
    ucn_v6_binding_key_t binding;
    ucn_v6_group_discovery_hint_t *hint;
    ucn_v6_group_hint_link_budget_t *link;
    ucn_v6_group_hint_group_budget_t *group;
    ucn_v6_group_hint_link_budget_t next_link;
    ucn_v6_group_hint_group_budget_t next_group;
    uint64_t deadline;
    if (!owner_is_valid(owner) || owner->faulted || opened == NULL ||
        ingress_link_id == 0U || ingress_link_generation == 0U ||
        ingress_link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        !opened->group_discovery_only || opened->hop_authenticated ||
        opened->endpoint_authorized ||
        opened->frame.frame_type != UCN_V6_FRAME_CONTROL ||
        opened->frame.protocol_opcode != UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO ||
        opened->frame.group.group_id == 0U ||
        opened->frame.group.group_generation == 0U ||
        opened->frame.group.group_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_SECURITY;
    }
    binding.realm_id = opened->frame.realm_id;
    binding.node_address = opened->frame.source_address;
    binding.binding_generation = opened->frame.source_binding_generation;
    if (!ucn_v6_binding_key_is_valid(&binding) ||
        opened->frame.session_generation == 0U) {
        return UCN_V6_ERR_SECURITY;
    }
    hint = find_hint(owner, &binding, opened->frame.session_generation, false);
    if (hint != NULL) {
        return now_us < hint->deadline_us ? UCN_V6_OK : UCN_V6_ERR_TIMEOUT;
    }
    link = find_hint_link(owner, ingress_link_id, ingress_link_generation, true);
    group = find_hint_group(owner, ingress_link_id,
                            ingress_link_generation,
                            opened->frame.group.group_id,
                            opened->frame.group.group_generation, true);
    if (link == NULL || group == NULL) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    preview_link_budget(link, ingress_link_id, ingress_link_generation,
                        now_us, &next_link);
    preview_group_budget(group, ingress_link_id, ingress_link_generation,
                         opened->frame.group.group_id,
                         opened->frame.group.group_generation,
                         now_us, &next_group);
    if (next_link.tokens == 0U || next_group.tokens == 0U) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    hint = find_hint(owner, &binding, opened->frame.session_generation, true);
    if (hint == NULL ||
        checked_deadline(now_us, UCN_V6_GROUP_HINT_TIMEOUT_US,
                         &deadline) != UCN_V6_OK) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    --next_link.tokens;
    --next_group.tokens;
    *link = next_link;
    *group = next_group;
    memset(hint, 0, sizeof(*hint));
    hint->occupied = true;
    hint->ingress_link_id = ingress_link_id;
    hint->ingress_link_generation = ingress_link_generation;
    hint->group_id = opened->frame.group.group_id;
    hint->group_generation = opened->frame.group.group_generation;
    hint->claimed_binding = binding;
    hint->claimed_session_generation = opened->frame.session_generation;
    hint->deadline_us = deadline;
    return UCN_V6_OK;
}

static uint32_t minimum_u32(uint32_t left, uint32_t right)
{
    return left < right ? left : right;
}

static uint16_t minimum_u16(uint16_t left, uint16_t right)
{
    return left < right ? left : right;
}

ucn_v6_result_t ucn_v6_capability_derive_path(
    const ucn_v6_path_budget_request_t *request,
    ucn_v6_path_capability_t *path)
{
    ucn_v6_path_capability_t derived;
    ucn_v6_frame_t frame;
    size_t overhead;
    size_t index;
    uint64_t uncertainty = 0U;
    if (request == NULL || path == NULL || request->hops == NULL ||
        request->hop_count == 0U || request->frame_contract == NULL ||
        request->frame_contract->payload != NULL ||
        request->frame_contract->payload_length != 0U ||
        !ucn_v6_principal_is_valid(&request->destination_principal) ||
        !ucn_v6_binding_key_is_valid(&request->destination_binding) ||
        request->session_generation == 0U ||
        request->destination_link_instance_generation == 0U ||
        !digest_is_nonzero(request->destination_capability_digest) ||
        request->route_generation == 0U || request->path_id == 0U ||
        request->path_generation == 0U || request->deadline_us == 0U ||
        request->path_policy_frame_mtu == 0U ||
        request->required_hop_suite_bits == 0U ||
        request->required_e2e_suite_bits == 0U ||
        request->policy_max_window == 0U ||
        request->policy_max_concurrency == 0U ||
        (uint32_t)request->policy_max_message_class >
            (uint32_t)UCN_V6_MESSAGE_T8K) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&derived, 0, sizeof(derived));
    derived.valid = true;
    derived.destination_principal = request->destination_principal;
    derived.destination_binding = request->destination_binding;
    derived.session_generation = request->session_generation;
    derived.destination_link_instance_generation =
        request->destination_link_instance_generation;
    memcpy(derived.destination_capability_digest,
           request->destination_capability_digest,
           sizeof(derived.destination_capability_digest));
    derived.route_generation = request->route_generation;
    derived.path_id = request->path_id;
    derived.path_generation = request->path_generation;
    derived.path_frame_mtu = request->path_policy_frame_mtu;
    derived.feature_bits = UINT32_MAX;
    derived.hop_suite_bits = UINT32_MAX;
    derived.e2e_suite_bits = UINT32_MAX;
    derived.max_message_class = request->policy_max_message_class;
    derived.max_window = request->policy_max_window;
    derived.max_concurrency = request->policy_max_concurrency;
    derived.timestamp_capability_bits = UCN_V6_CAPABILITY_KNOWN_TIMESTAMP_BITS;
    derived.deadline_us = request->deadline_us;
    for (index = 0U; index < request->hop_count; ++index) {
        const ucn_v6_capability_record_t *record = &request->hops[index];
        uint32_t frame_limit;
        if (!record_is_valid(record)) {
            return UCN_V6_ERR_ARGUMENT;
        }
        frame_limit = minimum_u32(record->link.link_frame_mtu,
                                  record->link.processing_frame_mtu);
        derived.path_frame_mtu = minimum_u32(derived.path_frame_mtu,
                                              frame_limit);
        derived.feature_bits &= record->peer.feature_bits;
        derived.hop_suite_bits &= record->peer.hop_suite_bits;
        derived.e2e_suite_bits &= record->peer.e2e_suite_bits;
        derived.max_message_class =
            (ucn_v6_message_class_t)minimum_u32(
                (uint32_t)derived.max_message_class,
                (uint32_t)record->peer.max_message_class);
        derived.max_window = minimum_u16(derived.max_window,
                                         record->peer.max_rx_window);
        derived.max_concurrency = minimum_u16(
            derived.max_concurrency,
            record->peer.max_concurrent_transfers);
        derived.timestamp_capability_bits &=
            record->link.timestamp_capability_bits;
        if (record->link.timestamp_capability_bits != 0U) {
            uncertainty += record->link.timestamp_uncertainty_us;
            if (uncertainty > UINT32_MAX) {
                return UCN_V6_ERR_EXHAUSTED;
            }
        }
    }
    derived.timestamp_uncertainty_us =
        derived.timestamp_capability_bits == 0U ? 0U : (uint32_t)uncertainty;
    if ((derived.feature_bits & request->required_feature_bits) !=
            request->required_feature_bits ||
        (derived.hop_suite_bits & request->required_hop_suite_bits) !=
            request->required_hop_suite_bits ||
        (derived.e2e_suite_bits & request->required_e2e_suite_bits) !=
            request->required_e2e_suite_bits ||
        derived.path_frame_mtu == 0U || derived.max_window == 0U ||
        derived.max_concurrency == 0U) {
        return UCN_V6_ERR_ACCESS;
    }
    frame = *request->frame_contract;
    if (ucn_v6_wire_encoded_size(&frame, &overhead) != UCN_V6_OK ||
        overhead >= derived.path_frame_mtu) {
        return UCN_V6_ERR_NO_SPACE;
    }
    derived.payload_budget = derived.path_frame_mtu - (uint32_t)overhead;
    if (request->fragment_header_bytes >= derived.payload_budget) {
        return UCN_V6_ERR_NO_SPACE;
    }
    derived.fragment_data_budget =
        derived.payload_budget - request->fragment_header_bytes;
    *path = derived;
    return UCN_V6_OK;
}

static bool path_key_equal(const ucn_v6_path_capability_t *left,
                           const ucn_v6_path_capability_t *right)
{
    return principal_equal(&left->destination_principal,
                           &right->destination_principal) &&
           left->destination_binding.realm_id ==
               right->destination_binding.realm_id &&
           left->destination_binding.node_address ==
               right->destination_binding.node_address &&
           left->path_id == right->path_id;
}

static bool path_semantically_equal(const ucn_v6_path_capability_t *left,
                                    const ucn_v6_path_capability_t *right)
{
    return left->valid == right->valid &&
           principal_equal(&left->destination_principal,
                           &right->destination_principal) &&
           ucn_v6_binding_key_equal(&left->destination_binding,
                                    &right->destination_binding) &&
           left->session_generation == right->session_generation &&
           left->destination_link_instance_generation ==
               right->destination_link_instance_generation &&
           memcmp(left->destination_capability_digest,
                  right->destination_capability_digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           left->route_generation == right->route_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation &&
           left->path_frame_mtu == right->path_frame_mtu &&
           left->payload_budget == right->payload_budget &&
           left->fragment_data_budget == right->fragment_data_budget &&
           left->feature_bits == right->feature_bits &&
           left->hop_suite_bits == right->hop_suite_bits &&
           left->e2e_suite_bits == right->e2e_suite_bits &&
           left->max_message_class == right->max_message_class &&
           left->max_window == right->max_window &&
           left->max_concurrency == right->max_concurrency &&
           left->timestamp_capability_bits ==
               right->timestamp_capability_bits &&
           left->timestamp_uncertainty_us ==
               right->timestamp_uncertainty_us &&
           left->deadline_us == right->deadline_us;
}

ucn_v6_result_t ucn_v6_capability_install_path(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_path_capability_t *path)
{
    ucn_v6_path_capability_t *target = NULL;
    ucn_v6_cached_peer_capability_t peer;
    size_t index;
    if (!owner_is_valid(owner) || owner->faulted || path == NULL ||
        !path->valid || !ucn_v6_principal_is_valid(
            &path->destination_principal) ||
        !ucn_v6_binding_key_is_valid(&path->destination_binding) ||
        path->session_generation == 0U ||
        path->destination_link_instance_generation == 0U ||
        !digest_is_nonzero(path->destination_capability_digest) ||
        path->route_generation == 0U || path->path_id == 0U ||
        path->path_generation == 0U || path->path_frame_mtu == 0U ||
        path->payload_budget == 0U || path->fragment_data_budget == 0U ||
        path->fragment_data_budget > path->payload_budget ||
        path->deadline_us == 0U || now_us >= path->deadline_us) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (ucn_v6_capability_copy_peer(
            owner, now_us, &path->destination_principal,
            &path->destination_binding, path->session_generation,
            path->destination_link_instance_generation, &peer) != UCN_V6_OK ||
        memcmp(peer.digest, path->destination_capability_digest,
               sizeof(peer.digest)) != 0) {
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid &&
            path_key_equal(&owner->paths[index], path)) {
            target = &owner->paths[index];
            break;
        }
        if (!owner->paths[index].valid && target == NULL) {
            target = &owner->paths[index];
        }
    }
    if (target == NULL) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    if (target->valid) {
        uint32_t expected_generation;
        if (path_semantically_equal(target, path)) {
            return UCN_V6_OK;
        }
        if (ucn_v6_serial_checked_next(target->path_generation,
                                       &expected_generation) != UCN_V6_OK ||
            path->path_generation != expected_generation) {
            return UCN_V6_ERR_REPLAY;
        }
    }
    *target = *path;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_copy_peer(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_principal_t *principal,
    const ucn_v6_binding_key_t *binding,
    uint32_t session_generation,
    uint32_t link_instance_generation,
    ucn_v6_cached_peer_capability_t *peer_out)
{
    size_t index;
    if (!owner_is_valid(owner) || owner->faulted ||
        !ucn_v6_principal_is_valid(principal) ||
        !ucn_v6_binding_key_is_valid(binding) || session_generation == 0U ||
        link_instance_generation == 0U || peer_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        const ucn_v6_cached_peer_capability_t *peer = &owner->peers[index];
        if (peer->valid && principal_equal(&peer->principal, principal) &&
            ucn_v6_binding_key_equal(&peer->binding, binding) &&
            peer->session_generation == session_generation &&
            peer->record.link.link_instance_generation ==
                link_instance_generation) {
            if (now_us >= peer->discovery_deadline_us ||
                now_us >= peer->capability_deadline_us) {
                return UCN_V6_ERR_TIMEOUT;
            }
            *peer_out = *peer;
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_capability_copy_path(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_principal_t *destination_principal,
    const ucn_v6_binding_key_t *destination_binding,
    uint32_t session_generation,
    uint32_t route_generation,
    uint16_t path_id,
    uint32_t path_generation,
    ucn_v6_path_capability_t *path_out)
{
    size_t index;
    ucn_v6_cached_peer_capability_t peer;
    if (!owner_is_valid(owner) || owner->faulted ||
        !ucn_v6_principal_is_valid(destination_principal) ||
        !ucn_v6_binding_key_is_valid(destination_binding) ||
        session_generation == 0U || route_generation == 0U || path_id == 0U ||
        path_generation == 0U || path_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        const ucn_v6_path_capability_t *path = &owner->paths[index];
        if (path->valid &&
            principal_equal(&path->destination_principal,
                            destination_principal) &&
            ucn_v6_binding_key_equal(&path->destination_binding,
                                     destination_binding) &&
            path->session_generation == session_generation &&
            path->route_generation == route_generation &&
            path->path_id == path_id &&
            path->path_generation == path_generation) {
            if (now_us >= path->deadline_us ||
                ucn_v6_capability_copy_peer(
                    owner, now_us, destination_principal,
                    destination_binding, session_generation,
                    path->destination_link_instance_generation,
                    &peer) != UCN_V6_OK ||
                memcmp(peer.digest, path->destination_capability_digest,
                       sizeof(peer.digest)) != 0) {
                return UCN_V6_ERR_TIMEOUT;
            }
            *path_out = *path;
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_capability_expire(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us)
{
    size_t index;
    if (!owner_is_valid(owner) || owner->faulted) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        if (owner->peers[index].valid &&
            (now_us >= owner->peers[index].discovery_deadline_us ||
             now_us >= owner->peers[index].capability_deadline_us)) {
            ucn_v6_principal_t expired_principal =
                owner->peers[index].principal;
            memset(&owner->peers[index], 0, sizeof(owner->peers[index]));
            invalidate_paths_for_principal(owner, &expired_principal);
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid &&
            now_us >= owner->paths[index].deadline_us) {
            memset(&owner->paths[index], 0, sizeof(owner->paths[index]));
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS; ++index) {
        if (owner->hints[index].occupied &&
            now_us >= owner->hints[index].deadline_us) {
            memset(&owner->hints[index], 0, sizeof(owner->hints[index]));
        }
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_copy_view(
    const ucn_v6_capability_owner_t *owner,
    ucn_v6_capability_view_t *view)
{
    ucn_v6_capability_view_t next;
    size_t index;
    if (!owner_is_valid(owner) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    next = owner->stats;
    next.cached_peers = 0U;
    next.installed_paths = 0U;
    next.group_hints = 0U;
    next.faulted = owner->faulted;
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        if (owner->peers[index].valid) {
            ++next.cached_peers;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid) {
            ++next.installed_paths;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS; ++index) {
        if (owner->hints[index].occupied) {
            ++next.group_hints;
        }
    }
    *view = next;
    return UCN_V6_OK;
}
