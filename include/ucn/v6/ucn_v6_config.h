#ifndef UCN_V6_CONFIG_H
#define UCN_V6_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "ucn/v6/ucn_v6_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_V6_API_VERSION UINT16_C(1)
#define UCN_V6_STORAGE_LAYOUT UINT16_C(4)
#define UCN_V6_STORAGE_ALIGNMENT ((size_t)8U)

#define UCN_V6_PROFILE_NANO 1U
#define UCN_V6_PROFILE_LITE 2U
#define UCN_V6_PROFILE_FULL 3U

#ifdef UCN_V6_USER_CONFIG_HEADER
#include UCN_V6_USER_CONFIG_HEADER
#endif

#ifndef UCN_V6_PROFILE
#define UCN_V6_PROFILE UCN_V6_PROFILE_FULL
#endif

#if UCN_V6_PROFILE != UCN_V6_PROFILE_NANO && \
    UCN_V6_PROFILE != UCN_V6_PROFILE_LITE && \
    UCN_V6_PROFILE != UCN_V6_PROFILE_FULL
#error "UCN_V6_PROFILE must be NANO, LITE, or FULL"
#endif

#ifndef UCN_V6_FEATURE_REALTIME_ENABLED
#define UCN_V6_FEATURE_REALTIME_ENABLED 1U
#endif
#ifndef UCN_V6_FEATURE_CLUSTER_ENABLED
#define UCN_V6_FEATURE_CLUSTER_ENABLED 1U
#endif
#ifndef UCN_V6_FEATURE_ADAPTER_ENABLED
#define UCN_V6_FEATURE_ADAPTER_ENABLED 1U
#endif

#if UCN_V6_FEATURE_REALTIME_ENABLED > 1U || \
    UCN_V6_FEATURE_CLUSTER_ENABLED > 1U || \
    UCN_V6_FEATURE_ADAPTER_ENABLED > 1U
#error "UCN v6 feature enable values must be 0 or 1"
#endif

#if UCN_V6_PROFILE == UCN_V6_PROFILE_NANO
#define UCN_V6_PROFILE_DEFAULT(nano_, lite_, full_) (nano_)
#elif UCN_V6_PROFILE == UCN_V6_PROFILE_LITE
#define UCN_V6_PROFILE_DEFAULT(nano_, lite_, full_) (lite_)
#else
#define UCN_V6_PROFILE_DEFAULT(nano_, lite_, full_) (full_)
#endif

#ifndef UCN_V6_CONFIG_MAX_BINDINGS
#define UCN_V6_CONFIG_MAX_BINDINGS UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_MAX_ACTIVE_GROUPS
#define UCN_V6_CONFIG_MAX_ACTIVE_GROUPS UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_BOOTSTRAP_PENDING
#define UCN_V6_CONFIG_BOOTSTRAP_PENDING UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_BOOTSTRAP_LINKS
#define UCN_V6_CONFIG_BOOTSTRAP_LINKS UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_OPERATION_SLOTS
#define UCN_V6_CONFIG_OPERATION_SLOTS UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_OPERATION_HIGH_WATERS
#define UCN_V6_CONFIG_OPERATION_HIGH_WATERS \
    UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS
#define UCN_V6_CONFIG_ACTIVE_GROUP_SLOTS UCN_V6_PROFILE_DEFAULT(1U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_STATIC_GROUP_SLOTS
#define UCN_V6_CONFIG_STATIC_GROUP_SLOTS UCN_V6_PROFILE_DEFAULT(1U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_GROUP_KEY_SLOTS
#define UCN_V6_CONFIG_GROUP_KEY_SLOTS UCN_V6_PROFILE_DEFAULT(1U, 2U, 4U)
#endif
#ifndef UCN_V6_CONFIG_OWNER_EVENT_DEPTH
#define UCN_V6_CONFIG_OWNER_EVENT_DEPTH UCN_V6_PROFILE_DEFAULT(32U, 128U, 255U)
#endif
#ifndef UCN_V6_CONFIG_SECURITY_SESSIONS
#define UCN_V6_CONFIG_SECURITY_SESSIONS UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_ACL_ENTRIES
#define UCN_V6_CONFIG_ACL_ENTRIES UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_GROUP_REPLAY_SOURCES
#define UCN_V6_CONFIG_GROUP_REPLAY_SOURCES \
    UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_CAPABILITY_PEERS
#define UCN_V6_CONFIG_CAPABILITY_PEERS UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_CAPABILITY_PATHS
#define UCN_V6_CONFIG_CAPABILITY_PATHS UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS
#define UCN_V6_CONFIG_GROUP_DISCOVERY_HINTS \
    UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS
#define UCN_V6_CONFIG_GROUP_DISCOVERY_LINKS \
    UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_ROUTE_SETS
#define UCN_V6_CONFIG_ROUTE_SETS UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_ROUTE_PATHS_PER_SET
#define UCN_V6_CONFIG_ROUTE_PATHS_PER_SET UCN_V6_PROFILE_DEFAULT(2U, 3U, 4U)
#endif
#ifndef UCN_V6_CONFIG_ROUTE_CANDIDATES
#define UCN_V6_CONFIG_ROUTE_CANDIDATES UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_ROUTE_FLOW_PINS
#define UCN_V6_CONFIG_ROUTE_FLOW_PINS UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_METRIC_PATHS
#define UCN_V6_CONFIG_METRIC_PATHS UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_QOS_Q0_DEPTH
#define UCN_V6_CONFIG_QOS_Q0_DEPTH UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_QOS_Q1_DEPTH
#define UCN_V6_CONFIG_QOS_Q1_DEPTH UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_QOS_Q2_DEPTH
#define UCN_V6_CONFIG_QOS_Q2_DEPTH UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_QOS_Q3_DEPTH
#define UCN_V6_CONFIG_QOS_Q3_DEPTH UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_QOS_FLOW_SLOTS
#define UCN_V6_CONFIG_QOS_FLOW_SLOTS UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_QOS_INFLIGHT
#define UCN_V6_CONFIG_QOS_INFLIGHT UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_TRANSFER_MAX_CLASS
#define UCN_V6_CONFIG_TRANSFER_MAX_CLASS UCN_V6_PROFILE_DEFAULT(3U, 6U, 8U)
#endif
#ifndef UCN_V6_CONFIG_TRANSFER_TX_SLOTS
#define UCN_V6_CONFIG_TRANSFER_TX_SLOTS UCN_V6_PROFILE_DEFAULT(1U, 2U, 4U)
#endif
#ifndef UCN_V6_CONFIG_TRANSFER_RX_SLOTS
#define UCN_V6_CONFIG_TRANSFER_RX_SLOTS UCN_V6_PROFILE_DEFAULT(1U, 2U, 4U)
#endif
#ifndef UCN_V6_CONFIG_TRANSFER_RECENT
#define UCN_V6_CONFIG_TRANSFER_RECENT UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_TRANSFER_WINDOW
#define UCN_V6_CONFIG_TRANSFER_WINDOW UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS
#define UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS
#define UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS \
    UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_REALTIME_ENDPOINTS
#define UCN_V6_CONFIG_REALTIME_ENDPOINTS UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_TIME_DOMAINS
#define UCN_V6_CONFIG_TIME_DOMAINS UCN_V6_PROFILE_DEFAULT(1U, 1U, 2U)
#endif
#ifndef UCN_V6_CONFIG_CLUSTER_MEMBERS
#define UCN_V6_CONFIG_CLUSTER_MEMBERS UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_CLUSTER_VOTERS
#define UCN_V6_CONFIG_CLUSTER_VOTERS UCN_V6_PROFILE_DEFAULT(3U, 7U, 16U)
#endif
#ifndef UCN_V6_CONFIG_CLUSTER_TOMBSTONES
#define UCN_V6_CONFIG_CLUSTER_TOMBSTONES UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_CLUSTER_DIRECTORY
#define UCN_V6_CONFIG_CLUSTER_DIRECTORY UCN_V6_PROFILE_DEFAULT(4U, 8U, 16U)
#endif
#ifndef UCN_V6_CONFIG_CLUSTER_TUNNELS
#define UCN_V6_CONFIG_CLUSTER_TUNNELS UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_ADAPTER_LINKS
#define UCN_V6_CONFIG_ADAPTER_LINKS UCN_V6_PROFILE_DEFAULT(2U, 4U, 8U)
#endif
#ifndef UCN_V6_CONFIG_ADAPTER_RX_SLOTS
#define UCN_V6_CONFIG_ADAPTER_RX_SLOTS UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_ADAPTER_TX_SLOTS
#define UCN_V6_CONFIG_ADAPTER_TX_SLOTS UCN_V6_PROFILE_DEFAULT(4U, 16U, 32U)
#endif
#ifndef UCN_V6_CONFIG_ADAPTER_FRAME_BYTES
#define UCN_V6_CONFIG_ADAPTER_FRAME_BYTES UCN_V6_PROFILE_DEFAULT(256U, 256U, 512U)
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
#if UCN_V6_CONFIG_TRANSFER_MAX_CLASS > 8U
#error "UCN_V6_CONFIG_TRANSFER_MAX_CLASS must be 0..8"
#endif
#if UCN_V6_CONFIG_TRANSFER_TX_SLOTS < 1U || \
    UCN_V6_CONFIG_TRANSFER_TX_SLOTS > 255U
#error "UCN_V6_CONFIG_TRANSFER_TX_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_TRANSFER_RX_SLOTS < 1U || \
    UCN_V6_CONFIG_TRANSFER_RX_SLOTS > 255U
#error "UCN_V6_CONFIG_TRANSFER_RX_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_TRANSFER_RECENT < 1U || \
    UCN_V6_CONFIG_TRANSFER_RECENT > 255U
#error "UCN_V6_CONFIG_TRANSFER_RECENT must be 1..255"
#endif
#if UCN_V6_CONFIG_TRANSFER_WINDOW < 1U || \
    UCN_V6_CONFIG_TRANSFER_WINDOW > 32U
#error "UCN_V6_CONFIG_TRANSFER_WINDOW must be 1..32"
#endif
#if UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS < 1U || \
    UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS > 255U
#error "UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS must be 1..255"
#endif
#if UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS < 1U || \
    UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS > 255U
#error "UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS must be 1..255"
#endif
#if UCN_V6_CONFIG_REALTIME_ENDPOINTS < 1U || \
    UCN_V6_CONFIG_REALTIME_ENDPOINTS > 255U
#error "UCN_V6_CONFIG_REALTIME_ENDPOINTS must be 1..255"
#endif
#if UCN_V6_CONFIG_TIME_DOMAINS < 1U || UCN_V6_CONFIG_TIME_DOMAINS > 32U
#error "UCN_V6_CONFIG_TIME_DOMAINS must be 1..32"
#endif
#if UCN_V6_CONFIG_CLUSTER_MEMBERS < 3U || \
    UCN_V6_CONFIG_CLUSTER_MEMBERS > 32U
#error "UCN_V6_CONFIG_CLUSTER_MEMBERS must be 3..32"
#endif
#if UCN_V6_CONFIG_CLUSTER_VOTERS < 3U || \
    UCN_V6_CONFIG_CLUSTER_VOTERS > UCN_V6_CONFIG_CLUSTER_MEMBERS
#error "UCN_V6_CONFIG_CLUSTER_VOTERS must be 3..CLUSTER_MEMBERS"
#endif
#if UCN_V6_CONFIG_CLUSTER_TOMBSTONES < 1U || \
    UCN_V6_CONFIG_CLUSTER_TOMBSTONES > 32U
#error "UCN_V6_CONFIG_CLUSTER_TOMBSTONES must be 1..32"
#endif
#if UCN_V6_CONFIG_CLUSTER_DIRECTORY < 1U || \
    UCN_V6_CONFIG_CLUSTER_DIRECTORY > 255U
#error "UCN_V6_CONFIG_CLUSTER_DIRECTORY must be 1..255"
#endif
#if UCN_V6_CONFIG_CLUSTER_TUNNELS < 1U || \
    UCN_V6_CONFIG_CLUSTER_TUNNELS > 255U
#error "UCN_V6_CONFIG_CLUSTER_TUNNELS must be 1..255"
#endif
#if UCN_V6_CONFIG_ADAPTER_LINKS < 1U || UCN_V6_CONFIG_ADAPTER_LINKS > 255U
#error "UCN_V6_CONFIG_ADAPTER_LINKS must be 1..255"
#endif
#if UCN_V6_CONFIG_ADAPTER_RX_SLOTS < 1U || \
    UCN_V6_CONFIG_ADAPTER_RX_SLOTS > 255U
#error "UCN_V6_CONFIG_ADAPTER_RX_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_ADAPTER_TX_SLOTS < 1U || \
    UCN_V6_CONFIG_ADAPTER_TX_SLOTS > 255U
#error "UCN_V6_CONFIG_ADAPTER_TX_SLOTS must be 1..255"
#endif
#if UCN_V6_CONFIG_ADAPTER_FRAME_BYTES < 64U || \
    UCN_V6_CONFIG_ADAPTER_FRAME_BYTES > 4096U
#error "UCN_V6_CONFIG_ADAPTER_FRAME_BYTES must be 64..4096"
#endif

enum {
    UCN_V6_FEATURE_IDENTITY = 1U << 0,
    UCN_V6_FEATURE_WIRE = 1U << 1,
    UCN_V6_FEATURE_MESSAGE = 1U << 2,
    UCN_V6_FEATURE_SECURITY = 1U << 3,
    UCN_V6_FEATURE_ROUTE = 1U << 4,
    UCN_V6_FEATURE_TRANSFER = 1U << 5,
    UCN_V6_FEATURE_REALTIME = 1U << 6,
    UCN_V6_FEATURE_CLUSTER = 1U << 7,
    UCN_V6_FEATURE_CAPABILITY = 1U << 8,
    UCN_V6_FEATURE_ADAPTER = 1U << 9,
    UCN_V6_FEATURE_QOS = 1U << 10
};

#if UCN_V6_FEATURE_REALTIME_ENABLED
#define UCN_V6_REALTIME_FEATURE_BIT ((uint32_t)UCN_V6_FEATURE_REALTIME)
#else
#define UCN_V6_REALTIME_FEATURE_BIT UINT32_C(0)
#endif
#if UCN_V6_FEATURE_CLUSTER_ENABLED
#define UCN_V6_CLUSTER_FEATURE_BIT ((uint32_t)UCN_V6_FEATURE_CLUSTER)
#else
#define UCN_V6_CLUSTER_FEATURE_BIT UINT32_C(0)
#endif
#if UCN_V6_FEATURE_ADAPTER_ENABLED
#define UCN_V6_ADAPTER_FEATURE_BIT ((uint32_t)UCN_V6_FEATURE_ADAPTER)
#else
#define UCN_V6_ADAPTER_FEATURE_BIT UINT32_C(0)
#endif

#define UCN_V6_COMPILED_FEATURE_BITS                                      \
    ((uint32_t)UCN_V6_FEATURE_IDENTITY | (uint32_t)UCN_V6_FEATURE_WIRE |  \
     (uint32_t)UCN_V6_FEATURE_MESSAGE |                                  \
     (uint32_t)UCN_V6_FEATURE_SECURITY |                                \
     (uint32_t)UCN_V6_FEATURE_CAPABILITY |                              \
     (uint32_t)UCN_V6_FEATURE_ROUTE |                                   \
     (uint32_t)UCN_V6_FEATURE_QOS |                                      \
     (uint32_t)UCN_V6_FEATURE_TRANSFER |                                 \
     UCN_V6_REALTIME_FEATURE_BIT | UCN_V6_CLUSTER_FEATURE_BIT |          \
     UCN_V6_ADAPTER_FEATURE_BIT)

#define UCN_V6_COMPILED_LAYOUT_HASH                                        \
    (UINT64_C(0xD65A000400000000) ^                                       \
     ((uint64_t)UCN_V6_PROFILE * UINT64_C(0x9E3779B97F4A7C15)) ^           \
     ((uint64_t)UCN_V6_COMPILED_FEATURE_BITS *                            \
      UINT64_C(0xD6E8FEB86659FD93)) ^                                     \
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
      UINT64_C(0x9C06FAF4D023E3AB)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_TRANSFER_MAX_CLASS *                          \
      UINT64_C(0xA0761D6478BD642F)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_TRANSFER_TX_SLOTS *                           \
      UINT64_C(0xE7037ED1A0B428DB)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_TRANSFER_RX_SLOTS *                           \
      UINT64_C(0x8EBC6AF09C88C6E3)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_TRANSFER_RECENT *                             \
      UINT64_C(0x589965CC75374CC3)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_TRANSFER_WINDOW *                             \
      UINT64_C(0x1D8E4E27C47D124F)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS *                       \
      UINT64_C(0xEB44ACCAB455D165)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS *                \
      UINT64_C(0xF1357AEA2E62A9C5)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_REALTIME_ENDPOINTS *                          \
      UINT64_C(0xC6BC279692B5CC83)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_TIME_DOMAINS *                                \
      UINT64_C(0xD6E8FEB86659FD93)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_CLUSTER_MEMBERS *                             \
      UINT64_C(0xA24BAED4963EE407)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_CLUSTER_VOTERS *                              \
      UINT64_C(0x9FB21C651E98DF25)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_CLUSTER_TOMBSTONES *                          \
      UINT64_C(0xC13FA9A902A6328F)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_CLUSTER_DIRECTORY *                           \
      UINT64_C(0x91E10DA5C79E7B1D)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_CLUSTER_TUNNELS *                             \
      UINT64_C(0xDB4F0B9175AE2165)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ADAPTER_LINKS *                               \
      UINT64_C(0xBBE0563303A4615F)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ADAPTER_RX_SLOTS *                            \
      UINT64_C(0x8EBC6AF09C88C6E3)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ADAPTER_TX_SLOTS *                            \
      UINT64_C(0x589965CC75374CC3)) ^                                      \
     ((uint64_t)UCN_V6_CONFIG_ADAPTER_FRAME_BYTES *                         \
      UINT64_C(0x1D8E4E27C47D124F)))

#if UCN_V6_CONFIG_TRANSFER_MAX_CLASS == 0U
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 32U
#elif UCN_V6_CONFIG_TRANSFER_MAX_CLASS == 1U
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 64U
#elif UCN_V6_CONFIG_TRANSFER_MAX_CLASS == 2U
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 128U
#elif UCN_V6_CONFIG_TRANSFER_MAX_CLASS == 3U
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 256U
#elif UCN_V6_CONFIG_TRANSFER_MAX_CLASS == 4U
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 512U
#elif UCN_V6_CONFIG_TRANSFER_MAX_CLASS == 5U
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 1024U
#elif UCN_V6_CONFIG_TRANSFER_MAX_CLASS == 6U
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 2048U
#elif UCN_V6_CONFIG_TRANSFER_MAX_CLASS == 7U
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 4096U
#else
#define UCN_V6_TRANSFER_MAX_MESSAGE_BYTES 8192U
#endif

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
#define UCN_V6_TRANSFER_OWNER_STORAGE_BYTES                               \
    ((size_t)(4096U +                                                     \
              UCN_V6_CONFIG_TRANSFER_TX_SLOTS * 4096U +                  \
              UCN_V6_CONFIG_TRANSFER_RX_SLOTS *                          \
                  (UCN_V6_TRANSFER_MAX_MESSAGE_BYTES + 4096U) +          \
              UCN_V6_CONFIG_TRANSFER_RECENT * 256U +                     \
              UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS * 512U +               \
              UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS * 128U))
#define UCN_V6_REALTIME_OWNER_STORAGE_BYTES                               \
    ((size_t)(2048U + UCN_V6_CONFIG_REALTIME_ENDPOINTS * 96U +            \
              UCN_V6_CONFIG_TIME_DOMAINS * 512U))
#define UCN_V6_CLUSTER_OWNER_STORAGE_BYTES                                \
    ((size_t)(8192U + UCN_V6_CONFIG_CLUSTER_MEMBERS * 192U +              \
              UCN_V6_CONFIG_CLUSTER_VOTERS * 160U * 2U +                 \
              UCN_V6_CONFIG_CLUSTER_TOMBSTONES * 96U +                   \
              UCN_V6_CONFIG_CLUSTER_DIRECTORY * 192U +                   \
              UCN_V6_CONFIG_CLUSTER_TUNNELS * 256U))
#define UCN_V6_ADAPTER_OWNER_STORAGE_BYTES                                \
    ((size_t)(4096U + UCN_V6_CONFIG_ADAPTER_LINKS * 384U +                \
              (UCN_V6_CONFIG_ADAPTER_RX_SLOTS +                          \
               UCN_V6_CONFIG_ADAPTER_TX_SLOTS) *                         \
                  (UCN_V6_CONFIG_ADAPTER_FRAME_BYTES + 128U)))
#define UCN_V6_FREERTOS_PORT_STORAGE_BYTES ((size_t)2048U)

#define UCN_V6_DECLARE_STORAGE_TYPE(name_, bytes_) \
    typedef union name_ {                         \
        uint64_t alignment_u64;                   \
        void *alignment_pointer;                  \
        uint8_t bytes[(bytes_)];                  \
    } name_

typedef struct ucn_v6_feature_manifest {
    uint16_t api_version;
    uint16_t storage_layout;
    uint16_t profile;
    uint16_t reserved_zero;
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
    uint16_t transfer_max_class;
    uint16_t transfer_tx_slots;
    uint16_t transfer_rx_slots;
    uint16_t transfer_recent;
    uint16_t transfer_window;
    uint16_t transfer_credit_links;
    uint16_t transfer_credit_reservations;
    uint16_t realtime_endpoints;
    uint16_t time_domains;
    uint16_t cluster_members;
    uint16_t cluster_voters;
    uint16_t cluster_tombstones;
    uint16_t cluster_directory;
    uint16_t cluster_tunnels;
    uint16_t adapter_links;
    uint16_t adapter_rx_slots;
    uint16_t adapter_tx_slots;
    uint16_t adapter_frame_bytes;
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
