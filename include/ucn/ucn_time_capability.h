#ifndef UCN_TIME_CAPABILITY_H
#define UCN_TIME_CAPABILITY_H

/* Optional bounded capability/session/path lease cache for Realtime v1.
 * 可选的 Realtime v1 有界能力、Session 与 Path 租约缓存。 */

#include "ucn/ucn_time_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_TIME_CAP_TIME_META_V1 UINT32_C(0x00000001)
#define UCN_TIME_CAP_SYNC_CLIENT_V1 UINT32_C(0x00000002)
#define UCN_TIME_CAP_SYNC_MASTER_V1 UINT32_C(0x00000004)
#define UCN_TIME_CAP_SAMPLE_HW_CAPTURE UINT32_C(0x00000008)
#define UCN_TIME_CAP_LINK_RX_HW_STAMP UINT32_C(0x00000010)
#define UCN_TIME_CAP_LINK_TX_HW_STAMP UINT32_C(0x00000020)
#define UCN_TIME_CAP_HOP_DEADLINE_V1 UINT32_C(0x00000040)
#define UCN_TIME_CAP_KNOWN_MASK UINT32_C(0x0000007F)

#ifndef UCN_TIME_CAPABILITY_CACHE_SIZE
#define UCN_TIME_CAPABILITY_CACHE_SIZE ((size_t)4U)
#endif

typedef char ucn_time_capability_cache_size_must_be_1_to_255[
    UCN_TIME_CAPABILITY_CACHE_SIZE >= 1U &&
    UCN_TIME_CAPABILITY_CACHE_SIZE <= 255U ? 1 : -1];

typedef struct ucn_time_capability_lease {
    ucn_node_id_t destination_node_id;
    ucn_session_id_t destination_session_id;
    ucn_endpoint_t endpoint;
    uint8_t metadata_version;
    ucn_realtime_mode_t mode;
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint32_t capabilities;
    ucn_time_path_identity_t forward_path;
    ucn_time_path_identity_t reverse_path;
    uint64_t expires_at_us;
    bool occupied;
} ucn_time_capability_lease_t;

typedef struct ucn_time_capability_cache {
    ucn_time_capability_lease_t entries[UCN_TIME_CAPABILITY_CACHE_SIZE];
    uint32_t installed;
    uint32_t replaced;
    uint32_t expired;
    uint32_t rejected;
} ucn_time_capability_cache_t;

/* EN: Returns true only for a complete cacheable realtime negotiation.
 * 中文：仅当实时协商结果完整且可安全缓存时返回 true。 */
bool ucn_time_capability_lease_is_valid(
    const ucn_time_capability_lease_t *lease);
/* EN: Clears a caller-owned bounded lease cache.
 * 中文：清零由调用方持有的有界能力租约缓存。 */
ucn_result_t ucn_time_capability_cache_init(
    ucn_time_capability_cache_t *cache);
/* EN: Installs or renews one exact live negotiation without eviction.
 * 中文：安装或续期一条精确且有效的协商结果，不淘汰其他租约。 */
ucn_result_t ucn_time_capability_cache_install(
    ucn_time_capability_cache_t *cache,
    const ucn_time_capability_lease_t *lease,
    uint64_t now_us);
/* EN: Checks identity, expiry and required capability bits without mutation.
 * 中文：以只读方式检查身份、有效期和必需能力位。 */
ucn_result_t ucn_time_capability_cache_admit(
    const ucn_time_capability_cache_t *cache,
    const ucn_time_capability_lease_t *required_identity,
    uint32_t required_capabilities,
    uint64_t now_us);
/* EN: Removes every lease whose half-open deadline has been reached.
 * 中文：清除所有已经到达半开截止时间的租约。 */
ucn_result_t ucn_time_capability_cache_step(
    ucn_time_capability_cache_t *cache,
    uint64_t now_us);
/* EN: Invalidates every cached endpoint owned by one destination Node.
 * 中文：使指定目标节点拥有的全部 Endpoint 租约失效。 */
ucn_result_t ucn_time_capability_cache_invalidate_node(
    ucn_time_capability_cache_t *cache,
    ucn_node_id_t destination_node_id);

#ifdef __cplusplus
}
#endif

#endif
