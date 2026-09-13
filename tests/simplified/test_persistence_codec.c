#include "internal/ucn_persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            abort();                                                        \
        }                                                                   \
    } while (0)

static ucn_result_t decode_record(
    const uint8_t *slot,
    size_t slot_bytes,
    uint8_t erased_value,
    const ucn_persist_manifest_entry_t *manifest,
    const uint8_t durable_manifest_digest[UCN_PERSIST_DIGEST_BYTES],
    ucn_i_persist_record_meta_t *meta_out,
    uint8_t *body_out,
    size_t body_capacity,
    ucn_i_persist_codec_workspace_t *workspace)
{
    ucn_i_persist_record_decode_request_t request;
    memset(&request, 0, sizeof(request));
    request.slot = slot;
    request.manifest = manifest;
    request.durable_manifest_digest = durable_manifest_digest;
    request.meta_out = meta_out;
    request.body_out = body_out;
    request.slot_bytes = slot_bytes;
    request.body_capacity = body_capacity;
    request.erased_value = erased_value;
    return ucn_i_persist_record_decode(&request, workspace);
}

static void test_hash_and_crc_oracles(void)
{
    ucn_i_persist_hash_workspace_t workspace;
    static const uint8_t expected_empty[16] = {
        0x64, 0x55, 0x0d, 0x6f, 0xfe, 0x2c, 0x0a, 0x01,
        0xa1, 0x4a, 0xba, 0x1e, 0xad, 0xe0, 0x20, 0x0c};
    static const uint8_t expected_abc[16] = {
        0xaa, 0x49, 0x38, 0x11, 0x9b, 0x1d, 0xc7, 0xb8,
        0x7c, 0xba, 0xd0, 0xff, 0xd2, 0x00, 0xd0, 0xae};
    static const uint8_t crc_input[] = "123456789";
    uint8_t digest[16];

    ucn_i_persist_blake2s128(NULL, 0U, digest, &workspace);
    CHECK(memcmp(digest, expected_empty, sizeof(digest)) == 0);
    ucn_i_persist_blake2s128((const uint8_t *)"abc", 3U, digest,
                             &workspace);
    CHECK(memcmp(digest, expected_abc, sizeof(digest)) == 0);
    CHECK(ucn_i_persist_crc32c(crc_input, sizeof(crc_input) - 1U) ==
           UINT32_C(0xE3069283));
}

static void test_manifest_and_record_golden(void)
{
    ucn_i_persist_codec_workspace_t workspace;
    ucn_persistence_digest_workspace_t manifest_workspace;
#if UCN_PROFILE == UCN_PROFILE_NANO
    static const uint8_t expected_manifest[16] = {
        0xeb, 0xf7, 0x53, 0x4c, 0x0e, 0x86, 0x04, 0x6f,
        0xaa, 0xb0, 0x69, 0x79, 0xf9, 0xad, 0x11, 0xf8};
#elif UCN_PROFILE == UCN_PROFILE_LITE
    static const uint8_t expected_manifest[16] = {
        0x07, 0x44, 0x68, 0x3f, 0xef, 0x36, 0x09, 0xa9,
        0x39, 0x6e, 0x25, 0x3b, 0x71, 0x65, 0x38, 0xcf};
#elif UCN_PROFILE == UCN_PROFILE_FULL
    static const uint8_t expected_manifest[16] = {
        0x7b, 0x6f, 0x51, 0x15, 0xb8, 0x8f, 0x80, 0x8a,
        0x1b, 0x81, 0x73, 0xd3, 0x2e, 0x71, 0xc8, 0x85};
#else
#error "UCN_PROFILE must select one manifest Golden"
#endif
    static const uint8_t expected_body_digest[16] = {
        0xe4, 0x81, 0x6b, 0x54, 0xf0, 0x28, 0xeb, 0x51,
        0x21, 0x25, 0x31, 0x14, 0xc1, 0x86, 0x12, 0xdc};
    static const uint8_t body[] = {1U, 2U, 3U};
    ucn_persist_manifest_entry_t entry;
    ucn_persist_manifest_t manifest;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_record_meta_t decoded;
    uint8_t digest[16];
    uint8_t slot[UCN_PERSIST_SLOT_BYTES];
    uint8_t decoded_body[UCN_PERSIST_BODY_BYTES];
    uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES];
    uint8_t saved;

    memset(&entry, 0, sizeof(entry));
    entry.struct_size = sizeof(entry);
    entry.api_version = UCN_PERSIST_API_VERSION;
    entry.domain.domain_kind = UCN_PERSIST_DOMAIN_PRODUCT_CONFIG;
    entry.domain.domain_id = 1U;
    entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    entry.schema_id = 1U;
    entry.schema_version = 1U;
    entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    entry.provider_atomicity_class = UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;

    memset(&manifest, 0, sizeof(manifest));
    manifest.struct_size = sizeof(manifest);
    manifest.api_version = UCN_PERSIST_API_VERSION;
    manifest.protocol_manifest_version = UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    manifest.entries = &entry;
    manifest.entry_count = 1U;
    manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &manifest, &manifest_workspace, digest) == UCN_OK);
    CHECK(memcmp(digest, expected_manifest, sizeof(digest)) == 0);

    memset(&meta, 0, sizeof(meta));
    meta.domain = entry.domain;
    meta.record_generation = 1U;
    meta.transaction_id = 5U;
    meta.body_bytes = sizeof(body);
    meta.schema_id = 1U;
    meta.schema_version = 1U;
    meta.operation_kind = 2U;
    CHECK(ucn_i_persist_body_digest(&meta, body, digest, &workspace) == UCN_OK);
    CHECK(memcmp(digest, expected_body_digest, sizeof(digest)) == 0);
    CHECK(ucn_i_persist_record_encode(
               &meta, expected_manifest, body, UCN_PERSIST_BODY_BYTES,
               sizeof(slot), 0xFFU, slot, &workspace) == UCN_OK);
    CHECK(memcmp(slot, "UC6R\x00\x01\x00\x60", 8U) == 0);
    CHECK(slot[8] == 0U && slot[9] == UCN_PERSIST_DOMAIN_PRODUCT_CONFIG);
    CHECK(memcmp(&slot[16], expected_manifest, 16U) == 0);
    CHECK(memcmp(&slot[72], expected_body_digest, 16U) == 0);
    CHECK(memcmp(&slot[96], body, sizeof(body)) == 0);
    CHECK(ucn_i_persist_marker_encode(1U, marker) == UCN_OK);
    memcpy(&slot[sizeof(slot) - sizeof(marker)], marker, sizeof(marker));
    memset(&decoded, 0xA5, sizeof(decoded));
    memset(decoded_body, 0xA5, sizeof(decoded_body));
    CHECK(decode_record(
               slot, sizeof(slot), 0xFFU, &entry, expected_manifest,
               &decoded, decoded_body, sizeof(decoded_body), &workspace) ==
           UCN_OK);
    CHECK(decoded.record_generation == 1U);
    CHECK(decoded.transaction_id == 5U);
    CHECK(memcmp(decoded_body, body, sizeof(body)) == 0);

    saved = slot[14];
    slot[14] = 1U;
    memset(&decoded, 0xA5, sizeof(decoded));
    memset(decoded_body, 0xA5, sizeof(decoded_body));
    CHECK(decode_record(
               slot, sizeof(slot), 0xFFU, &entry, expected_manifest,
               &decoded, decoded_body, sizeof(decoded_body), &workspace) ==
           UCN_ERR_MALFORMED);
    CHECK(((const uint8_t *)&decoded)[0] == 0xA5U);
    CHECK(decoded_body[0] == 0xA5U);
    slot[14] = saved;

    slot[96] ^= 1U;
    CHECK(decode_record(
               slot, sizeof(slot), 0xFFU, &entry, expected_manifest,
               &decoded, decoded_body, sizeof(decoded_body), &workspace) ==
           UCN_ERR_MALFORMED);
}

static void test_codec_aliases_fail_before_write(void)
{
    ucn_i_persist_codec_workspace_t workspace;
    ucn_persistence_digest_workspace_t manifest_workspace;
    ucn_persist_manifest_entry_t entry;
    ucn_persist_manifest_t manifest;
    ucn_i_persist_record_meta_t meta;
    static const uint8_t body[] = {0x91U};
    uint8_t manifest_digest[UCN_PERSIST_DIGEST_BYTES];
    uint8_t slot[UCN_PERSIST_SLOT_BYTES];
    uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES];
    union {
        uint64_t alignment;
        uint8_t bytes[sizeof(ucn_i_persist_record_meta_t) + 8U];
    } overlapping_outputs;

    memset(&entry, 0, sizeof(entry));
    entry.struct_size = sizeof(entry);
    entry.api_version = UCN_PERSIST_API_VERSION;
    entry.domain.domain_kind = UCN_PERSIST_DOMAIN_PRODUCT_CONFIG;
    entry.domain.domain_id = 1U;
    entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    entry.schema_id = 1U;
    entry.schema_version = 1U;
    entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    entry.provider_atomicity_class = UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    memset(&manifest, 0, sizeof(manifest));
    manifest.struct_size = sizeof(manifest);
    manifest.api_version = UCN_PERSIST_API_VERSION;
    manifest.protocol_manifest_version = UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    manifest.entries = &entry;
    manifest.entry_count = 1U;
    manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &manifest, &manifest_workspace, manifest_digest) == UCN_OK);
    memset(manifest.expected_digest, 0xA5, sizeof(manifest.expected_digest));
    CHECK(ucn_persistence_manifest_digest(
              &manifest, &manifest_workspace, manifest.expected_digest) ==
          UCN_ERR_CONFIG);
    CHECK(manifest.expected_digest[0] == 0xA5U);

    memset(&meta, 0, sizeof(meta));
    meta.domain = entry.domain;
    meta.record_generation = 1U;
    meta.transaction_id = 1U;
    meta.body_bytes = sizeof(body);
    meta.schema_id = 1U;
    meta.schema_version = 1U;
    meta.operation_kind = 1U;
    CHECK(ucn_i_persist_record_encode(
               &meta, manifest_digest, body, UCN_PERSIST_BODY_BYTES,
               sizeof(slot), 0xFFU, slot, &workspace) == UCN_OK);
    CHECK(ucn_i_persist_marker_encode(1U, marker) == UCN_OK);
    memcpy(&slot[sizeof(slot) - sizeof(marker)], marker, sizeof(marker));
    memset(&overlapping_outputs, 0xA5, sizeof(overlapping_outputs));
    CHECK(decode_record(
               slot, sizeof(slot), 0xFFU, &entry, manifest_digest,
               (ucn_i_persist_record_meta_t *)overlapping_outputs.bytes,
               &overlapping_outputs.bytes[1], 1U, &workspace) ==
           UCN_ERR_ARGUMENT);
    CHECK(overlapping_outputs.bytes[0] == 0xA5U);
    CHECK(overlapping_outputs.bytes[1] == 0xA5U);
}

static void test_record_field_negative_matrix(void)
{
    ucn_i_persist_codec_workspace_t workspace;
    ucn_persistence_digest_workspace_t manifest_workspace;
    ucn_persist_manifest_entry_t entry;
    ucn_persist_manifest_t manifest;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_record_meta_t decoded;
    static const uint8_t body[] = {0x11U, 0x22U, 0x33U};
    uint8_t manifest_digest[UCN_PERSIST_DIGEST_BYTES];
    uint8_t slot[UCN_PERSIST_SLOT_BYTES + 1U];
    uint8_t original[UCN_PERSIST_SLOT_BYTES];
    uint8_t decoded_body[UCN_PERSIST_BODY_BYTES];
    uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES];
    size_t offsets[24];
    const size_t marker_offset =
        UCN_PERSIST_SLOT_BYTES - UCN_PERSIST_COMMIT_MARKER_BYTES;
    size_t index;

    memset(&entry, 0, sizeof(entry));
    entry.struct_size = sizeof(entry);
    entry.api_version = UCN_PERSIST_API_VERSION;
    entry.domain.domain_kind = UCN_PERSIST_DOMAIN_PRODUCT_CONFIG;
    entry.domain.domain_id = 1U;
    entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    entry.schema_id = 1U;
    entry.schema_version = 1U;
    entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    entry.provider_atomicity_class = UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    memset(&manifest, 0, sizeof(manifest));
    manifest.struct_size = sizeof(manifest);
    manifest.api_version = UCN_PERSIST_API_VERSION;
    manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    manifest.entries = &entry;
    manifest.entry_count = 1U;
    manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &manifest, &manifest_workspace, manifest_digest) == UCN_OK);
    memset(&meta, 0, sizeof(meta));
    meta.domain = entry.domain;
    meta.record_generation = 1U;
    meta.transaction_id = 1U;
    meta.body_bytes = sizeof(body);
    meta.schema_id = 1U;
    meta.schema_version = 1U;
    meta.operation_kind = 1U;
    CHECK(ucn_i_persist_record_encode(
               &meta, manifest_digest, body, UCN_PERSIST_BODY_BYTES,
               UCN_PERSIST_SLOT_BYTES, 0xFFU, slot, &workspace) == UCN_OK);
    CHECK(ucn_i_persist_marker_encode(1U, marker) == UCN_OK);
    memcpy(&slot[marker_offset], marker, sizeof(marker));
    memcpy(original, slot, sizeof(original));

    offsets[0] = 0U;   /* magic */
    offsets[1] = 4U;   /* envelope version */
    offsets[2] = 6U;   /* envelope bytes */
    offsets[3] = 8U;   /* domain kind */
    offsets[4] = 10U;  /* schema id */
    offsets[5] = 12U;  /* schema version */
    offsets[6] = 14U;  /* reserved */
    offsets[7] = 16U;  /* manifest digest */
    offsets[8] = 32U;  /* domain id */
    offsets[9] = 40U;  /* record generation */
    offsets[10] = 48U; /* transaction id */
    offsets[11] = 56U; /* operation kind */
    offsets[12] = 58U; /* digest suite */
    offsets[13] = 60U; /* body bytes */
    offsets[14] = 64U; /* body CRC */
    offsets[15] = 68U; /* header CRC */
    offsets[16] = 72U; /* body digest */
    offsets[17] = 88U; /* reserved */
    offsets[18] = UCN_PERSIST_ENVELOPE_BYTES; /* body */
    offsets[19] = UCN_PERSIST_ENVELOPE_BYTES + sizeof(body); /* erased tail */
    offsets[20] = marker_offset;       /* marker magic */
    offsets[21] = marker_offset + 4U;  /* marker generation */
    offsets[22] = marker_offset + 12U; /* marker CRC */
    offsets[23] = marker_offset + 15U; /* marker CRC final byte */
    for (index = 0U; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        memcpy(slot, original, sizeof(original));
        slot[offsets[index]] ^= 1U;
        memset(&decoded, 0xA5, sizeof(decoded));
        memset(decoded_body, 0xA5, sizeof(decoded_body));
        CHECK(decode_record(
                   slot, UCN_PERSIST_SLOT_BYTES, 0xFFU, &entry,
                   manifest_digest, &decoded, decoded_body,
                   sizeof(decoded_body), &workspace) == UCN_ERR_MALFORMED);
        CHECK(((const uint8_t *)&decoded)[0] == 0xA5U);
        CHECK(decoded_body[0] == 0xA5U);
    }
    memcpy(slot, original, sizeof(original));
    memset(&decoded, 0xA5, sizeof(decoded));
    memset(decoded_body, 0xA5, sizeof(decoded_body));
    CHECK(decode_record(
               slot, UCN_PERSIST_SLOT_BYTES - 1U, 0xFFU, &entry,
               manifest_digest, &decoded, decoded_body,
               sizeof(decoded_body), &workspace) == UCN_ERR_ARGUMENT);
    CHECK(((const uint8_t *)&decoded)[0] == 0xA5U);
    CHECK(decoded_body[0] == 0xA5U);
    slot[UCN_PERSIST_SLOT_BYTES] = 0xA5U;
    CHECK(decode_record(
               slot, UCN_PERSIST_SLOT_BYTES + 1U, 0xFFU, &entry,
               manifest_digest, &decoded, decoded_body,
               sizeof(decoded_body), &workspace) == UCN_ERR_ARGUMENT);
    CHECK(((const uint8_t *)&decoded)[0] == 0xA5U);
    CHECK(decoded_body[0] == 0xA5U);
}

static void test_manifest_registry_is_fail_closed(void)
{
    ucn_persistence_digest_workspace_t manifest_workspace;
    ucn_persist_manifest_entry_t entry;
    ucn_persist_manifest_t manifest;
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];

    memset(&entry, 0, sizeof(entry));
    entry.struct_size = sizeof(entry);
    entry.api_version = UCN_PERSIST_API_VERSION;
    entry.domain.domain_kind = UCN_PERSIST_DOMAIN_PRODUCT_CONFIG;
    entry.domain.domain_id = 1U;
    entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    entry.schema_id = 1U;
    entry.schema_version = 1U;
    entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    entry.provider_atomicity_class = UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    memset(&manifest, 0, sizeof(manifest));
    manifest.struct_size = sizeof(manifest);
    manifest.api_version = UCN_PERSIST_API_VERSION;
    manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    manifest.entries = &entry;
    manifest.entry_count = 1U;
    manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &manifest, &manifest_workspace, digest) == UCN_OK);

    memset(digest, 0xA5, sizeof(digest));
    manifest.protocol_manifest_version++;
    CHECK(ucn_persistence_manifest_digest(&manifest, &manifest_workspace,
                                          digest) ==
           UCN_ERR_CONFIG);
    CHECK(digest[0] == 0xA5U);
    manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    manifest.composition_feature_bits ^= UINT64_C(0x100);
    CHECK(ucn_persistence_manifest_digest(&manifest, &manifest_workspace,
                                          digest) ==
           UCN_ERR_CONFIG);
    CHECK(digest[0] == 0xA5U);
    manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    entry.digest_suite++;
    CHECK(ucn_persistence_manifest_digest(&manifest, &manifest_workspace,
                                          digest) ==
           UCN_ERR_CONFIG);
    entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    entry.witness_policy++;
    CHECK(ucn_persistence_manifest_digest(&manifest, &manifest_workspace,
                                          digest) ==
           UCN_ERR_CONFIG);
    entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    entry.provider_atomicity_class++;
    CHECK(ucn_persistence_manifest_digest(&manifest, &manifest_workspace,
                                          digest) ==
           UCN_ERR_CONFIG);
}

static void test_manifest_workspace_aliases_fail_before_write(void)
{
    ucn_persistence_digest_workspace_t workspace;
    ucn_persist_manifest_entry_t entry;
    ucn_persist_manifest_t manifest;
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    uint8_t before[sizeof(workspace)];
    union {
        uint64_t alignment;
        uint8_t bytes[UCN_PERSIST_DIGEST_WORKSPACE_BYTES + 128U];
    } overlap;
    ucn_persistence_digest_workspace_t *overlap_workspace;
    ucn_persist_manifest_t *overlap_manifest;
    ucn_persist_manifest_entry_t *overlap_entry;

    memset(&entry, 0, sizeof(entry));
    entry.struct_size = sizeof(entry);
    entry.api_version = UCN_PERSIST_API_VERSION;
    entry.domain.domain_kind = UCN_PERSIST_DOMAIN_PRODUCT_CONFIG;
    entry.domain.domain_id = 1U;
    entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    entry.schema_id = 1U;
    entry.schema_version = 1U;
    entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    entry.provider_atomicity_class = UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    memset(&manifest, 0, sizeof(manifest));
    manifest.struct_size = sizeof(manifest);
    manifest.api_version = UCN_PERSIST_API_VERSION;
    manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    manifest.entries = &entry;
    manifest.entry_count = 1U;
    manifest.profile_id = UCN_PROFILE;

    memset(digest, 0xA5, sizeof(digest));
    CHECK(ucn_persistence_manifest_digest(&manifest, NULL, digest) ==
          UCN_ERR_CONFIG);
    CHECK(digest[0] == 0xA5U);

    memset(&workspace, 0x5A, sizeof(workspace));
    memcpy(before, &workspace, sizeof(before));
    CHECK(ucn_persistence_manifest_digest(
              &manifest, &workspace, &workspace.bytes[1]) == UCN_ERR_CONFIG);
    CHECK(memcmp(&workspace, before, sizeof(before)) == 0);

    memset(&overlap, 0, sizeof(overlap));
    overlap_workspace =
        (ucn_persistence_digest_workspace_t *)&overlap.bytes[0];
    overlap_manifest = (ucn_persist_manifest_t *)&overlap.bytes[128];
    *overlap_manifest = manifest;
    memcpy(before, overlap.bytes, sizeof(before));
    memset(digest, 0xA5, sizeof(digest));
    CHECK(ucn_persistence_manifest_digest(
              overlap_manifest, overlap_workspace, digest) ==
          UCN_ERR_CONFIG);
    CHECK(memcmp(overlap.bytes, before, sizeof(before)) == 0);
    CHECK(digest[0] == 0xA5U);

    memset(&overlap, 0, sizeof(overlap));
    overlap_workspace =
        (ucn_persistence_digest_workspace_t *)&overlap.bytes[0];
    overlap_entry =
        (ucn_persist_manifest_entry_t *)&overlap.bytes[128];
    *overlap_entry = entry;
    manifest.entries = overlap_entry;
    memcpy(before, overlap.bytes, sizeof(before));
    memset(digest, 0xA5, sizeof(digest));
    CHECK(ucn_persistence_manifest_digest(
              &manifest, overlap_workspace, digest) == UCN_ERR_CONFIG);
    CHECK(memcmp(overlap.bytes, before, sizeof(before)) == 0);
    CHECK(digest[0] == 0xA5U);
}

int main(void)
{
    test_hash_and_crc_oracles();
    test_manifest_and_record_golden();
    test_codec_aliases_fail_before_write();
    test_record_field_negative_matrix();
    test_manifest_registry_is_fail_closed();
    test_manifest_workspace_aliases_fail_before_write();
    return 0;
}
