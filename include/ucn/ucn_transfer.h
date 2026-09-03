#ifndef UCN_TRANSFER_H
#define UCN_TRANSFER_H

#include "ucn/ucn_node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-header fallbacks preserve direct-source and UCN_CONFIG_NO_DEFAULTS
 * builds.  Product/global definitions seen through ucn_config.h win because
 * every value remains guarded by #ifndef. */
#ifndef UCN_TRANSFER_MAX_MESSAGE_BYTES
#define UCN_TRANSFER_MAX_MESSAGE_BYTES ((size_t)8192U)
#endif
#ifndef UCN_TRANSFER_TX_SLOTS
#define UCN_TRANSFER_TX_SLOTS ((size_t)1U)
#endif
#ifndef UCN_TRANSFER_MAX_WINDOW
#define UCN_TRANSFER_MAX_WINDOW ((uint8_t)8U)
#endif
#ifndef UCN_TRANSFER_RX_SLOTS
#define UCN_TRANSFER_RX_SLOTS ((size_t)1U)
#endif
#ifndef UCN_TRANSFER_MAX_ENDPOINTS
#define UCN_TRANSFER_MAX_ENDPOINTS ((size_t)4U)
#endif
#ifndef UCN_TRANSFER_MAX_PEERS
#define UCN_TRANSFER_MAX_PEERS ((size_t)4U)
#endif
#ifndef UCN_TRANSFER_RECENT_COMPLETIONS
#define UCN_TRANSFER_RECENT_COMPLETIONS ((size_t)4U)
#endif
#ifndef UCN_TRANSFER_MAX_RETRIES
#define UCN_TRANSFER_MAX_RETRIES ((uint8_t)3U)
#endif
#ifndef UCN_TRANSFER_ACK_TIMEOUT_MS
#define UCN_TRANSFER_ACK_TIMEOUT_MS UINT32_C(250)
#endif
#ifndef UCN_TRANSFER_RX_TIMEOUT_MS
#define UCN_TRANSFER_RX_TIMEOUT_MS UINT32_C(5000)
#endif
#ifndef UCN_TRANSFER_COMPLETED_HOLD_MS
#define UCN_TRANSFER_COMPLETED_HOLD_MS UINT32_C(30000)
#endif
#ifndef UCN_TRANSFER_RECENT_COMPLETION_MS
#define UCN_TRANSFER_RECENT_COMPLETION_MS UINT32_C(5000)
#endif
#ifndef UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES
#define UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES ((uint16_t)16U)
#endif

#define UCN_TRANSFER_FORMAT_VERSION ((uint8_t)1U)
#define UCN_TRANSFER_FRAGMENT_HEADER_BYTES ((size_t)14U)
#define UCN_TRANSFER_ACK_BYTES ((size_t)8U)
#define UCN_TRANSFER_FLAG_START ((uint8_t)0x01U)
#define UCN_TRANSFER_FLAG_END ((uint8_t)0x02U)
#define UCN_TRANSFER_KNOWN_FLAGS \
    (UCN_TRANSFER_FLAG_START | UCN_TRANSFER_FLAG_END)
#define UCN_TRANSFER_RX_HANDLE_DIRECT UINT32_C(0)

typedef char ucn_transfer_max_message_must_be_32_to_8192[
    UCN_TRANSFER_MAX_MESSAGE_BYTES >= 32U &&
            UCN_TRANSFER_MAX_MESSAGE_BYTES <= 8192U ? 1 : -1];
typedef char ucn_transfer_tx_slots_must_be_1_to_255[
    UCN_TRANSFER_TX_SLOTS >= 1U && UCN_TRANSFER_TX_SLOTS <= 255U ? 1 : -1];
typedef char ucn_transfer_window_must_be_1_to_8[
    UCN_TRANSFER_MAX_WINDOW >= 1U && UCN_TRANSFER_MAX_WINDOW <= 8U ? 1 : -1];
typedef char ucn_transfer_rx_slots_must_be_1_to_255[
    UCN_TRANSFER_RX_SLOTS >= 1U && UCN_TRANSFER_RX_SLOTS <= 255U ? 1 : -1];
typedef char ucn_transfer_endpoints_must_be_1_to_255[
    UCN_TRANSFER_MAX_ENDPOINTS >= 1U &&
            UCN_TRANSFER_MAX_ENDPOINTS <= 255U ? 1 : -1];
typedef char ucn_transfer_peers_must_be_1_to_255[
    UCN_TRANSFER_MAX_PEERS >= 1U && UCN_TRANSFER_MAX_PEERS <= 255U ? 1 : -1];
typedef char ucn_transfer_recent_must_be_1_to_255[
    UCN_TRANSFER_RECENT_COMPLETIONS >= 1U &&
            UCN_TRANSFER_RECENT_COMPLETIONS <= 255U ? 1 : -1];

typedef enum ucn_transfer_class {
    UCN_TRANSFER_CLASS_T32 = 0,
    UCN_TRANSFER_CLASS_T64 = 1,
    UCN_TRANSFER_CLASS_T128 = 2,
    UCN_TRANSFER_CLASS_T256 = 3,
    UCN_TRANSFER_CLASS_T512 = 4,
    UCN_TRANSFER_CLASS_T1K = 5,
    UCN_TRANSFER_CLASS_T2K = 6,
    UCN_TRANSFER_CLASS_T4K = 7,
    UCN_TRANSFER_CLASS_T8K = 8,
    UCN_TRANSFER_CLASS_COUNT = 9
} ucn_transfer_class_t;

typedef enum ucn_transfer_ack_status {
    UCN_TRANSFER_ACK_OK = 0,
    UCN_TRANSFER_ACK_NO_SLOT = 1,
    UCN_TRANSFER_ACK_BAD_FORMAT = 2,
    UCN_TRANSFER_ACK_INTEGRITY_FAIL = 3,
    UCN_TRANSFER_ACK_EXPIRED = 4,
    UCN_TRANSFER_ACK_REJECTED = 5
} ucn_transfer_ack_status_t;

typedef enum ucn_transfer_completion_status {
    /* T32/T64 completed the existing single-frame Node send.  This does not
     * assert remote execution. */
    UCN_TRANSFER_COMPLETION_SENT = 0,
    /* T128..T8K received an ACK for the complete reassembled message. */
    UCN_TRANSFER_COMPLETION_DELIVERED = 1,
    UCN_TRANSFER_COMPLETION_REMOTE_REJECTED = 2,
    UCN_TRANSFER_COMPLETION_TIMEOUT = 3,
    UCN_TRANSFER_COMPLETION_RETRY_EXHAUSTED = 4,
    UCN_TRANSFER_COMPLETION_SEND_FAILED = 5
} ucn_transfer_completion_status_t;

typedef uint32_t ucn_transfer_rx_handle_t;

typedef struct ucn_transfer_fragment {
    ucn_endpoint_t target_endpoint;
    ucn_transfer_class_t transfer_class;
    uint8_t flags;
    uint16_t transfer_id;
    uint16_t total_length;
    uint16_t fragment_offset;
    uint32_t message_crc32;
    const uint8_t *data;
    uint16_t data_length;
} ucn_transfer_fragment_t;

typedef struct ucn_transfer_ack {
    ucn_endpoint_t target_endpoint;
    uint16_t transfer_id;
    uint16_t next_expected_offset;
    ucn_transfer_ack_status_t status;
} ucn_transfer_ack_t;

typedef void (*ucn_transfer_receive_fn)(
    void *context,
    ucn_node_id_t source,
    ucn_session_id_t source_session_id,
    ucn_endpoint_t endpoint,
    ucn_transfer_class_t transfer_class,
    const uint8_t *data,
    uint16_t length,
    ucn_transfer_rx_handle_t handle);

typedef void (*ucn_transfer_completion_fn)(
    void *context,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    uint16_t transfer_id,
    ucn_transfer_completion_status_t status);

typedef uint32_t (*ucn_transfer_now_ms_fn)(void *context);

typedef struct ucn_transfer_config {
    ucn_node_t *node;
    /* One authoritative monotonic 32-bit millisecond clock is mandatory.
     * Send, RX callbacks, ACK/retry handling and Step all sample this source;
     * callers must not provide a second cached timestamp. */
    ucn_transfer_now_ms_fn now_ms;
    void *now_context;
    /* Zero selects the largest fragment data length that fits the current
     * build.  Runtime MTU failures shrink it down to the fixed 16-byte floor. */
    uint16_t fragment_data_limit;
    uint8_t max_retries;
    uint32_t ack_timeout_ms;
    uint32_t rx_timeout_ms;
    uint32_t completed_hold_ms;
    uint32_t recent_completion_ms;
    /* ucn_transfer_init owns Node's generic RX handler for 0x22/0x23.  Frames
     * outside Transfer and static Endpoints are passed to this fallback. */
    ucn_rx_handler_t fallback_rx_handler;
    void *fallback_rx_context;
} ucn_transfer_config_t;

struct ucn_transfer;

typedef struct ucn_transfer_endpoint_binding {
    bool occupied;
    struct ucn_transfer *owner;
    ucn_endpoint_t endpoint;
    ucn_transfer_class_t maximum_class;
    /* Receive-side fail-closed gate.  Fragment/ACK transmission currently
     * follows the Node-wide security policy because target_endpoint lives in
     * the Transfer payload, not in frame.message_type. */
    bool require_e2e;
    ucn_transfer_receive_fn handler;
    void *context;
} ucn_transfer_endpoint_binding_t;

typedef struct ucn_transfer_peer_capability {
    bool occupied;
    ucn_node_id_t node_id;
    ucn_transfer_class_t maximum_class;
    uint8_t maximum_window_size;
    /* Fragmented messages allowed to be simultaneously active for this peer.
     * Zero is normalized to one when the peer is registered, preserving the
     * original v5 single-reassembly-slot contract. */
    uint8_t maximum_concurrent_transfers;
} ucn_transfer_peer_capability_t;

typedef struct ucn_transfer_tx_slot {
    bool occupied;
    ucn_node_id_t destination;
    ucn_endpoint_t endpoint;
    ucn_transfer_class_t transfer_class;
    uint16_t transfer_id;
    const uint8_t *data;
    uint16_t total_length;
    uint16_t acknowledged_offset;
    /* Highest offset submitted at least once.  The interval
     * [acknowledged_offset, inflight_end_offset) is cumulatively outstanding. */
    uint16_t inflight_end_offset;
    uint16_t resend_offset;
    uint16_t resend_end_offset;
    uint16_t fragment_data_limit;
    uint32_t message_crc32;
    uint32_t deadline_ms;
    uint32_t ack_deadline_ms;
    uint8_t retry_count;
    uint8_t window_size;
    bool awaiting_ack;
    bool resend_active;
    /* A Go-Back-N replay has been fully submitted and is waiting for fresh
     * cumulative progress.  Duplicate ACKs at the same offset must not start
     * additional recovery rounds before either progress or ACK timeout. */
    bool recovery_waiting_ack;
    ucn_transfer_completion_fn completion;
    void *completion_context;
} ucn_transfer_tx_slot_t;

typedef struct ucn_transfer_rx_slot {
    bool occupied;
    bool completed;
    ucn_node_id_t source;
    ucn_session_id_t source_session_id;
    ucn_endpoint_t endpoint;
    ucn_transfer_class_t transfer_class;
    uint16_t transfer_id;
    uint16_t total_length;
    uint16_t received_length;
    uint16_t fragment_count;
    uint16_t generation;
    uint32_t message_crc32;
    uint32_t deadline_ms;
    uint8_t data[UCN_TRANSFER_MAX_MESSAGE_BYTES];
} ucn_transfer_rx_slot_t;

typedef struct ucn_transfer_recent_completion {
    bool occupied;
    ucn_node_id_t source;
    ucn_session_id_t source_session_id;
    ucn_endpoint_t endpoint;
    uint16_t transfer_id;
    uint16_t total_length;
    uint32_t message_crc32;
    uint32_t expires_at_ms;
} ucn_transfer_recent_completion_t;

typedef struct ucn_transfer_stats {
    uint32_t direct_sent;
    uint32_t tx_accepted;
    uint32_t fragments_sent;
    uint32_t fragments_retried;
    uint32_t messages_delivered;
    uint32_t tx_failed;
    uint32_t fragments_received;
    uint32_t fragments_duplicate_or_out_of_order;
    uint32_t messages_reassembled;
    uint32_t rx_rejected;
    uint32_t integrity_failed;
    uint32_t rx_slot_full;
    uint32_t rx_expired;
    uint32_t completed_hold_expired;
    uint32_t acknowledgements_sent;
    uint32_t acknowledgements_received;
    uint32_t fragments_in_flight_peak;
    uint32_t window_recovery_rounds;
} ucn_transfer_stats_t;

typedef struct ucn_transfer {
    ucn_transfer_config_t config;
    ucn_transfer_endpoint_binding_t endpoints[UCN_TRANSFER_MAX_ENDPOINTS];
    ucn_transfer_peer_capability_t peers[UCN_TRANSFER_MAX_PEERS];
    ucn_transfer_tx_slot_t tx_slots[UCN_TRANSFER_TX_SLOTS];
    ucn_transfer_rx_slot_t rx_slots[UCN_TRANSFER_RX_SLOTS];
    ucn_transfer_recent_completion_t
        recent[UCN_TRANSFER_RECENT_COMPLETIONS];
    ucn_transfer_stats_t stats;
    uint32_t now_ms;
    uint16_t next_transfer_id;
    size_t next_tx_slot;
    uint8_t tx_window_size;
    bool initialized;
} ucn_transfer_t;

size_t ucn_transfer_class_max_bytes(ucn_transfer_class_t transfer_class);
ucn_transfer_class_t ucn_transfer_smallest_class_for_length(size_t length);
uint32_t ucn_transfer_crc32(const uint8_t *data, size_t length);

ucn_result_t ucn_transfer_encode_fragment(
    const ucn_transfer_fragment_t *fragment,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
ucn_result_t ucn_transfer_decode_fragment(
    const uint8_t *input,
    size_t input_length,
    ucn_transfer_fragment_t *fragment);
ucn_result_t ucn_transfer_encode_ack(const ucn_transfer_ack_t *ack,
                                     uint8_t output[UCN_TRANSFER_ACK_BYTES]);
ucn_result_t ucn_transfer_decode_ack(const uint8_t *input,
                                     size_t input_length,
                                     ucn_transfer_ack_t *ack);

ucn_result_t ucn_transfer_init(ucn_transfer_t *transfer,
                               const ucn_transfer_config_t *config);
ucn_result_t ucn_transfer_bind_endpoint(
    ucn_transfer_t *transfer,
    ucn_endpoint_t endpoint,
    ucn_transfer_class_t maximum_class,
    bool require_e2e,
    ucn_transfer_receive_fn handler,
    void *context);
ucn_result_t ucn_transfer_set_peer_capability(
    ucn_transfer_t *transfer,
    ucn_node_id_t node_id,
    ucn_transfer_class_t maximum_class);
/* Set the local desired cumulative-ACK window.  Initialization always starts
 * at 1, preserving the original Stop-and-Wait behavior.  Call only while no
 * TX Slot is active. */
ucn_result_t ucn_transfer_set_tx_window_size(
    ucn_transfer_t *transfer,
    uint8_t tx_window_size);
/* Window capability is deliberately separate from size capability.  Peers
 * created by ucn_transfer_set_peer_capability() default to 1, preserving
 * interoperability with the original v5 Stop-and-Wait receiver. */
ucn_result_t ucn_transfer_set_peer_window_capability(
    ucn_transfer_t *transfer,
    ucn_node_id_t node_id,
    uint8_t maximum_window_size);
/* This is a local/product capability contract, not a wire-format change.
 * Peers default to one concurrent fragmented Transfer. Raise the limit only
 * when the destination provisions at least the same number of RX Slots. */
ucn_result_t ucn_transfer_set_peer_concurrency_capability(
    ucn_transfer_t *transfer,
    ucn_node_id_t node_id,
    uint8_t maximum_concurrent_transfers);

/* T32/T64 use the existing single-frame Endpoint path.  T128..T8K retain the
 * caller's immutable buffer until completion and use bounded fragmentation.
 * UCN_OK means accepted (or synchronously sent for T32/T64), never that a
 * remote actuator executed the message. */
ucn_result_t ucn_transfer_send(
    ucn_transfer_t *transfer,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_transfer_class_t transfer_class,
    const uint8_t *data,
    uint16_t length,
    ucn_transfer_completion_fn completion,
    void *completion_context);

/* Call from the single Protocol Owner.  The Transfer samples config.now_ms on
 * every call, so the callback must observe the same monotonic clock used by
 * the Node/Port.  To preserve Core Q0-Q3 and maintenance priority, call the
 * selected Port/Protocol Owner step first and call this only when Core reports
 * UCN_ERR_NOT_FOUND.  At most one new/retried fragment is submitted per call. */
ucn_result_t ucn_transfer_step(ucn_transfer_t *transfer);

/* A completed fragmented message remains in its fixed RX Slot until the
 * consumer releases this handle or the bounded completed-hold timeout fires.
 * Direct T32/T64 callbacks receive UCN_TRANSFER_RX_HANDLE_DIRECT. */
ucn_result_t ucn_transfer_release_received(ucn_transfer_t *transfer,
                                           ucn_transfer_rx_handle_t handle);

const ucn_transfer_stats_t *ucn_transfer_get_stats(
    const ucn_transfer_t *transfer);

#ifdef __cplusplus
}
#endif

#endif
