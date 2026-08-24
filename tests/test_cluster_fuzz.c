/* CLV2-M00-06 codec fuzz smoke (CLV2-M00.1).
 *
 * Deterministic xorshift32 mutation over one legal seed per wire Type
 * 1-19: random 1-4 byte flips (plus occasional length corruption) feed
 * ucn_cluster_message_decode() and a live ucn_cluster_receive().  The
 * gate is crash-freedom (run under ASan/UBSan) plus strict invariants:
 *  - decode returns only UCN_OK / UCN_ERR_MALFORMED / UCN_ERR_ARGUMENT;
 *  - a decode that returns UCN_OK must satisfy the wire invariants
 *    (type 1..19, role 0..8, non-zero epoch fields, flag whitelist);
 *  - receive returns only the documented dispatch error set.
 *
 * This is a smoke gate, not a libFuzzer replacement; the seed is fixed so
 * every run replays the exact same corpus. */
#include "test_support.h"

#include <string.h>

#include "ucn/ucn_cluster.h"

#define FUZZ_CASES UINT32_C(20000)
#define FUZZ_SEED UINT32_C(0x1C0DEC0D)

static uint32_t fuzz_rand(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return x;
}

static uint32_t fuzz_now(void *context)
{
    (void)context;
    return 1U;
}

static ucn_result_t fuzz_send(void *context,
                             ucn_node_id_t destination,
                             ucn_endpoint_t endpoint,
                             const uint8_t *payload,
                             uint16_t payload_length)
{
    (void)context;
    (void)destination;
    (void)endpoint;
    (void)payload;
    (void)payload_length;
    return UCN_OK;
}

static bool fuzz_flags_valid(uint8_t type, uint8_t flags)
{
    if (type == (uint8_t)UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC) {
        return flags == 0U || flags == UCN_CLUSTER_FLAG_SYNC_BEGIN ||
               flags == UCN_CLUSTER_FLAG_SYNC_END ||
               flags == UCN_CLUSTER_FLAG_SYNC_DELTA;
    }
    return flags == 0U;
}

static bool fuzz_decode_invariant_ok(const ucn_cluster_message_t *message)
{
    return message->type >= UCN_CLUSTER_MSG_ADVERTISE &&
           message->type <= UCN_CLUSTER_MSG_BACKUP_REJECT &&
           message->role >= UCN_CLUSTER_ROLE_DISABLED &&
           message->role <= UCN_CLUSTER_ROLE_RECOVERY_HEAD &&
           message->cluster_id != 0U && message->term != 0U &&
           message->head_node_id != 0U &&
           message->head_node_id != UCN_NODE_BROADCAST &&
           fuzz_flags_valid((uint8_t)message->type, message->flags);
}

static int cluster_fuzz_build_seed(uint8_t type, uint8_t seed[UCN_CLUSTER_MESSAGE_BYTES])
{
    ucn_cluster_message_t message;

    (void)memset(&message, 0, sizeof(message));
    message.type = (ucn_cluster_message_type_t)type;
    message.cluster_id = 1U;
    message.term = 1U;
    message.head_node_id = 1U;
    message.head_score = 5000U;
    message.available_capacity = 3U;
    message.lease_ms = 8000U;
    message.nonce = 1U;
    message.backup_generation = 1U;
    message.membership_sequence = 1U;
    message.member_node_id = 2U;
    message.member_nonce = 1U;
    message.recovery_nonce = 1U;
    message.recovery_ttl_ms = 30U;
    message.sync_token = 1U;
    message.reject_reason = UCN_CLUSTER_BACKUP_REJECT_COVERAGE;
    switch (message.type) {
    case UCN_CLUSTER_MSG_JOIN_REQUEST:
        message.role = UCN_CLUSTER_ROLE_DETACHED;
        break;
    case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
    case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
    case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
        message.role = UCN_CLUSTER_ROLE_HEAD;
        break;
    case UCN_CLUSTER_MSG_BACKUP_READY:
    case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
    case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
    case UCN_CLUSTER_MSG_BACKUP_REJECT:
        message.role = UCN_CLUSTER_ROLE_BACKUP;
        break;
    case UCN_CLUSTER_MSG_TAKEOVER_ACK:
    case UCN_CLUSTER_MSG_RECOVERY_ACK:
        message.role = UCN_CLUSTER_ROLE_MEMBER;
        break;
    case UCN_CLUSTER_MSG_RECOVERY_DECLARE:
        message.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
        break;
    default:
        message.role = UCN_CLUSTER_ROLE_HEAD;
        break;
    }
    return ucn_cluster_message_encode(&message, seed);
}

static int cluster_test_codec_fuzz(void)
{
    uint8_t seeds[19][UCN_CLUSTER_MESSAGE_BYTES];
    ucn_cluster_t cluster;
    ucn_cluster_config_t config;
    uint32_t rng = FUZZ_SEED;
    uint32_t case_index;
    uint8_t type;

    for (type = 1U; type <= 19U; ++type) {
        TEST_ASSERT(cluster_fuzz_build_seed(type, seeds[type - 1U]) ==
                    UCN_OK);
    }
    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = 9U;
    config.enabled = true;
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    config.head_capable = false;
    config.require_protected_control = true;
    config.head_score = 1000U;
    config.member_capacity = 0U;
    config.observation_ms = 1000U;
    config.recovery_observation_ms = 1000U;
    config.election_window_ms = 1000U;
    config.advertise_interval_ms = 1000U;
    config.join_retry_ms = 1000U;
    config.keepalive_interval_ms = 1000U;
    config.lease_ms = 8000U;
    config.head_min_tenure_ms = 50U;
    config.token_bucket.burst = UINT16_MAX;
    config.token_bucket.refill_ms = 1U;
    config.recovery_head_ttl_ms = 30U;
    config.recovery_backoff_max_ms = 5U;
    config.now_ms = fuzz_now;
    config.now_context = NULL;
    config.send = fuzz_send;
    config.send_context = NULL;
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);

    for (case_index = 0U; case_index < FUZZ_CASES; ++case_index) {
        uint8_t mutated[UCN_CLUSTER_MESSAGE_BYTES];
        ucn_cluster_message_t decoded;
        ucn_result_t result;
        uint32_t flips;
        uint32_t flip;

        type = (uint8_t)(fuzz_rand(&rng) % 19U) + 1U;
        (void)memcpy(mutated, seeds[type - 1U], sizeof(mutated));
        flips = (fuzz_rand(&rng) % 4U) + 1U;
        for (flip = 0U; flip < flips; ++flip) {
            mutated[fuzz_rand(&rng) % UCN_CLUSTER_MESSAGE_BYTES] ^=
                (uint8_t)(1U << (fuzz_rand(&rng) % 8U));
        }
        /* Decode gate: crash-free + strict return set + invariants. */
        result = ucn_cluster_message_decode(mutated, sizeof(mutated),
                                            &decoded);
        TEST_ASSERT(result == UCN_OK || result == UCN_ERR_MALFORMED ||
                    result == UCN_ERR_ARGUMENT);
        if (result == UCN_OK) {
            TEST_ASSERT(fuzz_decode_invariant_ok(&decoded));
        }
        /* Length corruption case (1 in 8): a wrong length must reject
         * (never crash).  Force the length away from the legal 32 so the
         * decode gate is unambiguous. */
        if ((case_index % 8U) == 0U) {
            size_t bad_len = (size_t)(fuzz_rand(&rng) % 64U);

            if (bad_len == UCN_CLUSTER_MESSAGE_BYTES) {
                bad_len = UCN_CLUSTER_MESSAGE_BYTES - 1U;
            }
            result = ucn_cluster_message_decode(mutated, bad_len, &decoded);
            TEST_ASSERT(result == UCN_ERR_MALFORMED ||
                        result == UCN_ERR_ARGUMENT);
        }
        /* Live receive gate: only the documented dispatch error set. */
        result = ucn_cluster_receive(&cluster, (ucn_node_id_t)2U, true,
                                     mutated, sizeof(mutated));
        TEST_ASSERT(result == UCN_OK || result == UCN_ERR_ACCESS ||
                    result == UCN_ERR_NO_SPACE || result == UCN_ERR_REPLAY ||
                    result == UCN_ERR_NOT_FOUND);
    }
    return 0;
}

int test_cluster_fuzz(void)
{
    TEST_ASSERT(cluster_test_codec_fuzz() == 0);
    return 0;
}
