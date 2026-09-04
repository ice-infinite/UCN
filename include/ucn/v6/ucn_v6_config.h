#ifndef UCN_V6_CONFIG_H
#define UCN_V6_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_API_VERSION UINT16_C(1)
#define UCN_V6_STORAGE_LAYOUT UINT16_C(1)
#define UCN_V6_STORAGE_ALIGNMENT ((size_t)8U)

#ifndef UCN_V6_CONFIG_MAX_BINDINGS
#define UCN_V6_CONFIG_MAX_BINDINGS 16U
#endif
#ifndef UCN_V6_CONFIG_MAX_ACTIVE_GROUPS
#define UCN_V6_CONFIG_MAX_ACTIVE_GROUPS 8U
#endif
#ifndef UCN_V6_CONFIG_BOOTSTRAP_PENDING
#define UCN_V6_CONFIG_BOOTSTRAP_PENDING 8U
#endif
#ifndef UCN_V6_CONFIG_BOOTSTRAP_LINKS
#define UCN_V6_CONFIG_BOOTSTRAP_LINKS 8U
#endif
#ifndef UCN_V6_CONFIG_OPERATION_SLOTS
#define UCN_V6_CONFIG_OPERATION_SLOTS 8U
#endif
#ifndef UCN_V6_CONFIG_OPERATION_HIGH_WATERS
#define UCN_V6_CONFIG_OPERATION_HIGH_WATERS 8U
#endif
#ifndef UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS
#define UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS 8U
#endif
#ifndef UCN_V6_CONFIG_STATIC_GROUP_SLOTS
#define UCN_V6_CONFIG_STATIC_GROUP_SLOTS 8U
#endif
#ifndef UCN_V6_CONFIG_GROUP_KEY_SLOTS
#define UCN_V6_CONFIG_GROUP_KEY_SLOTS 4U
#endif
#ifndef UCN_V6_CONFIG_OWNER_EVENT_DEPTH
#define UCN_V6_CONFIG_OWNER_EVENT_DEPTH 255U
#endif
#ifndef UCN_V6_CONFIG_SECURITY_SESSIONS
#define UCN_V6_CONFIG_SECURITY_SESSIONS 8U
#endif
#ifndef UCN_V6_CONFIG_ACL_ENTRIES
#define UCN_V6_CONFIG_ACL_ENTRIES 16U
#endif
#ifndef UCN_V6_CONFIG_GROUP_REPLAY_SOURCES
#define UCN_V6_CONFIG_GROUP_REPLAY_SOURCES 16U
#endif
#ifndef UCN_V6_CONFIG_CAPABILITY_PEERS
#define UCN_V6_CONFIG_CAPABILITY_PEERS 16U
#endif
#ifndef UCN_V6_CONFIG_CAPABILITY_PATHS
#define UCN_V6_CONFIG_CAPABILITY_PATHS 16U
#endif
#ifndef UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS
#define UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS 8U
#endif
#ifndef UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS
#define UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS 8U
#endif
#ifndef UCN_V6_CONFIG_ROUTE_SETS
#define UCN_V6_CONFIG_ROUTE_SETS 16U
#endif
#ifndef UCN_V6_CONFIG_ROUTE_PATHS_PER_SET
#define UCN_V6_CONFIG_ROUTE_PATHS_PER_SET 4U
#endif
#ifndef UCN_V6_CONFIG_ROUTE_CANDIDATES
#define UCN_V6_CONFIG_ROUTE_CANDIDATES 16U
#endif
#ifndef UCN_V6_CONFIG_ROUTE_FLOW_PINS
#define UCN_V6_CONFIG_ROUTE_FLOW_PINS 32U
#endif
#ifndef UCN_V6_CONFIG_METRIC_PATHS
#define UCN_V6_CONFIG_METRIC_PATHS 32U
#endif
#ifndef UCN_V6_CONFIG_QOS_Q0_DEPTH
#define UCN_V6_CONFIG_QOS_Q0_DEPTH 16U
#endif
#ifndef UCN_V6_CONFIG_QOS_Q1_DEPTH
#define UCN_V6_CONFIG_QOS_Q1_DEPTH 16U
#endif
#ifndef UCN_V6_CONFIG_QOS_Q2_DEPTH
#define UCN_V6_CONFIG_QOS_Q2_DEPTH 32U
#endif
#ifndef UCN_V6_CONFIG_QOS_Q3_DEPTH
#define UCN_V6_CONFIG_QOS_Q3_DEPTH 32U
#endif
#ifndef UCN_V6_CONFIG_QOS_FLOW_SLOTS
#define UCN_V6_CONFIG_QOS_FLOW_SLOTS 32U
#endif
#ifndef UCN_V6_CONFIG_QOS_INFLIGHT
#define UCN_V6_CONFIG_QOS_INFLIGHT 32U
#endif

#if UCN_V6_CONFIG_MAX_BINDINGS < 1U || UCN_V6_CONFIG_MAX_BINDINGS > 255U
#error "UCN_V6_CONFIG_MAX_BINDINGS must be 1..255"
#endif
#if UCN_V6_CONFIG_MAX_ACTIVE_GROUPS < 1U || \
    UCN_V6_CONFIG_MAX_ACTIVE_GROUPS > 255U
#error "UCN_V6_CONFIG_MAX_ACTIVE_GROUPS must be 1..255"
#endif
#if UCN_V6_CONFIG_BOOTSTRAP_PENDING < 1U || \
    UCN_V6_CONFIG_BOOTSTRAP_PENDING > 255U
#error "UCN_V6_CONFIG_BOOTSTRAP_PENDING must be 1..255"
#endif
#if UCN_V6_CONFIG_BOOTSTRAP_LINKS < 1U || \
    UCN_V6_CONFIG_BOOTSTRAP_LINKS > 255U
#error "UCN_V6_CONFIG_BOOTSTRAP_LINKS must be 1..255"
#endif
#if UCN_V6_CONFIG_OPERATION_SLOTS < 1U || \
    UCN_V6_CONFIG_OPERATION_SLOTS > 255U
#error "UCN_V6_CONFIG_OPERATION_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_OPERATION_HIGH_WATERS < 1U || \
    UCN_V6_CONFIG_OPERATION_HIGH_WATERS > 255U
#error "UCN_V6_CONFIG_OPERATION_HIGH_WATERS must be 1..255"
#endif
#if UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS < 1U || \
    UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS > 255U
#error "UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_STATIC_GROUP_SLOTS < 1U || \
    UCN_V6_CONFIG_STATIC_GROUP_SLOTS > 255U
#error "UCN_V6_CONFIG_STATIC_GROUP_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_GROUP_KEY_SLOTS < 1U || \
    UCN_V6_CONFIG_GROUP_KEY_SLOTS > 255U
#error "UCN_V6_CONFIG_GROUP_KEY_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_OWNER_EVENT_DEPTH < 1U || \
    UCN_V6_CONFIG_OWNER_EVENT_DEPTH > 65535U
#error "UCN_V6_CONFIG_OWNER_EVENT_DEPTH must be 1..65535"
#endif
#if UCN_V6_CONFIG_SECURITY_SESSIONS < 1U || \
    UCN_V6_CONFIG_SECURITY_SESSIONS > 255U
#error "UCN_V6_CONFIG_SECURITY_SESSIONS must be 1..255"
#endif
#if UCN_V6_CONFIG_ACL_ENTRIES < 1U || UCN_V6_CONFIG_ACL_ENTRIES > 255U
#error "UCN_V6_CONFIG_ACL_ENTRIES must be 1..255"
#endif
#if UCN_V6_CONFIG_GROUP_REPLAY_SOURCES < 1U || \
    UCN_V6_CONFIG_GROUP_REPLAY_SOURCES > 255U
#error "UCN_V6_CONFIG_GROUP_REPLAY_SOURCES must be 1..255"
#endif
#if UCN_V6_CONFIG_CAPABILITY_PEERS < 1U || \
    UCN_V6_CONFIG_CAPABILITY_PEERS > 255U
#error "UCN_V6_CONFIG_CAPABILITY_PEERS must be 1..255"
#endif
#if UCN_V6_CONFIG_CAPABILITY_PATHS < 1U || \
    UCN_V6_CONFIG_CAPABILITY_PATHS > 255U
#error "UCN_V6_CONFIG_CAPABILITY_PATHS must be 1..255"
#endif
#if UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS < 1U || \
    UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS > 255U
#error "UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS must be 1..255"
#endif
#if UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS < 1U || \
    UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS > 255U
#error "UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS must be 1..255"
#endif
#if UCN_V6_CONFIG_ROUTE_SETS < 1U || UCN_V6_CONFIG_ROUTE_SETS > 255U
#error "UCN_V6_CONFIG_ROUTE_SETS must be 1..255"
#endif
#if UCN_V6_CONFIG_ROUTE_PATHS_PER_SET < 1U || \
    UCN_V6_CONFIG_ROUTE_PATHS_PER_SET > 16U
#error "UCN_V6_CONFIG_ROUTE_PATHS_PER_SET must be 1..16"
#endif
#if UCN_V6_CONFIG_ROUTE_CANDIDATES < 1U || \
    UCN_V6_CONFIG_ROUTE_CANDIDATES > 255U
#error "UCN_V6_CONFIG_ROUTE_CANDIDATES must be 1..255"
#endif
#if UCN_V6_CONFIG_ROUTE_FLOW_PINS < 1U || \
    UCN_V6_CONFIG_ROUTE_FLOW_PINS > 255U
#error "UCN_V6_CONFIG_ROUTE_FLOW_PINS must be 1..255"
#endif
#if UCN_V6_CONFIG_METRIC_PATHS < 1U || UCN_V6_CONFIG_METRIC_PATHS > 255U
#error "UCN_V6_CONFIG_METRIC_PATHS must be 1..255"
#endif
#if UCN_V6_CONFIG_QOS_Q0_DEPTH < 1U || UCN_V6_CONFIG_QOS_Q0_DEPTH > 255U
#error "UCN_V6_CONFIG_QOS_Q0_DEPTH must be 1..255"
#endif
#if UCN_V6_CONFIG_QOS_Q1_DEPTH < 1U || UCN_V6_CONFIG_QOS_Q1_DEPTH > 255U
#error "UCN_V6_CONFIG_QOS_Q1_DEPTH must be 1..255"
#endif
#if UCN_V6_CONFIG_QOS_Q2_DEPTH < 1U || UCN_V6_CONFIG_QOS_Q2_DEPTH > 255U
#error "UCN_V6_CONFIG_QOS_Q2_DEPTH must be 1..255"
#endif
#if UCN_V6_CONFIG_QOS_Q3_DEPTH < 1U || UCN_V6_CONFIG_QOS_Q3_DEPTH > 255U
#error "UCN_V6_CONFIG_QOS_Q3_DEPTH must be 1..255"
#endif
#if UCN_V6_CONFIG_QOS_FLOW_SLOTS < 1U || UCN_V6_CONFIG_QOS_FLOW_SLOTS > 255U
#error "UCN_V6_CONFIG_QOS_FLOW_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_QOS_INFLIGHT < 1U || UCN_V6_CONFIG_QOS_INFLIGHT > 255U
#error "UCN_V6_CONFIG_QOS_INFLIGHT must be 1..255"
#endif

#include "ucn/v6/ucn_v6_identity.h"

enum {
    UCN_V6_FEATURE_IDENTITY = 1U << 0,
    UCN_V6_FEATURE_WIRE = 1U << 1,
    UCN_V6_FEATURE_MESSAGE = 1U << 2,
    UCN_V6_FEATURE_SECURITY = 1U << 3,
    UCN_V6_FEATURE_ROUTE = 1U << 4,
    UCN_V6_FEATURE_TRANSFER = 1U << 5,
    UCN_V6_FEATURE_REALTIME = 1U << 6,
    UCN_V6_FEATURE_CLUSTER = 1U << 7,
    UCN_V6_FEATURE_CAPABILITY = 1U << 8
};

#define UCN_V6_COMPILED_FEATURE_BITS                                      \
    ((uint32_t)UCN_V6_FEATURE_IDENTITY | (uint32_t)UCN_V6_FEATURE_WIRE |  \
     (uint32_t)UCN_V6_FEATURE_MESSAGE |                                  \
     (uint32_t)UCN_V6_FEATURE_SECURITY |                                \
     (uint32_t)UCN_V6_FEATURE_CAPABILITY |                              \
     (uint32_t)UCN_V6_FEATURE_ROUTE)

#define UCN_V6_COMPILED_LAYOUT_HASH                                        \
    (UINT64_C(0xD65A000100000000) ^                                       \
     ((uint64_t)UCN_V6_CONFIG_MAX_BINDINGS * UINT64_C(0x100000001B3)) ^    \
     ((uint64_t)UCN_V6_CONFIG_MAX_ACTIVE_GROUPS *                          \
      UINT64_C(0x9E3779B185EBCA87)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_BOOTSTRAP_PENDING *                          \
      UINT64_C(0xC2B2AE3D27D4EB4F)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_BOOTSTRAP_LINKS *                            \
      UINT64_C(0x165667B19E3779F9)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_OPERATION_SLOTS *                            \
      UINT64_C(0x85EBCA77C2B2AE63)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_OPERATION_HIGH_WATERS *                      \
      UINT64_C(0x27D4EB2F165667C5)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS *                         \
      UINT64_C(0x94D049BB133111EB)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_STATIC_GROUP_SLOTS *                         \
      UINT64_C(0xBF58476D1CE4E5B9)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_GROUP_KEY_SLOTS *                            \
      UINT64_C(0xD6E8FEB86659FD93)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_OWNER_EVENT_DEPTH *                          \
      UINT64_C(0xA24BAED4963EE407)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_SECURITY_SESSIONS *                          \
      UINT64_C(0x9FB21C651E98DF25)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ACL_ENTRIES *                                \
      UINT64_C(0xC13FA9A902A6328F)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_GROUP_REPLAY_SOURCES *                        \
      UINT64_C(0x91E10DA5C79E7B1D)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_CAPABILITY_PEERS *                            \
      UINT64_C(0xDB4F0B9175AE2165)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_CAPABILITY_PATHS *                            \
      UINT64_C(0xBBE0563303A4615F)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS *                       \
      UINT64_C(0xA0F2EC75A1FE1575)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS *                       \
      UINT64_C(0x89E182857D9ED689)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ROUTE_SETS *                                  \
      UINT64_C(0xE7037ED1A0B428DB)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ROUTE_PATHS_PER_SET *                         \
      UINT64_C(0x8EBC6AF09C88C6E3)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ROUTE_CANDIDATES *                            \
      UINT64_C(0x589965CC75374CC3)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ROUTE_FLOW_PINS *                             \
      UINT64_C(0x1D8E4E27C47D124F)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_METRIC_PATHS *                                \
      UINT64_C(0xEB44ACCAB455D165)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_QOS_Q0_DEPTH *                                \
      UINT64_C(0xF1357AEA2E62A9C5)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_QOS_Q1_DEPTH *                                \
      UINT64_C(0xC6BC279692B5CC83)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_QOS_Q2_DEPTH *                                \
      UINT64_C(0xD6E8FEB86659FD93)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_QOS_Q3_DEPTH *                                \
      UINT64_C(0xA5A3564E27F8862B)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_QOS_FLOW_SLOTS *                              \
      UINT64_C(0x8D58AC26AFE12E47)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_QOS_INFLIGHT *                                \
      UINT64_C(0x9C06FAF4D023E3AB)))

#define UCN_V6_OPERATION_ID_ALLOCATOR_STORAGE_BYTES ((size_t)256U)
#define UCN_V6_PROTOCOL_OWNER_STORAGE_BYTES ((size_t)1024U)
#define UCN_V6_SECURITY_MANAGER_STORAGE_BYTES                             \
    ((size_t)(2048U + UCN_V6_CONFIG_SECURITY_SESSIONS * 512U +           \
              UCN_V6_CONFIG_ACL_ENTRIES * 128U +                         \
              UCN_V6_CONFIG_STATIC_GROUP_SLOTS *                         \
                  UCN_V6_CONFIG_GROUP_KEY_SLOTS * 192U +                  \
              UCN_V6_CONFIG_GROUP_REPLAY_SOURCES * 128U))
#define UCN_V6_CAPABILITY_OWNER_STORAGE_BYTES                             \
    ((size_t)(1024U + UCN_V6_CONFIG_CAPABILITY_PEERS * 256U +            \
              UCN_V6_CONFIG_CAPABILITY_PATHS * 160U +                    \
              UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS * 136U +               \
              UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS * 32U))
#define UCN_V6_ROUTE_OWNER_STORAGE_BYTES                                  \
    ((size_t)(1024U + UCN_V6_CONFIG_ROUTE_SETS * 128U +                   \
              UCN_V6_CONFIG_ROUTE_SETS *                                  \
                  (512U + UCN_V6_CONFIG_ROUTE_PATHS_PER_SET * 384U) +     \
              UCN_V6_CONFIG_ROUTE_CANDIDATES *                            \
                  (384U + UCN_V6_CONFIG_ROUTE_PATHS_PER_SET * 384U) +     \
              UCN_V6_CONFIG_ROUTE_FLOW_PINS * 128U))
#define UCN_V6_METRIC_OWNER_STORAGE_BYTES                                 \
    ((size_t)(1024U + UCN_V6_CONFIG_METRIC_PATHS * 256U))
#define UCN_V6_QOS_OWNER_STORAGE_BYTES                                    \
    ((size_t)(2048U +                                                    \
              (UCN_V6_CONFIG_QOS_Q0_DEPTH + UCN_V6_CONFIG_QOS_Q1_DEPTH + \
               UCN_V6_CONFIG_QOS_Q2_DEPTH + UCN_V6_CONFIG_QOS_Q3_DEPTH) * \
                  256U +                                                  \
              UCN_V6_CONFIG_QOS_FLOW_SLOTS * 256U +                      \
              UCN_V6_CONFIG_QOS_INFLIGHT * 64U))

#define UCN_V6_DECLARE_STORAGE_TYPE(name_, bytes_) \
    typedef union name_ {                         \
        uint64_t alignment_u64;                   \
        void *alignment_pointer;                  \
        uint8_t bytes[(bytes_)];                  \
    } name_

typedef struct ucn_v6_feature_manifest {
    uint16_t api_version;
    uint16_t storage_layout;
    uint32_t feature_bits;
    uint64_t layout_hash;
    uint16_t max_bindings;
    uint16_t max_active_groups;
    uint16_t bootstrap_pending;
    uint16_t bootstrap_links;
    uint16_t operation_slots;
    uint16_t operation_high_waters;
    uint16_t active_group_slots;
    uint16_t static_group_slots;
    uint16_t group_key_slots;
    uint16_t owner_event_depth;
    uint16_t security_sessions;
    uint16_t acl_entries;
    uint16_t group_replay_sources;
    uint16_t capability_peers;
    uint16_t capability_paths;
    uint16_t group_discovery_hints;
    uint16_t group_discovery_links;
    uint16_t route_sets;
    uint16_t route_paths_per_set;
    uint16_t route_candidates;
    uint16_t route_flow_pins;
    uint16_t metric_paths;
    uint16_t qos_q0_depth;
    uint16_t qos_q1_depth;
    uint16_t qos_q2_depth;
    uint16_t qos_q3_depth;
    uint16_t qos_flow_slots;
    uint16_t qos_inflight;
} ucn_v6_feature_manifest_t;

/* EN: Returns the one build-time feature and storage manifest.
 * 中文：返回本次构建唯一的 Feature 与 Storage Manifest。 */
const ucn_v6_feature_manifest_t *ucn_v6_compiled_manifest(void);
/* EN: Requires an exact application/library Manifest match.
 * 中文：要求应用与协议库的 Manifest 完全一致。 */
ucn_v6_result_t ucn_v6_manifest_validate_exact(
    const ucn_v6_feature_manifest_t *manifest);
/* EN: Checks capacity and alignment before any in-place object write.
 * 中文：在原地对象发生任何写入前检查容量与对齐。 */
ucn_v6_result_t ucn_v6_storage_validate(
    const void *storage,
    size_t storage_bytes,
    size_t required_bytes,
    size_t required_alignment);

#ifdef __cplusplus
}
#endif

#endif
