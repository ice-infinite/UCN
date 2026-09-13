#ifndef UCN_CONFIG_H
#define UCN_CONFIG_H

#include "ucn/ucn_types.h"

#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_API_VERSION UINT16_C(1)
#define UCN_STORAGE_LAYOUT UINT16_C(1)

/* EN: This mask describes the simplified binary that is being built, not the
 * pre-simplification optional-v6 targets which may coexist during migration.
 * Every implemented simplified feature must own one stable bit here.
 * 中文：该掩码描述正在构建的简化版二进制，而不是迁移期并存的旧 v6 可选目标。
 * 每项已经实现的简化版能力都必须在这里拥有一个稳定 bit。 */
#define UCN_MANIFEST_FEATURE_COMMON_OWNER UINT32_C(0x00000001)
#define UCN_MANIFEST_FEATURE_C1_WIRE UINT32_C(0x00000002)
#define UCN_MANIFEST_FEATURE_ADAPTER_FOUNDATION UINT32_C(0x00000004)
#define UCN_MANIFEST_FEATURE_STATIC_RUNTIME UINT32_C(0x00000008)
#define UCN_MANIFEST_FEATURE_PERSISTENCE UINT32_C(0x00000010)
#ifndef UCN_FEATURE_PERSISTENCE_ENABLED
#define UCN_FEATURE_PERSISTENCE_ENABLED 0
#endif
#if UCN_FEATURE_PERSISTENCE_ENABLED
#define UCN_I_MANIFEST_PERSISTENCE_BIT UCN_MANIFEST_FEATURE_PERSISTENCE
#else
#define UCN_I_MANIFEST_PERSISTENCE_BIT UINT32_C(0)
#endif
#define UCN_COMPILED_FEATURE_MASK                                         \
    (UCN_MANIFEST_FEATURE_COMMON_OWNER | UCN_MANIFEST_FEATURE_C1_WIRE |   \
     UCN_MANIFEST_FEATURE_ADAPTER_FOUNDATION |                            \
     UCN_MANIFEST_FEATURE_STATIC_RUNTIME | UCN_I_MANIFEST_PERSISTENCE_BIT)

/* The product/profile manifest hash binds this binary to the exact compiled
 * capacity and layout contract.  Applications must copy the matching constant
 * into ucn_config_t; ucn_init() rejects a different binary/configuration pair
 * before writing caller-owned storage.
 *
 * 产品/Profile 清单哈希把当前二进制与精确的容量及布局合同绑定。应用必须把
 * 对应常量写入 ucn_config_t；若二进制与配置不匹配，ucn_init() 会在写入调用方
 * Storage 前失败关闭。 */

#define UCN_PROFILE_NANO 1
#define UCN_PROFILE_LITE 2
#define UCN_PROFILE_FULL 3

#ifndef UCN_PROFILE
#define UCN_PROFILE UCN_PROFILE_FULL
#endif

#if UCN_PROFILE == UCN_PROFILE_NANO
#define UCN_LINK_COUNT 2U
#define UCN_ENDPOINT_COUNT 8U
#define UCN_BINDING_COUNT 4U
#define UCN_STATIC_PATH_COUNT 4U
#define UCN_REQUEST_COUNT 4U
#define UCN_RECEIPT_COUNT 4U
#define UCN_ATTEMPT_COUNT 4U
#define UCN_BUFFER_OBLIGATION_COUNT 6U
#define UCN_ADAPTER_RX_SLOT_COUNT 4U
#define UCN_ADAPTER_TX_SLOT_COUNT 4U
#define UCN_ADAPTER_FRAME_BYTES 256U
#define UCN_Q0_DEPTH 4U
#define UCN_Q1_DEPTH 4U
#define UCN_Q2_DEPTH 4U
#define UCN_Q3_DEPTH 4U
#define UCN_STORAGE_BYTES 16384U
#elif UCN_PROFILE == UCN_PROFILE_LITE
#define UCN_LINK_COUNT 4U
#define UCN_ENDPOINT_COUNT 16U
#define UCN_BINDING_COUNT 8U
#define UCN_STATIC_PATH_COUNT 16U
#define UCN_REQUEST_COUNT 8U
#define UCN_RECEIPT_COUNT 8U
#define UCN_ATTEMPT_COUNT 8U
#define UCN_BUFFER_OBLIGATION_COUNT 16U
#define UCN_ADAPTER_RX_SLOT_COUNT 16U
#define UCN_ADAPTER_TX_SLOT_COUNT 16U
#define UCN_ADAPTER_FRAME_BYTES 256U
#define UCN_Q0_DEPTH 8U
#define UCN_Q1_DEPTH 8U
#define UCN_Q2_DEPTH 16U
#define UCN_Q3_DEPTH 16U
#define UCN_STORAGE_BYTES 49152U
#elif UCN_PROFILE == UCN_PROFILE_FULL
#define UCN_LINK_COUNT 8U
#define UCN_ENDPOINT_COUNT 32U
#define UCN_BINDING_COUNT 16U
#define UCN_STATIC_PATH_COUNT 32U
#define UCN_REQUEST_COUNT 16U
#define UCN_RECEIPT_COUNT 16U
#define UCN_ATTEMPT_COUNT 16U
#define UCN_BUFFER_OBLIGATION_COUNT 32U
#define UCN_ADAPTER_RX_SLOT_COUNT 32U
#define UCN_ADAPTER_TX_SLOT_COUNT 32U
#define UCN_ADAPTER_FRAME_BYTES 512U
#define UCN_Q0_DEPTH 16U
#define UCN_Q1_DEPTH 16U
#define UCN_Q2_DEPTH 32U
#define UCN_Q3_DEPTH 32U
#define UCN_STORAGE_BYTES 98304U
#else
#error "UCN_PROFILE must be UCN_PROFILE_NANO, UCN_PROFILE_LITE, or UCN_PROFILE_FULL"
#endif

#define UCN_STORAGE_ALIGNMENT 8U
#define UCN_TX_SLOT_COUNT (UCN_Q0_DEPTH + UCN_Q1_DEPTH + UCN_Q2_DEPTH + UCN_Q3_DEPTH)

UCN_STATIC_ASSERT(UCN_LINK_COUNT > 0U && UCN_LINK_COUNT <= UINT8_MAX,
                  link_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_ENDPOINT_COUNT > 0U &&
                      UCN_ENDPOINT_COUNT <= UINT8_MAX,
                  endpoint_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_BINDING_COUNT > 0U &&
                      UCN_BINDING_COUNT <= UINT8_MAX,
                  binding_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_STATIC_PATH_COUNT > 0U &&
                      UCN_STATIC_PATH_COUNT <= UINT8_MAX,
                  path_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_REQUEST_COUNT > 0U &&
                      UCN_REQUEST_COUNT <= UINT8_MAX,
                  request_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_RECEIPT_COUNT > 0U &&
                      UCN_RECEIPT_COUNT <= UINT8_MAX,
                  receipt_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_ATTEMPT_COUNT > 0U &&
                      UCN_ATTEMPT_COUNT <= UINT8_MAX,
                  attempt_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_BUFFER_OBLIGATION_COUNT > 0U &&
                      UCN_BUFFER_OBLIGATION_COUNT <= UINT8_MAX,
                  buffer_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_ADAPTER_RX_SLOT_COUNT > 0U &&
                      UCN_ADAPTER_RX_SLOT_COUNT <= UINT8_MAX,
                  adapter_rx_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_ADAPTER_TX_SLOT_COUNT > 0U &&
                      UCN_ADAPTER_TX_SLOT_COUNT <= UINT8_MAX,
                  adapter_tx_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_Q0_DEPTH > 0U && UCN_Q0_DEPTH <= UINT8_MAX,
                  q0_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_Q1_DEPTH > 0U && UCN_Q1_DEPTH <= UINT8_MAX,
                  q1_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_Q2_DEPTH > 0U && UCN_Q2_DEPTH <= UINT8_MAX,
                  q2_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_Q3_DEPTH > 0U && UCN_Q3_DEPTH <= UINT8_MAX,
                  q3_capacity_must_be_u8_bounded);
UCN_STATIC_ASSERT(UCN_TX_SLOT_COUNT <= UINT8_MAX,
                  total_tx_capacity_must_be_u8_bounded);

typedef union ucn_storage {
    uint64_t force_alignment;
    unsigned char bytes[UCN_STORAGE_BYTES];
} ucn_storage_t;

#define UCN_DECLARE_STORAGE(name_) ucn_storage_t name_

typedef struct ucn_static_binding {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t address;
    uint32_t binding_generation;
    uint64_t principal_digest;
} ucn_static_binding_t;

typedef struct ucn_config {
    uint16_t struct_size;
    uint16_t api_version;
    /* Exact compiled-storage contract; both fields are mandatory. / 精确编译期
     * Storage 合同；两个字段均为必填。 */
    uint16_t storage_layout;
    uint16_t manifest_reserved_zero;
    uint64_t compiled_manifest_hash;
    uint32_t runtime_instance;
    uint32_t realm_id;
    uint32_t local_address;
    uint32_t local_binding_generation;
    uint64_t local_principal_digest;
    const ucn_static_binding_t *bindings;
    uint16_t binding_count;
    uint8_t address_width;
    uint8_t trusted_o0_network;
    uint32_t reserved_zero;
} ucn_config_t;

/* EN: Canonical compiled-manifest fingerprint. Values are serialized as
 * ordered unsigned 32-bit big-endian fields and folded with FNV-1a-64. The
 * hash is therefore derived from the active compilation contract; it is not a
 * hand-maintained profile literal. The ordered inputs bind API/Layout,
 * simplified features, every fixed capacity, Storage, and key public ABI
 * scalars. A one-value source mutation changes the consumer-visible constant
 * and the library-side admission value together.
 *
 * 中文：规范编译清单指纹。所有值按固定顺序序列化为 32 位无符号大端字段，再以
 * FNV-1a-64 折叠。该哈希由当前实际编译合同推导，不再是手写的 Profile 常量。
 * 有序输入绑定 API/Layout、简化版 Feature、全部固定容量、Storage 与关键公共
 * ABI 标量；任一源码配置值变化都会同步改变消费者常量和生产库准入值。 */
#define UCN_I_MANIFEST_FNV_OFFSET UINT64_C(0xCBF29CE484222325)
#define UCN_I_MANIFEST_FNV_PRIME UINT64_C(0x00000100000001B3)
#define UCN_I_MANIFEST_FNV_BYTE(hash_, byte_)                              \
    (hash_ ^ ((uint64_t)(byte_) & UINT64_C(0xFF))) *                       \
        UCN_I_MANIFEST_FNV_PRIME
#define UCN_I_MANIFEST_FNV_U32(hash_, value_)                              \
    UCN_I_MANIFEST_FNV_BYTE(                                                \
        UCN_I_MANIFEST_FNV_BYTE(                                            \
            UCN_I_MANIFEST_FNV_BYTE(                                        \
                UCN_I_MANIFEST_FNV_BYTE(                                    \
                    (hash_), ((uint64_t)(value_) >> 24U)),                   \
                ((uint64_t)(value_) >> 16U)),                               \
            ((uint64_t)(value_) >> 8U)),                                    \
        (uint64_t)(value_))

#define UCN_I_MANIFEST_H00 UCN_I_MANIFEST_FNV_OFFSET
#define UCN_I_MANIFEST_H01 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H00, UINT32_C(0x55434E4D))
#define UCN_I_MANIFEST_H02 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H01, UINT32_C(1))
#define UCN_I_MANIFEST_H03 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H02, UCN_API_VERSION)
#define UCN_I_MANIFEST_H04 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H03, UCN_STORAGE_LAYOUT)
#define UCN_I_MANIFEST_H05 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H04, UCN_PROFILE)
#define UCN_I_MANIFEST_H06 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H05, UCN_COMPILED_FEATURE_MASK)
#define UCN_I_MANIFEST_H07 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H06, UCN_LINK_COUNT)
#define UCN_I_MANIFEST_H08 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H07, UCN_ENDPOINT_COUNT)
#define UCN_I_MANIFEST_H09 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H08, UCN_BINDING_COUNT)
#define UCN_I_MANIFEST_H10 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H09, UCN_STATIC_PATH_COUNT)
#define UCN_I_MANIFEST_H11 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H10, UCN_REQUEST_COUNT)
#define UCN_I_MANIFEST_H12 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H11, UCN_RECEIPT_COUNT)
#define UCN_I_MANIFEST_H13 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H12, UCN_ATTEMPT_COUNT)
#define UCN_I_MANIFEST_H14                                               \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H13,                           \
                           UCN_BUFFER_OBLIGATION_COUNT)
#define UCN_I_MANIFEST_H15 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H14, UCN_ADAPTER_RX_SLOT_COUNT)
#define UCN_I_MANIFEST_H16 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H15, UCN_ADAPTER_TX_SLOT_COUNT)
#define UCN_I_MANIFEST_H17 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H16, UCN_ADAPTER_FRAME_BYTES)
#define UCN_I_MANIFEST_H18 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H17, UCN_Q0_DEPTH)
#define UCN_I_MANIFEST_H19 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H18, UCN_Q1_DEPTH)
#define UCN_I_MANIFEST_H20 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H19, UCN_Q2_DEPTH)
#define UCN_I_MANIFEST_H21 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H20, UCN_Q3_DEPTH)
#define UCN_I_MANIFEST_H22 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H21, UCN_TX_SLOT_COUNT)
#define UCN_I_MANIFEST_H23 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H22, UCN_STORAGE_BYTES)
#define UCN_I_MANIFEST_H24 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H23, UCN_STORAGE_ALIGNMENT)
#define UCN_I_MANIFEST_H25 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H24, CHAR_BIT)
#define UCN_I_MANIFEST_H26 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H25, sizeof(void *))
#define UCN_I_MANIFEST_H27 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H26, sizeof(ucn_result_t))
#define UCN_I_MANIFEST_H28 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H27, sizeof(ucn_handle_t))
#define UCN_I_MANIFEST_H29                                               \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H28,                           \
                           sizeof(ucn_static_binding_t))
#define UCN_I_MANIFEST_H30 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H29, sizeof(ucn_config_t))
#define UCN_I_MANIFEST_H31 \
    UCN_I_MANIFEST_FNV_U32(UCN_I_MANIFEST_H30, sizeof(ucn_storage_t))

#define UCN_COMPILED_MANIFEST_HASH ((uint64_t)UCN_I_MANIFEST_H31)
#define UCN_COMPILED_MANIFEST_FIELD_COUNT UINT16_C(31)

size_t ucn_storage_required(void);

#ifdef __cplusplus
}
#endif

#endif
