#ifndef UCN_INTERNAL_OPERATION_WIRE_H
#define UCN_INTERNAL_OPERATION_WIRE_H

#include <stdbool.h>
#include <stdint.h>

/* EN: Language-independent Operation Flags contract, protocol/contracts/v6-shared-v1.json.
 * 中文：语言无关的 Operation Flags 合同，模式与终态位不可从本地 enum 布局推导。 */
#define UCN_I_OPERATION_MODE_MASK UINT8_C(3)
#define UCN_I_OPERATION_REPEATABLE UINT8_C(0)
#define UCN_I_OPERATION_VOLATILE_DEDUP UINT8_C(1)
#define UCN_I_OPERATION_DURABLE_AT_MOST_ONCE UINT8_C(2)
#define UCN_I_OPERATION_FINAL UINT8_C(4)
#define UCN_I_OPERATION_RESERVED_MASK UINT8_C(248)
/* EN: Durable Data-only Error codes; ordinary application errors cannot claim these outcomes.
 * 中文：仅耐久Data Error使用的结果码，普通业务错误不能冒充这两种结论。 */
#define UCN_I_DURABLE_RPC_NO_EFFECT UINT16_C(0xFFFD)
#define UCN_I_DURABLE_RPC_IN_DOUBT UINT16_C(0xFFFE)
#define UCN_I_WIRE_FRAGMENT_MAX UINT16_C(32767)
#define UCN_I_WIRE_DURATION_MS_MAX UINT32_C(2147483647)

static inline bool ucn_i_operation_flags_valid(uint8_t interaction,
                                               uint8_t flags)
{
    uint8_t mode = flags & UCN_I_OPERATION_MODE_MASK;
    if (interaction == 0U) return flags == 0U;
    if (interaction > 3U || mode == UCN_I_OPERATION_MODE_MASK ||
        (flags & UCN_I_OPERATION_RESERVED_MASK) != 0U) return false;
    return interaction == 1U ? (flags & UCN_I_OPERATION_FINAL) == 0U :
                              (flags & UCN_I_OPERATION_FINAL) != 0U;
}

#endif
