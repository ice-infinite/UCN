#include "internal/ucn_persistence.h"

#include <string.h>

#if defined(_MSC_VER)
#define UCN_I_ALWAYS_INLINE __forceinline
#define UCN_I_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define UCN_I_ALWAYS_INLINE inline __attribute__((always_inline))
#define UCN_I_NOINLINE __attribute__((noinline))
#else
#define UCN_I_ALWAYS_INLINE inline
#define UCN_I_NOINLINE
#endif

UCN_STATIC_ASSERT(sizeof(ucn_i_persist_hash_workspace_t) <=
                      UCN_I_PERSIST_MANIFEST_DIGEST_OFFSET,
                  persistence_codec_hash_and_digest_must_not_overlap);

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

typedef ucn_i_persist_hash_workspace_t ucn_i_blake2s_t;

static const uint32_t blake2s_iv[8] = {
    UINT32_C(0x6A09E667), UINT32_C(0xBB67AE85),
    UINT32_C(0x3C6EF372), UINT32_C(0xA54FF53A),
    UINT32_C(0x510E527F), UINT32_C(0x9B05688C),
    UINT32_C(0x1F83D9AB), UINT32_C(0x5BE0CD19)};

static const uint8_t blake2s_sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

/* Column G calls followed by diagonal G calls, in the RFC order.
 * 先列后对角，保留BLAKE2s原始G顺序；每次仅保留四个工作字。 */
static const uint8_t blake2s_mix[8][4] = {
    {0,4,8,12}, {1,5,9,13}, {2,6,10,14}, {3,7,11,15},
    {0,5,10,15}, {1,6,11,12}, {2,7,8,13}, {3,4,9,14}};

static UCN_I_ALWAYS_INLINE uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static uint32_t rotr32(uint32_t value, uint8_t count)
{
    return (value >> count) | (value << (32U - count));
}

#define B2S_G(a_, b_, c_, d_, x_, y_)                                    \
    do {                                                                  \
        (a_) = (a_) + (b_) + (x_);                                       \
        (d_) = rotr32((d_) ^ (a_), 16U);                                 \
        (c_) = (c_) + (d_);                                               \
        (b_) = rotr32((b_) ^ (c_), 12U);                                 \
        (a_) = (a_) + (b_) + (y_);                                       \
        (d_) = rotr32((d_) ^ (a_), 8U);                                  \
        (c_) = (c_) + (d_);                                               \
        (b_) = rotr32((b_) ^ (c_), 7U);                                  \
    } while (0)

static void blake2s_compress(ucn_i_blake2s_t *state,
                             bool final)
{
    uint8_t round;
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        state->v[index] = state->h[index];
        state->v[index + 8U] = blake2s_iv[index];
    }
    state->v[12] ^= state->t[0];
    state->v[13] ^= state->t[1];
    if (final) {
        state->v[14] = ~state->v[14];
    }
    for (round = 0U; round < 10U; ++round) {
        const uint8_t *s = blake2s_sigma[round];
        for (index = 0U; index < 8U; ++index) {
            const uint8_t *positions = blake2s_mix[index];
            uint32_t a = state->v[positions[0]];
            uint32_t b = state->v[positions[1]];
            uint32_t c = state->v[positions[2]];
            uint32_t d = state->v[positions[3]];
            B2S_G(a,b,c,d,read_le32(&state->buffer[(size_t)s[2U*index]*4U]),
                  read_le32(&state->buffer[(size_t)s[2U*index+1U]*4U]));
            state->v[positions[0]] = a;
            state->v[positions[1]] = b;
            state->v[positions[2]] = c;
            state->v[positions[3]] = d;
        }
    }
    for (index = 0U; index < 8U; ++index) {
        state->h[index] ^= state->v[index] ^ state->v[index + 8U];
    }
}

static void blake2s_add_count(ucn_i_blake2s_t *state, uint32_t add)
{
    const uint32_t old = state->t[0];
    state->t[0] += add;
    if (state->t[0] < old) {
        ++state->t[1];
    }
}

static void blake2s_init(ucn_i_blake2s_t *state)
{
    uint8_t index;
    memset(state, 0, sizeof(*state));
    for (index = 0U; index < 8U; ++index) {
        state->h[index] = blake2s_iv[index];
    }
    state->h[0] ^= UINT32_C(0x01010010);
}

static void blake2s_update(ucn_i_blake2s_t *state,
                           const uint8_t *bytes,
                           size_t length)
{
    while (length > 0U) {
        const size_t available = 64U - state->buffered;
        const size_t take = length < available ? length : available;
        memcpy(&state->buffer[state->buffered], bytes, take);
        state->buffered += take;
        bytes += take;
        length -= take;
        if (state->buffered == 64U && length > 0U) {
            blake2s_add_count(state, 64U);
            blake2s_compress(state, false);
            state->buffered = 0U;
        }
    }
}

static void blake2s_update_byte(ucn_i_blake2s_t *state, uint8_t byte)
{
    if (state->buffered == 64U) {
        blake2s_add_count(state, 64U);
        blake2s_compress(state, false);
        state->buffered = 0U;
    }
    state->buffer[state->buffered] = byte;
    ++state->buffered;
}

static void blake2s_final(ucn_i_blake2s_t *state,
                          uint8_t digest[UCN_PERSIST_DIGEST_BYTES])
{
    uint8_t index;
    blake2s_add_count(state, (uint32_t)state->buffered);
    memset(&state->buffer[state->buffered], 0, 64U - state->buffered);
    blake2s_compress(state, true);
    for (index = 0U; index < UCN_PERSIST_DIGEST_BYTES / 4U; ++index) {
        write_le32(&digest[(size_t)index * 4U], state->h[index]);
    }
    memset(state, 0, sizeof(*state));
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void write_be64(uint8_t *bytes, uint64_t value)
{
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (56U - (uint32_t)index * 8U));
    }
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

uint32_t ucn_i_persist_crc32c(const uint8_t *bytes, size_t length)
{
    if (bytes == NULL && length != 0U) {
        return 0U;
    }
    return ~crc32c_update(UINT32_MAX, bytes, length);
}

void ucn_i_persist_blake2s128(const uint8_t *bytes,
                              size_t length,
                              uint8_t digest[UCN_PERSIST_DIGEST_BYTES],
                              ucn_i_persist_hash_workspace_t *workspace)
{
    if ((bytes == NULL && length != 0U) || digest == NULL ||
        workspace == NULL ||
        ranges_overlap(workspace, sizeof(*workspace), bytes, length) ||
        ranges_overlap(workspace, sizeof(*workspace), digest,
                       UCN_PERSIST_DIGEST_BYTES)) {
        return;
    }
    blake2s_init(workspace);
    if (length != 0U) {
        blake2s_update(workspace, bytes, length);
    }
    blake2s_final(workspace, digest);
}

bool ucn_i_persist_bytes_are_value(const uint8_t *bytes,
                                   size_t length,
                                   uint8_t value)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

bool ucn_i_persist_domain_equal(ucn_persist_domain_key_t left,
                                ucn_persist_domain_key_t right)
{
    return left.domain_id == right.domain_id &&
           left.domain_kind == right.domain_kind &&
           left.reserved_zero == 0U && left.reserved_zero2 == 0U &&
           right.reserved_zero == 0U && right.reserved_zero2 == 0U;
}

static bool domain_key_is_valid(const ucn_persist_domain_key_t *key)
{
    return key->domain_kind >= UCN_PERSIST_DOMAIN_IDENTITY_BINDING &&
           key->domain_kind <= UCN_PERSIST_DOMAIN_PRODUCT_CONFIG &&
           key->domain_id != 0U && key->domain_id != UINT64_MAX &&
           key->reserved_zero == 0U && key->reserved_zero2 == 0U;
}

static void hash_be16(ucn_i_blake2s_t *state, uint16_t value)
{
    blake2s_update_byte(state, (uint8_t)(value >> 8U));
    blake2s_update_byte(state, (uint8_t)value);
}

static void hash_be32(ucn_i_blake2s_t *state, uint32_t value)
{
    blake2s_update_byte(state, (uint8_t)(value >> 24U));
    blake2s_update_byte(state, (uint8_t)(value >> 16U));
    blake2s_update_byte(state, (uint8_t)(value >> 8U));
    blake2s_update_byte(state, (uint8_t)value);
}

static void hash_be64(ucn_i_blake2s_t *state, uint64_t value)
{
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        blake2s_update_byte(
            state, (uint8_t)(value >> (56U - (uint32_t)index * 8U)));
    }
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

static UCN_I_NOINLINE ucn_result_t manifest_digest_validated(
    const ucn_persist_manifest_t *manifest,
    ucn_i_blake2s_t *hash,
    uint8_t digest_out[UCN_PERSIST_DIGEST_BYTES])
{
    static const uint8_t domain[] = "UCN6-DURABLE-MANIFEST-V1";
    uint16_t index;

    blake2s_init(hash);
    blake2s_update(hash, domain, sizeof(domain) - 1U);
    hash_be32(hash, manifest->protocol_manifest_version);
    hash_be32(hash, manifest->storage_layout_version);
    hash_be64(hash, manifest->composition_feature_bits);
    blake2s_update(hash, &manifest->profile_id, 1U);
    hash_be16(hash, manifest->entry_count);
    for (index = 0U; index < manifest->entry_count; ++index) {
        const ucn_persist_manifest_entry_t *entry = &manifest->entries[index];
        hash_be16(hash, entry->domain.domain_kind);
        hash_be64(hash, entry->domain.domain_id);
        hash_be16(hash, entry->schema_id);
        hash_be16(hash, entry->schema_version);
        hash_be32(hash, entry->body_capacity_bytes);
        hash_be32(hash, entry->slot_capacity_bytes);
        blake2s_update(hash, &entry->witness_policy, 1U);
        blake2s_update(hash, &entry->provider_atomicity_class, 1U);
        hash_be16(hash, entry->digest_suite);
    }
    blake2s_final(hash, digest_out);
    return UCN_OK;
}

ucn_result_t ucn_persistence_manifest_digest(
    const ucn_persist_manifest_t *manifest,
    ucn_persistence_digest_workspace_t *workspace,
    uint8_t digest_out[UCN_PERSIST_DIGEST_BYTES])
{
    ucn_i_blake2s_t *hash;
    uint16_t index;

    if (manifest == NULL || workspace == NULL || digest_out == NULL ||
        sizeof(*hash) > sizeof(workspace->bytes) ||
        manifest->struct_size != sizeof(*manifest) ||
        manifest->api_version != UCN_PERSIST_API_VERSION ||
        manifest->protocol_manifest_version !=
            UCN_PERSIST_PROTOCOL_MANIFEST_VERSION ||
        manifest->storage_layout_version != UCN_PERSIST_STORAGE_LAYOUT ||
        manifest->composition_feature_bits != UCN_COMPILED_FEATURE_MASK ||
        manifest->profile_id != UCN_PROFILE || manifest->reserved_zero != 0U ||
        manifest->entry_count == 0U ||
        manifest->entry_count > UCN_PERSIST_DOMAIN_COUNT ||
        manifest->entries == NULL ||
        ranges_overlap(manifest, sizeof(*manifest), digest_out,
                       UCN_PERSIST_DIGEST_BYTES) ||
        ranges_overlap(manifest->entries,
                       (size_t)manifest->entry_count *
                           sizeof(manifest->entries[0]),
                       digest_out, UCN_PERSIST_DIGEST_BYTES) ||
        ranges_overlap(workspace, sizeof(*workspace), manifest,
                       sizeof(*manifest)) ||
        ranges_overlap(workspace, sizeof(*workspace), manifest->entries,
                       (size_t)manifest->entry_count *
                           sizeof(manifest->entries[0])) ||
        (ranges_overlap(workspace, sizeof(*workspace), digest_out,
                        UCN_PERSIST_DIGEST_BYTES) &&
         digest_out !=
             &workspace->bytes[UCN_I_PERSIST_MANIFEST_DIGEST_OFFSET])) {
        return UCN_ERR_CONFIG;
    }
    for (index = 0U; index < manifest->entry_count; ++index) {
        if (!manifest_entry_is_valid(&manifest->entries[index])) {
            return UCN_ERR_CONFIG;
        }
        if (index != 0U) {
            const ucn_persist_domain_key_t previous =
                manifest->entries[index - 1U].domain;
            const ucn_persist_domain_key_t current =
                manifest->entries[index].domain;
            if (current.domain_kind < previous.domain_kind ||
                (current.domain_kind == previous.domain_kind &&
                 current.domain_id <= previous.domain_id)) {
                return UCN_ERR_CONFIG;
            }
        }
    }

    hash = (ucn_i_blake2s_t *)(void *)workspace->bytes;
    return manifest_digest_validated(manifest, hash, digest_out);
}

ucn_result_t ucn_i_persist_body_digest(
    const ucn_i_persist_record_meta_t *meta,
    const uint8_t *body,
    uint8_t digest_out[UCN_PERSIST_DIGEST_BYTES],
    ucn_i_persist_codec_workspace_t *workspace)
{
    static const uint8_t domain[] = "UCN6-PERSIST-BODY-V1";
    if (meta == NULL || digest_out == NULL ||
        workspace == NULL ||
        (meta->body_bytes != 0U && body == NULL) ||
        (meta != &workspace->meta &&
         ranges_overlap(workspace, sizeof(*workspace), meta, sizeof(*meta))) ||
        ranges_overlap(workspace, sizeof(*workspace), body,
                       meta->body_bytes) ||
        (digest_out != workspace->digest &&
         ranges_overlap(workspace, sizeof(*workspace), digest_out,
                        UCN_PERSIST_DIGEST_BYTES)) ||
        !domain_key_is_valid(&meta->domain) || meta->schema_id == 0U ||
        meta->schema_version == 0U || meta->record_generation == 0U ||
        meta->transaction_id == 0U || meta->transaction_id == UINT64_MAX ||
        meta->operation_kind == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    blake2s_init(&workspace->hash);
    blake2s_update(&workspace->hash, domain, sizeof(domain) - 1U);
    write_be16(&workspace->canonical[0], meta->domain.domain_kind);
    write_be64(&workspace->canonical[2], meta->domain.domain_id);
    write_be16(&workspace->canonical[10], meta->schema_id);
    write_be16(&workspace->canonical[12], meta->schema_version);
    write_be64(&workspace->canonical[14], meta->record_generation);
    write_be64(&workspace->canonical[22], meta->transaction_id);
    write_be16(&workspace->canonical[30], meta->operation_kind);
    write_be32(&workspace->canonical[32], meta->body_bytes);
    blake2s_update(&workspace->hash, workspace->canonical,
                   sizeof(workspace->canonical));
    if (meta->body_bytes != 0U) {
        blake2s_update(&workspace->hash, body, meta->body_bytes);
    }
    blake2s_final(&workspace->hash, digest_out);
    return UCN_OK;
}

ucn_result_t ucn_i_persist_marker_encode(
    uint64_t record_generation,
    uint8_t marker_out[UCN_PERSIST_COMMIT_MARKER_BYTES])
{
    uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES];
    if (record_generation == 0U || record_generation == UINT64_MAX ||
        marker_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    memcpy(marker, "UC6C", 4U);
    write_be64(&marker[4], record_generation);
    write_be32(&marker[12], ucn_i_persist_crc32c(marker, 12U));
    memcpy(marker_out, marker, sizeof(marker));
    return UCN_OK;
}

ucn_result_t ucn_i_persist_marker_decode(
    const uint8_t marker[UCN_PERSIST_COMMIT_MARKER_BYTES],
    uint64_t *record_generation_out)
{
    uint64_t generation;
    if (marker == NULL || record_generation_out == NULL ||
        ranges_overlap(marker, UCN_PERSIST_COMMIT_MARKER_BYTES,
                       record_generation_out,
                       sizeof(*record_generation_out)) ||
        memcmp(marker, "UC6C", 4U) != 0 ||
        read_be32(&marker[12]) != ucn_i_persist_crc32c(marker, 12U)) {
        return UCN_ERR_MALFORMED;
    }
    generation = read_be64(&marker[4]);
    if (generation == 0U || generation == UINT64_MAX) {
        return UCN_ERR_MALFORMED;
    }
    *record_generation_out = generation;
    return UCN_OK;
}

ucn_result_t ucn_i_persist_record_encode(
    const ucn_i_persist_record_encode_request_t *request,
    ucn_i_persist_codec_workspace_t *workspace)
{
    const ucn_i_persist_record_meta_t *meta;
    const uint8_t *manifest_digest;
    const uint8_t *body;
    size_t body_capacity, slot_bytes, marker_offset;
    uint8_t erased_value, *slot_out;
    uint32_t header_crc;
    if (request == NULL || workspace == NULL ||
        ranges_overlap(request,sizeof(*request),workspace,sizeof(*workspace)) ||
        ranges_overlap(workspace,sizeof(*workspace),request->slot_out,request->slot_bytes) ||
        ranges_overlap(workspace,sizeof(*workspace),request->manifest_digest,UCN_PERSIST_DIGEST_BYTES) ||
        ranges_overlap(request,sizeof(*request),request->slot_out,request->slot_bytes))
        return UCN_ERR_ARGUMENT;
    meta=request->meta; manifest_digest=request->manifest_digest;
    body=request->body; body_capacity=request->body_capacity;
    slot_bytes=request->slot_bytes; erased_value=request->erased_value;
    slot_out=request->slot_out;
    marker_offset = slot_bytes - UCN_PERSIST_COMMIT_MARKER_BYTES;
    if (meta == NULL || manifest_digest == NULL || slot_out == NULL ||
        workspace == NULL ||
        (meta->body_bytes != 0U && body == NULL) ||
        meta->body_bytes > body_capacity ||
        slot_bytes < UCN_PERSIST_ENVELOPE_BYTES +
                         UCN_PERSIST_COMMIT_MARKER_BYTES ||
        meta->body_bytes > marker_offset - UCN_PERSIST_ENVELOPE_BYTES ||
        ucn_i_persist_body_digest(meta, body, workspace->digest,
                                  workspace) != UCN_OK ||
        ranges_overlap(slot_out, slot_bytes, meta, sizeof(*meta)) ||
        ranges_overlap(slot_out, slot_bytes, body, meta->body_bytes) ||
        ranges_overlap(slot_out, slot_bytes, manifest_digest,
                       UCN_PERSIST_DIGEST_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(slot_out, erased_value, slot_bytes);
    memcpy(slot_out, "UC6R", 4U);
    write_be16(&slot_out[4], UCN_PERSIST_RECORD_ENVELOPE_VERSION);
    write_be16(&slot_out[6], UCN_PERSIST_ENVELOPE_BYTES);
    write_be16(&slot_out[8], meta->domain.domain_kind);
    write_be16(&slot_out[10], meta->schema_id);
    write_be16(&slot_out[12], meta->schema_version);
    write_be16(&slot_out[14], 0U);
    memcpy(&slot_out[16], manifest_digest, UCN_PERSIST_DIGEST_BYTES);
    write_be64(&slot_out[32], meta->domain.domain_id);
    write_be64(&slot_out[40], meta->record_generation);
    write_be64(&slot_out[48], meta->transaction_id);
    write_be16(&slot_out[56], meta->operation_kind);
    write_be16(&slot_out[58], UCN_I_PERSIST_DIGEST_SUITE_BLAKE2S_128);
    write_be32(&slot_out[60], meta->body_bytes);
    write_be32(&slot_out[64],
               ucn_i_persist_crc32c(body, meta->body_bytes));
    write_be32(&slot_out[68], 0U);
    memcpy(&slot_out[72], workspace->digest, UCN_PERSIST_DIGEST_BYTES);
    memset(&slot_out[88], 0, 8U);
    if (meta->body_bytes != 0U) {
        memcpy(&slot_out[UCN_PERSIST_ENVELOPE_BYTES], body,
               meta->body_bytes);
    }
    header_crc = ucn_i_persist_crc32c(slot_out,
                                      UCN_PERSIST_ENVELOPE_BYTES);
    write_be32(&slot_out[68], header_crc);
    return UCN_OK;
}


ucn_result_t ucn_i_persist_record_decode(
    const ucn_i_persist_record_decode_request_t *request,
    ucn_i_persist_codec_workspace_t *workspace)
{
    ucn_i_persist_record_meta_t *meta;
    size_t marker_offset;
    const ucn_result_t result = ucn_i_persist_record_header_decode(request, workspace);
    if (result != UCN_OK) return result;
    meta = &workspace->meta;
    marker_offset = request->slot_bytes - UCN_PERSIST_COMMIT_MARKER_BYTES;
    if (read_be32(&request->slot[64]) !=
            ucn_i_persist_crc32c(
                &request->slot[UCN_PERSIST_ENVELOPE_BYTES],
                                  meta->body_bytes) ||
        ucn_i_persist_body_digest(
            meta, &request->slot[UCN_PERSIST_ENVELOPE_BYTES],
            workspace->digest, workspace) != UCN_OK ||
        memcmp(&request->slot[72], workspace->digest,
               UCN_PERSIST_DIGEST_BYTES) != 0 ||
        !ucn_i_persist_bytes_are_value(
            &request->slot[UCN_PERSIST_ENVELOPE_BYTES + meta->body_bytes],
            marker_offset - UCN_PERSIST_ENVELOPE_BYTES - meta->body_bytes,
            request->erased_value)) {
        return UCN_ERR_MALFORMED;
    }
    memcpy(meta->body_digest, workspace->digest,
           UCN_PERSIST_DIGEST_BYTES);
    if (meta->body_bytes != 0U) {
        memcpy(request->body_out,
               &request->slot[UCN_PERSIST_ENVELOPE_BYTES], meta->body_bytes);
    }
    *request->meta_out = *meta;
    return UCN_OK;
}
