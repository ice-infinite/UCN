#ifndef UCN_INTERNAL_DIGEST_H
#define UCN_INTERNAL_DIGEST_H

#include "ucn/ucn_types.h"

#define UCN_I_DIGEST128_BYTES 16U

/* EN: Caller-owned SHA-256 workspace keeps hashing deterministic and avoids
 * dynamic allocation. The v1 canonical digest is the first 16 SHA-256 bytes.
 * 中文：调用方持有 SHA-256 工作区，确保摘要过程确定且无动态内存。v1
 * canonical digest 固定取 SHA-256 的前 16 字节。 */
typedef struct ucn_i_sha256_workspace {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    uint8_t block_bytes;
    uint8_t reserved_zero[7];
} ucn_i_sha256_workspace_t;

ucn_result_t ucn_i_sha256_128(
    const uint8_t *bytes,
    size_t length,
    uint8_t digest_out[UCN_I_DIGEST128_BYTES],
    ucn_i_sha256_workspace_t *workspace);

#endif
