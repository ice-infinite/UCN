#include "../internal/ucn_v6_capability_private.h"

#include <limits.h>
#include <string.h>

#define UCN_V6_CAPABILITY_SCHEMA UINT16_C(1)
#define UCN_V6_CAPABILITY_KNOWN_FEATURES                                 \
    ((uint32_t)(UCN_V6_FEATURE_IDENTITY | UCN_V6_FEATURE_WIRE |         \
                UCN_V6_FEATURE_MESSAGE | UCN_V6_FEATURE_SECURITY |      \
                UCN_V6_FEATURE_ROUTE | UCN_V6_FEATURE_TRANSFER |        \
                UCN_V6_FEATURE_REALTIME | UCN_V6_FEATURE_CLUSTER |      \
                UCN_V6_FEATURE_CAPABILITY | UCN_V6_FEATURE_ADAPTER |    \
                UCN_V6_FEATURE_QOS))
#define UCN_V6_CAPABILITY_KNOWN_LINK_FLAGS UINT16_C(0x001F)
#define UCN_V6_CAPABILITY_KNOWN_TIMESTAMP_BITS UINT16_C(0x000F)
#define UCN_V6_CAPABILITY_HOP_SUITE_BITS UINT32_C(0x00000002)
#define UCN_V6_CAPABILITY_E2E_SUITE_BITS UINT32_C(0x0000000E)
#define UCN_V6_CAPABILITY_REALTIME_MODE_BITS UINT16_C(0x0007)

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

static bool session_key_is_valid(const ucn_v6_session_key_t *session)
{
    return session != NULL &&
           ucn_v6_principal_is_valid(&session->principal) &&
           ucn_v6_binding_key_is_valid(&session->binding) &&
           session->session_generation != 0U &&
           session->session_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool session_key_equal(const ucn_v6_session_key_t *left,
                              const ucn_v6_session_key_t *right)
{
    return session_key_is_valid(left) && session_key_is_valid(right) &&
           principal_equal(&left->principal, &right->principal) &&
           ucn_v6_binding_key_equal(&left->binding, &right->binding) &&
           left->session_generation == right->session_generation;
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
        record->peer.max_concurrent_transfers == 0U ||
        (record->peer.realtime_mode_bits &
         (uint16_t)~UCN_V6_CAPABILITY_REALTIME_MODE_BITS) != 0U ||
        (((record->peer.feature_bits & UCN_V6_FEATURE_REALTIME) == 0U) !=
         (record->peer.realtime_mode_bits == 0U)) ||
        ((record->peer.realtime_mode_bits &
          (UCN_V6_REALTIME_MODE_SYNCED | UCN_V6_REALTIME_MODE_DEADLINE)) !=
             0U &&
         (record->peer.clock_domain_id == 0U ||
          record->peer.clock_domain_generation == 0U ||
          record->peer.clock_domain_generation >
              UCN_V6_SERIAL_ROTATION_THRESHOLD)) ||
        ((record->peer.realtime_mode_bits &
          (UCN_V6_REALTIME_MODE_SYNCED | UCN_V6_REALTIME_MODE_DEADLINE)) ==
             0U &&
         (record->peer.clock_domain_id != 0U ||
          record->peer.clock_domain_generation != 0U))) {
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
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->invalidation_head <
               UCN_V6_CAPABILITY_INVALIDATION_DEPTH &&
           owner->invalidation_count <=
               UCN_V6_CAPABILITY_INVALIDATION_DEPTH;
}

static bool invalidation_equal(
    const ucn_v6_stack_invalidation_t *left,
    const ucn_v6_stack_invalidation_t *right)
{
    return left->type == right->type && left->link_id == right->link_id &&
           left->link_generation == right->link_generation &&
           ucn_v6_binding_key_equal(&left->session.binding,
                                    &right->session.binding) &&
           principal_equal(&left->session.principal,
                           &right->session.principal) &&
           left->session.session_generation ==
               right->session.session_generation &&
           left->capability_generation == right->capability_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation;
}

static bool invalidation_queue_has_space(
    const ucn_v6_capability_owner_t *owner,
    size_t required)
{
    return required <= UCN_V6_CAPABILITY_INVALIDATION_DEPTH &&
           (size_t)owner->invalidation_count <=
               UCN_V6_CAPABILITY_INVALIDATION_DEPTH - required;
}

static void invalidation_push(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    size_t tail = ((size_t)owner->invalidation_head +
                   owner->invalidation_count) %
                  UCN_V6_CAPABILITY_INVALIDATION_DEPTH;
    owner->invalidations[tail] = *invalidation;
    ++owner->invalidation_count;
}

static const ucn_v6_stack_invalidation_t *invalidation_at(
    const ucn_v6_capability_owner_t *owner,
    size_t offset)
{
    size_t index = ((size_t)owner->invalidation_head + offset) %
                   UCN_V6_CAPABILITY_INVALIDATION_DEPTH;
    return &owner->invalidations[index];
}

static bool invalidation_covers(
    const ucn_v6_stack_invalidation_t *parent,
    const ucn_v6_stack_invalidation_t *child)
{
    if (parent == NULL || child == NULL || parent->type > child->type ||
        parent->link_id != child->link_id ||
        parent->link_generation != child->link_generation) {
        return false;
    }
    if (parent->type == UCN_V6_STACK_INVALIDATE_LINK) {
        return true;
    }
    if (!ucn_v6_binding_key_equal(&parent->session.binding,
                                  &child->session.binding) ||
        !principal_equal(&parent->session.principal,
                         &child->session.principal) ||
        parent->session.session_generation !=
            child->session.session_generation) {
        return false;
    }
    if (parent->type == UCN_V6_STACK_INVALIDATE_SESSION) {
        return true;
    }
    if (parent->capability_generation != child->capability_generation) {
        return false;
    }
    if (parent->type == UCN_V6_STACK_INVALIDATE_CAPABILITY) {
        return true;
    }
    return parent->path_id == child->path_id &&
           parent->path_generation == child->path_generation;
}

static void invalidation_remove_at(
    ucn_v6_capability_owner_t *owner,
    size_t offset)
{
    size_t cursor;
    for (cursor = offset; cursor + 1U < owner->invalidation_count; ++cursor) {
        size_t destination =
            ((size_t)owner->invalidation_head + cursor) %
            UCN_V6_CAPABILITY_INVALIDATION_DEPTH;
        size_t source = (destination + 1U) %
                        UCN_V6_CAPABILITY_INVALIDATION_DEPTH;
        owner->invalidations[destination] = owner->invalidations[source];
    }
    if (owner->invalidation_count != 0U) {
        size_t tail = ((size_t)owner->invalidation_head +
                       owner->invalidation_count - 1U) %
                      UCN_V6_CAPABILITY_INVALIDATION_DEPTH;
        memset(&owner->invalidations[tail], 0,
               sizeof(owner->invalidations[tail]));
        --owner->invalidation_count;
    }
}

static void invalidation_remove_descendants(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *parent)
{
    size_t offset = 0U;
    while (offset < owner->invalidation_count) {
        if (invalidation_covers(parent, invalidation_at(owner, offset))) {
            invalidation_remove_at(owner, offset);
        } else {
            ++offset;
        }
    }
}

static bool invalidation_queue_contains_cover(
    const ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *target)
{
    size_t offset;
    for (offset = 0U; offset < owner->invalidation_count; ++offset) {
        if (invalidation_covers(invalidation_at(owner, offset), target)) {
            return true;
        }
    }
    return false;
}

static size_t invalidation_queue_descendant_count(
    const ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *parent)
{
    size_t offset;
    size_t count = 0U;
    for (offset = 0U; offset < owner->invalidation_count; ++offset) {
        if (invalidation_covers(parent, invalidation_at(owner, offset))) {
            ++count;
        }
    }
    return count;
}

static bool invalidation_project_enqueue(
    const ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation,
    size_t *projected_count)
{
    size_t descendants;
    if (projected_count == NULL) {
        return false;
    }
    if (invalidation_queue_contains_cover(owner, invalidation)) {
        return true;
    }
    descendants = invalidation_queue_descendant_count(owner, invalidation);
    if (*projected_count < descendants) {
        return false;
    }
    *projected_count = *projected_count - descendants + 1U;
    return *projected_count <= UCN_V6_CAPABILITY_INVALIDATION_DEPTH;
}

static bool invalidation_enqueue(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    size_t offset;
    size_t covered = 0U;
    for (offset = 0U; offset < owner->invalidation_count; ++offset) {
        const ucn_v6_stack_invalidation_t *queued =
            invalidation_at(owner, offset);
        if (invalidation_covers(queued, invalidation)) {
            return true;
        }
        if (invalidation_covers(invalidation, queued)) {
            ++covered;
        }
    }
    if (covered == 0U &&
        !invalidation_queue_has_space(owner, 1U)) {
        return false;
    }
    invalidation_remove_descendants(owner, invalidation);
    invalidation_push(owner, invalidation);
    return true;
}

static bool peer_session_key_is_valid(
    const ucn_v6_cached_peer_capability_t *peer)
{
    return peer != NULL && peer->valid &&
           ucn_v6_principal_is_valid(&peer->principal) &&
           ucn_v6_binding_key_is_valid(&peer->binding) &&
           peer->session_generation != 0U &&
           peer->session_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           peer->ingress_link_id != 0U &&
           peer->ingress_link_id != UINT16_MAX &&
           peer->ingress_link_generation != 0U &&
           peer->ingress_link_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool invalidation_from_peer(
    const ucn_v6_cached_peer_capability_t *peer,
    ucn_v6_stack_invalidation_type_t type,
    ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_stack_invalidation_t next;
    if (!peer_session_key_is_valid(peer) || invalidation == NULL ||
        (type != UCN_V6_STACK_INVALIDATE_LINK &&
         type != UCN_V6_STACK_INVALIDATE_SESSION &&
         type != UCN_V6_STACK_INVALIDATE_CAPABILITY)) {
        return false;
    }
    memset(&next, 0, sizeof(next));
    next.type = type;
    next.link_id = peer->ingress_link_id;
    next.link_generation = peer->ingress_link_generation;
    if (type >= UCN_V6_STACK_INVALIDATE_SESSION) {
        next.session.binding = peer->binding;
        next.session.principal = peer->principal;
        next.session.session_generation = peer->session_generation;
    }
    if (type >= UCN_V6_STACK_INVALIDATE_CAPABILITY) {
        next.capability_generation = peer->record.capability_generation;
    }
    if (!ucn_v6_stack_invalidation_is_valid(&next)) {
        return false;
    }
    *invalidation = next;
    return true;
}

static bool invalidation_from_path(
    const ucn_v6_cached_peer_capability_t *peer,
    const ucn_v6_path_capability_t *path,
    ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_stack_invalidation_t next;
    if (!invalidation_from_peer(peer, UCN_V6_STACK_INVALIDATE_CAPABILITY,
                                &next) ||
        path == NULL || !path->valid || path->path_id == 0U ||
        path->path_id == UINT16_MAX || path->path_generation == 0U ||
        path->path_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return false;
    }
    next.type = UCN_V6_STACK_INVALIDATE_PATH;
    next.path_id = path->path_id;
    next.path_generation = path->path_generation;
    if (!ucn_v6_stack_invalidation_is_valid(&next)) {
        return false;
    }
    *invalidation = next;
    return true;
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
    write_u16(&encoded[60], record->peer.realtime_mode_bits);
    write_u16(&encoded[62], record->peer.clock_domain_id);
    write_u32(&encoded[64], record->peer.clock_domain_generation);
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
        input_length != UCN_V6_CAPABILITY_RECORD_BYTES) {
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
    decoded.peer.realtime_mode_bits = read_u16(&input[60]);
    decoded.peer.clock_domain_id = read_u16(&input[62]);
    decoded.peer.clock_domain_generation = read_u32(&input[64]);
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
    if (ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     owner_out, sizeof(*owner_out)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     manifest, sizeof(*manifest)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     local_record, sizeof(*local_record))) {
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

/* Security is the sole owner of the physical and Session parent selected for
 * an authenticated ingress frame.  Capability must never accept a second,
 * caller-supplied copy of that identity: doing so would let one Link's proof
 * populate another Link's cache and survive the real parent's invalidation.
 *
 * Security 是认证入站帧物理父级与 Session 父级的唯一所有者。Capability
 * 不能再接受调用方复制的第二份身份，否则一个 Link 的证明可能写入另一个 Link
 * 的缓存，并逃过真实父级的失效事件。 */
static bool opened_peer_parent_is_consistent(
    const ucn_v6_security_open_result_t *opened)
{
    return opened != NULL &&
           opened->ingress_link_instance_id != 0U &&
           opened->ingress_link_instance_id != UINT16_MAX &&
           opened->ingress_link_instance_generation != 0U &&
           opened->ingress_link_instance_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           principal_equal(&opened->ingress_peer_session.principal,
                           &opened->authenticated_principal) &&
           opened->ingress_peer_session.binding.realm_id ==
               opened->frame.realm_id &&
           opened->ingress_peer_session.binding.node_address ==
               opened->frame.source_address &&
           opened->ingress_peer_session.binding.binding_generation ==
               opened->frame.source_binding_generation &&
           opened->ingress_peer_session.session_generation ==
               opened->frame.session_generation;
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

static void invalidate_paths_for_parent_principal(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_principal_t *principal)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid &&
            principal_equal(
                &owner->paths[index].local_parent_session.principal,
                            principal)) {
            memset(&owner->paths[index], 0, sizeof(owner->paths[index]));
        }
    }
}

static bool peer_lease_expired(
    const ucn_v6_cached_peer_capability_t *peer,
    uint64_t now_us)
{
    return peer != NULL && peer->valid &&
           ((peer->discovery_deadline_us != 0U &&
             now_us >= peer->discovery_deadline_us) ||
            (peer->capability_deadline_us != 0U &&
             now_us >= peer->capability_deadline_us));
}

ucn_v6_result_t ucn_v6_capability_ingest_peer_hello(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_summary_t *summary,
    ucn_v6_hello_disposition_t *disposition)
{
    uint8_t payload[UCN_V6_CAPABILITY_HELLO_BYTES];
    ucn_v6_cached_peer_capability_t *peer;
    uint64_t deadline;
    if (!owner_is_valid(owner) || owner->faulted || summary == NULL ||
        disposition == NULL || !opened_peer_parent_is_consistent(opened) ||
        ucn_v6_capability_summary_encode(summary, payload) != UCN_V6_OK ||
        !frame_matches_payload(opened, UCN_V6_PROTOCOL_OPCODE_PEER_HELLO,
                               payload, sizeof(payload)) ||
        opened->frame.session_generation == 0U ||
        opened->frame.source_binding_generation == 0U ||
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
        peer->ingress_link_id != opened->ingress_link_instance_id ||
        peer->ingress_link_generation !=
            opened->ingress_link_instance_generation ||
        peer->record.link.link_instance_generation !=
            summary->link_instance_generation ||
        peer->record.capability_generation !=
            summary->capability_generation ||
        memcmp(peer->digest, summary->digest, sizeof(peer->digest)) != 0 ||
        now_us >= peer->capability_deadline_us) {
        *disposition = UCN_V6_HELLO_QUERY_REQUIRED;
        return UCN_V6_OK;
    }
    peer->ingress_link_id = opened->ingress_link_instance_id;
    peer->discovery_deadline_us = deadline;
    *disposition = UCN_V6_HELLO_MATCHED;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_ingest_advertise(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    const ucn_v6_capability_record_t *record)
{
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    ucn_v6_cached_peer_capability_t *peer;
    uint64_t discovery_deadline;
    uint64_t capability_deadline;
    uint32_t expected_generation;
    ucn_v6_stack_invalidation_t invalidation;
    bool emit_invalidation = false;
    if (!owner_is_valid(owner) || owner->faulted ||
        !opened_peer_parent_is_consistent(opened) ||
        ucn_v6_capability_record_encode(record, payload) != UCN_V6_OK ||
        ucn_v6_capability_digest(record, digest) != UCN_V6_OK ||
        !frame_matches_payload(
            opened, UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE,
            payload, sizeof(payload)) ||
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
            peer->ingress_link_id == opened->ingress_link_instance_id &&
            peer->ingress_link_generation ==
                opened->ingress_link_instance_generation;
        if (!same_domain) {
            /* Parent replacement is owned by Link/Security and must arrive as
             * an explicit invalidation before a new Session domain can occupy
             * this Principal slot.  Inferring replacement from an ADVERTISE
             * would let a delayed old Session overwrite a newer parent. */
            increment_saturated(&owner->stats.rejected_authentication);
            return UCN_V6_ERR_REPLAY;
        } else if (peer->record.capability_generation ==
                   record->capability_generation) {
            if (memcmp(peer->digest, digest, sizeof(peer->digest)) != 0) {
                return UCN_V6_ERR_REPLAY;
            }
            if (!invalidation_from_peer(
                    peer, UCN_V6_STACK_INVALIDATE_CAPABILITY,
                    &invalidation)) {
                owner->faulted = true;
                return UCN_V6_ERR_STATE;
            }
            if (invalidation_queue_contains_cover(owner, &invalidation)) {
                /* An exact-generation refresh is safe only after every
                 * consumer has observed the prior lease revocation.  The
                 * next generation is independently identifiable and may
                 * advance while the old event is still queued. */
                return UCN_V6_ERR_STATE;
            }
            peer->ingress_link_id = opened->ingress_link_instance_id;
            peer->ingress_link_generation =
                opened->ingress_link_instance_generation;
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
            if (!invalidation_from_peer(
                    peer, UCN_V6_STACK_INVALIDATE_CAPABILITY,
                    &invalidation)) {
                owner->faulted = true;
                return UCN_V6_ERR_STATE;
            }
            emit_invalidation = true;
        }
    }
    if (peer == NULL) {
        peer = find_peer(owner, &opened->authenticated_principal, true);
    }
    if (peer == NULL) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    if (emit_invalidation &&
        !invalidation_enqueue(owner, &invalidation)) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    invalidate_paths_for_parent_principal(
        owner, &opened->authenticated_principal);
    memset(peer, 0, sizeof(*peer));
    peer->valid = true;
    peer->principal = opened->authenticated_principal;
    peer->binding.realm_id = opened->frame.realm_id;
    peer->binding.node_address = opened->frame.source_address;
    peer->binding.binding_generation =
        opened->frame.source_binding_generation;
    peer->session_generation = opened->frame.session_generation;
    peer->ingress_link_id = opened->ingress_link_instance_id;
    peer->ingress_link_generation = opened->ingress_link_instance_generation;
    peer->record = *record;
    memcpy(peer->digest, digest, sizeof(peer->digest));
    peer->discovery_deadline_us = discovery_deadline;
    peer->capability_deadline_us = capability_deadline;
    return UCN_V6_OK;
}

static ucn_v6_group_discovery_hint_t *find_hint(
    ucn_v6_capability_owner_t *owner,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    const ucn_v6_binding_key_t *binding,
    uint32_t session_generation,
    bool allow_empty)
{
    ucn_v6_group_discovery_hint_t *empty = NULL;
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS; ++index) {
        ucn_v6_group_discovery_hint_t *hint = &owner->hints[index];
        if (hint->occupied &&
            hint->ingress_link_id == ingress_link_id &&
            hint->ingress_link_generation == ingress_link_generation &&
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
        next->last_activity_us = now_us;
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
        next->last_activity_us = now_us;
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
    const ucn_v6_security_open_result_t *opened)
{
    ucn_v6_binding_key_t binding;
    ucn_v6_group_discovery_hint_t *hint;
    ucn_v6_group_hint_link_budget_t *link;
    ucn_v6_group_hint_group_budget_t *group;
    ucn_v6_group_hint_link_budget_t next_link;
    ucn_v6_group_hint_group_budget_t next_group;
    uint16_t ingress_link_id;
    uint32_t ingress_link_generation;
    uint64_t deadline;
    if (!owner_is_valid(owner) || owner->faulted || opened == NULL ||
        opened->ingress_link_instance_id == 0U ||
        opened->ingress_link_instance_id == UINT16_MAX ||
        opened->ingress_link_instance_generation == 0U ||
        opened->ingress_link_instance_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
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
    /* Security is the sole source of the authenticated physical parent.
     * Accepting a second caller-supplied Link key would allow a frame opened
     * on Link A to consume or populate Link B's bounded discovery state. */
    ingress_link_id = opened->ingress_link_instance_id;
    ingress_link_generation = opened->ingress_link_instance_generation;
    binding.realm_id = opened->frame.realm_id;
    binding.node_address = opened->frame.source_address;
    binding.binding_generation = opened->frame.source_binding_generation;
    if (!ucn_v6_binding_key_is_valid(&binding) ||
        opened->frame.session_generation == 0U) {
        return UCN_V6_ERR_SECURITY;
    }
    hint = find_hint(owner, ingress_link_id, ingress_link_generation,
                     &binding, opened->frame.session_generation, false);
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
    hint = find_hint(owner, ingress_link_id, ingress_link_generation,
                     &binding, opened->frame.session_generation, true);
    if (hint == NULL ||
        checked_deadline(now_us, UCN_V6_GROUP_HINT_TIMEOUT_US,
                         &deadline) != UCN_V6_OK) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    --next_link.tokens;
    --next_group.tokens;
    next_link.last_activity_us = now_us;
    next_group.last_activity_us = now_us;
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

static bool path_capability_is_valid(
    const ucn_v6_path_capability_t *path,
    uint64_t now_us)
{
    return path != NULL && path->valid &&
           ucn_v6_principal_is_valid(&path->destination_principal) &&
           ucn_v6_binding_key_is_valid(&path->destination_binding) &&
           path->destination_session_generation != 0U &&
           path->destination_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           path->destination_capability_generation != 0U &&
           path->destination_capability_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           digest_is_nonzero(path->destination_capability_digest) &&
           (path->destination_realtime_mode_bits &
            (uint16_t)~UCN_V6_CAPABILITY_REALTIME_MODE_BITS) == 0U &&
           (((path->destination_realtime_mode_bits &
              (UCN_V6_REALTIME_MODE_SYNCED |
               UCN_V6_REALTIME_MODE_DEADLINE)) != 0U) ==
            (path->destination_clock_domain_id != 0U &&
             path->destination_clock_domain_generation != 0U &&
             path->destination_clock_domain_generation <=
                 UCN_V6_SERIAL_ROTATION_THRESHOLD)) &&
           session_key_is_valid(&path->local_parent_session) &&
           path->local_parent_link_id != 0U &&
           path->local_parent_link_id != UINT16_MAX &&
           path->local_parent_link_generation != 0U &&
           path->local_parent_link_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           path->local_parent_capability_generation != 0U &&
           path->local_parent_capability_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           digest_is_nonzero(path->local_parent_capability_digest) &&
           path->route_generation != 0U &&
           path->route_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           path->path_id != 0U && path->path_id != UINT16_MAX &&
           path->path_generation != 0U &&
           path->path_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           path->hop_count != 0U &&
           path->hop_count <= UCN_V6_PATH_HOP_LIMIT &&
           path->path_frame_mtu != 0U &&
           path->path_frame_mtu <= UCN_V6_WIRE_MAX_FRAME_BYTES &&
           path->payload_budget != 0U &&
           path->payload_budget < path->path_frame_mtu &&
           path->fragment_data_budget != 0U &&
           path->fragment_data_budget <= path->payload_budget &&
           path->feature_bits != 0U &&
           (path->feature_bits & ~UCN_V6_CAPABILITY_KNOWN_FEATURES) == 0U &&
           path->hop_suite_bits != 0U &&
           (path->hop_suite_bits & ~UCN_V6_CAPABILITY_HOP_SUITE_BITS) == 0U &&
           path->e2e_suite_bits != 0U &&
           (path->e2e_suite_bits & ~UCN_V6_CAPABILITY_E2E_SUITE_BITS) == 0U &&
           (uint32_t)path->max_message_class <=
               (uint32_t)UCN_V6_MESSAGE_T8K &&
           path->max_window != 0U && path->max_concurrency != 0U &&
           (path->timestamp_capability_bits &
            (uint16_t)~UCN_V6_CAPABILITY_KNOWN_TIMESTAMP_BITS) == 0U &&
           ((path->timestamp_capability_bits == 0U) ==
            (path->timestamp_uncertainty_us == 0U)) &&
           path->deadline_us != 0U && now_us < path->deadline_us;
}

bool ucn_v6_capability_cached_peer_is_live(
    const ucn_v6_cached_peer_capability_t *peer,
    uint64_t now_us)
{
    uint8_t digest[UCN_V6_CAPABILITY_DIGEST_BYTES];
    return peer_session_key_is_valid(peer) && record_is_valid(&peer->record) &&
           peer->discovery_deadline_us != 0U &&
           peer->capability_deadline_us != 0U &&
           now_us < peer->discovery_deadline_us &&
           now_us < peer->capability_deadline_us &&
           ucn_v6_capability_digest(&peer->record, digest) == UCN_V6_OK &&
           memcmp(peer->digest, digest, sizeof(digest)) == 0;
}

bool ucn_v6_capability_path_is_live(
    const ucn_v6_path_capability_t *path,
    const ucn_v6_cached_peer_capability_t *parent_peer,
    uint64_t now_us)
{
    return path_capability_is_valid(path, now_us) &&
           ucn_v6_capability_cached_peer_is_live(parent_peer, now_us) &&
           principal_equal(&path->local_parent_session.principal,
                           &parent_peer->principal) &&
           ucn_v6_binding_key_equal(&path->local_parent_session.binding,
                                    &parent_peer->binding) &&
           path->local_parent_session.session_generation ==
               parent_peer->session_generation &&
           path->local_parent_link_id == parent_peer->ingress_link_id &&
           path->local_parent_link_generation ==
               parent_peer->ingress_link_generation &&
           path->local_parent_capability_generation ==
               parent_peer->record.capability_generation &&
           memcmp(path->local_parent_capability_digest,
                  parent_peer->digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           (path->feature_bits &
            ~parent_peer->record.peer.feature_bits) == 0U &&
           (path->hop_suite_bits &
            ~parent_peer->record.peer.hop_suite_bits) == 0U &&
           (path->e2e_suite_bits &
            ~parent_peer->record.peer.e2e_suite_bits) == 0U &&
           (uint32_t)path->max_message_class <=
               (uint32_t)parent_peer->record.peer.max_message_class &&
           path->max_window <= parent_peer->record.peer.max_rx_window &&
           path->max_concurrency <=
               parent_peer->record.peer.max_concurrent_transfers &&
           (path->timestamp_capability_bits &
            (uint16_t)~parent_peer->record.link
                .timestamp_capability_bits) == 0U &&
           (path->timestamp_capability_bits == 0U ||
            path->timestamp_uncertainty_us >=
                parent_peer->record.link.timestamp_uncertainty_us) &&
           path->path_frame_mtu <=
               parent_peer->record.link.link_frame_mtu &&
           path->path_frame_mtu <=
               parent_peer->record.link.processing_frame_mtu;
}

ucn_v6_result_t ucn_v6_capability_path_reduce_begin(
    const ucn_v6_path_budget_request_t *request,
    ucn_v6_path_budget_accumulator_t *accumulator)
{
    ucn_v6_path_budget_accumulator_t initialized;
    ucn_v6_frame_t frame;
    size_t overhead;
    if (request == NULL || accumulator == NULL ||
        request->frame_contract == NULL ||
        request->frame_contract->payload != NULL ||
        request->frame_contract->payload_length != 0U ||
        !ucn_v6_principal_is_valid(&request->destination_principal) ||
        !ucn_v6_binding_key_is_valid(&request->destination_binding) ||
        request->destination_session_generation == 0U ||
        request->destination_session_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        request->destination_capability_generation == 0U ||
        request->destination_capability_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        !digest_is_nonzero(request->destination_capability_digest) ||
        (request->destination_realtime_mode_bits &
         (uint16_t)~UCN_V6_CAPABILITY_REALTIME_MODE_BITS) != 0U ||
        (((request->destination_realtime_mode_bits &
           (UCN_V6_REALTIME_MODE_SYNCED |
            UCN_V6_REALTIME_MODE_DEADLINE)) != 0U) !=
         (request->destination_clock_domain_id != 0U &&
          request->destination_clock_domain_generation != 0U &&
          request->destination_clock_domain_generation <=
              UCN_V6_SERIAL_ROTATION_THRESHOLD)) ||
        request->route_generation == 0U ||
        request->route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        request->path_id == 0U || request->path_id == UINT16_MAX ||
        request->path_generation == 0U ||
        request->path_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        request->deadline_us == 0U ||
        request->path_policy_frame_mtu == 0U ||
        request->required_hop_suite_bits == 0U ||
        request->required_e2e_suite_bits == 0U ||
        request->policy_max_window == 0U ||
        request->policy_max_concurrency == 0U ||
        (uint32_t)request->policy_max_message_class >
            (uint32_t)UCN_V6_MESSAGE_T8K) {
        return UCN_V6_ERR_ARGUMENT;
    }
    frame = *request->frame_contract;
    if (ucn_v6_wire_encoded_size(&frame, &overhead) != UCN_V6_OK ||
        overhead > UINT32_MAX) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.active = true;
    initialized.required_feature_bits = request->required_feature_bits;
    initialized.required_hop_suite_bits =
        request->required_hop_suite_bits;
    initialized.required_e2e_suite_bits =
        request->required_e2e_suite_bits;
    initialized.frame_overhead_bytes = (uint32_t)overhead;
    initialized.fragment_header_bytes = request->fragment_header_bytes;
    initialized.derived.valid = true;
    initialized.derived.immutable_for_realtime = request->fixed_path;
    initialized.derived.destination_principal = request->destination_principal;
    initialized.derived.destination_binding = request->destination_binding;
    initialized.derived.destination_session_generation =
        request->destination_session_generation;
    initialized.derived.destination_capability_generation =
        request->destination_capability_generation;
    memcpy(initialized.derived.destination_capability_digest,
           request->destination_capability_digest,
           sizeof(initialized.derived.destination_capability_digest));
    initialized.derived.destination_realtime_mode_bits =
        request->destination_realtime_mode_bits;
    initialized.derived.destination_clock_domain_id =
        request->destination_clock_domain_id;
    initialized.derived.destination_clock_domain_generation =
        request->destination_clock_domain_generation;
    initialized.derived.route_generation = request->route_generation;
    initialized.derived.path_id = request->path_id;
    initialized.derived.path_generation = request->path_generation;
    initialized.derived.path_frame_mtu = request->path_policy_frame_mtu;
    initialized.derived.feature_bits = UINT32_MAX;
    initialized.derived.hop_suite_bits = UINT32_MAX;
    initialized.derived.e2e_suite_bits = UINT32_MAX;
    initialized.derived.max_message_class =
        request->policy_max_message_class;
    initialized.derived.max_window = request->policy_max_window;
    initialized.derived.max_concurrency = request->policy_max_concurrency;
    initialized.derived.timestamp_capability_bits =
        UCN_V6_CAPABILITY_KNOWN_TIMESTAMP_BITS;
    initialized.derived.deadline_us = request->deadline_us;
    *accumulator = initialized;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_path_reduce_hop(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    ucn_v6_path_budget_accumulator_t *accumulator,
    const ucn_v6_capability_peer_ref_t *hop_ref)
{
    ucn_v6_cached_peer_capability_t cached;
    const ucn_v6_capability_record_t *record;
    ucn_v6_path_capability_t next;
    uint64_t uncertainty;
    uint32_t frame_limit;
    if (!owner_is_valid(owner) || accumulator == NULL ||
        !accumulator->active || accumulator->local_parent_bound ||
        hop_ref == NULL ||
        !ucn_v6_principal_is_valid(&hop_ref->principal) ||
        !ucn_v6_binding_key_is_valid(&hop_ref->binding) ||
        hop_ref->session_generation == 0U ||
        hop_ref->session_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        hop_ref->ingress_link_id == 0U ||
        hop_ref->ingress_link_id == UINT16_MAX ||
        hop_ref->ingress_link_generation == 0U ||
        hop_ref->ingress_link_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        ucn_v6_capability_copy_peer(
            owner, now_us, &hop_ref->principal, &hop_ref->binding,
            hop_ref->session_generation, hop_ref->ingress_link_generation,
            &cached) != UCN_V6_OK ||
        cached.ingress_link_id != hop_ref->ingress_link_id) {
        return UCN_V6_ERR_ARGUMENT;
    }
    record = &cached.record;
    if (!record_is_valid(record)) {
        return UCN_V6_ERR_STATE;
    }
    if (accumulator->hop_count == UCN_V6_PATH_HOP_LIMIT) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    uncertainty = accumulator->timestamp_uncertainty_us;
    if (record->link.timestamp_capability_bits != 0U) {
        uncertainty += record->link.timestamp_uncertainty_us;
        if (uncertainty > UINT32_MAX) {
            return UCN_V6_ERR_EXHAUSTED;
        }
    }
    next = accumulator->derived;
    next.local_parent_session.principal = cached.principal;
    next.local_parent_session.binding = cached.binding;
    next.local_parent_session.session_generation =
        cached.session_generation;
    next.local_parent_link_id = cached.ingress_link_id;
    next.local_parent_link_generation = cached.ingress_link_generation;
    next.local_parent_capability_generation =
        cached.record.capability_generation;
    memcpy(next.local_parent_capability_digest, cached.digest,
           sizeof(next.local_parent_capability_digest));
    frame_limit = minimum_u32(record->link.link_frame_mtu,
                              record->link.processing_frame_mtu);
    next.path_frame_mtu = minimum_u32(next.path_frame_mtu, frame_limit);
    next.feature_bits &= record->peer.feature_bits;
    next.hop_suite_bits &= record->peer.hop_suite_bits;
    next.e2e_suite_bits &= record->peer.e2e_suite_bits;
    next.max_message_class = (ucn_v6_message_class_t)minimum_u32(
        (uint32_t)next.max_message_class,
        (uint32_t)record->peer.max_message_class);
    next.max_window = minimum_u16(next.max_window,
                                  record->peer.max_rx_window);
    next.max_concurrency = minimum_u16(
        next.max_concurrency, record->peer.max_concurrent_transfers);
    next.timestamp_capability_bits &=
        record->link.timestamp_capability_bits;
    /* A derived Path is a bounded lease over every contributing Hop, not a
     * timeless copy of their claims.  Keep the earliest discovery/capability
     * deadline so an intermediate Hop can never be relied on after the
     * authenticated record used by this reduction expires.  An exact-next
     * generation may coexist until this old lease ends; immediate physical
     * failure is still fenced hop-by-hop by Link/Session invalidation. */
    if (cached.discovery_deadline_us < next.deadline_us) {
        next.deadline_us = cached.discovery_deadline_us;
    }
    if (cached.capability_deadline_us < next.deadline_us) {
        next.deadline_us = cached.capability_deadline_us;
    }
    accumulator->derived = next;
    accumulator->timestamp_uncertainty_us = uncertainty;
    ++accumulator->hop_count;
    accumulator->local_parent_bound = true;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_path_reduce_downstream(
    ucn_v6_path_budget_accumulator_t *accumulator,
    const ucn_v6_path_capability_t *downstream,
    uint64_t local_downstream_deadline_us)
{
    ucn_v6_path_capability_t next;
    uint64_t uncertainty;
    uint32_t total_hops;
    if (accumulator == NULL || !accumulator->active ||
        accumulator->downstream_reduced || downstream == NULL ||
        !path_capability_is_valid(downstream, 0U) ||
        local_downstream_deadline_us == 0U ||
        !principal_equal(&accumulator->derived.destination_principal,
                         &downstream->destination_principal) ||
        !ucn_v6_binding_key_equal(
            &accumulator->derived.destination_binding,
            &downstream->destination_binding) ||
        accumulator->derived.destination_session_generation !=
            downstream->destination_session_generation ||
        accumulator->derived.destination_capability_generation !=
            downstream->destination_capability_generation ||
        memcmp(accumulator->derived.destination_capability_digest,
               downstream->destination_capability_digest,
               UCN_V6_CAPABILITY_DIGEST_BYTES) != 0 ||
        accumulator->derived.destination_realtime_mode_bits !=
            downstream->destination_realtime_mode_bits ||
        accumulator->derived.destination_clock_domain_id !=
            downstream->destination_clock_domain_id ||
        accumulator->derived.destination_clock_domain_generation !=
            downstream->destination_clock_domain_generation) {
        return UCN_V6_ERR_ARGUMENT;
    }
    total_hops = (uint32_t)accumulator->hop_count + downstream->hop_count;
    if (total_hops > UCN_V6_PATH_HOP_LIMIT) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    uncertainty = accumulator->timestamp_uncertainty_us;
    if (downstream->timestamp_capability_bits != 0U) {
        uncertainty += downstream->timestamp_uncertainty_us;
        if (uncertainty > UINT32_MAX) {
            return UCN_V6_ERR_EXHAUSTED;
        }
    }
    next = accumulator->derived;
    next.path_frame_mtu = minimum_u32(
        next.path_frame_mtu, downstream->path_frame_mtu);
    next.feature_bits &= downstream->feature_bits;
    next.hop_suite_bits &= downstream->hop_suite_bits;
    next.e2e_suite_bits &= downstream->e2e_suite_bits;
    next.max_message_class = (ucn_v6_message_class_t)minimum_u32(
        (uint32_t)next.max_message_class,
        (uint32_t)downstream->max_message_class);
    next.max_window = minimum_u16(next.max_window,
                                  downstream->max_window);
    next.max_concurrency = minimum_u16(next.max_concurrency,
                                       downstream->max_concurrency);
    next.timestamp_capability_bits &=
        downstream->timestamp_capability_bits;
    if (local_downstream_deadline_us < next.deadline_us) {
        next.deadline_us = local_downstream_deadline_us;
    }
    accumulator->derived = next;
    accumulator->timestamp_uncertainty_us = uncertainty;
    accumulator->hop_count = (uint16_t)total_hops;
    accumulator->downstream_reduced = true;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_path_reduce_finalize(
    ucn_v6_path_budget_accumulator_t *accumulator,
    ucn_v6_path_capability_t *path)
{
    ucn_v6_path_capability_t derived;
    if (accumulator == NULL || path == NULL || !accumulator->active ||
        !accumulator->local_parent_bound || accumulator->hop_count == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    derived = accumulator->derived;
    derived.timestamp_uncertainty_us =
        derived.timestamp_capability_bits == 0U ? 0U :
        (uint32_t)accumulator->timestamp_uncertainty_us;
    derived.hop_count = accumulator->hop_count;
    if ((derived.feature_bits & accumulator->required_feature_bits) !=
            accumulator->required_feature_bits ||
        (derived.hop_suite_bits & accumulator->required_hop_suite_bits) !=
            accumulator->required_hop_suite_bits ||
        (derived.e2e_suite_bits & accumulator->required_e2e_suite_bits) !=
            accumulator->required_e2e_suite_bits ||
        derived.path_frame_mtu == 0U || derived.max_window == 0U ||
        derived.max_concurrency == 0U) {
        return UCN_V6_ERR_ACCESS;
    }
    if (accumulator->frame_overhead_bytes >= derived.path_frame_mtu) {
        return UCN_V6_ERR_NO_SPACE;
    }
    derived.payload_budget = derived.path_frame_mtu -
                             accumulator->frame_overhead_bytes;
    if (accumulator->fragment_header_bytes >= derived.payload_budget) {
        return UCN_V6_ERR_NO_SPACE;
    }
    derived.fragment_data_budget =
        derived.payload_budget - accumulator->fragment_header_bytes;
    *path = derived;
    accumulator->active = false;
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

static const ucn_v6_cached_peer_capability_t *find_path_peer(
    const ucn_v6_capability_owner_t *owner,
    const ucn_v6_path_capability_t *path)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        const ucn_v6_cached_peer_capability_t *peer = &owner->peers[index];
        if (peer->valid &&
            principal_equal(&peer->principal,
                            &path->local_parent_session.principal) &&
            ucn_v6_binding_key_equal(&peer->binding,
                                     &path->local_parent_session.binding) &&
            peer->session_generation ==
                path->local_parent_session.session_generation &&
            peer->ingress_link_id == path->local_parent_link_id &&
            peer->ingress_link_generation ==
                path->local_parent_link_generation &&
            peer->record.capability_generation ==
                path->local_parent_capability_generation &&
            memcmp(peer->digest, path->local_parent_capability_digest,
                   UCN_V6_CAPABILITY_DIGEST_BYTES) == 0) {
            return peer;
        }
    }
    return NULL;
}

static bool path_semantically_equal(const ucn_v6_path_capability_t *left,
                                    const ucn_v6_path_capability_t *right)
{
    return left->valid == right->valid &&
           left->immutable_for_realtime == right->immutable_for_realtime &&
           principal_equal(&left->destination_principal,
                           &right->destination_principal) &&
           ucn_v6_binding_key_equal(&left->destination_binding,
                                    &right->destination_binding) &&
           left->destination_session_generation ==
               right->destination_session_generation &&
           left->destination_capability_generation ==
               right->destination_capability_generation &&
           memcmp(left->destination_capability_digest,
                  right->destination_capability_digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           left->destination_realtime_mode_bits ==
               right->destination_realtime_mode_bits &&
           left->destination_clock_domain_id ==
               right->destination_clock_domain_id &&
           left->destination_clock_domain_generation ==
               right->destination_clock_domain_generation &&
           session_key_equal(&left->local_parent_session,
                             &right->local_parent_session) &&
           left->local_parent_link_id == right->local_parent_link_id &&
           left->local_parent_link_generation ==
               right->local_parent_link_generation &&
           left->local_parent_capability_generation ==
               right->local_parent_capability_generation &&
           memcmp(left->local_parent_capability_digest,
                  right->local_parent_capability_digest,
                  UCN_V6_CAPABILITY_DIGEST_BYTES) == 0 &&
           left->route_generation == right->route_generation &&
           left->path_id == right->path_id &&
           left->path_generation == right->path_generation &&
           left->hop_count == right->hop_count &&
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

static bool path_refresh_claim_equal(
    const ucn_v6_path_capability_t *left,
    const ucn_v6_path_capability_t *right)
{
    ucn_v6_path_capability_t left_claim;
    ucn_v6_path_capability_t right_claim;
    if (left == NULL || right == NULL) {
        return false;
    }
    left_claim = *left;
    right_claim = *right;
    left_claim.deadline_us = 0U;
    right_claim.deadline_us = 0U;
    return path_semantically_equal(&left_claim, &right_claim);
}

ucn_v6_result_t ucn_v6_capability_install_path(
    ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_path_capability_t *path)
{
    ucn_v6_path_capability_t *target = NULL;
    ucn_v6_cached_peer_capability_t peer;
    ucn_v6_stack_invalidation_t invalidation;
    bool emit_invalidation = false;
    size_t index;
    if (!owner_is_valid(owner) || owner->faulted ||
        !path_capability_is_valid(path, now_us)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (ucn_v6_capability_copy_peer(
            owner, now_us, &path->local_parent_session.principal,
            &path->local_parent_session.binding,
            path->local_parent_session.session_generation,
            path->local_parent_link_generation, &peer) != UCN_V6_OK ||
        memcmp(peer.digest, path->local_parent_capability_digest,
               sizeof(peer.digest)) != 0 ||
        peer.ingress_link_id != path->local_parent_link_id ||
        peer.record.capability_generation !=
            path->local_parent_capability_generation) {
        return UCN_V6_ERR_STATE;
    }
    if (!ucn_v6_capability_path_is_live(path, &peer, now_us)) {
        return UCN_V6_ERR_ACCESS;
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
        if (target->path_generation == path->path_generation) {
            const ucn_v6_cached_peer_capability_t *current_peer =
                find_path_peer(owner, target);
            if (!path_refresh_claim_equal(target, path) ||
                current_peer == NULL ||
                !invalidation_from_path(current_peer, target,
                                        &invalidation)) {
                return UCN_V6_ERR_REPLAY;
            }
            if (invalidation_queue_contains_cover(owner, &invalidation)) {
                /* The exact-generation Path may become live again only after
                 * every consumer has observed its prior lease revocation. */
                return UCN_V6_ERR_STATE;
            }
            *target = *path;
            return UCN_V6_OK;
        }
        if (ucn_v6_serial_checked_next(target->path_generation,
                                       &expected_generation) != UCN_V6_OK ||
            path->path_generation != expected_generation) {
            return UCN_V6_ERR_REPLAY;
        }
        {
            const ucn_v6_cached_peer_capability_t *old_peer =
                find_path_peer(owner, target);
            if (old_peer == NULL ||
                !invalidation_from_path(old_peer, target, &invalidation)) {
                owner->faulted = true;
                return UCN_V6_ERR_STATE;
            }
            emit_invalidation = true;
        }
    }
    if (emit_invalidation &&
        !invalidation_enqueue(owner, &invalidation)) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
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
    uint32_t ingress_link_generation,
    ucn_v6_cached_peer_capability_t *peer_out)
{
    size_t index;
    if (!owner_is_valid(owner) || owner->faulted ||
        !ucn_v6_principal_is_valid(principal) ||
        !ucn_v6_binding_key_is_valid(binding) || session_generation == 0U ||
        session_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        ingress_link_generation == 0U ||
        ingress_link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        peer_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        const ucn_v6_cached_peer_capability_t *peer = &owner->peers[index];
        if (peer->valid && principal_equal(&peer->principal, principal) &&
            ucn_v6_binding_key_equal(&peer->binding, binding) &&
            peer->session_generation == session_generation &&
            peer->ingress_link_generation == ingress_link_generation) {
            if (!ucn_v6_capability_cached_peer_is_live(peer, now_us)) {
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
    uint32_t destination_session_generation,
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
        destination_session_generation == 0U ||
        destination_session_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        route_generation == 0U ||
        route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        path_id == 0U || path_id == UINT16_MAX || path_generation == 0U ||
        path_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        path_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        const ucn_v6_path_capability_t *path = &owner->paths[index];
        if (path->valid &&
            principal_equal(&path->destination_principal,
                            destination_principal) &&
            ucn_v6_binding_key_equal(&path->destination_binding,
                                     destination_binding) &&
            path->destination_session_generation ==
                destination_session_generation &&
            path->route_generation == route_generation &&
            path->path_id == path_id &&
            path->path_generation == path_generation) {
            if (ucn_v6_capability_copy_peer(
                    owner, now_us,
                    &path->local_parent_session.principal,
                    &path->local_parent_session.binding,
                    path->local_parent_session.session_generation,
                    path->local_parent_link_generation, &peer) != UCN_V6_OK ||
                !ucn_v6_capability_path_is_live(path, &peer, now_us)) {
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
    size_t projected_count;
    ucn_v6_stack_invalidation_t checked;
    if (!owner_is_valid(owner) || owner->faulted) {
        return UCN_V6_ERR_ARGUMENT;
    }
    /* First validate and project the whole coalesced invalidation batch.  No
     * cached authority is removed unless every required old-parent event
     * fits.  Expiring peers and standalone paths are disjoint, so descendants
     * of one generated event cannot be counted by another generated event. */
    projected_count = owner->invalidation_count;
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        if (peer_lease_expired(&owner->peers[index], now_us)) {
            if (!invalidation_from_peer(
                    &owner->peers[index],
                    UCN_V6_STACK_INVALIDATE_CAPABILITY, &checked)) {
                owner->faulted = true;
                return UCN_V6_ERR_STATE;
            }
            if (!invalidation_project_enqueue(
                    owner, &checked, &projected_count)) {
                increment_saturated(&owner->stats.rejected_capacity);
                return UCN_V6_ERR_NO_SPACE;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid &&
            owner->paths[index].deadline_us != 0U &&
            now_us >= owner->paths[index].deadline_us) {
            const ucn_v6_cached_peer_capability_t *peer =
                find_path_peer(owner, &owner->paths[index]);
            bool peer_expires = peer_lease_expired(peer, now_us);
            if (peer == NULL ||
                (!peer_expires && !invalidation_from_path(
                    peer, &owner->paths[index], &checked))) {
                owner->faulted = true;
                return UCN_V6_ERR_STATE;
            }
            if (!peer_expires &&
                !invalidation_project_enqueue(
                    owner, &checked, &projected_count)) {
                increment_saturated(&owner->stats.rejected_capacity);
                return UCN_V6_ERR_NO_SPACE;
            }
        }
    }
    if (projected_count > UCN_V6_CAPABILITY_INVALIDATION_DEPTH) {
        increment_saturated(&owner->stats.rejected_capacity);
        return UCN_V6_ERR_NO_SPACE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        if (peer_lease_expired(&owner->peers[index], now_us)) {
            (void)invalidation_from_peer(
                &owner->peers[index],
                UCN_V6_STACK_INVALIDATE_CAPABILITY, &checked);
            if (!invalidation_enqueue(owner, &checked)) {
                owner->faulted = true;
                return UCN_V6_ERR_STATE;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid &&
            owner->paths[index].deadline_us != 0U &&
            now_us >= owner->paths[index].deadline_us) {
            const ucn_v6_cached_peer_capability_t *peer =
                find_path_peer(owner, &owner->paths[index]);
            bool peer_expires = peer_lease_expired(peer, now_us);
            if (!peer_expires) {
                (void)invalidation_from_path(
                    peer, &owner->paths[index], &checked);
                if (!invalidation_enqueue(owner, &checked)) {
                    owner->faulted = true;
                    return UCN_V6_ERR_STATE;
                }
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        if (peer_lease_expired(&owner->peers[index], now_us)) {
            /* Lease expiry is not parent retirement.  Preserve the exact
             * Session-bound generation/digest history so a later ADVERTISE
             * in the same parent domain must be an exact refresh or the
             * checked-next generation.  Zero deadlines also mark this
             * invalidation as emitted, preventing duplicate queue entries. */
            invalidate_paths_for_parent_principal(
                owner, &owner->peers[index].principal);
            owner->peers[index].discovery_deadline_us = 0U;
            owner->peers[index].capability_deadline_us = 0U;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid &&
            owner->paths[index].deadline_us != 0U &&
            now_us >= owner->paths[index].deadline_us) {
            /* A Path lease is a child-liveness grant, not retirement of its
             * generation domain.  Retain the immutable key and generation so
             * a stale route transaction cannot reinstall an old generation.
             * Link/Session/Capability parent retirement reclaims the slot. */
            owner->paths[index].deadline_us = 0U;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS; ++index) {
        if (owner->hints[index].occupied &&
            now_us >= owner->hints[index].deadline_us) {
            memset(&owner->hints[index], 0, sizeof(owner->hints[index]));
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS; ++index) {
        ucn_v6_group_hint_link_budget_t *link = &owner->hint_links[index];
        size_t hint_index;
        bool referenced = false;
        if (!link->occupied || now_us < link->last_activity_us ||
            now_us - link->last_activity_us <
                UCN_V6_GROUP_HINT_TIMEOUT_US) {
            continue;
        }
        for (hint_index = 0U;
             hint_index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS;
             ++hint_index) {
            if (owner->hints[hint_index].occupied &&
                owner->hints[hint_index].ingress_link_id ==
                    link->ingress_link_id &&
                owner->hints[hint_index].ingress_link_generation ==
                    link->ingress_link_generation) {
                referenced = true;
                break;
            }
        }
        if (!referenced) {
            memset(link, 0, sizeof(*link));
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS; ++index) {
        ucn_v6_group_hint_group_budget_t *group = &owner->hint_groups[index];
        size_t hint_index;
        bool referenced = false;
        if (!group->occupied || now_us < group->last_activity_us ||
            now_us - group->last_activity_us <
                UCN_V6_GROUP_HINT_TIMEOUT_US) {
            continue;
        }
        for (hint_index = 0U;
             hint_index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS;
             ++hint_index) {
            if (owner->hints[hint_index].occupied &&
                owner->hints[hint_index].ingress_link_id ==
                    group->ingress_link_id &&
                owner->hints[hint_index].ingress_link_generation ==
                    group->ingress_link_generation &&
                owner->hints[hint_index].group_id == group->group_id &&
                owner->hints[hint_index].group_generation ==
                    group->group_generation) {
                referenced = true;
                break;
            }
        }
        if (!referenced) {
            memset(group, 0, sizeof(*group));
        }
    }
    return UCN_V6_OK;
}

static bool peer_matches_invalidation_parent(
    const ucn_v6_cached_peer_capability_t *peer,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    if (!peer_session_key_is_valid(peer) || invalidation == NULL ||
        peer->ingress_link_id != invalidation->link_id ||
        peer->ingress_link_generation !=
            invalidation->link_generation) {
        return false;
    }
    if (invalidation->type == UCN_V6_STACK_INVALIDATE_LINK) {
        return true;
    }
    return ucn_v6_binding_key_equal(&peer->binding,
                                    &invalidation->session.binding) &&
           principal_equal(&peer->principal,
                           &invalidation->session.principal) &&
           peer->session_generation ==
               invalidation->session.session_generation;
}

ucn_v6_result_t ucn_v6_capability_apply_invalidation(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    size_t index;
    if (!owner_is_valid(owner) || owner->faulted ||
        !ucn_v6_stack_invalidation_is_valid(invalidation)) {
        return UCN_V6_ERR_ARGUMENT;
    }

    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        ucn_v6_cached_peer_capability_t *peer = &owner->peers[index];
        if (!peer_matches_invalidation_parent(peer, invalidation)) {
            continue;
        }
        if (invalidation->type == UCN_V6_STACK_INVALIDATE_LINK ||
            invalidation->type == UCN_V6_STACK_INVALIDATE_SESSION) {
            ucn_v6_principal_t retired_principal = peer->principal;
            memset(peer, 0, sizeof(*peer));
            invalidate_paths_for_parent_principal(owner,
                                                  &retired_principal);
            continue;
        }
        if (invalidation->type == UCN_V6_STACK_INVALIDATE_CAPABILITY &&
            peer->record.capability_generation ==
            invalidation->capability_generation) {
            peer->discovery_deadline_us = 0U;
            peer->capability_deadline_us = 0U;
            invalidate_paths_for_parent_principal(owner,
                                                  &peer->principal);
        }
    }

    if (invalidation->type == UCN_V6_STACK_INVALIDATE_LINK) {
        /* Group HELLO is authenticated in a Group context rather than a Peer
         * Session, so its fixed-capacity hints and rate budgets are not found
         * by the peer loop above.  They are nevertheless exact children of
         * the physical Link generation and must retire on reopen. */
        for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS;
             ++index) {
            if (owner->hints[index].occupied &&
                owner->hints[index].ingress_link_id ==
                    invalidation->link_id &&
                owner->hints[index].ingress_link_generation ==
                    invalidation->link_generation) {
                memset(&owner->hints[index], 0, sizeof(owner->hints[index]));
            }
            if (owner->hint_groups[index].occupied &&
                owner->hint_groups[index].ingress_link_id ==
                    invalidation->link_id &&
                owner->hint_groups[index].ingress_link_generation ==
                    invalidation->link_generation) {
                memset(&owner->hint_groups[index], 0,
                       sizeof(owner->hint_groups[index]));
            }
        }
        for (index = 0U; index < UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS;
             ++index) {
            if (owner->hint_links[index].occupied &&
                owner->hint_links[index].ingress_link_id ==
                    invalidation->link_id &&
                owner->hint_links[index].ingress_link_generation ==
                    invalidation->link_generation) {
                memset(&owner->hint_links[index], 0,
                       sizeof(owner->hint_links[index]));
            }
        }
    }

    if (invalidation->type == UCN_V6_STACK_INVALIDATE_PATH) {
        for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
            ucn_v6_path_capability_t *path = &owner->paths[index];
            const ucn_v6_cached_peer_capability_t *peer =
                path->valid ? find_path_peer(owner, path) : NULL;
            if (path->valid &&
                peer != NULL &&
                peer->record.capability_generation ==
                    invalidation->capability_generation &&
                path->path_id == invalidation->path_id &&
                path->path_generation == invalidation->path_generation &&
                path->local_parent_link_id == invalidation->link_id &&
                path->local_parent_link_generation ==
                    invalidation->link_generation &&
                path->local_parent_session.session_generation ==
                    invalidation->session.session_generation &&
                ucn_v6_binding_key_equal(
                    &path->local_parent_session.binding,
                    &invalidation->session.binding) &&
                principal_equal(&path->local_parent_session.principal,
                                &invalidation->session.principal)) {
                /* PATH revokes liveness only.  The exact generation remains
                 * occupied until its Capability/Session/Link parent retires,
                 * preventing the same child domain from rolling back after a
                 * timeout or delayed invalidation. */
                path->deadline_us = 0U;
            }
        }
    }
    /* A parent invalidation subsumes every queued descendant from that exact
     * parent generation.  Removing those entries is safe after the caller has
     * presented the parent event and prevents an obsolete child backlog from
     * consuming lifetime capacity.  Narrow or older events never remove a
     * broader/newer entry. */
    invalidation_remove_descendants(owner, invalidation);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_invalidation_peek(
    const ucn_v6_capability_owner_t *owner,
    ucn_v6_stack_invalidation_t *invalidation)
{
    if (!owner_is_valid(owner) || invalidation == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (owner->invalidation_count == 0U) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *invalidation = owner->invalidations[owner->invalidation_head];
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_invalidation_ack(
    ucn_v6_capability_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    if (!owner_is_valid(owner) || invalidation == NULL ||
        !ucn_v6_stack_invalidation_is_valid(invalidation)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (owner->invalidation_count == 0U) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (!invalidation_equal(
            &owner->invalidations[owner->invalidation_head], invalidation)) {
        return UCN_V6_ERR_STATE;
    }
    memset(&owner->invalidations[owner->invalidation_head], 0,
           sizeof(owner->invalidations[owner->invalidation_head]));
    owner->invalidation_head = (uint16_t)(
        ((size_t)owner->invalidation_head + 1U) %
        UCN_V6_CAPABILITY_INVALIDATION_DEPTH);
    --owner->invalidation_count;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_capability_copy_view(
    const ucn_v6_capability_owner_t *owner,
    uint64_t now_us,
    ucn_v6_capability_view_t *view)
{
    ucn_v6_capability_view_t next;
    size_t index;
    if (!owner_is_valid(owner) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    next = owner->stats;
    next.occupied_peer_slots = 0U;
    next.live_peers = 0U;
    next.occupied_path_slots = 0U;
    next.live_paths = 0U;
    next.group_hints = 0U;
    next.pending_invalidations = owner->invalidation_count;
    next.faulted = owner->faulted;
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PEERS; ++index) {
        if (owner->peers[index].valid) {
            ++next.occupied_peer_slots;
            if (ucn_v6_capability_cached_peer_is_live(
                    &owner->peers[index], now_us)) {
                ++next.live_peers;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_CAPABILITY_PATHS; ++index) {
        if (owner->paths[index].valid) {
            const ucn_v6_cached_peer_capability_t *peer =
                find_path_peer(owner, &owner->paths[index]);
            ++next.occupied_path_slots;
            if (peer != NULL && ucn_v6_capability_path_is_live(
                                    &owner->paths[index], peer, now_us)) {
                ++next.live_paths;
            }
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
