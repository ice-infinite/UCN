#include "ucn/ucn_time_capability.h"

#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "assertion failed: %s:%d: %s\n",          \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (0)

static ucn_time_capability_lease_t make_lease(ucn_node_id_t destination,
                                               ucn_endpoint_t endpoint)
{
    ucn_time_capability_lease_t lease;

    (void)memset(&lease, 0, sizeof(lease));
    lease.destination_node_id = destination;
    lease.destination_session_id = 200U + destination;
    lease.endpoint = endpoint;
    lease.metadata_version = UCN_REALTIME_ENVELOPE_VERSION;
    lease.mode = UCN_REALTIME_MODE_DEADLINE;
    lease.clock_domain_id = 7U;
    lease.domain_generation = 11U;
    lease.capabilities = UCN_TIME_CAP_TIME_META_V1 |
                         UCN_TIME_CAP_SYNC_CLIENT_V1 |
                         UCN_TIME_CAP_LINK_RX_HW_STAMP;
    lease.forward_path.owner_node_id = 1U;
    lease.forward_path.owner_session_id = 101U;
    lease.forward_path.path_id = 10U + destination;
    lease.forward_path.destination_node_id = destination;
    lease.reverse_path.owner_node_id = destination;
    lease.reverse_path.owner_session_id = lease.destination_session_id;
    lease.reverse_path.path_id = 20U + destination;
    lease.reverse_path.destination_node_id = 1U;
    lease.expires_at_us = UINT64_C(1000);
    lease.occupied = true;
    return lease;
}

/* EN: Exercises exact identity admission, replacement and expiry boundaries.
 * 中文：验证精确身份准入、替换及到期边界。 */
static bool test_exact_admission_and_expiry(void)
{
    ucn_time_capability_cache_t cache;
    ucn_time_capability_lease_t lease = make_lease(2U, 0x40U);
    ucn_time_capability_lease_t required = lease;

    TEST_ASSERT(ucn_time_capability_cache_init(&cache) == UCN_OK);
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_OK);
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &cache, &required,
                    UCN_TIME_CAP_TIME_META_V1 |
                        UCN_TIME_CAP_SYNC_CLIENT_V1,
                    999U) == UCN_OK);
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &cache, &required, UCN_TIME_CAP_SYNC_MASTER_V1, 999U) ==
                UCN_ERR_UNSUPPORTED);

    required.destination_session_id++;
    required.reverse_path.owner_session_id =
        required.destination_session_id;
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &cache, &required, UCN_TIME_CAP_TIME_META_V1, 999U) ==
                UCN_ERR_NOT_FOUND);
    required = lease;
    required.domain_generation++;
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &cache, &required, UCN_TIME_CAP_TIME_META_V1, 999U) ==
                UCN_ERR_NOT_FOUND);
    required = lease;
    required.reverse_path.path_id++;
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &cache, &required, UCN_TIME_CAP_TIME_META_V1, 999U) ==
                UCN_ERR_NOT_FOUND);

    required = lease;
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &cache, &required, UCN_TIME_CAP_TIME_META_V1, 1000U) ==
                UCN_ERR_STATE);
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &cache, &required, UCN_TIME_CAP_TIME_META_V1, 1001U) ==
                UCN_ERR_STATE);
    TEST_ASSERT(ucn_time_capability_cache_step(&cache, 999U) == UCN_OK);
    TEST_ASSERT(cache.entries[0U].occupied);
    TEST_ASSERT(ucn_time_capability_cache_step(&cache, 1000U) == UCN_OK);
    TEST_ASSERT(!cache.entries[0U].occupied && cache.expired == 1U);

    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_OK);
    cache.entries[0U].capabilities |= UINT32_C(0x80000000);
    TEST_ASSERT(ucn_time_capability_cache_admit(
                    &cache, &lease, UCN_TIME_CAP_TIME_META_V1, 1U) ==
                UCN_ERR_STATE);
    return true;
}

/* EN: Proves full caches never evict unrelated live negotiations.
 * 中文：证明缓存满时绝不淘汰无关且仍有效的协商结果。 */
static bool test_bounded_no_eviction_and_replacement(void)
{
    ucn_time_capability_cache_t cache;
    ucn_time_capability_lease_t before[UCN_TIME_CAPABILITY_CACHE_SIZE];
    ucn_time_capability_lease_t lease;
    size_t index;

    TEST_ASSERT(ucn_time_capability_cache_init(&cache) == UCN_OK);
    for (index = 0U; index < UCN_TIME_CAPABILITY_CACHE_SIZE; ++index) {
        lease = make_lease((ucn_node_id_t)(10U + index),
                           (ucn_endpoint_t)(0x40U + index));
        TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                    UCN_OK);
    }
    (void)memcpy(before, cache.entries, sizeof(before));
    lease = make_lease(99U, 0x70U);
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(memcmp(cache.entries, before, sizeof(before)) == 0);
    TEST_ASSERT(cache.rejected == 1U);

    lease = cache.entries[1U];
    lease.expires_at_us = 2000U;
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_OK);
    TEST_ASSERT(cache.replaced == 1U);

    (void)memcpy(before, cache.entries, sizeof(before));
    lease.destination_session_id++;
    lease.reverse_path.owner_session_id = lease.destination_session_id;
    lease.domain_generation++;
    lease.expires_at_us = 3000U;
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(memcmp(cache.entries, before, sizeof(before)) == 0);
    TEST_ASSERT(ucn_time_capability_cache_invalidate_node(
                    &cache, lease.destination_node_id) == UCN_OK);
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_OK);
    TEST_ASSERT(cache.installed == UCN_TIME_CAPABILITY_CACHE_SIZE + 1U);
    TEST_ASSERT(cache.replaced == 1U);
    TEST_ASSERT(cache.entries[1U].destination_session_id ==
                lease.destination_session_id);

    /* A full cache containing expired entries must not require an external
     * step before a new unrelated negotiation can be installed. */
    cache.entries[0U].expires_at_us = 10U;
    lease = make_lease(100U, 0x71U);
    lease.expires_at_us = 4000U;
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 10U) ==
                UCN_OK);
    TEST_ASSERT(cache.entries[0U].destination_node_id == 100U &&
                cache.expired == 1U &&
                cache.installed == UCN_TIME_CAPABILITY_CACHE_SIZE + 2U);
    return true;
}

/* EN: Covers malformed leases, per-Node invalidation and saturated counters.
 * 中文：覆盖畸形租约、按 Node 失效及饱和计数器。 */
static bool test_fail_closed_and_invalidation(void)
{
    ucn_time_capability_cache_t cache;
    ucn_time_capability_cache_t before;
    ucn_time_capability_lease_t lease = make_lease(2U, 0x40U);

    TEST_ASSERT(ucn_time_capability_cache_init(&cache) == UCN_OK);
    before = cache;
    lease.capabilities |= UINT32_C(0x80000000);
    TEST_ASSERT(!ucn_time_capability_lease_is_valid(&lease));
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(memcmp(&cache, &before, sizeof(cache)) == 0);

    lease = make_lease(2U, 0x40U);
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_OK);
    lease = make_lease(2U, 0x41U);
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_OK);
    lease = make_lease(3U, 0x42U);
    TEST_ASSERT(ucn_time_capability_cache_install(&cache, &lease, 0U) ==
                UCN_OK);
    TEST_ASSERT(ucn_time_capability_cache_invalidate_node(&cache, 2U) ==
                UCN_OK);
    TEST_ASSERT(!cache.entries[0U].occupied && !cache.entries[1U].occupied);
    TEST_ASSERT(cache.entries[2U].occupied);
    TEST_ASSERT(ucn_time_capability_cache_invalidate_node(&cache, 2U) ==
                UCN_ERR_NOT_FOUND);

    cache.expired = UINT32_MAX;
    TEST_ASSERT(ucn_time_capability_cache_step(&cache, 1000U) == UCN_OK);
    TEST_ASSERT(cache.expired == UINT32_MAX);
    return true;
}

int main(void)
{
    if (!test_exact_admission_and_expiry() ||
        !test_bounded_no_eviction_and_replacement() ||
        !test_fail_closed_and_invalidation()) {
        return 1;
    }
    (void)puts("time capability tests passed");
    return 0;
}
