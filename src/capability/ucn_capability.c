#include "internal/ucn_capability.h"

#include "internal/ucn_checked.h"
#include "internal/ucn_digest.h"

#include <string.h>

#define UCN_I_CAPABILITY_MAGIC UINT32_C(0x55434341)
#define UCN_I_CAPABILITY_MAX_FRAME_BYTES UINT32_C(65678)
#define UCN_I_CAPABILITY_KNOWN_LINK_FLAGS UINT16_C(0x001F)
#define UCN_I_CAPABILITY_KNOWN_TIMESTAMP_BITS UINT16_C(0x000F)
#define UCN_I_CAPABILITY_KNOWN_REALTIME_BITS UINT16_C(0x0007)

/* EN: Only Record68 and Ack75 may use this fixed two-part control envelope.
 * 中文：仅Record68和Ack75允许使用固定两片控制封装。 */
static bool fragment_valid(const ucn_i_capability_fragment_t *v)
{
    uint16_t total;
    uint16_t half;
    if (v == NULL || v->count != 2U || v->index >= 2U) { return false; }
    total = v->opcode == 0x0103U ? 68U : v->opcode == 0x0105U ? 75U : 0U;
    half = (uint16_t)((total + 1U) / 2U);
    return total != 0U && total == v->total_bytes &&
           v->chunk_bytes == (v->index == 0U ? half : total - half);
}

/* EN: Checked shape/alias/capacity; failure changes neither output nor length.
 * 中文：预检形状、别名和容量；失败不写输出及长度。 */
ucn_result_t ucn_i_capability_fragment_encode(
    const ucn_i_capability_fragment_t *value, uint8_t *output,
    size_t capacity, size_t *length_out)
{
    size_t n;
    if (!fragment_valid(value) || output == NULL || length_out == NULL ||
        ucn_i_ranges_overlap(value, sizeof(*value), output, capacity) ||
        ucn_i_ranges_overlap(value, sizeof(*value), length_out, sizeof(*length_out)) ||
        ucn_i_ranges_overlap(output, capacity, length_out, sizeof(*length_out))) {
        return UCN_ERR_ARGUMENT;
    }
    n = UCN_I_CAPABILITY_FRAGMENT_PREFIX + value->chunk_bytes;
    if (capacity < n) { return UCN_ERR_NO_SPACE; }
    output[0] = (uint8_t)(value->opcode >> 8U);
    output[1] = (uint8_t)value->opcode;
    output[2] = (uint8_t)(value->total_bytes >> 8U);
    output[3] = (uint8_t)value->total_bytes;
    output[4] = value->index;
    output[5] = value->count;
    memcpy(output + 6U, value->chunk, value->chunk_bytes);
    *length_out = n;
    return UCN_OK;
}

/* EN: Strict exact decode; do not expose unauthenticated bytes as an Owner proof.
 * 中文：严格精确解码，不把未认证字节当作Owner证明。 */
ucn_result_t ucn_i_capability_fragment_decode(
    const uint8_t *input, size_t length, ucn_i_capability_fragment_t *value_out)
{
    ucn_i_capability_fragment_t v;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, length, value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (length < 6U || length > 44U) { return UCN_ERR_MALFORMED; }
    memset(&v, 0, sizeof(v));
    v.opcode = (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
    v.total_bytes = (uint16_t)(((uint16_t)input[2] << 8U) | input[3]);
    v.index = input[4];
    v.count = input[5];
    v.chunk_bytes = (uint8_t)(length - 6U);
    if (!fragment_valid(&v)) { return UCN_ERR_MALFORMED; }
    memcpy(v.chunk, input + 6U, v.chunk_bytes);
    *value_out = v;
    return UCN_OK;
}

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

static bool profile_select_valid(const ucn_i_profile_select_t *select)
{
    return select != NULL && select->local_generation != 0U &&
           select->peer_generation != 0U &&
           (select->feature_bits & ~UCN_I_CAPABILITY_KNOWN_FEATURES) == 0U &&
           (select->feature_bits & UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES) ==
               UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES &&
           select->hop_suite_bits != 0U &&
           (select->hop_suite_bits & ~UCN_I_CAPABILITY_KNOWN_HOP_SUITES) ==
               0U &&
           select->e2e_suite_bits != 0U &&
           (select->e2e_suite_bits & ~UCN_I_CAPABILITY_KNOWN_E2E_SUITES) ==
               0U &&
           select->max_rx_window != 0U &&
           select->max_concurrent_transfers != 0U &&
           (select->realtime_mode_bits & UINT16_C(0xFFF8)) == 0U &&
           select->max_message_class <= UCN_I_MESSAGE_T8K &&
           select->reserved_zero == 0U &&
           bytes_nonzero(select->local_digest,
                         sizeof(select->local_digest)) &&
           bytes_nonzero(select->peer_digest,
                         sizeof(select->peer_digest));
}

static bool profile_ack_valid(const ucn_i_profile_ack_t *ack)
{
    return ack != NULL && profile_select_valid(&ack->selected) &&
           bytes_nonzero(ack->transcript_digest,
                         sizeof(ack->transcript_digest));
}

/* EN: Compare every semantic Record field; a digest match is only a prefilter.
 * 中文：逐字段精确比较完整Record，摘要一致只作预筛选，不使用padding。 */
static bool record_equal(const ucn_i_capability_record_t *a, const ucn_i_capability_record_t *b)
{
#define SAME_LINK(field_) (a->link.field_ == b->link.field_)
#define SAME_PEER(field_) (a->peer.field_ == b->peer.field_)
    return a->capability_generation == b->capability_generation &&
        SAME_LINK(link_instance_generation) && SAME_LINK(carrier_mtu) && SAME_LINK(link_frame_mtu) &&
        SAME_LINK(processing_frame_mtu) && SAME_LINK(nominal_rate_bps) && SAME_LINK(timestamp_uncertainty_us) &&
        SAME_LINK(carrier_header_bytes) && SAME_LINK(carrier_padding_bytes) && SAME_LINK(carrier_crc_bytes) &&
        SAME_LINK(carrier_tag_bytes) && SAME_LINK(carrier_max_fragments) && SAME_LINK(link_flags) &&
        SAME_LINK(timestamp_capability_bits) && SAME_LINK(hardware_priority_count) &&
        SAME_PEER(feature_bits) && SAME_PEER(hop_suite_bits) && SAME_PEER(e2e_suite_bits) &&
        SAME_PEER(clock_domain_generation) && SAME_PEER(max_rx_window) && SAME_PEER(max_concurrent_transfers) &&
        SAME_PEER(realtime_mode_bits) && SAME_PEER(clock_domain_id) && SAME_PEER(max_message_class);
#undef SAME_LINK
#undef SAME_PEER
}

/* EN: Serialize full SELECT cause, never struct padding or a caller digest alone.
 * 中文：序列化完整SELECT因果域，不使用结构padding或仅信任调用方摘要。 */
/* Canonical header encoding is shared as one codec step; no by-value DTO.
 * 规范头作为独立编码步骤，避免保留多份参数和字段临时值。 */
static void select_context_encode(const ucn_i_capability_select_context_t *context,
                                  uint8_t bytes[93])
{
    put32(bytes,0U,context->realm);
    put32(bytes,4U,context->source_address);put32(bytes,8U,context->source_generation);
    memcpy(bytes+12U,context->source_principal,16U);
    put32(bytes,28U,context->destination_address);put32(bytes,32U,context->destination_generation);
    memcpy(bytes+36U,context->destination_principal,16U);
    put32(bytes,52U,context->session);put32(bytes,56U,context->key);put32(bytes,60U,context->policy);
    bytes[64]=context->origin;memcpy(bytes+65U,context->fingerprint,16U);
    put32(bytes,81U,(uint32_t)(context->transaction>>32U));put32(bytes,85U,(uint32_t)context->transaction);
    put16(bytes,89U,context->service);put16(bytes,91U,UINT16_C(0x0104));
}

ucn_result_t ucn_i_capability_select_transcript(
    const ucn_i_capability_select_context_t *context,
    const ucn_i_profile_select_t *selected,
    uint8_t digest_out[16], ucn_i_sha256_workspace_t *work)
{
    static const uint8_t domain[] = "UCN6-CAPABILITY-SELECT-V1";
    uint8_t bytes[93];
    ucn_result_t result;
    if (context == NULL || selected == NULL || digest_out == NULL || work == NULL ||
        !profile_select_valid(selected) || context->realm == 0U || context->realm == UINT32_MAX ||
        context->source_address == 0U || context->destination_address == 0U ||
        context->source_address == UINT32_MAX || context->destination_address == UINT32_MAX ||
        context->source_address == context->destination_address ||
        context->source_generation == 0U || context->destination_generation == 0U ||
        !bytes_nonzero(context->source_principal,16U) || !bytes_nonzero(context->destination_principal,16U) ||
        memcmp(context->source_principal,context->destination_principal,16U) == 0 ||
        context->session == 0U || context->key == 0U || context->policy == 0U ||
        context->origin < 1U || context->origin > 2U || !bytes_nonzero(context->fingerprint,16U) ||
        context->transaction == 0U || context->service == 0U || context->service == UINT16_MAX ||
        ucn_i_ranges_overlap(context,sizeof(*context),digest_out,16U) ||
        ucn_i_ranges_overlap(selected,sizeof(*selected),digest_out,16U) ||
        ucn_i_ranges_overlap(context,sizeof(*context),work,sizeof(*work)) ||
        ucn_i_ranges_overlap(selected,sizeof(*selected),work,sizeof(*work)) ||
        ucn_i_ranges_overlap(digest_out,16U,work,sizeof(*work))) return UCN_ERR_ARGUMENT;
    result=ucn_i_sha256_begin(work);
    if(result!=UCN_OK)return result;
    result=ucn_i_sha256_append(work,domain,sizeof(domain)-1U);
    put32(bytes,0U,152U);
    if(result==UCN_OK)result=ucn_i_sha256_append(work,bytes,4U);
    select_context_encode(context,bytes);
    if(result==UCN_OK)result=ucn_i_sha256_append(work,bytes,93U);
    if(result==UCN_OK)result=ucn_i_profile_select_encode(selected,bytes);
    if(result==UCN_OK)result=ucn_i_sha256_append(work,bytes,59U);
    if(result!=UCN_OK)return result;
    return ucn_i_sha256_finish128(work,digest_out);
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

ucn_result_t ucn_i_profile_select_encode(
    const ucn_i_profile_select_t *select,
    uint8_t output[UCN_I_PROFILE_SELECT_BYTES])
{
    uint8_t bytes[UCN_I_PROFILE_SELECT_BYTES];

    if (!profile_select_valid(select) || output == NULL ||
        ucn_i_ranges_overlap(select, sizeof(*select), output,
                             UCN_I_PROFILE_SELECT_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(bytes, 0, sizeof(bytes));
    put32(bytes, 0U, select->local_generation);
    put32(bytes, 4U, select->peer_generation);
    put32(bytes, 8U, select->feature_bits);
    put32(bytes, 12U, select->hop_suite_bits);
    put32(bytes, 16U, select->e2e_suite_bits);
    put16(bytes, 20U, select->max_rx_window);
    put16(bytes, 22U, select->max_concurrent_transfers);
    put16(bytes, 24U, select->realtime_mode_bits);
    bytes[26] = select->max_message_class;
    memcpy(&bytes[27], select->local_digest,
           sizeof(select->local_digest));
    memcpy(&bytes[43], select->peer_digest,
           sizeof(select->peer_digest));
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_profile_select_decode(
    const uint8_t input[UCN_I_PROFILE_SELECT_BYTES],
    ucn_i_profile_select_t *select_out)
{
    ucn_i_profile_select_t select;

    if (input == NULL || select_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_PROFILE_SELECT_BYTES,
                             select_out, sizeof(*select_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&select, 0, sizeof(select));
    select.local_generation = get32(input, 0U);
    select.peer_generation = get32(input, 4U);
    select.feature_bits = get32(input, 8U);
    select.hop_suite_bits = get32(input, 12U);
    select.e2e_suite_bits = get32(input, 16U);
    select.max_rx_window = get16(input, 20U);
    select.max_concurrent_transfers = get16(input, 22U);
    select.realtime_mode_bits = get16(input, 24U);
    select.max_message_class = input[26];
    memcpy(select.local_digest, &input[27],
           sizeof(select.local_digest));
    memcpy(select.peer_digest, &input[43],
           sizeof(select.peer_digest));
    if (!profile_select_valid(&select)) {
        return UCN_ERR_MALFORMED;
    }
    *select_out = select;
    return UCN_OK;
}

ucn_result_t ucn_i_profile_ack_encode(
    const ucn_i_profile_ack_t *ack,
    uint8_t output[UCN_I_PROFILE_ACK_BYTES])
{
    uint8_t bytes[UCN_I_PROFILE_ACK_BYTES];
    ucn_result_t result;

    if (!profile_ack_valid(ack) || output == NULL ||
        ucn_i_ranges_overlap(ack, sizeof(*ack), output,
                             UCN_I_PROFILE_ACK_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_profile_select_encode(&ack->selected, bytes);
    if (result != UCN_OK) {
        return result;
    }
    memcpy(&bytes[UCN_I_PROFILE_SELECT_BYTES], ack->transcript_digest,
           sizeof(ack->transcript_digest));
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_profile_ack_decode(
    const uint8_t input[UCN_I_PROFILE_ACK_BYTES],
    ucn_i_profile_ack_t *ack_out)
{
    ucn_i_profile_ack_t ack;
    ucn_result_t result;

    if (input == NULL || ack_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_PROFILE_ACK_BYTES,
                             ack_out, sizeof(*ack_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&ack, 0, sizeof(ack));
    result = ucn_i_profile_select_decode(input, &ack.selected);
    if (result != UCN_OK) {
        return result;
    }
    memcpy(ack.transcript_digest, &input[UCN_I_PROFILE_SELECT_BYTES],
           sizeof(ack.transcript_digest));
    if (!profile_ack_valid(&ack)) {
        return UCN_ERR_MALFORMED;
    }
    *ack_out = ack;
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

static void make_ref(
    const ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *view,
    ucn_i_capability_ref_t *reference)
{
    memset(reference, 0, sizeof(*reference));
    reference->runtime_instance = owner->runtime_instance;
    reference->realm_id = owner->realm_id;
    reference->address = view->address;
    reference->binding_generation = view->binding_generation;
    reference->session_generation = view->session_generation;
    reference->link_id = view->link_id;
    reference->link_generation = view->link_generation;
    reference->security_owner_instance = owner->security_owner_instance;
    memcpy(reference->principal, view->principal, sizeof(reference->principal));
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
    if (!authenticated_valid(owner, authenticated, now_us)) {
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    make_ref(owner, authenticated, &reference);
    for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
        const ucn_i_cached_capability_t *value = &owner->peers[index].value;

        if (owner->peers[index].occupied != 0U && owner->peers[index].revoked == 0U &&
            ref_equal(&value->reference, &reference) &&
            value->record.link.link_instance_generation ==
                summary->link_instance_generation &&
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

static ucn_result_t advertise_ingest_impl(
    ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *authenticated,
    const ucn_i_capability_record_t *record,
    uint64_t now_us,
    ucn_i_capability_ref_t *reference_out,
    const ucn_i_capability_ref_t *predecessor)
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
        (predecessor != NULL &&
         (ucn_i_ranges_overlap(owner, sizeof(*owner), predecessor,
                               sizeof(*predecessor)) ||
          ucn_i_ranges_overlap(predecessor, sizeof(*predecessor), reference_out,
                               sizeof(*reference_out)))) ||
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
    /* Authenticated ingress is local; the signed Record's Link is remote.
     * 认证入口属于本端；签名Record内的Link属于对端，代际不可跨节点比较。 */
    if (!authenticated_valid(owner, authenticated, now_us)) {
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    make_ref(owner, authenticated, &reference);
    if (predecessor != NULL) {
        if (predecessor->reserved_zero != 0U ||
            reference.runtime_instance != predecessor->runtime_instance ||
            reference.security_owner_instance != predecessor->security_owner_instance ||
            reference.realm_id != predecessor->realm_id ||
            reference.address != predecessor->address ||
            reference.binding_generation != predecessor->binding_generation ||
            memcmp(reference.principal, predecessor->principal, 16U) != 0 ||
            reference.session_generation <= predecessor->session_generation) {
            owner_unlock(owner);
            return UCN_ERR_REPLAY;
        }
        for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
            if (owner->peers[index].occupied != 0U &&
                ref_equal(&owner->peers[index].value.reference, predecessor)) {
                empty = index;
                break;
            }
        }
        if (empty == UINT16_MAX) {
            owner_unlock(owner);
            return UCN_ERR_NOT_FOUND;
        }
    }
    for (index = 0U; index < UCN_I_CAPABILITY_PEER_COUNT; ++index) {
        if (predecessor != NULL && index == empty) {
            continue;
        }
        if (owner->peers[index].occupied == 0U) {
            if (empty == UINT16_MAX) {
                empty = index;
            }
            continue;
        }
        if (ref_equal(&owner->peers[index].value.reference, &reference)) {
            const ucn_i_cached_capability_t *previous =
                &owner->peers[index].value;

            if (record->link.link_instance_generation !=
                previous->record.link.link_instance_generation) {
                owner_unlock(owner);
                return UCN_ERR_SECURITY;
            }

            if (record->capability_generation ==
                previous->record.capability_generation) {
                if (owner->peers[index].revoked != 0U) {
                    owner_unlock(owner); return UCN_ERR_REPLAY;
                }
                if (memcmp(digest, previous->digest, 16U) != 0 || !record_equal(record,&previous->record)) {
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
                   reference.principal, sizeof(reference.principal)) == 0 ||
            (predecessor != NULL &&
             owner->peers[index].value.reference.address == reference.address)) {
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
    owner->peers[empty].revoked = 0U;
    owner->peers[empty].expired_notified = 0U;
    *reference_out = reference;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_capability_advertise_ingest(
    ucn_i_capability_owner_t *owner,
    const ucn_i_authenticated_peer_view_t *authenticated,
    const ucn_i_capability_record_t *record,
    uint64_t now_us, ucn_i_capability_ref_t *reference_out)
{
    return advertise_ingest_impl(owner, authenticated, record, now_us,
                                  reference_out, NULL);
}

/* EN: Replace the exact retained parent with a strictly newer authenticated one.
 * ZH: 用严格更新的认证父域替换精确旧父域，不先清槽，不继承旧Profile/租期。 */
ucn_result_t ucn_i_capability_replace_session(
    ucn_i_capability_owner_t *owner,
    const ucn_i_capability_ref_t *predecessor,
    const ucn_i_authenticated_peer_view_t *authenticated,
    const ucn_i_capability_record_t *record,
    uint64_t now_us, ucn_i_capability_ref_t *reference_out)
{
    if (predecessor == NULL) { return UCN_ERR_ARGUMENT; }
    return advertise_ingest_impl(owner, authenticated, record, now_us,
                                  reference_out, predecessor);
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
            if (owner->peers[index].revoked != 0U) {
                owner_unlock(owner); return UCN_ERR_STATE;
            }
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

/* EN: Keep the generation/digest high-water until the parent Session is fenced.
 * 中文：保留代际/摘要高水位，只有父Session真正围栏后才允许显式清槽。 */
ucn_result_t ucn_i_capability_revoke(
    ucn_i_capability_owner_t *owner,
    const ucn_i_capability_ref_t *reference,
    const ucn_i_capability_query_t *exact)
{
    uint16_t index;
    ucn_result_t result;
    if (owner == NULL || reference == NULL || exact == NULL ||
        exact->requested_generation == 0U || !query_valid(exact) ||
        ucn_i_ranges_overlap(owner,sizeof(*owner),reference,sizeof(*reference)) ||
        ucn_i_ranges_overlap(owner,sizeof(*owner),exact,sizeof(*exact))) {
        return UCN_ERR_ARGUMENT;
    }
    result=owner_lock(owner); if (result != UCN_OK) return result;
    for (index=0U; index<UCN_I_CAPABILITY_PEER_COUNT; ++index) {
        const ucn_i_cached_capability_t *previous=&owner->peers[index].value;
        if (owner->peers[index].occupied != 0U && ref_equal(&previous->reference,reference)) {
            if (exact->requested_generation != previous->record.capability_generation ||
                memcmp(exact->known_digest,previous->digest,16U) != 0) {
                owner_unlock(owner); return UCN_ERR_SECURITY;
            }
            owner->peers[index].revoked=1U;
            owner_unlock(owner); return UCN_OK;
        }
    }
    owner_unlock(owner); return UCN_ERR_NOT_FOUND;
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
        if (owner->peers[index].occupied != 0U && owner->peers[index].expired_notified == 0U &&
            (now_us >= owner->peers[index].value.discovery_deadline_us ||
             now_us >= owner->peers[index].value.capability_deadline_us)) {
            expired_reference = owner->peers[index].value.reference;
            /* EN: Expiry is notification, never same-Session high-water deletion.
             * 中文：到期只通知，不删除同Session防回退高水位。 */
            owner->peers[index].expired_notified = 1U;
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
