/* CLV2-14-03: optional Host/test dual-format dispatcher.
 *
 * Keep this translation unit outside the strict v4 archive: dispatching a
 * v3 frame necessarily references the legacy v3 decoder.  A strict-v4 MCU
 * can therefore link ucn_cluster_wire_v4 without pulling an unresolved v3
 * dependency, while an explicitly compatible build links the dual-stack
 * archive together with both frozen codecs.
 */

#include "ucn/ucn_cluster_wire_v4.h"

#include <string.h>

#define WIRE_VERSION_OFFSET ((size_t)0U)

ucn_result_t ucn_cluster_wire_detect_format(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_wire_format_t *format)
{
    ucn_cluster_wire_format_t detected;

    if (input == NULL || format == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length == UCN_CLUSTER_WIRE_V3_MESSAGE_BYTES &&
        input[WIRE_VERSION_OFFSET] == UCN_CLUSTER_WIRE_V3_FORMAT_VERSION) {
        detected = UCN_CLUSTER_WIRE_FORMAT_V3;
    } else if (input_length == UCN_CLUSTER_WIRE_V4_MESSAGE_BYTES &&
               input[WIRE_VERSION_OFFSET] == UCN_CLUSTER_WIRE_V4_FORMAT_VERSION) {
        detected = UCN_CLUSTER_WIRE_FORMAT_V4;
    } else {
        return UCN_ERR_MALFORMED;
    }
    *format = detected;
    return UCN_OK;
}

ucn_result_t ucn_cluster_wire_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_wire_message_t *output)
{
    ucn_cluster_wire_format_t format;
    ucn_cluster_wire_message_t decoded;
    ucn_result_t result;

    if (output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_cluster_wire_detect_format(input, input_length, &format);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.format = format;
    if (format == UCN_CLUSTER_WIRE_FORMAT_V3) {
        result = ucn_cluster_message_decode(input, input_length, &decoded.body.v3);
    } else {
        result = ucn_cluster_wire_v4_decode(input, input_length, &decoded.body.v4);
    }
    if (result != UCN_OK) {
        return result;
    }
    *output = decoded;
    return UCN_OK;
}
