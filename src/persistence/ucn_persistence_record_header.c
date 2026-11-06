#include "internal/ucn_persistence.h"
#include <string.h>

static bool ranges_overlap(const void *left,
                           size_t left_bytes,
                           const void *right,
                           size_t right_bytes)
{
    const uintptr_t left_start = (uintptr_t)left;
    const uintptr_t right_start = (uintptr_t)right;
    if (left == NULL || right == NULL || left_bytes == 0U || right_bytes == 0U) {
        return false;
    }
    if (left_start > UINTPTR_MAX - left_bytes ||
        right_start > UINTPTR_MAX - right_bytes) {
        return true;
    }
    return left_start < right_start + right_bytes &&
           right_start < left_start + left_bytes;
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | bytes[3];
}

static uint64_t read_be64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | bytes[index];
    }
    return value;
}

static uint32_t crc32c_update(uint32_t crc,
                              const uint8_t *bytes,
                              size_t length)
{
    size_t index;
    uint8_t bit;
    for (index = 0U; index < length; ++index) {
        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0x82F63B78) & mask);
        }
    }
    return crc;
}

static bool domain_key_is_valid(const ucn_persist_domain_key_t *key)
{
    return key->domain_kind >= UCN_PERSIST_DOMAIN_IDENTITY_BINDING &&
           key->domain_kind <= UCN_PERSIST_DOMAIN_PRODUCT_CONFIG &&
           key->domain_id != 0U && key->domain_id != UINT64_MAX &&
           key->reserved_zero == 0U && key->reserved_zero2 == 0U;
}

static bool manifest_entry_is_valid(const ucn_persist_manifest_entry_t *entry)
{
    return entry != NULL && entry->struct_size == sizeof(*entry) &&
           entry->api_version == UCN_PERSIST_API_VERSION &&
           domain_key_is_valid(&entry->domain) && entry->schema_id != 0U &&
           entry->schema_version != 0U &&
           entry->digest_suite == UCN_PERSIST_DIGEST_BLAKE2S_128 &&
           entry->witness_policy ==
               UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC &&
           entry->provider_atomicity_class ==
               UCN_PERSIST_ATOMIC_COMMIT_MARKER_16 &&
           entry->body_capacity_bytes <= UCN_PERSIST_BODY_BYTES &&
           entry->slot_capacity_bytes >=
               UCN_PERSIST_ENVELOPE_BYTES + UCN_PERSIST_COMMIT_MARKER_BYTES &&
           entry->slot_capacity_bytes <= UCN_PERSIST_SLOT_BYTES &&
           entry->body_capacity_bytes <=
               entry->slot_capacity_bytes - UCN_PERSIST_ENVELOPE_BYTES -
                   UCN_PERSIST_COMMIT_MARKER_BYTES;
}

static uint32_t record_header_crc(const uint8_t *slot)
{
    static const uint8_t zero_crc_field[4] = {0U, 0U, 0U, 0U};
    uint32_t crc = crc32c_update(UINT32_MAX, slot, 68U);
    crc = crc32c_update(crc, zero_crc_field, sizeof(zero_crc_field));
    return ~crc32c_update(crc, &slot[72], UCN_PERSIST_ENVELOPE_BYTES - 72U);
}

ucn_result_t ucn_i_persist_record_header_decode(
    const ucn_i_persist_record_decode_request_t *request,
    ucn_i_persist_codec_workspace_t *workspace)
{
    ucn_i_persist_record_meta_t *meta;
    size_t marker_offset;

    if (request == NULL || workspace == NULL ||
        ranges_overlap(request, sizeof(*request), workspace,
                       sizeof(*workspace))) {
        return UCN_ERR_ARGUMENT;
    }
    if (request->slot == NULL || request->manifest == NULL ||
        request->durable_manifest_digest == NULL ||
        request->meta_out == NULL ||
        !manifest_entry_is_valid(request->manifest) ||
        request->slot_bytes != request->manifest->slot_capacity_bytes ||
        request->slot_bytes > UCN_PERSIST_SLOT_BYTES ||
        ranges_overlap(request, sizeof(*request), request->meta_out,
                       sizeof(*request->meta_out)) ||
        ranges_overlap(request, sizeof(*request), request->body_out,
                       request->body_capacity) ||
        ranges_overlap(request->slot, request->slot_bytes,
                       request->meta_out, sizeof(*request->meta_out)) ||
        ranges_overlap(request->slot, request->slot_bytes,
                       request->body_out, request->body_capacity) ||
        ranges_overlap(request->meta_out, sizeof(*request->meta_out),
                       request->body_out, request->body_capacity) ||
        ranges_overlap(workspace, sizeof(*workspace), request->slot,
                       request->slot_bytes) ||
        ranges_overlap(workspace, sizeof(*workspace), request->manifest,
                       sizeof(*request->manifest)) ||
        ranges_overlap(workspace, sizeof(*workspace),
                       request->durable_manifest_digest,
                       UCN_PERSIST_DIGEST_BYTES) ||
        ranges_overlap(workspace, sizeof(*workspace), request->meta_out,
                       sizeof(*request->meta_out)) ||
        ranges_overlap(workspace, sizeof(*workspace), request->body_out,
                       request->body_capacity)) {
        return UCN_ERR_ARGUMENT;
    }
    meta = &workspace->meta;
    marker_offset = request->slot_bytes - UCN_PERSIST_COMMIT_MARKER_BYTES;
    if (ucn_i_persist_marker_decode(&request->slot[marker_offset],
                                    &meta->record_generation) != UCN_OK ||
        read_be64(&request->slot[40]) != meta->record_generation ||
        memcmp(request->slot, "UC6R", 4U) != 0 ||
        read_be16(&request->slot[4]) != UCN_PERSIST_RECORD_ENVELOPE_VERSION ||
        read_be16(&request->slot[6]) != UCN_PERSIST_ENVELOPE_BYTES ||
        read_be16(&request->slot[8]) != request->manifest->domain.domain_kind ||
        read_be16(&request->slot[10]) != request->manifest->schema_id ||
        read_be16(&request->slot[12]) != request->manifest->schema_version ||
        read_be16(&request->slot[14]) != 0U ||
        memcmp(&request->slot[16], request->durable_manifest_digest,
               UCN_PERSIST_DIGEST_BYTES) != 0 ||
        read_be64(&request->slot[32]) != request->manifest->domain.domain_id ||
        read_be16(&request->slot[58]) !=
            UCN_I_PERSIST_DIGEST_SUITE_BLAKE2S_128 ||
        !ucn_i_persist_bytes_are_value(&request->slot[88], 8U, 0U)) {
        return UCN_ERR_MALFORMED;
    }
    memset(meta, 0, sizeof(*meta));
    meta->domain = request->manifest->domain;
    meta->schema_id = read_be16(&request->slot[10]);
    meta->schema_version = read_be16(&request->slot[12]);
    meta->record_generation = read_be64(&request->slot[40]);
    meta->transaction_id = read_be64(&request->slot[48]);
    meta->operation_kind = read_be16(&request->slot[56]);
    meta->body_bytes = read_be32(&request->slot[60]);
    if (meta->record_generation == 0U ||
        meta->record_generation == UINT64_MAX || meta->transaction_id == 0U ||
        meta->transaction_id == UINT64_MAX || meta->operation_kind == 0U ||
        meta->body_bytes > request->manifest->body_capacity_bytes ||
        meta->body_bytes > request->body_capacity ||
        meta->body_bytes > marker_offset - UCN_PERSIST_ENVELOPE_BYTES ||
        (meta->body_bytes != 0U && request->body_out == NULL)) {
        return UCN_ERR_MALFORMED;
    }
    return read_be32(&request->slot[68]) == record_header_crc(request->slot)
               ? UCN_OK : UCN_ERR_MALFORMED;
}
