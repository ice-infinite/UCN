#ifndef UCN_CLUSTER_FEDERATION_H
#define UCN_CLUSTER_FEDERATION_H

#include "ucn/ucn_cluster.h"

#ifdef __cplusplus
extern "C" {
#endif

/* C06 is an optional Extended boundary.  It uses a static Endpoint rather
 * than adding a Core message type or changing Wire v5. */
#ifndef UCN_CLUSTER_FED_MAX_DIRECTORY_AUTHORITIES
#define UCN_CLUSTER_FED_MAX_DIRECTORY_AUTHORITIES ((size_t)2U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS
#define UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS ((size_t)17U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS
#define UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS ((size_t)32U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_LOCATOR_CACHE
#define UCN_CLUSTER_FED_MAX_LOCATOR_CACHE ((size_t)16U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS
#define UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_PENDING
#define UCN_CLUSTER_FED_MAX_PENDING ((size_t)2U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS
#define UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_CLUSTER_HEAD_LEASES
#define UCN_CLUSTER_FED_MAX_CLUSTER_HEAD_LEASES ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_FED_DIRECTORY_LEASE_MS
#define UCN_CLUSTER_FED_DIRECTORY_LEASE_MS UINT32_C(8000)
#endif
#ifndef UCN_CLUSTER_FED_LOCATOR_REFRESH_MS
#define UCN_CLUSTER_FED_LOCATOR_REFRESH_MS UINT32_C(2000)
#endif
#ifndef UCN_CLUSTER_FED_QUERY_TIMEOUT_MS
#define UCN_CLUSTER_FED_QUERY_TIMEOUT_MS UINT32_C(1000)
#endif
#ifndef UCN_CLUSTER_FED_TRANSACTION_LEASE_MS
#define UCN_CLUSTER_FED_TRANSACTION_LEASE_MS UINT32_C(3000)
#endif
#ifndef UCN_CLUSTER_FED_HANDOVER_RETRY_MS
#define UCN_CLUSTER_FED_HANDOVER_RETRY_MS UINT32_C(250)
#endif
#ifndef UCN_CLUSTER_FED_HANDOVER_MAX_ATTEMPTS
#define UCN_CLUSTER_FED_HANDOVER_MAX_ATTEMPTS ((uint8_t)3U)
#endif

#define UCN_CLUSTER_FEDERATION_FORMAT_VERSION ((uint8_t)1U)
#define UCN_CLUSTER_FEDERATION_ENDPOINT ((ucn_endpoint_t)0xA1U)
#define UCN_CLUSTER_FEDERATION_COMMON_HEADER_BYTES ((size_t)8U)
#define UCN_CLUSTER_FEDERATION_LOCATOR_BYTES ((size_t)32U)
#define UCN_CLUSTER_FEDERATION_QUERY_BYTES ((size_t)20U)
#define UCN_CLUSTER_FEDERATION_ERROR_BYTES ((size_t)20U)
/* C07.4 fixed-size Cluster Head handover: cluster_id, new Head, new Term,
 * backup generation and an opaque fixed proof validated by the product. */
#define UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES ((size_t)16U)
#define UCN_CLUSTER_FEDERATION_HANDOVER_BYTES ((size_t)40U)
#define UCN_CLUSTER_FEDERATION_SUBMIT_HEADER_BYTES ((size_t)16U)
#define UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES ((size_t)28U)
/* DELIVER carries the original two Cluster IDs as well.  They are routing
 * metadata, but retaining them lets the final node make a complete inner
 * security decision without trusting an intermediate Head. */
#define UCN_CLUSTER_FEDERATION_DELIVER_HEADER_BYTES ((size_t)28U)
#define UCN_CLUSTER_FEDERATION_MAX_INNER_PAYLOAD_BYTES \
    (UCN_MAX_PAYLOAD_BYTES - UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES)

typedef char ucn_cluster_fed_authorities_must_be_nonzero[
    UCN_CLUSTER_FED_MAX_DIRECTORY_AUTHORITIES >= 1U ? 1 : -1];
typedef char ucn_cluster_fed_local_locators_must_cover_head_and_members[
    UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS >= UCN_CLUSTER_MAX_MEMBERS + 1U ? 1 : -1];
typedef char ucn_cluster_fed_pending_must_be_nonzero[
    UCN_CLUSTER_FED_MAX_PENDING >= 1U ? 1 : -1];
typedef char ucn_cluster_fed_seen_must_be_nonzero[
    UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS >= 1U ? 1 : -1];
typedef char ucn_cluster_fed_tunnel_header_must_fit_payload[
    UCN_MAX_PAYLOAD_BYTES >= UCN_CLUSTER_FEDERATION_TUNNEL_HEADER_BYTES ? 1 : -1];

typedef enum ucn_cluster_federation_kind {
    UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER = 1,
    UCN_CLUSTER_FED_KIND_LOCATOR_WITHDRAW = 2,
    UCN_CLUSTER_FED_KIND_LOCATOR_QUERY = 3,
    UCN_CLUSTER_FED_KIND_LOCATOR_REPLY = 4,
    UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT = 5,
    UCN_CLUSTER_FED_KIND_TUNNEL_DATA = 6,
    UCN_CLUSTER_FED_KIND_TUNNEL_DELIVER = 7,
    UCN_CLUSTER_FED_KIND_TUNNEL_ERROR = 8,
    UCN_CLUSTER_FED_KIND_NEXT_CLUSTER_ANNOUNCE = 9,
    UCN_CLUSTER_FED_KIND_NEXT_CLUSTER_WITHDRAW = 10,
    UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER = 11
} ucn_cluster_federation_kind_t;

typedef enum ucn_cluster_federation_error {
    UCN_CLUSTER_FED_ERROR_DIRECTORY_NOT_FOUND = 1,
    UCN_CLUSTER_FED_ERROR_DIRECTORY_STALE = 2,
    UCN_CLUSTER_FED_ERROR_UNAUTHORIZED = 3,
    UCN_CLUSTER_FED_ERROR_TTL = 4,
    UCN_CLUSTER_FED_ERROR_MTU = 5,
    UCN_CLUSTER_FED_ERROR_DOWNSTREAM = 6,
    UCN_CLUSTER_FED_ERROR_TIMEOUT = 7
} ucn_cluster_federation_error_t;

typedef struct ucn_cluster_locator {
    ucn_node_id_t node_id;
    uint32_t cluster_id;
    ucn_node_id_t head_node_id;
    uint32_t term;
    uint32_t lease_ms;
    uint32_t record_nonce;
} ucn_cluster_locator_t;

typedef struct ucn_cluster_federation_query {
    ucn_node_id_t target_node_id;
    uint32_t requester_cluster_id;
    ucn_node_id_t requester_head_node_id;
} ucn_cluster_federation_query_t;

typedef struct ucn_cluster_federation_submit {
    ucn_node_id_t final_node_id;
    ucn_endpoint_t endpoint;
    ucn_traffic_class_t traffic_class;
    const uint8_t *inner_payload;
    uint16_t inner_length;
} ucn_cluster_federation_submit_t;

typedef struct ucn_cluster_federation_tunnel {
    ucn_node_id_t origin_node_id;
    ucn_node_id_t final_node_id;
    uint32_t origin_cluster_id;
    uint32_t destination_cluster_id;
    ucn_endpoint_t endpoint;
    ucn_traffic_class_t traffic_class;
    const uint8_t *inner_payload;
    uint16_t inner_length;
} ucn_cluster_federation_tunnel_t;

typedef struct ucn_cluster_federation_delivery {
    ucn_node_id_t origin_node_id;
    ucn_node_id_t final_node_id;
    uint32_t origin_cluster_id;
    uint32_t destination_cluster_id;
    ucn_endpoint_t endpoint;
    ucn_traffic_class_t traffic_class;
    const uint8_t *inner_payload;
    uint16_t inner_length;
} ucn_cluster_federation_delivery_t;

typedef struct ucn_cluster_federation_error_message {
    ucn_cluster_federation_error_t error;
    ucn_node_id_t origin_node_id;
    ucn_node_id_t final_node_id;
} ucn_cluster_federation_error_message_t;

/* C07.4 atomic Head handover request.  cluster_id and new_term make the
 * takeover monotonic; backup_generation ties it to the Backup assignment;
 * proof is opaque to the protocol and validated by authorize_handover(). */
typedef struct ucn_cluster_federation_handover {
    uint32_t cluster_id;
    ucn_node_id_t new_head_node_id;
    uint32_t new_term;
    uint32_t backup_generation;
    uint8_t proof[UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES];
} ucn_cluster_federation_handover_t;

typedef struct ucn_cluster_federation_message {
    ucn_cluster_federation_kind_t kind;
    uint8_t flags;
    uint8_t hop_limit;
    uint32_t transaction_id;
    union {
        ucn_cluster_locator_t locator;
        ucn_cluster_federation_query_t query;
        ucn_cluster_federation_submit_t submit;
        ucn_cluster_federation_tunnel_t tunnel;
        ucn_cluster_federation_delivery_t delivery;
        ucn_cluster_federation_error_message_t error;
        ucn_cluster_federation_handover_t handover;
    } body;
} ucn_cluster_federation_message_t;

typedef uint32_t (*ucn_cluster_federation_now_ms_fn)(void *context);
typedef enum ucn_cluster_federation_inner_security_mode {
    /* The safe C06.3 default when Tunnel handling is explicitly enabled. */
    UCN_CLUSTER_FED_INNER_SECURITY_REQUIRED = 0,
    /* Bench/diagnostic only: Core outer protection remains the only guard. */
    UCN_CLUSTER_FED_INNER_SECURITY_PROTECTED_OUTER_ONLY = 1
} ucn_cluster_federation_inner_security_mode_t;

/* Metadata bound to an inner end-to-end security operation.  Cluster IDs are
 * carried to the final node for audit/context, while source/final Endpoint,
 * QoS and transaction are mandatory semantic bindings. */
typedef struct ucn_cluster_federation_inner_aad {
    uint32_t transaction_id;
    ucn_node_id_t origin_node_id;
    ucn_node_id_t final_node_id;
    uint32_t origin_cluster_id;
    uint32_t destination_cluster_id;
    ucn_endpoint_t endpoint;
    ucn_traffic_class_t traffic_class;
} ucn_cluster_federation_inner_aad_t;

typedef ucn_result_t (*ucn_cluster_federation_send_fn)(
    void *context,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length);
/* This product callback establishes which Node IDs are allowed to act as
 * Head/Directory clients.  It is mandatory on a Directory Authority. */
typedef bool (*ucn_cluster_federation_authorize_head_fn)(
    void *context,
    ucn_node_id_t source);
/* C07.4: validates a takeover on a Directory Authority.  Only the BACKUP_READY
 * Backup of a Cluster may be authorized; the callback inspects cluster_id,
 * new Head, Term, backup generation and the opaque proof.  It is mandatory
 * whenever require_protected_control=true; NULL is permitted only in an
 * explicit unprotected laboratory configuration. */
typedef bool (*ucn_cluster_federation_authorize_handover_fn)(
    void *context,
    const ucn_cluster_federation_handover_t *handover);
/* Build the opaque proof before a handover is sent.  The callback owns all
 * product key material; UCN binds only the fixed handover fields. */
typedef ucn_result_t (*ucn_cluster_federation_build_handover_proof_fn)(
    void *context,
    const ucn_cluster_federation_handover_t *handover,
    uint8_t proof[UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES]);
typedef ucn_result_t (*ucn_cluster_federation_inner_seal_fn)(
    void *context,
    const ucn_cluster_federation_inner_aad_t *aad,
    const uint8_t *plaintext,
    uint16_t plaintext_length,
    uint8_t *ciphertext,
    uint16_t ciphertext_capacity,
    uint16_t *ciphertext_length);
typedef ucn_result_t (*ucn_cluster_federation_inner_open_fn)(
    void *context,
    const ucn_cluster_federation_inner_aad_t *aad,
    const uint8_t *ciphertext,
    uint16_t ciphertext_length,
    uint8_t *plaintext,
    uint16_t plaintext_capacity,
    uint16_t *plaintext_length);
typedef ucn_result_t (*ucn_cluster_federation_deliver_fn)(
    void *context,
    const ucn_cluster_federation_inner_aad_t *aad,
    const uint8_t *payload,
    uint16_t payload_length);
typedef void (*ucn_cluster_federation_error_fn)(
    void *context,
    uint32_t transaction_id,
    const ucn_cluster_federation_error_message_t *error);

typedef struct ucn_cluster_federation_config {
    ucn_node_id_t local_node_id;
    const ucn_cluster_t *cluster;
    bool enabled;
    bool directory_authority;
    bool require_protected_control;
    /* Disabled by default to preserve C06.2 Directory-only products.  When
     * enabled, REQUIRED inner security is the default and a provider is
     * mandatory unless the product explicitly selects the diagnostic mode. */
    bool enable_tunnel;
    ucn_cluster_federation_inner_security_mode_t inner_security_mode;
    uint8_t default_hop_limit;
    uint32_t directory_lease_ms;
    uint32_t locator_refresh_ms;
    uint32_t query_timeout_ms;
    const ucn_node_id_t *directory_authorities;
    size_t directory_authority_count;
    ucn_cluster_federation_now_ms_fn now_ms;
    void *now_context;
    ucn_cluster_federation_send_fn send;
    void *send_context;
    ucn_cluster_federation_authorize_head_fn authorize_head;
    void *authorize_context;
    ucn_cluster_federation_authorize_handover_fn authorize_handover;
    void *authorize_handover_context;
    ucn_cluster_federation_build_handover_proof_fn build_handover_proof;
    void *handover_proof_context;
    ucn_cluster_federation_inner_seal_fn seal_inner;
    ucn_cluster_federation_inner_open_fn open_inner;
    void *inner_security_context;
    ucn_cluster_federation_deliver_fn deliver;
    void *deliver_context;
    ucn_cluster_federation_error_fn on_error;
    void *error_context;
} ucn_cluster_federation_config_t;

/* C07.4 fixed ClusterHeadLease indirection.  One entry per Cluster maps the
 * stable Cluster ID to the Head that currently owns Directory publication
 * and Tunnel Gateway duty.  Replacing it atomically is the whole handover. */
typedef struct ucn_cluster_federation_cluster_head_lease {
    bool occupied;
    uint32_t cluster_id;
    ucn_node_id_t head_node_id;
    uint32_t term;
    uint32_t backup_generation;
    uint32_t lease_expires_at_ms;
    uint8_t handover_proof[UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES];
} ucn_cluster_federation_cluster_head_lease_t;

typedef struct ucn_cluster_federation_directory_record {
    bool occupied;
    ucn_cluster_locator_t locator;
    uint32_t expires_at_ms;
} ucn_cluster_federation_directory_record_t;

typedef struct ucn_cluster_federation_locator_cache_entry {
    bool occupied;
    ucn_cluster_locator_t locator;
    uint32_t expires_at_ms;
} ucn_cluster_federation_locator_cache_entry_t;

typedef struct ucn_cluster_federation_next_cluster_entry {
    bool occupied;
    uint32_t cluster_id;
    ucn_node_id_t head_node_id;
    uint32_t term;
    uint32_t expires_at_ms;
} ucn_cluster_federation_next_cluster_entry_t;

typedef struct ucn_cluster_federation_local_locator_entry {
    bool occupied;
    bool observed;
    bool withdrawal_pending;
    size_t authority_cursor;
    ucn_cluster_locator_t locator;
} ucn_cluster_federation_local_locator_entry_t;

typedef struct ucn_cluster_federation_pending_query {
    bool occupied;
    uint32_t transaction_id;
    ucn_node_id_t target_node_id;
    ucn_node_id_t authority_node_id;
    uint32_t deadline_ms;
    size_t next_authority_cursor;
    size_t attempted_authority_count;
} ucn_cluster_federation_pending_query_t;

typedef struct ucn_cluster_federation_seen_transaction {
    bool occupied;
    uint32_t transaction_id;
    ucn_node_id_t origin_node_id;
    ucn_node_id_t final_node_id;
    /* The remote Head expected to report a downstream error.  Zero means
     * this record is terminal-only and cannot forward an Error upstream. */
    ucn_node_id_t remote_head_node_id;
    uint32_t expires_at_ms;
} ucn_cluster_federation_seen_transaction_t;

typedef struct ucn_cluster_federation_stats {
    uint32_t messages_sent;
    uint32_t messages_received;
    uint32_t malformed_messages;
    uint32_t security_rejected;
    uint32_t authorization_rejected;
    uint32_t replay_rejected;
    uint32_t records_registered;
    uint32_t records_withdrawn;
    uint32_t records_expired;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t cache_expired;
    uint32_t queries_sent;
    uint32_t query_timeouts;
    uint32_t query_errors;
    uint32_t local_locator_overflow;
    uint32_t send_failures;
    uint32_t tunnel_submits;
    uint32_t tunnel_data_sent;
    uint32_t tunnel_deliveries;
    uint32_t tunnel_errors;
    uint32_t tunnel_replays;
    uint32_t tunnel_ttl_rejected;
    uint32_t tunnel_stale_rejected;
    uint32_t handovers_accepted;
    uint32_t handovers_rejected;
} ucn_cluster_federation_stats_t;

typedef struct ucn_cluster_federation {
    ucn_cluster_federation_config_t config;
    uint32_t now_ms;
    uint32_t next_transaction_id;
    uint32_t next_record_nonce;
    uint32_t next_publish_at_ms;
    size_t publish_locator_cursor;
    size_t query_authority_cursor;
    ucn_node_id_t directory_authorities[UCN_CLUSTER_FED_MAX_DIRECTORY_AUTHORITIES];
    ucn_cluster_federation_local_locator_entry_t
        local_locators[UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS];
    ucn_cluster_federation_directory_record_t
        directory_records[UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS];
    ucn_cluster_federation_locator_cache_entry_t
        locator_cache[UCN_CLUSTER_FED_MAX_LOCATOR_CACHE];
    ucn_cluster_federation_next_cluster_entry_t
        next_clusters[UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS];
    ucn_cluster_federation_pending_query_t
        pending[UCN_CLUSTER_FED_MAX_PENDING];
    ucn_cluster_federation_seen_transaction_t
        seen[UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS];
    ucn_cluster_federation_cluster_head_lease_t
        cluster_head_leases[UCN_CLUSTER_FED_MAX_CLUSTER_HEAD_LEASES];
    /* C07.4 local-Head transition tracking for automatic handover publish. */
    bool was_local_head;
    uint32_t last_handover_term;
    bool handover_pending;
    uint32_t pending_handover_term;
    uint32_t next_handover_retry_ms;
    uint8_t handover_attempts;
    ucn_cluster_federation_stats_t stats;
} ucn_cluster_federation_t;

/* Size is zero for a malformed/unencodable object.  C06.1 freezes the format;
 * C06.2 uses only Locator messages.  Tunnel dispatch remains a later task. */
size_t ucn_cluster_federation_message_encoded_size(
    const ucn_cluster_federation_message_t *message);
ucn_result_t ucn_cluster_federation_message_encode(
    const ucn_cluster_federation_message_t *message,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length);
ucn_result_t ucn_cluster_federation_message_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_federation_message_t *message);

/* C06.2 Directory Runtime plus opt-in C06.3 single-frame Tunnel Runtime.
 * When enable_tunnel is false, Submit/Data/Deliver remain unsupported. */
ucn_result_t ucn_cluster_federation_init(
    ucn_cluster_federation_t *federation,
    const ucn_cluster_federation_config_t *config);
ucn_result_t ucn_cluster_federation_receive(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    bool protected_outer,
    const uint8_t *payload,
    size_t payload_length);
ucn_result_t ucn_cluster_federation_step(
    ucn_cluster_federation_t *federation);
/* A successful call means a valid cached record exists or a bounded directory
 * query was accepted.  Completion is observed through find_locator(). */
ucn_result_t ucn_cluster_federation_query_locator(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t target_node_id);
/* Submit one cross-Cluster message through the current local Head.  It never
 * changes ordinary ucn_node_send_endpoint() behavior.  The source Cluster
 * Head must already have a valid Locator for final_node_id; otherwise it
 * starts a bounded Directory query and reports DIRECTORY_NOT_FOUND to the
 * source, which may retry later with a new transaction. */
ucn_result_t ucn_cluster_federation_send(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t final_node_id,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length);
const ucn_cluster_locator_t *ucn_cluster_federation_find_locator(
    const ucn_cluster_federation_t *federation,
    ucn_node_id_t target_node_id);
const ucn_cluster_federation_next_cluster_entry_t *
ucn_cluster_federation_find_next_cluster(
    const ucn_cluster_federation_t *federation,
    uint32_t cluster_id);
/* C07.4: the newly promoted Head publishes one ClusterHeadLease handover to
 * every configured Directory Authority.  Called automatically by step() on a
 * local-Head transition and available for explicit product use. */
ucn_result_t ucn_cluster_federation_publish_handover(
    ucn_cluster_federation_t *federation);
const ucn_cluster_federation_cluster_head_lease_t *
ucn_cluster_federation_find_head_lease(
    const ucn_cluster_federation_t *federation,
    uint32_t cluster_id);
const ucn_cluster_federation_stats_t *ucn_cluster_federation_get_stats(
    const ucn_cluster_federation_t *federation);

#ifdef __cplusplus
}
#endif

#endif
