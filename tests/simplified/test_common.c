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
    {
        static const uint8_t full_expected[32]={0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,
            0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23,0xB0,0x03,0x61,0xA3,0x96,0x17,0x7A,0x9C,
            0xB4,0x10,0xFF,0x61,0xF2,0x00,0x15,0xAD};
        uint8_t full[32],alias[40],before[40];
        CHECK(ucn_i_sha256_256((const uint8_t *)"abc",3U,full,&workspace)==UCN_OK);
        CHECK(memcmp(full,full_expected,32U)==0);
        memset(alias,0xA5,sizeof(alias));memcpy(before,alias,sizeof(alias));
        CHECK(ucn_i_sha256_256(alias+1U,3U,alias,&workspace)==UCN_ERR_ARGUMENT);
        CHECK(memcmp(alias,before,sizeof(alias))==0);
        workspace_before=workspace;
        CHECK(ucn_i_sha256_256((const uint8_t *)"abc",3U,(uint8_t *)&workspace,&workspace)==UCN_ERR_ARGUMENT);
        CHECK(memcmp(&workspace,&workspace_before,sizeof(workspace))==0);
    }
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

/* Independently generated with Python hashlib; catches padding, split-block and
 * canonical-prefix changes, not a second call to the implementation as oracle.
 * 独立已知答案覆盖块/填充边界和64字节域后的长度前缀。 */
static int test_digest_block_boundaries(void)
{
    static const size_t lengths[] = {0U,55U,56U,63U,64U,65U};
    static const char *const expected[] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59",
        "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562",
        "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488",
        "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108",
        "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"
    };
    static const char hex[] = "0123456789abcdef";
    static const char *const canonical[] = {
        "dc9c013b56eb838139806c47cad12047", "57af07e4839d3e45b2a66a87fd18133b",
        "5d7fac92535b4ef6f0053ed69cc6fb6c", "1e80b604370ccdedb858f60544a57c8f"
    };
    ucn_i_sha256_workspace_t work;
    uint8_t input[65],domain[64],digest[32];
    char encoded[65];
    size_t i,j;
    for(i=0U;i<sizeof(input);++i) input[i]=(uint8_t)i;
    for(i=0U;i<sizeof(lengths)/sizeof(lengths[0]);++i) {
        CHECK(ucn_i_sha256_256(input,lengths[i],digest,&work)==UCN_OK);
        for(j=0U;j<32U;++j) { encoded[2U*j]=hex[digest[j]>>4U]; encoded[2U*j+1U]=hex[digest[j]&15U]; }
        encoded[64]='\0'; CHECK(strcmp(encoded,expected[i])==0);
    }
    memset(domain,'D',sizeof(domain));
    for(i=61U;i<=64U;++i) {
        CHECK(ucn_i_canonical_digest128(domain,i,input,sizeof(input),digest,&work)==UCN_OK);
        for(j=0U;j<16U;++j) { encoded[2U*j]=hex[digest[j]>>4U]; encoded[2U*j+1U]=hex[digest[j]&15U]; }
        encoded[32]='\0'; CHECK(strcmp(encoded,canonical[i-61U])==0);
    }
    return 0;
}

static int test_digest_stream_and_retirement(void)
{
    static const uint8_t expected[16] = {0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,
        0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23};
    ucn_i_sha256_workspace_t work,before;
    uint8_t digest[16];
    CHECK(ucn_i_sha256_begin(&work)==UCN_OK);
    CHECK(ucn_i_sha256_append(&work,(const uint8_t *)"a",1U)==UCN_OK);
    before=work;
    CHECK(ucn_i_sha256_append(&work,(const uint8_t *)&work,1U)==UCN_ERR_ARGUMENT);
    CHECK(memcmp(&work,&before,sizeof(work))==0);
    CHECK(ucn_i_sha256_append(&work,(const uint8_t *)"bc",2U)==UCN_OK);
    CHECK(ucn_i_sha256_finish128(&work,digest)==UCN_OK);
    CHECK(memcmp(digest,expected,sizeof(digest))==0);
    before=work; memset(digest,0xA5,sizeof(digest));
    CHECK(ucn_i_sha256_finish128(&work,digest)==UCN_ERR_STATE);
    CHECK(memcmp(&work,&before,sizeof(work))==0 && digest[0]==0xA5U && digest[15]==0xA5U);
    CHECK(ucn_i_sha256_begin(&work)==UCN_OK);
    CHECK(ucn_i_sha256_append(&work,(const uint8_t *)"a",1U)==UCN_OK);
    work.block_bytes=0U; before=work;
    CHECK(ucn_i_sha256_append(&work,NULL,0U)==UCN_ERR_STATE);
    CHECK(memcmp(&work,&before,sizeof(work))==0);
    CHECK(ucn_i_sha256_finish128(&work,digest)==UCN_ERR_STATE);
    CHECK(memcmp(&work,&before,sizeof(work))==0 && digest[0]==0xA5U);
    return 0;
}

int main(void)
{
    CHECK(test_result_and_handle_abi() == 0);
    CHECK(test_checked_generations() == 0);
    CHECK(test_checked_time_and_size() == 0);
    CHECK(test_range_overlap() == 0);
    CHECK(test_canonical_digest() == 0);
    CHECK(test_digest_block_boundaries() == 0);
    CHECK(test_digest_stream_and_retirement() == 0);
    puts("UCN simplified common tests passed");
    return 0;
}
