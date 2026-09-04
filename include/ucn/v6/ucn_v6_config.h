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

#include "ucn/v6/ucn_v6_identity.h"

enum {
    UCN_V6_FEATURE_IDENTITY = 1U << 0,
    UCN_V6_FEATURE_WIRE = 1U << 1,
    UCN_V6_FEATURE_MESSAGE = 1U << 2,
    UCN_V6_FEATURE_SECURITY = 1U << 3,
    UCN_V6_FEATURE_ROUTE = 1U << 4,
    UCN_V6_FEATURE_TRANSFER = 1U << 5,
    UCN_V6_FEATURE_REALTIME = 1U << 6,
    UCN_V6_FEATURE_CLUSTER = 1U << 7
};

#define UCN_V6_COMPILED_FEATURE_BITS                                      \
    ((uint32_t)UCN_V6_FEATURE_IDENTITY | (uint32_t)UCN_V6_FEATURE_WIRE |  \
     (uint32_t)UCN_V6_FEATURE_MESSAGE)

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
      UINT64_C(0xA24BAED4963EE407)))

#define UCN_V6_OPERATION_ID_ALLOCATOR_STORAGE_BYTES ((size_t)256U)
#define UCN_V6_PROTOCOL_OWNER_STORAGE_BYTES ((size_t)1024U)

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
