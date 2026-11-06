#ifndef UCN_CONTROL_WIRE_H
#define UCN_CONTROL_WIRE_H

#include "internal/ucn_wire.h"

#define UCN_I_C0_COMMON_BYTES 3U
#define UCN_I_C0_FIXED_EXCLUDING_ADDRESSES 25U
#define UCN_I_C0_ORIGIN_TAG_BYTES 16U
#define UCN_I_C0_H1_TRAILER_BYTES 16U
#define UCN_I_C0_ORIGIN_AAD_MAX_BYTES 56U
#define UCN_I_DIAGNOSTIC_ECHO_BYTES 8U

typedef uint8_t ucn_i_payload_kind_t;
enum {
    UCN_I_PAYLOAD_DATA = 0,
    UCN_I_PAYLOAD_CONTROL = 1,
    UCN_I_PAYLOAD_TRANSFER = 2,
    UCN_I_PAYLOAD_DIAGNOSTIC = 3
};

typedef uint8_t ucn_i_origin_security_t;
enum {
    UCN_I_ORIGIN_O0 = 0,
    UCN_I_ORIGIN_O1 = 1,
    UCN_I_ORIGIN_O2 = 2
};

typedef uint8_t ucn_i_control_hop_profile_t;
enum {
    UCN_I_CONTROL_H0 = 0,
    UCN_I_CONTROL_H1 = 1
};

typedef uint8_t ucn_i_opcode_domain_t;
enum {
    UCN_I_OPCODE_IDENTITY_SECURITY = 0,
    UCN_I_OPCODE_CAPABILITY_NEIGHBOR = 1,
    UCN_I_OPCODE_ROUTE = 2,
    UCN_I_OPCODE_FLOW_PATH = 3,
    UCN_I_OPCODE_TRANSPORT = 4,
    UCN_I_OPCODE_SERVICE_OPERATION = 5,
    UCN_I_OPCODE_TIME = 6,
    UCN_I_OPCODE_GROUP = 7,
    UCN_I_OPCODE_CLUSTER = 8,
    UCN_I_OPCODE_DIAGNOSTICS = 9
};

typedef uint16_t ucn_i_protocol_opcode_t;
enum {
    UCN_I_OPCODE_BOOTSTRAP_HELLO = 0x0001,
    UCN_I_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE = 0x0002,
    UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE = 0x0003,
    UCN_I_OPCODE_IDENTITY_CHALLENGE = 0x0004,
    UCN_I_OPCODE_IDENTITY_RESPONSE = 0x0005,
    UCN_I_OPCODE_ADDRESS_OFFER = 0x0006,
    UCN_I_OPCODE_DEVICE_COMMIT = 0x0007,
    UCN_I_OPCODE_FINAL_COMMIT = 0x0008,
    UCN_I_OPCODE_ADDRESS_RENEW = 0x0009,
    UCN_I_OPCODE_ADDRESS_REVOKE = 0x000A,
    UCN_I_OPCODE_PEER_REAUTH_HELLO = 0x0010,
    UCN_I_OPCODE_PEER_REAUTH_CHALLENGE = 0x0011,
    UCN_I_OPCODE_PEER_REAUTH_RESPONSE = 0x0012,
    UCN_I_OPCODE_PEER_SESSION_PREPARE = 0x0013,
    UCN_I_OPCODE_PEER_SESSION_ACCEPT = 0x0014,
    UCN_I_OPCODE_PEER_SESSION_COMMIT = 0x0015,
    UCN_I_OPCODE_PEER_SESSION_ABORT = 0x0016,
    UCN_I_OPCODE_NEIGHBOR_HELLO = 0x0101,
    UCN_I_OPCODE_NEIGHBOR_KEEPALIVE = 0x0102,
    UCN_I_OPCODE_CAPABILITY_ADVERTISE = 0x0103,
    UCN_I_OPCODE_CAPABILITY_SELECT = 0x0104,
    UCN_I_OPCODE_CAPABILITY_ACK = 0x0105,
    UCN_I_OPCODE_CAPABILITY_REVOKE = 0x0106,
    UCN_I_OPCODE_CAPABILITY_QUERY = 0x0107,
    UCN_I_OPCODE_CAPABILITY_FRAGMENT = 0x0108,
    UCN_I_OPCODE_ROUTE_RREQ = 0x0201,
    UCN_I_OPCODE_ROUTE_RREP = 0x0202,
    UCN_I_OPCODE_ROUTE_RERR = 0x0203,
    UCN_I_OPCODE_ROUTE_PROBE = 0x0204,
    UCN_I_OPCODE_ROUTE_PROBE_ACK = 0x0205,
    UCN_I_OPCODE_ROUTE_FRAGMENT = 0x0206,
    UCN_I_OPCODE_FLOW_PREPARE = 0x0301,
    UCN_I_OPCODE_FLOW_ACCEPT = 0x0302,
    UCN_I_OPCODE_FLOW_COMMIT = 0x0303,
    UCN_I_OPCODE_FLOW_ABORT = 0x0304,
    UCN_I_OPCODE_FLOW_RETIRE = 0x0305,
    UCN_I_OPCODE_PATH_ACTIVATE_STAGE = 0x0310,
    UCN_I_OPCODE_PATH_STAGE_ACK = 0x0311,
    UCN_I_OPCODE_PATH_ACTIVATE_COMMIT = 0x0312,
    UCN_I_OPCODE_PATH_COMMIT_ACK = 0x0313,
    UCN_I_OPCODE_PATH_ACTIVATE_ABORT = 0x0314,
    UCN_I_OPCODE_PATH_TERMINAL_RECEIPT = 0x0315,
    UCN_I_OPCODE_PATH_PROBE = 0x0316,
    UCN_I_OPCODE_PATH_PROBE_ACK = 0x0317,
    UCN_I_OPCODE_DELIVERY_ACK = 0x0401,
    UCN_I_OPCODE_TRANSPORT_PARENT_PREPARE = 0x0410,
    UCN_I_OPCODE_TRANSPORT_PARENT_ACCEPT = 0x0411,
    UCN_I_OPCODE_TRANSPORT_PARENT_COMMIT = 0x0412,
    UCN_I_OPCODE_TRANSPORT_PARENT_ABORT = 0x0413,
    UCN_I_OPCODE_TRANSFER_SETUP = 0x0420,
    UCN_I_OPCODE_TRANSFER_SETUP_ACK = 0x0421,
    UCN_I_OPCODE_TRANSFER_ABORT = 0x0422,
    UCN_I_OPCODE_TRANSFER_TERMINAL_RECEIPT = 0x0423,
    UCN_I_OPCODE_TRANSFER_SACK_CREDIT = 0x0424,
    UCN_I_OPCODE_OPERATION_QUERY = 0x0501,
    UCN_I_OPCODE_OPERATION_RECEIPT = 0x0502,
    UCN_I_OPCODE_OPERATION_CANCEL = 0x0503,
    UCN_I_OPCODE_OPERATION_RESULT_ACK = 0x0504,
    UCN_I_OPCODE_OPERATION_RESUME = 0x0505,
    UCN_I_OPCODE_OPERATION_RESULT = 0x0506,
    UCN_I_OPCODE_TIME_SYNC = 0x0601,
    UCN_I_OPCODE_TIME_FOLLOW_UP = 0x0602,
    UCN_I_OPCODE_TIME_DELAY_REQUEST = 0x0603,
    UCN_I_OPCODE_TIME_DELAY_RESPONSE = 0x0604,
    UCN_I_OPCODE_TIME_SYNC_ABORT = 0x0605,
    UCN_I_OPCODE_GROUP_PREPARE = 0x0701,
    UCN_I_OPCODE_GROUP_ACCEPT = 0x0702,
    UCN_I_OPCODE_GROUP_COMMIT = 0x0703,
    UCN_I_OPCODE_GROUP_ABORT = 0x0704,
    UCN_I_OPCODE_GROUP_RETIRE = 0x0705,
    UCN_I_OPCODE_GROUP_KEY_PREPARE = 0x0710,
    UCN_I_OPCODE_GROUP_KEY_ACK = 0x0711,
    UCN_I_OPCODE_GROUP_KEY_COMMIT = 0x0712,
    UCN_I_OPCODE_GROUP_TREE_STAGE = 0x0720,
    UCN_I_OPCODE_GROUP_TREE_ACK = 0x0721,
    UCN_I_OPCODE_GROUP_TREE_COMMIT = 0x0722,
    UCN_I_OPCODE_GROUP_TREE_ABORT = 0x0723,
    UCN_I_OPCODE_GROUP_MEMBERSHIP_SNAPSHOT = 0x0730,
    UCN_I_OPCODE_CLUSTER_ADVERTISE = 0x0801,
    UCN_I_OPCODE_CLUSTER_JOIN = 0x0802,
    UCN_I_OPCODE_CLUSTER_CONFIG_PREPARE = 0x0803,
    UCN_I_OPCODE_CLUSTER_CONFIG_ACK = 0x0804,
    UCN_I_OPCODE_CLUSTER_CONFIG_COMMIT = 0x0805,
    UCN_I_OPCODE_CLUSTER_BACKUP_ASSIGN = 0x0806,
    UCN_I_OPCODE_CLUSTER_BACKUP_READY = 0x0807,
    UCN_I_OPCODE_CLUSTER_TAKEOVER_VOTE = 0x0808,
    UCN_I_OPCODE_CLUSTER_TAKEOVER_COMMIT = 0x0809,
    UCN_I_OPCODE_CLUSTER_HANDOVER_PREPARE = 0x080A,
    UCN_I_OPCODE_CLUSTER_HANDOVER_READY = 0x080B,
    UCN_I_OPCODE_CLUSTER_HANDOVER_COMMIT = 0x080C,
    UCN_I_OPCODE_CLUSTER_RECOVERY_VOTE = 0x080D,
    UCN_I_OPCODE_CLUSTER_RECOVERY_COMMIT = 0x080E,
    UCN_I_OPCODE_CLUSTER_REKEY_COMMIT = 0x080F,
    UCN_I_OPCODE_CLUSTER_DIRECTORY = 0x0810,
    UCN_I_OPCODE_CLUSTER_TUNNEL = 0x0811,
    UCN_I_OPCODE_DIAGNOSTIC_PING = 0x0901,
    UCN_I_OPCODE_DIAGNOSTIC_PONG = 0x0902,
    UCN_I_OPCODE_DIAGNOSTIC_STATUS_QUERY = 0x0903,
    UCN_I_OPCODE_DIAGNOSTIC_STATUS_RESULT = 0x0904,
    UCN_I_OPCODE_DIAGNOSTIC_ERROR_REPORT = 0x0905
};

/* EN: Registry membership, relay eligibility and local typed termination are
 * deliberately different capabilities.  A relay may forward an authenticated
 * opaque body without claiming that the destination Owner can execute it.
 * 中文：Registry 登记、允许中继与本地强类型终结是三种不同能力。中继可以
 * 转发已认证的不透明 Body，但不得因此声称目标 Owner 已能执行该 Opcode。 */
typedef uint8_t ucn_i_protocol_opcode_support_t;
enum {
    UCN_I_OPCODE_SUPPORT_REGISTERED = 1U << 0,
    UCN_I_OPCODE_SUPPORT_RELAY = 1U << 1,
    UCN_I_OPCODE_SUPPORT_BOOTSTRAP_TERMINAL = 1U << 2,
    UCN_I_OPCODE_SUPPORT_AUTH_TERMINAL = 1U << 3
};

typedef struct ucn_i_c0_frame {
    const uint8_t *payload;
    const uint8_t *origin_tag;
    const uint8_t *hop_tag;
    size_t payload_bytes;
    uint64_t transaction_id;
    uint32_t realm_id;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t source_binding_generation;
    uint32_t destination_binding_generation;
    uint32_t hop_sequence;
    uint16_t opcode;
    uint8_t traffic_class;
    uint8_t delivery_guarantee;
    uint8_t interaction_role;
    uint8_t payload_kind;
    uint8_t origin_security;
    uint8_t hop_limit;
} ucn_i_c0_frame_t;

typedef struct ucn_i_diagnostic_echo {
    uint64_t nonce;
} ucn_i_diagnostic_echo_t;

#define UCN_I_PARENT_PUBLICATION_BYTES 32U
/* EN: Proposal identity is echoed in the body, never borrowed as local nonce.
 * 中文：提案身份只在正文回显，不能借用为本地nonce；Codec不宣称认证。 */
typedef struct ucn_i_parent_publication {
    uint64_t proposal_transaction;
    uint32_t parent_generation;
    uint8_t context_digest[16];
    uint8_t disposition;
} ucn_i_parent_publication_t;

ucn_result_t ucn_i_parent_publication_encode(
    uint16_t opcode, const ucn_i_parent_publication_t *value,
    uint8_t output[UCN_I_PARENT_PUBLICATION_BYTES]);
ucn_result_t ucn_i_parent_publication_decode(
    uint16_t opcode, const uint8_t *input, size_t input_bytes,
    ucn_i_parent_publication_t *value_out);


bool ucn_i_protocol_opcode_valid(uint16_t opcode);
ucn_result_t ucn_i_protocol_opcode_support(
    uint16_t opcode,
    ucn_i_protocol_opcode_support_t *support_out);
ucn_result_t ucn_i_protocol_opcode_domain(uint16_t opcode,
                                          uint8_t *domain_out);
ucn_result_t ucn_i_c0_header_bytes(uint8_t address_width,
                                   size_t *header_bytes_out);
ucn_result_t ucn_i_c0_encoded_size(uint8_t address_width,
                                   uint8_t origin_security,
                                   uint8_t hop_profile,
                                   size_t payload_bytes,
                                   size_t *encoded_bytes_out);
ucn_result_t ucn_i_c0_encode(const ucn_i_c0_frame_t *frame,
                             uint8_t address_width,
                             uint8_t hop_profile,
                             uint8_t *output,
                             size_t output_capacity,
                             size_t *output_bytes);
ucn_result_t ucn_i_c0_decode(const uint8_t *input,
                             size_t input_bytes,
                             uint8_t address_width,
                             uint8_t hop_profile,
                             ucn_i_c0_frame_t *frame_out);
ucn_result_t ucn_i_c0_origin_aad_build(
    const ucn_i_c0_frame_t *frame,
    uint8_t address_width,
    uint8_t output[UCN_I_C0_ORIGIN_AAD_MAX_BYTES],
    size_t *output_bytes);
ucn_result_t ucn_i_diagnostic_echo_encode(
    const ucn_i_diagnostic_echo_t *value,
    uint8_t output[UCN_I_DIAGNOSTIC_ECHO_BYTES]);
ucn_result_t ucn_i_diagnostic_echo_decode(
    const uint8_t input[UCN_I_DIAGNOSTIC_ECHO_BYTES],
    ucn_i_diagnostic_echo_t *value_out);

#endif
