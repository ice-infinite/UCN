#ifndef UCN_V6_WIRE_H
#define UCN_V6_WIRE_H

#include "ucn/v6/ucn_v6_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_WIRE_MAGIC_0 ((uint8_t)0x55U)
#define UCN_V6_WIRE_MAGIC_1 ((uint8_t)0x43U)
#define UCN_V6_HEADER_CONTRACT_1 ((uint8_t)1U)
#define UCN_V6_SECURITY_TAG_BYTES ((size_t)16U)
#define UCN_V6_CANONICAL_AAD_BYTES ((size_t)80U)
#define UCN_V6_BASE_FRAME_BYTES_A0 ((size_t)36U)
#define UCN_V6_BASE_FRAME_BYTES_A1 ((size_t)38U)
#define UCN_V6_BASE_FRAME_BYTES_A2 ((size_t)40U)
#define UCN_V6_BASE_FRAME_BYTES_A3 ((size_t)42U)
#define UCN_V6_WIRE_MAX_FRAME_BYTES ((size_t)65669U)
#define UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO UINT16_C(1)

typedef enum ucn_v6_address_class {
    UCN_V6_ADDRESS_CLASS_A0 = 0,
    UCN_V6_ADDRESS_CLASS_A1 = 1,
    UCN_V6_ADDRESS_CLASS_A2 = 2,
    UCN_V6_ADDRESS_CLASS_A3 = 3
} ucn_v6_address_class_t;

typedef enum ucn_v6_frame_type {
    UCN_V6_FRAME_BOOTSTRAP = 1,
    UCN_V6_FRAME_CONTROL = 2,
    UCN_V6_FRAME_DATA = 3,
    UCN_V6_FRAME_TRANSFER = 4,
    UCN_V6_FRAME_DIAGNOSTIC = 5
} ucn_v6_frame_type_t;

typedef enum ucn_v6_traffic_class {
    UCN_V6_TRAFFIC_Q0 = 0,
    UCN_V6_TRAFFIC_Q1 = 1,
    UCN_V6_TRAFFIC_Q2 = 2,
    UCN_V6_TRAFFIC_Q3 = 3
} ucn_v6_traffic_class_t;

typedef enum ucn_v6_delivery_guarantee {
    UCN_V6_DELIVERY_BEST_EFFORT = 0,
    UCN_V6_DELIVERY_LATEST = 1,
    UCN_V6_DELIVERY_RELIABLE = 2
} ucn_v6_delivery_guarantee_t;

typedef enum ucn_v6_interaction_role {
    UCN_V6_INTERACTION_ONE_WAY = 0,
    UCN_V6_INTERACTION_REQUEST = 1,
    UCN_V6_INTERACTION_RESULT = 2,
    UCN_V6_INTERACTION_ERROR = 3
} ucn_v6_interaction_role_t;

typedef enum ucn_v6_e2e_mode {
    UCN_V6_E2E_NONE = 0,
    UCN_V6_E2E_AUTH_ONLY = 1,
    UCN_V6_E2E_AEAD = 2
} ucn_v6_e2e_mode_t;

enum {
    UCN_V6_FLAG_PEER_HOP_CONTEXT = 1U << 0,
    UCN_V6_FLAG_GROUP_CONTEXT = 1U << 1,
    UCN_V6_FLAG_E2E_CONTEXT = 1U << 2,
    UCN_V6_FLAG_PROTOCOL_CONTEXT = 1U << 3,
    UCN_V6_FLAG_MESSAGE_CONTEXT = 1U << 4,
    UCN_V6_FLAG_ROUTE_CONTEXT = 1U << 5,
    UCN_V6_FLAG_PATH_CONTEXT = 1U << 6,
    UCN_V6_FLAG_HOP_BUDGET_CONTEXT = 1U << 7
};

typedef struct ucn_v6_peer_hop_context {
    uint8_t suite_id;
    uint16_t key_id;
    uint32_t key_generation;
} ucn_v6_peer_hop_context_t;

typedef struct ucn_v6_group_context {
    uint32_t group_id;
    uint32_t group_generation;
    uint8_t suite_id;
    uint16_t key_id;
    uint32_t key_generation;
} ucn_v6_group_context_t;

typedef struct ucn_v6_e2e_context {
    ucn_v6_e2e_mode_t mode;
    uint8_t suite_id;
    uint16_t key_id;
    uint32_t key_generation;
} ucn_v6_e2e_context_t;

typedef struct ucn_v6_message_context {
    uint16_t source_endpoint;
    uint16_t destination_endpoint;
    ucn_v6_interaction_role_t interaction_role;
    uint64_t operation_id;
} ucn_v6_message_context_t;

typedef struct ucn_v6_path_context {
    uint16_t path_id;
    uint32_t path_generation;
} ucn_v6_path_context_t;

typedef struct ucn_v6_hop_budget_context {
    uint64_t initial_budget_us;
    uint64_t remaining_budget_us;
} ucn_v6_hop_budget_context_t;

typedef struct ucn_v6_frame {
    ucn_v6_address_class_t address_class;
    ucn_v6_frame_type_t frame_type;
    uint8_t flags;
    ucn_v6_traffic_class_t traffic_class;
    ucn_v6_delivery_guarantee_t delivery_guarantee;
    uint8_t hop_limit;
    uint8_t header_contract;
    uint32_t realm_id;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t source_binding_generation;
    uint32_t destination_binding_generation;
    uint32_t session_generation;
    uint32_t packet_sequence;
    ucn_v6_peer_hop_context_t peer_hop;
    ucn_v6_group_context_t group;
    ucn_v6_e2e_context_t e2e;
    uint16_t protocol_opcode;
    ucn_v6_message_context_t message;
    uint32_t route_generation;
    ucn_v6_path_context_t path;
    ucn_v6_hop_budget_context_t hop_budget;
    const uint8_t *payload;
    uint16_t payload_length;
    uint8_t e2e_tag[UCN_V6_SECURITY_TAG_BYTES];
    uint8_t link_tag[UCN_V6_SECURITY_TAG_BYTES];
} ucn_v6_frame_t;

/* EN: Returns the exact on-wire address width or zero for an invalid class.
 * 中文：返回线上地址的精确字节数；非法档位返回零。 */
size_t ucn_v6_address_bytes(ucn_v6_address_class_t address_class);
/* EN: Returns the largest routable address below the link-local sentinel.
 * 中文：返回小于链路本地保留值的最大可路由地址。 */
uint32_t ucn_v6_address_max_ordinary(
    ucn_v6_address_class_t address_class);
/* EN: Validates semantic fields and computes the exact encoded length.
 * 中文：校验语义字段并计算精确编码长度。 */
ucn_v6_result_t ucn_v6_wire_encoded_size(
    const ucn_v6_frame_t *frame,
    size_t *encoded_size);
/* EN: Encodes one canonical v6 frame without partial output on rejection.
 * The payload storage must not overlap the output storage.
 * 中文：编码唯一规范的 v6 帧；拒绝时不产生部分输出。载荷存储区不得与
 * 输出存储区重叠。 */
ucn_v6_result_t ucn_v6_wire_encode(
    const ucn_v6_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
/* EN: Decodes one exact v6 frame and writes the view only after all gates pass.
 * 中文：精确解码 v6 帧，全部门禁通过后才写回视图。 */
ucn_v6_result_t ucn_v6_wire_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_frame_t *frame);
/* EN: Emits the fixed 80-byte end-to-end associated-data sequence.
 * 中文：输出固定 80 字节的端到端关联数据序列。 */
ucn_v6_result_t ucn_v6_wire_write_canonical_aad(
    const ucn_v6_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
/* EN: Calculates reflected Castagnoli CRC32C for early random-error rejection.
 * 中文：计算反射式 Castagnoli CRC32C，用于随机错误早拒绝。 */
uint32_t ucn_v6_crc32c(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif
