#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_wire_v4.h"

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

static const uint8_t HOST_V3_ADVERTISE[UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES] = {
    0x03, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x64, 0x00, 0x20, 0x00, 0x00, 0x03, 0xE8,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
};

static int expect_v3_dispatch(void)
{
    ucn_cluster_wire_message_t message;

    ASSERT_TRUE(ucn_cluster_wire_decode(HOST_V3_ADVERTISE,
                                        sizeof(HOST_V3_ADVERTISE),
                                        &message) == UCN_OK);
    ASSERT_TRUE(message.format == UCN_CLUSTER_WIRE_FORMAT_V3);
    ASSERT_TRUE(message.body.v3.type == UCN_CLUSTER_MSG_ADVERTISE);
    ASSERT_TRUE(message.body.v3.role == UCN_CLUSTER_ROLE_HEAD);
    ASSERT_TRUE(message.body.v3.flags == 0U);
    ASSERT_TRUE(message.body.v3.cluster_id == 1U);
    ASSERT_TRUE(message.body.v3.term == 1U);
    ASSERT_TRUE(message.body.v3.head_node_id == 2U);
    ASSERT_TRUE(message.body.v3.head_score == 100U);
    ASSERT_TRUE(message.body.v3.available_capacity == 32U);
    ASSERT_TRUE(message.body.v3.lease_ms == 1000U);
    ASSERT_TRUE(message.body.v3.nonce == 1U);
    return 0;
}

static int expect_v4_dispatch(
    const uint8_t input[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES])
{
    ucn_cluster_wire_message_t message;

    ASSERT_TRUE(ucn_cluster_wire_decode(input,
                                        UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES,
                                        &message) == UCN_OK);
    ASSERT_TRUE(message.format == UCN_CLUSTER_WIRE_FORMAT_V4);
    ASSERT_TRUE(message.body.v4.type == UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE);
    ASSERT_TRUE(message.body.v4.role == UCN_CLUSTER_ROLE_HEAD);
    ASSERT_TRUE(message.body.v4.flags == 0U);
    ASSERT_TRUE(message.body.v4.cluster_id == 7U);
    ASSERT_TRUE(message.body.v4.term == 9U);
    ASSERT_TRUE(message.body.v4.head_node_id == 11U);
    ASSERT_TRUE(message.body.v4.words[0U] == UINT32_C(0x00640020));
    ASSERT_TRUE(message.body.v4.words[1U] == 1000U);
    ASSERT_TRUE(message.body.v4.words[2U] == UINT32_C(0x10203040));
    ASSERT_TRUE(message.body.v4.words[3U] == UINT32_C(0x03040001));
    ASSERT_TRUE(message.body.v4.words[4U] == 0U);
    ASSERT_TRUE(message.body.v4.words[5U] == 0U);
    return 0;
}

static int expect_dispatch_rejected_unchanged(const uint8_t *input,
                                               size_t input_length)
{
    ucn_cluster_wire_message_t message;
    ucn_cluster_wire_message_t expected;

    (void)memset(&message, 0xA5, sizeof(message));
    expected = message;
    ASSERT_TRUE(ucn_cluster_wire_decode(input, input_length, &message) ==
                UCN_ERR_MALFORMED);
    ASSERT_TRUE(memcmp(&message, &expected, sizeof(message)) == 0);
    return 0;
}

int main(void)
{
    ucn_cluster_wire_v4_frame_t v4_frame;
    uint8_t v4_advertise[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];
    uint8_t v3_as_40[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];
    uint8_t v4_as_32[UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES];
    uint8_t v4_wrong_version[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];
    uint8_t v3_wrong_version[UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES];
    uint8_t oversized[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES + 1U];

    (void)memset(&v4_frame, 0, sizeof(v4_frame));
    v4_frame.type = UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE;
    v4_frame.role = UCN_CLUSTER_ROLE_HEAD;
    v4_frame.cluster_id = 7U;
    v4_frame.term = 9U;
    v4_frame.head_node_id = 11U;
    v4_frame.words[0U] = UINT32_C(0x00640020);
    v4_frame.words[1U] = 1000U;
    v4_frame.words[2U] = UINT32_C(0x10203040);
    v4_frame.words[3U] = UINT32_C(0x03040001);
    ASSERT_TRUE(ucn_cluster_wire_v4_encode(&v4_frame, v4_advertise) == UCN_OK);

    /* The same Host codec endpoint must dispatch each input independently. */
    ASSERT_TRUE(expect_v3_dispatch() == 0);
    ASSERT_TRUE(expect_v4_dispatch(v4_advertise) == 0);
    ASSERT_TRUE(expect_v3_dispatch() == 0);
    ASSERT_TRUE(expect_v4_dispatch(v4_advertise) == 0);

    (void)memset(v3_as_40, 0, sizeof(v3_as_40));
    (void)memcpy(v3_as_40, HOST_V3_ADVERTISE, sizeof(HOST_V3_ADVERTISE));
    ASSERT_TRUE(expect_dispatch_rejected_unchanged(v3_as_40,
                                                    sizeof(v3_as_40)) == 0);

    (void)memcpy(v4_as_32, v4_advertise, sizeof(v4_as_32));
    ASSERT_TRUE(expect_dispatch_rejected_unchanged(v4_as_32,
                                                    sizeof(v4_as_32)) == 0);

    (void)memcpy(v4_wrong_version, v4_advertise, sizeof(v4_wrong_version));
    v4_wrong_version[0U] = UCN_CLUSTER_WIRE_V3_FORMAT_VERSION;
    ASSERT_TRUE(expect_dispatch_rejected_unchanged(v4_wrong_version,
                                                    sizeof(v4_wrong_version)) == 0);

    (void)memcpy(v3_wrong_version, HOST_V3_ADVERTISE,
                 sizeof(v3_wrong_version));
    v3_wrong_version[0U] = UCN_CLUSTER_WIRE_V4_FORMAT_VERSION;
    ASSERT_TRUE(expect_dispatch_rejected_unchanged(v3_wrong_version,
                                                    sizeof(v3_wrong_version)) == 0);

    ASSERT_TRUE(expect_dispatch_rejected_unchanged(HOST_V3_ADVERTISE,
                                                    sizeof(HOST_V3_ADVERTISE) - 1U) ==
                0);
    ASSERT_TRUE(expect_dispatch_rejected_unchanged(v4_advertise,
                                                    sizeof(v4_advertise) - 1U) == 0);
    (void)memcpy(oversized, v4_advertise, sizeof(v4_advertise));
    oversized[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES] = 0U;
    ASSERT_TRUE(expect_dispatch_rejected_unchanged(oversized,
                                                    sizeof(oversized)) == 0);

    printf("Host v3/v4 dual-stack compatibility gate passed.\n");
    return 0;
}
