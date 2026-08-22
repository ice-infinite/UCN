#include "test_support.h"

#include <string.h>

#include "ucn/ucn_cluster_wire_v4.h"

int test_cluster_wire_v4_encoder_closed(void)
{
    ucn_cluster_wire_v4_frame_t frame;
    uint8_t output[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];
    uint8_t expected_output[UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES];

    (void)memset(&frame, 0, sizeof(frame));
    (void)memset(output, 0xA5, sizeof(output));
    (void)memcpy(expected_output, output, sizeof(expected_output));
    frame.type = UCN_CLUSTER_WIRE_V4_MSG_ADVERTISE;
    frame.role = UCN_CLUSTER_ROLE_HEAD;
    frame.cluster_id = 1U;
    frame.term = 1U;
    frame.head_node_id = 2U;
    frame.words[0U] = UINT32_C(0x00640020);
    frame.words[1U] = 1000U;
    frame.words[2U] = 1U;
    frame.words[3U] = UINT32_C(0x03040001);

    TEST_ASSERT(ucn_cluster_wire_v4_encode(&frame, output) == UCN_ERR_CONFIG);
    TEST_ASSERT(memcmp(output, expected_output, sizeof(output)) == 0);
    return 0;
}
