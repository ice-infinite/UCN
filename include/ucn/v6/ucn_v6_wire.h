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
#define UCN_V6_BASE_FRAME_BYTES_A0 ((size_t)41U)
#define UCN_V6_BASE_FRAME_BYTES_A1 ((size_t)43U)
#define UCN_V6_BASE_FRAME_BYTES_A2 ((size_t)45U)
#define UCN_V6_BASE_FRAME_BYTES_A3 ((size_t)47U)
/* EN: Conservative upper bound for A3 plus every canonical fixed extension,
 * both authentication tags and the largest 16-bit Payload.  Some Frame-Type
 * contracts deliberately admit less than this bound.
 * 中文：A3、全部规范固定扩展、两个认证 Tag 与最大 16-bit Payload 的保守
 * 上界；具体 Frame Type 合同可以允许更小的实际最大值。 */
#define UCN_V6_WIRE_MAX_FRAME_BYTES ((size_t)65678U)
#define UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO UINT16_C(1)
#define UCN_V6_PROTOCOL_OPCODE_PEER_HELLO UINT16_C(2)
#define UCN_V6_PROTOCOL_OPCODE_CAPABILITY_QUERY UINT16_C(3)
#define UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE UINT16_C(4)
#define UCN_V6_PROTOCOL_OPCODE_TRANSFER_FRAGMENT UINT16_C(256)
#define UCN_V6_PROTOCOL_OPCODE_TRANSFER_SACK UINT16_C(257)
#define UCN_V6_PROTOCOL_OPCODE_TRANSFER_CREDIT UINT16_C(258)
#define UCN_V6_PROTOCOL_OPCODE_TRANSFER_RESULT UINT16_C(259)
#define UCN_V6_PROTOCOL_OPCODE_TIME_SYNC UINT16_C(512)
#define UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_REQUEST UINT16_C(513)
#define UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE UINT16_C(514)
#define UCN_V6_PROTOCOL_OPCODE_CLUSTER_CONTROL UINT16_C(768)
#define UCN_V6_PROTOCOL_OPCODE_CLUSTER_DIRECTORY UINT16_C(769)
#define UCN_V6_PROTOCOL_OPCODE_CLUSTER_TUNNEL UINT16_C(770)

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
    /* EN: Remaining forwarding scope.  It shares the 1..65534 domain used by
     * Route/Capability hop_count; zero is invalid and UINT16_MAX is reserved.
     * 中文：剩余转发范围，与 Route/Capability hop_count 共用 1..65534 域；
     * 0 非法，UINT16_MAX 保留。 */
    uint16_t hop_limit;
    uint8_t header_contract;
    uint32_t realm_id;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t source_binding_generation;
    uint32_t destination_binding_generation;
    uint32_t session_generation;
    /* EN: Origin sequence is immutable across relays and belongs to the
     * end-to-end or Group replay domain. Hop sequence is replaced and
     * authenticated independently at every Peer hop.
     * 中文：Origin 序号跨中继保持不变，归端到端或 Group 重放域所有；Hop
     * 序号在每个 Peer 跳独立替换并认证。 */
    uint32_t origin_sequence;
    uint32_t hop_sequence;
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
/* EN: Validates semantic fields and computes the exact encoded length. The
 * result object must not overlap the input frame or its payload storage.
 * 中文：校验语义字段并计算精确编码长度；结果对象不得与输入 Frame 或其
 * Payload 存储区重叠。 */
ucn_v6_result_t ucn_v6_wire_encoded_size(
    const ucn_v6_frame_t *frame,
    size_t *encoded_size);
/* EN: Encodes one canonical v6 frame without partial output on rejection.
 * The frame, payload, output bytes, and output-length object must be pairwise
 * disjoint.
 * 中文：编码唯一规范的 v6 帧；拒绝时不产生部分输出。Frame、Payload、
 * 输出字节与长度对象必须两两互不重叠。 */
ucn_v6_result_t ucn_v6_wire_encode(
    const ucn_v6_frame_t *frame,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
/* EN: Decodes one exact v6 frame and writes the view only after all gates pass.
 * The encoded input and output frame object must be disjoint.
 * 中文：精确解码 v6 帧，全部门禁通过后才写回视图；编码输入与输出 Frame
 * 对象必须完全分离。 */
ucn_v6_result_t ucn_v6_wire_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_v6_frame_t *frame);
/* EN: Emits the fixed 80-byte end-to-end associated-data sequence. The frame,
 * output bytes, and output-length object must be disjoint.
 * 中文：输出固定 80 字节的端到端关联数据序列；Frame、输出字节与长度对象
 * 必须互不重叠。 */
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
