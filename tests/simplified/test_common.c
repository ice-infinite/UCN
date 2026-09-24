#include "ucn/ucn_types.h"
#include "internal/ucn_checked.h"
#include "internal/ucn_digest.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition_) do { \
    if (!(condition_)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition_); \
        return 1; \
    } \
} while (0)

static int test_result_and_handle_abi(void)
{
    ucn_handle_t handle = {7U, 3U, 1U, 9U, UCN_OBJECT_KIND_SEND, 0U};
    ucn_handle_t zero_handle = {0};
    volatile size_t result_size = sizeof(ucn_result_t);
    volatile size_t handle_size = sizeof(ucn_handle_t);
    volatile ucn_result_t first_result = UCN_OK;
    volatile ucn_result_t last_result = UCN_ERR_IN_DOUBT;

    CHECK(result_size == 4U);
    CHECK(handle_size == 12U);
    CHECK(first_result == 0 && last_result == -15);
    CHECK(ucn_i_handle_matches(&handle, 7U, 3U, 2U,
                               UCN_OBJECT_KIND_SEND));
    CHECK(!ucn_i_handle_matches(NULL, 7U, 3U, 2U,
                                UCN_OBJECT_KIND_SEND));
    CHECK(!ucn_i_handle_matches(&zero_handle, 7U, 3U, 2U,
                                UCN_OBJECT_KIND_SEND));
    handle.reserved_zero = 1U;
    CHECK(!ucn_i_handle_matches(&handle, 7U, 3U, 2U,
                                UCN_OBJECT_KIND_SEND));
    handle.reserved_zero = 0U;
    CHECK(!ucn_i_handle_matches(&handle, 8U, 3U, 2U,
                                UCN_OBJECT_KIND_SEND));
    CHECK(!ucn_i_handle_matches(&handle, 7U, 3U, 1U,
                                UCN_OBJECT_KIND_SEND));
    CHECK(!ucn_i_handle_matches(&handle, 7U, 3U, 2U,
                                UCN_OBJECT_KIND_PATH));
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    CHECK(ucn_i_handle_matches(&handle, 7U, 3U, 2U,
                               UCN_OBJECT_KIND_PERSISTENCE));
    CHECK(!ucn_i_handle_matches(&handle, 7U, 3U, 2U,
                                UCN_OBJECT_KIND_LINK));
    handle.object_kind = 0U;
    CHECK(!ucn_i_handle_matches(&handle, 7U, 3U, 2U, 0U));
    handle.object_kind = UINT8_MAX;
    CHECK(!ucn_i_handle_matches(&handle, 7U, 3U, 2U, UINT8_MAX));
    handle.object_kind = UCN_OBJECT_KIND_SEND;
    handle.generation = 0U;
    CHECK(!ucn_i_handle_matches(&handle, 7U, 3U, 2U,
                                UCN_OBJECT_KIND_SEND));
    return 0;
}

static int test_checked_generations(void)
{
    uint32_t out32 = UINT32_C(0xA5A5A5A5);
    uint64_t out64 = UINT64_C(0xA5A5A5A5A5A5A5A5);
    CHECK(ucn_i_u32_allocate_first(true, &out32) == UCN_OK);
    CHECK(out32 == 1U);
    CHECK(ucn_i_u32_allocate_first(true, NULL) == UCN_ERR_ARGUMENT);
    out32 = UINT32_C(0xA5A5A5A5);
    CHECK(ucn_i_u32_allocate_first(false, &out32) == UCN_ERR_STATE);
    CHECK(out32 == UINT32_C(0xA5A5A5A5));
    CHECK(ucn_i_u32_checked_next(1U, &out32) == UCN_OK && out32 == 2U);
    out32 = UINT32_C(0xA5A5A5A5);
    CHECK(ucn_i_u32_checked_next(0U, &out32) == UCN_ERR_ARGUMENT);
    CHECK(out32 == UINT32_C(0xA5A5A5A5));
    CHECK(ucn_i_u32_checked_next(UINT32_MAX, &out32) == UCN_ERR_EXHAUSTED);
    CHECK(out32 == UINT32_C(0xA5A5A5A5));

    CHECK(ucn_i_u64_allocate_first(true, &out64) == UCN_OK && out64 == 1U);
    CHECK(ucn_i_u64_allocate_first(true, NULL) == UCN_ERR_ARGUMENT);
    out64 = UINT64_C(0xA5A5A5A5A5A5A5A5);
    CHECK(ucn_i_u64_allocate_first(false, &out64) == UCN_ERR_STATE);
    CHECK(out64 == UINT64_C(0xA5A5A5A5A5A5A5A5));
    CHECK(ucn_i_u64_checked_next(0U, &out64) == UCN_ERR_ARGUMENT);
    CHECK(out64 == UINT64_C(0xA5A5A5A5A5A5A5A5));
    CHECK(ucn_i_u64_checked_next(1U, &out64) == UCN_OK && out64 == 2U);
    out64 = UINT64_C(0xA5A5A5A5A5A5A5A5);
    CHECK(ucn_i_u64_checked_next(UINT64_MAX, &out64) == UCN_ERR_EXHAUSTED);
    CHECK(out64 == UINT64_C(0xA5A5A5A5A5A5A5A5));
    return 0;
}

static int test_checked_time_and_size(void)
{
    uint64_t deadline = UINT64_C(0xA5A5A5A5A5A5A5A5);
    size_t value = (size_t)123U;
    CHECK(ucn_i_deadline_from_duration_us(100U, 50U, &deadline) == UCN_OK);
    CHECK(deadline == 150U);
    CHECK(!ucn_i_deadline_expired_us(149U, deadline));
    CHECK(ucn_i_deadline_expired_us(150U, deadline));
    CHECK(ucn_i_deadline_expired_us(0U, 0U));
    deadline = UINT64_C(0xA5A5A5A5A5A5A5A5);
    CHECK(ucn_i_deadline_from_duration_us(UINT64_MAX, 1U, &deadline) ==
          UCN_ERR_EXHAUSTED);
    CHECK(deadline == UINT64_C(0xA5A5A5A5A5A5A5A5));
    CHECK(ucn_i_deadline_from_duration_us(1U, 0U, &deadline) ==
          UCN_ERR_ARGUMENT);
    CHECK(deadline == UINT64_C(0xA5A5A5A5A5A5A5A5));

    CHECK(ucn_i_size_add(2U, 3U, &value) == UCN_OK && value == 5U);
    value = (size_t)123U;
    CHECK(ucn_i_size_add(SIZE_MAX, 1U, &value) == UCN_ERR_EXHAUSTED);
    CHECK(value == (size_t)123U);
    CHECK(ucn_i_size_multiply(4U, 5U, &value) == UCN_OK && value == 20U);
    CHECK(ucn_i_size_multiply(0U, SIZE_MAX, &value) == UCN_OK && value == 0U);
    value = (size_t)123U;
    CHECK(ucn_i_size_multiply(SIZE_MAX, 2U, &value) == UCN_ERR_EXHAUSTED);
    CHECK(value == (size_t)123U);
    return 0;
}

static int test_range_overlap(void)
{
    uint8_t bytes[16];

    memset(bytes, 0, sizeof(bytes));
    CHECK(!ucn_i_ranges_overlap(NULL, 1U, bytes, sizeof(bytes)));
    CHECK(!ucn_i_ranges_overlap(bytes, 0U, bytes, sizeof(bytes)));
    CHECK(ucn_i_ranges_overlap(bytes, sizeof(bytes), bytes, sizeof(bytes)));
    CHECK(ucn_i_ranges_overlap(&bytes[2], 4U, &bytes[5], 4U));
    CHECK(ucn_i_ranges_overlap(&bytes[5], 4U, &bytes[2], 4U));
    CHECK(!ucn_i_ranges_overlap(&bytes[0], 4U, &bytes[4], 4U));
    CHECK(!ucn_i_ranges_overlap(&bytes[4], 4U, &bytes[0], 4U));
    return 0;
}

static int test_canonical_digest(void)
{
    static const uint8_t empty_expected[16] = {
        0xE3U, 0xB0U, 0xC4U, 0x42U, 0x98U, 0xFCU, 0x1CU, 0x14U,
        0x9AU, 0xFBU, 0xF4U, 0xC8U, 0x99U, 0x6FU, 0xB9U, 0x24U
    };
    static const uint8_t abc_expected[16] = {
        0xBAU, 0x78U, 0x16U, 0xBFU, 0x8FU, 0x01U, 0xCFU, 0xEAU,
        0x41U, 0x41U, 0x40U, 0xDEU, 0x5DU, 0xAEU, 0x22U, 0x23U
    };
    ucn_i_sha256_workspace_t workspace;
    ucn_i_sha256_workspace_t workspace_before;
    uint8_t digest[16];

    memset(&workspace, 0, sizeof(workspace));
    memset(digest, 0xA5, sizeof(digest));
    CHECK(ucn_i_sha256_128(NULL, 0U, digest, &workspace) == UCN_OK);
    CHECK(memcmp(digest, empty_expected, sizeof(digest)) == 0);
    CHECK(ucn_i_sha256_128((const uint8_t *)"abc", 3U, digest,
                           &workspace) == UCN_OK);
    CHECK(memcmp(digest, abc_expected, sizeof(digest)) == 0);
    memset(digest, 0xA5, sizeof(digest));
    CHECK(ucn_i_sha256_128(NULL, 1U, digest, &workspace) ==
          UCN_ERR_ARGUMENT);
    CHECK(digest[0] == 0xA5U && digest[15] == 0xA5U);

    memset(&workspace, 0x5A, sizeof(workspace));
    workspace_before = workspace;
    CHECK(ucn_i_sha256_128((const uint8_t *)"abc", 3U,
                           ((uint8_t *)&workspace) + 1U,
                           &workspace) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&workspace, &workspace_before, sizeof(workspace)) == 0);

    memset(digest, 0xA5, sizeof(digest));
    CHECK(ucn_i_sha256_128((const uint8_t *)&workspace, 3U,
                           digest, &workspace) == UCN_ERR_ARGUMENT);
    CHECK(digest[0] == 0xA5U && digest[15] == 0xA5U);
    return 0;
}

int main(void)
{
    CHECK(test_result_and_handle_abi() == 0);
    CHECK(test_checked_generations() == 0);
    CHECK(test_checked_time_and_size() == 0);
    CHECK(test_range_overlap() == 0);
    CHECK(test_canonical_digest() == 0);
    puts("UCN simplified common tests passed");
    return 0;
}
