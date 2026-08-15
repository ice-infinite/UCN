#ifndef UCN_TYPES_H
#define UCN_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ucn/ucn_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UCN_PROTOCOL_VERSION
#define UCN_PROTOCOL_VERSION ((uint8_t)5U)
#endif
typedef char ucn_protocol_version_must_fit_six_wire_bits[
    UCN_PROTOCOL_VERSION <= 0x3FU ? 1 : -1];
#define UCN_FRAME_MAGIC_0 ((uint8_t)0x55U)
#define UCN_FRAME_MAGIC_1 ((uint8_t)0x43U)
#define UCN_FRAME_W0_HEADER_SIZE ((size_t)17U)
#define UCN_FRAME_W1_HEADER_SIZE ((size_t)21U)
#define UCN_FRAME_W2_HEADER_SIZE ((size_t)26U)
#define UCN_FRAME_W3_HEADER_SIZE ((size_t)30U)
#define UCN_FRAME_W0_ROUTE_HEADER_SIZE ((size_t)18U)
#define UCN_FRAME_W1_ROUTE_HEADER_SIZE ((size_t)23U)
#define UCN_FRAME_W2_ROUTE_HEADER_SIZE ((size_t)28U)
#define UCN_FRAME_W3_ROUTE_HEADER_SIZE ((size_t)32U)
#define UCN_FRAME_W0_PATH_HEADER_SIZE ((size_t)19U)
#define UCN_FRAME_W1_PATH_HEADER_SIZE ((size_t)25U)
#define UCN_FRAME_W2_PATH_HEADER_SIZE ((size_t)31U)
#define UCN_FRAME_W3_PATH_HEADER_SIZE ((size_t)36U)
/* Compatibility names mean the conservative fixed W3 encoding. */
#define UCN_FRAME_HEADER_SIZE UCN_FRAME_W3_HEADER_SIZE
#define UCN_FRAME_ROUTE_HEADER_SIZE UCN_FRAME_W3_ROUTE_HEADER_SIZE
#define UCN_FRAME_PATH_HEADER_SIZE UCN_FRAME_W3_PATH_HEADER_SIZE
#define UCN_E2E_TAG_SIZE ((size_t)16U)
#define UCN_FRAME_FLAG_ROUTE_EXTENSION ((uint8_t)0x01U)
#define UCN_FRAME_FLAG_E2E_PROTECTED ((uint8_t)0x02U)
#define UCN_FRAME_FLAG_DIAGNOSTIC ((uint8_t)0x04U)
#define UCN_FRAME_FLAG_PATH_ID ((uint8_t)0x08U)
#define UCN_FRAME_KNOWN_FLAGS \
    (UCN_FRAME_FLAG_ROUTE_EXTENSION | UCN_FRAME_FLAG_E2E_PROTECTED | \
     UCN_FRAME_FLAG_DIAGNOSTIC | UCN_FRAME_FLAG_PATH_ID)
#ifndef UCN_MAX_FRAME_BYTES
#define UCN_MAX_FRAME_BYTES ((size_t)256U)
#endif

typedef char ucn_max_frame_must_fit_header[
    UCN_MAX_FRAME_BYTES >= (UCN_FRAME_W3_HEADER_SIZE + 1U) ? 1 : -1];

#ifndef UCN_MAX_PAYLOAD_BYTES
/* Header compression does not silently grow the static payload buffers. */
#define UCN_MAX_PAYLOAD_BYTES \
    ((UCN_MAX_FRAME_BYTES > (size_t)32U) ? \
         (UCN_MAX_FRAME_BYTES - (size_t)32U) : (size_t)0U)
#endif
typedef char ucn_payload_buffer_must_not_be_empty[
    UCN_MAX_PAYLOAD_BYTES >= 1U ? 1 : -1];
#define UCN_NODE_BROADCAST UINT32_C(0xFFFFFFFF)
#ifndef UCN_MAX_HOPS
#define UCN_MAX_HOPS ((uint8_t)16U)
#endif

typedef char ucn_max_hops_must_be_1_to_254[
    UCN_MAX_HOPS >= 1U && UCN_MAX_HOPS <= 254U ? 1 : -1];

typedef uint32_t ucn_node_id_t;
typedef uint32_t ucn_network_id_t;
typedef uint32_t ucn_sequence_t;
typedef uint32_t ucn_session_id_t;
typedef uint32_t ucn_route_cost_t;

#define UCN_ROUTE_COST_UNKNOWN UINT32_MAX
#define UCN_ROUTE_COST_MAX (UINT32_MAX - UINT32_C(1))

/* Zero-valued fields inherit the Node defaults when embedded in a Route
 * Policy.  The Node-level setter resolves zero Hop/Cost to bounded defaults. */
typedef struct ucn_route_constraints {
    uint8_t max_hops;
    ucn_route_cost_t max_route_cost;
    uint16_t max_verified_rtt_ms;
    bool require_verified_rtt;
} ucn_route_constraints_t;

typedef struct ucn_route_quality {
    bool available;
    uint8_t hop_count;
    ucn_route_cost_t route_cost;
    bool verified_rtt_valid;
    uint16_t verified_rtt_ms;
} ucn_route_quality_t;

/* API values intentionally differ from the 2-bit wire code so a zero-filled
 * v4-style frame remains an explicit "unspecified" request.  The frame codec
 * resolves unspecified standalone frames to W3; Node TX uses its configured
 * fixed domain or the explicitly enabled route-aware automatic selector. */
typedef uint8_t ucn_wire_profile_t;
enum {
    UCN_WIRE_PROFILE_UNSPECIFIED = 0U,
    UCN_WIRE_PROFILE_W0_LOCAL = 1U,
    UCN_WIRE_PROFILE_W1_EDGE = 2U,
    UCN_WIRE_PROFILE_W2_MESH = 3U,
    UCN_WIRE_PROFILE_W3_BACKBONE = 4U
};

typedef enum ucn_result {
    UCN_OK = 0,
    UCN_ERR_ARGUMENT = -1,
    UCN_ERR_CONFIG = -2,
    UCN_ERR_NO_SPACE = -3,
    UCN_ERR_TOO_LARGE = -4,
    UCN_ERR_MALFORMED = -5,
    UCN_ERR_VERSION = -6,
    UCN_ERR_NETWORK = -7,
    UCN_ERR_CRC = -8,
    UCN_ERR_TTL = -9,
    UCN_ERR_UNSUPPORTED = -10,
    UCN_ERR_LINK_DOWN = -11,
    UCN_ERR_NOT_FOUND = -12,
    UCN_ERR_SECURITY = -13,
    UCN_ERR_REPLAY = -14,
    UCN_ERR_ACCESS = -15,
    /* CLV2-01-04: fail-closed rejection of an illegal FSM transition.
     * Added for the single cluster_transition() entry point; the caller
     * may not assume any state changed. */
    UCN_ERR_STATE = -16
} ucn_result_t;

typedef enum ucn_traffic_class {
    UCN_TRAFFIC_Q0_CRITICAL = 0,
    UCN_TRAFFIC_Q1_REALTIME = 1,
    UCN_TRAFFIC_Q2_NORMAL = 2,
    UCN_TRAFFIC_Q3_BULK = 3
} ucn_traffic_class_t;

typedef enum ucn_delivery_semantic {
    UCN_DELIVERY_BEST_EFFORT = 0,
    UCN_DELIVERY_LATEST_VALUE = 1,
    /* Q0 queue ownership is retained only while a local Link TX queue reports
     * transient UCN_ERR_NO_SPACE.  This is bounded admission retry, not a
     * remote delivery guarantee or an end-to-end retransmission mode. */
    UCN_DELIVERY_RETRY_ON_BACKPRESSURE = 2
} ucn_delivery_semantic_t;

typedef enum ucn_message_type {
    UCN_MSG_HELLO = 0x01,
    UCN_MSG_JOIN_REQ = 0x02,
    UCN_MSG_JOIN_CHALLENGE = 0x03,
    UCN_MSG_JOIN_ACCEPT = 0x04,
    UCN_MSG_ROUTE_REQ = 0x10,
    UCN_MSG_ROUTE_REPLY = 0x11,
    UCN_MSG_ROUTE_ERROR = 0x12,
    UCN_MSG_HEARTBEAT = 0x13,
    UCN_MSG_PATH_PROBE = 0x14,
    UCN_MSG_PATH_PROBE_ACK = 0x15,
    UCN_MSG_PATH_ACTIVATE = 0x16,
    UCN_MSG_PATH_ACTIVATE_ACK = 0x17,
    UCN_MSG_PATH_TRACE_REQ = 0x18,
    UCN_MSG_PATH_TRACE_REPLY = 0x19,
    UCN_MSG_NODE_SNAPSHOT_REQ = 0x1A,
    UCN_MSG_NODE_SNAPSHOT_REPLY = 0x1B,
    UCN_MSG_PATH_INSTALL = 0x1C,
    UCN_MSG_PATH_REVOKE = 0x1D,
    UCN_MSG_POLICY_DIAGNOSTIC_REQ = 0x1E,
    UCN_MSG_POLICY_DIAGNOSTIC_REPLY = 0x1F,
    UCN_MSG_DATA_Q0 = 0x20,
    UCN_MSG_DATA_Q1 = 0x21,
    /* Optional UCN-Extended Transfer messages.  They remain ordinary routed
     * data for the Core; only endpoint nodes that instantiate ucn_transfer_t
     * interpret or reassemble them. */
    UCN_MSG_TRANSFER_FRAGMENT = 0x22,
    UCN_MSG_TRANSFER_ACK = 0x23
} ucn_message_type_t;

typedef struct ucn_frame {
    uint8_t message_type;
    ucn_wire_profile_t wire_profile;
    ucn_traffic_class_t traffic_class;
    uint8_t flags;
    uint8_t hop_limit;
    ucn_network_id_t network_id;
    ucn_node_id_t source;
    ucn_node_id_t destination;
    ucn_sequence_t sequence;
    ucn_session_id_t session_id;
    bool has_route_extension;
    uint16_t route_epoch;
    bool has_path_id;
    uint32_t path_id;
    const uint8_t *payload;
    uint16_t payload_length;
    const uint8_t *auth_tag;
} ucn_frame_t;

typedef struct ucn_config {
    ucn_network_id_t network_id;
    ucn_node_id_t node_id;
    uint8_t default_hop_limit;
} ucn_config_t;

#ifdef __cplusplus
}
#endif

#endif
