#include "internal/ucn_digest.h"

#include "internal/ucn_checked.h"

#include <string.h>

static const uint32_t sha256_k[64] = {
    UINT32_C(0x428A2F98), UINT32_C(0x71374491), UINT32_C(0xB5C0FBCF),
    UINT32_C(0xE9B5DBA5), UINT32_C(0x3956C25B), UINT32_C(0x59F111F1),
    UINT32_C(0x923F82A4), UINT32_C(0xAB1C5ED5), UINT32_C(0xD807AA98),
    UINT32_C(0x12835B01), UINT32_C(0x243185BE), UINT32_C(0x550C7DC3),
    UINT32_C(0x72BE5D74), UINT32_C(0x80DEB1FE), UINT32_C(0x9BDC06A7),
    UINT32_C(0xC19BF174), UINT32_C(0xE49B69C1), UINT32_C(0xEFBE4786),
    UINT32_C(0x0FC19DC6), UINT32_C(0x240CA1CC), UINT32_C(0x2DE92C6F),
    UINT32_C(0x4A7484AA), UINT32_C(0x5CB0A9DC), UINT32_C(0x76F988DA),
    UINT32_C(0x983E5152), UINT32_C(0xA831C66D), UINT32_C(0xB00327C8),
    UINT32_C(0xBF597FC7), UINT32_C(0xC6E00BF3), UINT32_C(0xD5A79147),
    UINT32_C(0x06CA6351), UINT32_C(0x14292967), UINT32_C(0x27B70A85),
    UINT32_C(0x2E1B2138), UINT32_C(0x4D2C6DFC), UINT32_C(0x53380D13),
    UINT32_C(0x650A7354), UINT32_C(0x766A0ABB), UINT32_C(0x81C2C92E),
    UINT32_C(0x92722C85), UINT32_C(0xA2BFE8A1), UINT32_C(0xA81A664B),
    UINT32_C(0xC24B8B70), UINT32_C(0xC76C51A3), UINT32_C(0xD192E819),
    UINT32_C(0xD6990624), UINT32_C(0xF40E3585), UINT32_C(0x106AA070),
    UINT32_C(0x19A4C116), UINT32_C(0x1E376C08), UINT32_C(0x2748774C),
    UINT32_C(0x34B0BCB5), UINT32_C(0x391C0CB3), UINT32_C(0x4ED8AA4A),
    UINT32_C(0x5B9CCA4F), UINT32_C(0x682E6FF3), UINT32_C(0x748F82EE),
    UINT32_C(0x78A5636F), UINT32_C(0x84C87814), UINT32_C(0x8CC70208),
    UINT32_C(0x90BEFFFA), UINT32_C(0xA4506CEB), UINT32_C(0xBEF9A3F7),
    UINT32_C(0xC67178F2)
};

static uint32_t rotate_right(uint32_t value, uint8_t amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           bytes[3];
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void compress(ucn_i_sha256_workspace_t *workspace,
                     const uint8_t block[64])
{
    uint32_t schedule[16];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint8_t index;

    for (index = 0U; index < 16U; ++index) {
        schedule[index] = read_be32(&block[(size_t)index * 4U]);
    }
    a = workspace->state[0];
    b = workspace->state[1];
    c = workspace->state[2];
    d = workspace->state[3];
    e = workspace->state[4];
    f = workspace->state[5];
    g = workspace->state[6];
    h = workspace->state[7];
    for (index = 0U; index < 64U; ++index) {
        uint32_t word;
        uint32_t s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                      rotate_right(e, 25U);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1;
        uint32_t s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                      rotate_right(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2;

        if (index < 16U) {
            word = schedule[index];
        } else {
            uint32_t left = schedule[(index + 1U) & 15U];
            uint32_t right = schedule[(index + 14U) & 15U];
            uint32_t extend0 = rotate_right(left, 7U) ^
                               rotate_right(left, 18U) ^ (left >> 3U);
            uint32_t extend1 = rotate_right(right, 17U) ^
                               rotate_right(right, 19U) ^ (right >> 10U);

            word = schedule[index & 15U] + extend0 +
                   schedule[(index + 9U) & 15U] + extend1;
            schedule[index & 15U] = word;
        }
        temp1 = h + s1 + choose + sha256_k[index] + word;
        temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    workspace->state[0] += a;
    workspace->state[1] += b;
    workspace->state[2] += c;
    workspace->state[3] += d;
    workspace->state[4] += e;
    workspace->state[5] += f;
    workspace->state[6] += g;
    workspace->state[7] += h;
}

static void initialize(ucn_i_sha256_workspace_t *workspace)
{
    memset(workspace, 0, sizeof(*workspace));
    workspace->state[0] = UINT32_C(0x6A09E667);
    workspace->state[1] = UINT32_C(0xBB67AE85);
    workspace->state[2] = UINT32_C(0x3C6EF372);
    workspace->state[3] = UINT32_C(0xA54FF53A);
    workspace->state[4] = UINT32_C(0x510E527F);
    workspace->state[5] = UINT32_C(0x9B05688C);
    workspace->state[6] = UINT32_C(0x1F83D9AB);
    workspace->state[7] = UINT32_C(0x5BE0CD19);
}

static void update(ucn_i_sha256_workspace_t *workspace,
                   const uint8_t *bytes,
                   size_t length)
{
    size_t offset = 0U;

    while (offset < length) {
        size_t available = 64U - workspace->block_bytes;
        size_t take = length - offset < available ? length - offset : available;

        memcpy(&workspace->block[workspace->block_bytes], &bytes[offset], take);
        workspace->block_bytes = (uint8_t)(workspace->block_bytes + take);
        workspace->total_bytes += take;
        offset += take;
        if (workspace->block_bytes == 64U) {
            compress(workspace, workspace->block);
            workspace->block_bytes = 0U;
        }
    }
}

static void finalize(ucn_i_sha256_workspace_t *workspace,
                     uint8_t digest_out[UCN_I_DIGEST128_BYTES])
{
    uint64_t total_bits = workspace->total_bytes * UINT64_C(8);
    uint8_t index;

    workspace->block[workspace->block_bytes++] = UINT8_C(0x80);
    if (workspace->block_bytes > 56U) {
        memset(&workspace->block[workspace->block_bytes], 0,
               64U - workspace->block_bytes);
        compress(workspace, workspace->block);
        workspace->block_bytes = 0U;
    }
    memset(&workspace->block[workspace->block_bytes], 0,
           56U - workspace->block_bytes);
    for (index = 0U; index < 8U; ++index) {
        workspace->block[63U - index] = (uint8_t)(total_bits >>
                                                   ((uint32_t)index * 8U));
    }
    compress(workspace, workspace->block);
    for (index = 0U; index < 4U; ++index) {
        write_be32(&digest_out[(size_t)index * 4U], workspace->state[index]);
    }
}

ucn_result_t ucn_i_sha256_128(
    const uint8_t *bytes,
    size_t length,
    uint8_t digest_out[UCN_I_DIGEST128_BYTES],
    ucn_i_sha256_workspace_t *workspace)
{
    if ((bytes == NULL && length != 0U) || digest_out == NULL ||
        workspace == NULL ||
        ucn_i_ranges_overlap(bytes, length, digest_out,
                             UCN_I_DIGEST128_BYTES) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), digest_out,
                             UCN_I_DIGEST128_BYTES) ||
        ucn_i_ranges_overlap(bytes, length, workspace, sizeof(*workspace)) ||
        length > UINT64_MAX / UINT64_C(8)) {
        return UCN_ERR_ARGUMENT;
    }
    initialize(workspace);
    if (length != 0U) {
        update(workspace, bytes, length);
    }
    finalize(workspace, digest_out);
    return UCN_OK;
}
