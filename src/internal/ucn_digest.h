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
    /* Input bytes and rolling schedule have disjoint lifetimes.
     * 输入块与滚动调度字生命周期不重叠，复用同一64字节而非另占栈。 */
    union {
        uint8_t block[64];
        uint32_t schedule[16];
    } storage;
    uint8_t block_bytes;
    uint8_t stream_active;
    uint8_t reserved_zero[6];
} ucn_i_sha256_workspace_t;

/* EN: Incremental internal codec hashing; append aliases and retired streams
 * fail without changing state/output. No new workspace bytes or heap.
 * 中文：内部Codec逐段摘要；拒绝别名/已退休流，工作区尺寸不增加。 */
ucn_result_t ucn_i_sha256_begin(ucn_i_sha256_workspace_t *workspace);
ucn_result_t ucn_i_sha256_append(ucn_i_sha256_workspace_t *workspace,
                                const uint8_t *bytes, size_t length);
ucn_result_t ucn_i_sha256_finish128(ucn_i_sha256_workspace_t *workspace,
                                   uint8_t digest_out[16]);

ucn_result_t ucn_i_sha256_128(
    const uint8_t *bytes,
    size_t length,
    uint8_t digest_out[UCN_I_DIGEST128_BYTES],
    ucn_i_sha256_workspace_t *workspace);
/* EN: Full SHA-256 for existing 32-byte Bootstrap transcript chains.
 * 中文：既有32字节Bootstrap前序消息链所用完整SHA-256。 */
ucn_result_t ucn_i_sha256_256(const uint8_t *bytes, size_t length,
    uint8_t digest_out[32], ucn_i_sha256_workspace_t *workspace);

/* EN: Canonical digest = SHA256(domain || u32be(length) || bytes)[0..16].
 * 中文：规范摘要有明确域与长度前缀；不能用裸 SHA 或持久化 BLAKE 摘要替代。 */
ucn_result_t ucn_i_canonical_digest128(
    const uint8_t *domain, size_t domain_bytes,
    const uint8_t *bytes, size_t length,
    uint8_t digest_out[UCN_I_DIGEST128_BYTES],
    ucn_i_sha256_workspace_t *workspace);

#define UCN_I_DIGEST_CANONICAL(domain_, bytes_, length_, out_, work_) \
    ucn_i_canonical_digest128((const uint8_t *)(domain_), sizeof(domain_) - 1U, \
                              (bytes_), (length_), (out_), (work_))

#endif
