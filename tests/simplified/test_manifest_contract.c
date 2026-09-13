#include "ucn/ucn_simplified.h"

#include <limits.h>
#include <stdio.h>

static int failures;

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition_); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static uint64_t fnv_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * UINT64_C(0x00000100000001B3);
}

static uint64_t canonical_hash(const uint32_t *fields, size_t field_count)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    size_t index;

    for (index = 0U; index < field_count; ++index) {
        const uint32_t value = fields[index];

        hash = fnv_byte(hash, (uint8_t)(value >> 24U));
        hash = fnv_byte(hash, (uint8_t)(value >> 16U));
        hash = fnv_byte(hash, (uint8_t)(value >> 8U));
        hash = fnv_byte(hash, (uint8_t)value);
    }
    return hash;
}

int main(void)
{
    uint32_t fields[] = {
        UINT32_C(0x55434E4D),
        UINT32_C(1),
        UCN_API_VERSION,
        UCN_STORAGE_LAYOUT,
        UCN_PROFILE,
        UCN_COMPILED_FEATURE_MASK,
        UCN_LINK_COUNT,
        UCN_ENDPOINT_COUNT,
        UCN_BINDING_COUNT,
        UCN_STATIC_PATH_COUNT,
        UCN_REQUEST_COUNT,
        UCN_RECEIPT_COUNT,
        UCN_ATTEMPT_COUNT,
        UCN_BUFFER_OBLIGATION_COUNT,
        UCN_ADAPTER_RX_SLOT_COUNT,
        UCN_ADAPTER_TX_SLOT_COUNT,
        UCN_ADAPTER_FRAME_BYTES,
        UCN_Q0_DEPTH,
        UCN_Q1_DEPTH,
        UCN_Q2_DEPTH,
        UCN_Q3_DEPTH,
        UCN_TX_SLOT_COUNT,
        UCN_STORAGE_BYTES,
        UCN_STORAGE_ALIGNMENT,
        CHAR_BIT,
        (uint32_t)sizeof(void *),
        (uint32_t)sizeof(ucn_result_t),
        (uint32_t)sizeof(ucn_handle_t),
        (uint32_t)sizeof(ucn_static_binding_t),
        (uint32_t)sizeof(ucn_config_t),
        (uint32_t)sizeof(ucn_storage_t)};
    const size_t field_count = sizeof(fields) / sizeof(fields[0]);
    const uint64_t expected = UCN_COMPILED_MANIFEST_HASH;
    size_t index;

    CHECK(field_count == UCN_COMPILED_MANIFEST_FIELD_COUNT);
    CHECK(canonical_hash(fields, field_count) == expected);

    /* EN: Every canonical field is independently mutation-sensitive. This
     * catches both a forgotten field and a hand-written profile literal.
     * 中文：逐字段执行独立单值变异，既能发现遗漏字段，也能识别手写 Profile 常量。 */
    for (index = 0U; index < field_count; ++index) {
        const uint32_t original = fields[index];

        fields[index] = original ^ UINT32_C(1);
        CHECK(canonical_hash(fields, field_count) != expected);
        fields[index] = original;
    }

    if (failures != 0) {
        return 1;
    }
    printf("UCN simplified manifest contract passed: fields=%u hash=%016llX\n",
           (unsigned)field_count, (unsigned long long)expected);
    return 0;
}
