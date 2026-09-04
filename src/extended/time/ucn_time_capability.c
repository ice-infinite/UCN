/* Optional bounded Realtime capability lease cache.
 * 可选的有界实时能力租约缓存。 */

#include "ucn/ucn_time_capability.h"

#include <string.h>

/* EN: Saturating-increments one cache diagnostic counter.
 * 中文：对能力缓存诊断计数执行饱和递增。 */
static void increment_saturated(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
}

/* EN: Checks whether a Node ID is a usable unicast identity.
 * 中文：检查 Node ID 是否为可用的单播身份。 */
static bool node_id_is_unicast(ucn_node_id_t id)
{
    return id != 0U && id != UCN_NODE_BROADCAST;
}

/* EN: Compares complete directional Path identities field by field.
 * 中文：逐字段比较完整的定向 Path 身份。 */
static bool path_equal(const ucn_time_path_identity_t *left,
                       const ucn_time_path_identity_t *right)
{
    return left->owner_node_id == right->owner_node_id &&
           left->owner_session_id == right->owner_session_id &&
           left->path_id == right->path_id &&
           left->destination_node_id == right->destination_node_id;
}

static bool identity_equal(const ucn_time_capability_lease_t *actual,
                           const ucn_time_capability_lease_t *required);
static bool negotiation_equal(const ucn_time_capability_lease_t *left,
                              const ucn_time_capability_lease_t *right);

/* EN: Validates a complete cacheable negotiation result.
 * 中文：校验一个完整且可缓存的协商结果。 */
bool ucn_time_capability_lease_is_valid(
    const ucn_time_capability_lease_t *lease)
{
    return lease != NULL && lease->occupied &&
           node_id_is_unicast(lease->destination_node_id) &&
           lease->destination_session_id != 0U &&
           ucn_endpoint_is_static(lease->endpoint) &&
           lease->metadata_version == UCN_REALTIME_ENVELOPE_VERSION &&
           (lease->mode == UCN_REALTIME_MODE_SYNCED_STAMP ||
            lease->mode == UCN_REALTIME_MODE_DEADLINE) &&
           lease->clock_domain_id != 0U &&
           lease->clock_domain_id <= UCN_REALTIME_CLOCK_DOMAIN_ID_MAX &&
           lease->domain_generation != 0U &&
           lease->domain_generation <= UCN_REALTIME_DOMAIN_GENERATION_MAX &&
           lease->capabilities != 0U &&
           (lease->capabilities & ~UCN_TIME_CAP_KNOWN_MASK) == 0U &&
           (lease->capabilities & UCN_TIME_CAP_TIME_META_V1) != 0U &&
           ucn_time_path_identity_is_valid(&lease->forward_path) &&
           ucn_time_path_identity_is_valid(&lease->reverse_path) &&
           lease->forward_path.owner_node_id ==
               lease->reverse_path.destination_node_id &&
           lease->forward_path.destination_node_id ==
               lease->destination_node_id &&
           lease->reverse_path.owner_node_id ==
               lease->destination_node_id &&
           lease->reverse_path.owner_session_id ==
               lease->destination_session_id &&
           lease->expires_at_us != 0U;
}

/* EN: Initializes caller-owned fixed capability-cache storage.
 * 中文：初始化调用者拥有的固定能力缓存。 */
ucn_result_t ucn_time_capability_cache_init(
    ucn_time_capability_cache_t *cache)
{
    if (cache == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    (void)memset(cache, 0, sizeof(*cache));
    return UCN_OK;
}

/* EN: Installs one unexpired exact lease without evicting unrelated entries.
 * 中文：安装一条未过期的精确租约，且不淘汰无关记录。 */
ucn_result_t ucn_time_capability_cache_install(
    ucn_time_capability_cache_t *cache,
    const ucn_time_capability_lease_t *lease,
    uint64_t now_us)
{
    size_t index;
    size_t target = UCN_TIME_CAPABILITY_CACHE_SIZE;
    bool replacement = false;
    bool reclaim_expired = false;

    if (cache == NULL || !ucn_time_capability_lease_is_valid(lease) ||
        now_us >= lease->expires_at_us) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_TIME_CAPABILITY_CACHE_SIZE; ++index) {
        const bool expired = cache->entries[index].occupied &&
            now_us >= cache->entries[index].expires_at_us;

        if (cache->entries[index].occupied && !expired &&
            cache->entries[index].destination_node_id ==
                lease->destination_node_id &&
            cache->entries[index].endpoint == lease->endpoint) {
            if (!negotiation_equal(&cache->entries[index], lease)) {
                increment_saturated(&cache->rejected);
                return UCN_ERR_REPLAY;
            }
            if (lease->expires_at_us <= cache->entries[index].expires_at_us) {
                increment_saturated(&cache->rejected);
                return UCN_ERR_REPLAY;
            }
            target = index;
            replacement = true;
            break;
        }
        if ((!cache->entries[index].occupied || expired) &&
            target == UCN_TIME_CAPABILITY_CACHE_SIZE) {
            target = index;
            reclaim_expired = expired;
        }
    }
    if (target == UCN_TIME_CAPABILITY_CACHE_SIZE) {
        increment_saturated(&cache->rejected);
        return UCN_ERR_NO_SPACE;
    }
    cache->entries[target] = *lease;
    if (replacement) {
        increment_saturated(&cache->replaced);
    } else {
        if (reclaim_expired) {
            increment_saturated(&cache->expired);
        }
        increment_saturated(&cache->installed);
    }
    return UCN_OK;
}

/* EN: Compares every identity field that makes cached admission safe.
 * 中文：比较保证缓存准入安全的全部身份字段。 */
static bool identity_equal(const ucn_time_capability_lease_t *actual,
                           const ucn_time_capability_lease_t *required)
{
    return actual->destination_node_id == required->destination_node_id &&
           actual->destination_session_id ==
               required->destination_session_id &&
           actual->endpoint == required->endpoint &&
           actual->metadata_version == required->metadata_version &&
           actual->mode == required->mode &&
           actual->clock_domain_id == required->clock_domain_id &&
           actual->domain_generation == required->domain_generation &&
           path_equal(&actual->forward_path, &required->forward_path) &&
           path_equal(&actual->reverse_path, &required->reverse_path);
}

/* EN: Requires an exact negotiation before only its deadline is renewed.
 * 中文：仅允许精确相同的协商结果续期。 */
static bool negotiation_equal(const ucn_time_capability_lease_t *left,
                              const ucn_time_capability_lease_t *right)
{
    return identity_equal(left, right) &&
           left->capabilities == right->capabilities;
}

/* EN: Admits only an exact, live lease with all explicitly required bits.
 * 中文：仅准入身份精确、尚未过期且具备全部必需能力位的租约。 */
ucn_result_t ucn_time_capability_cache_admit(
    const ucn_time_capability_cache_t *cache,
    const ucn_time_capability_lease_t *required_identity,
    uint32_t required_capabilities,
    uint64_t now_us)
{
    size_t index;

    if (cache == NULL || !ucn_time_capability_lease_is_valid(required_identity) ||
        required_capabilities == 0U ||
        (required_capabilities & ~UCN_TIME_CAP_KNOWN_MASK) != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_TIME_CAPABILITY_CACHE_SIZE; ++index) {
        if (cache->entries[index].occupied &&
            identity_equal(&cache->entries[index], required_identity)) {
            if (!ucn_time_capability_lease_is_valid(
                    &cache->entries[index])) {
                return UCN_ERR_STATE;
            }
            if (now_us >= cache->entries[index].expires_at_us) {
                return UCN_ERR_STATE;
            }
            return (cache->entries[index].capabilities & required_capabilities) ==
                    required_capabilities ? UCN_OK : UCN_ERR_UNSUPPORTED;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

/* EN: Expires leases at the exact half-open deadline.
 * 中文：在精确的半开 deadline 使租约失效。 */
ucn_result_t ucn_time_capability_cache_step(
    ucn_time_capability_cache_t *cache,
    uint64_t now_us)
{
    size_t index;

    if (cache == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_TIME_CAPABILITY_CACHE_SIZE; ++index) {
        if (cache->entries[index].occupied &&
            now_us >= cache->entries[index].expires_at_us) {
            (void)memset(&cache->entries[index], 0,
                         sizeof(cache->entries[index]));
            increment_saturated(&cache->expired);
        }
    }
    return UCN_OK;
}

/* EN: Invalidates all cached endpoints for one restarted/departed Node.
 * 中文：使一个重启或离网 Node 的全部 Endpoint 缓存失效。 */
ucn_result_t ucn_time_capability_cache_invalidate_node(
    ucn_time_capability_cache_t *cache,
    ucn_node_id_t destination_node_id)
{
    size_t index;
    bool found = false;

    if (cache == NULL || !node_id_is_unicast(destination_node_id)) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_TIME_CAPABILITY_CACHE_SIZE; ++index) {
        if (cache->entries[index].occupied &&
            cache->entries[index].destination_node_id == destination_node_id) {
            (void)memset(&cache->entries[index], 0,
                         sizeof(cache->entries[index]));
            found = true;
        }
    }
    return found ? UCN_OK : UCN_ERR_NOT_FOUND;
}
