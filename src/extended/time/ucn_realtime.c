/* Optional, stateless UCN Realtime Metadata v1 codec.
 * 可选、无状态的 UCN Realtime Metadata v1 编解码器。 */

#include "ucn/ucn_realtime.h"

#include <string.h>

#define VERSION_MODE_OFFSET ((size_t)0U)
#define QUALITY_FLAGS_OFFSET ((size_t)1U)
#define CLOCK_DOMAIN_ID_OFFSET ((size_t)2U)
#define DOMAIN_GENERATION_OFFSET ((size_t)4U)
#define CAPTURE_TIME_OFFSET ((size_t)8U)

#define VERSION_SHIFT ((uint8_t)4U)
#define VERSION_MASK ((uint8_t)0xF0U)
#define MODE_MASK ((uint8_t)0x03U)
#define VERSION_MODE_RESERVED_MASK ((uint8_t)0x0CU)

#define UNCERTAINTY_CLASS_SHIFT ((uint8_t)3U)
#define SAMPLE_CAPTURE_HARDWARE_FLAG ((uint8_t)0x04U)
#define DOMAIN_TIME_VALID_FLAG ((uint8_t)0x02U)
#define SOURCE_HOLDOVER_FLAG ((uint8_t)0x01U)

/* EN: Reads a network-order 16-bit integer.
 * 中文：读取网络大端序 16 位整数。 */
static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | (uint16_t)input[1]);
}

/* EN: Reads a network-order 32-bit integer.
 * 中文：读取网络大端序 32 位整数。 */
static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

/* EN: Reads a network-order 64-bit integer.
 * 中文：读取网络大端序 64 位整数。 */
static uint64_t read_u64_be(const uint8_t *input)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | (uint64_t)input[index];
    }
    return value;
}

/* EN: Writes a network-order 16-bit integer.
 * 中文：写入网络大端序 16 位整数。 */
static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

/* EN: Writes a network-order 32-bit integer.
 * 中文：写入网络大端序 32 位整数。 */
static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

/* EN: Writes a network-order 64-bit integer.
 * 中文：写入网络大端序 64 位整数。 */
static void write_u64_be(uint8_t *output, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        output[7U - index] = (uint8_t)value;
        value >>= 8U;
    }
}

/* EN: Returns true for modes that require a shared Time Domain.
 * 中文：判断该模式是否要求共享 Time Domain。 */
static bool mode_uses_shared_domain(ucn_realtime_mode_t mode)
{
    return mode == UCN_REALTIME_MODE_SYNCED_STAMP ||
           mode == UCN_REALTIME_MODE_DEADLINE;
}

/* EN: Validates every semantic field combination accepted by Metadata v1.
 * 中文：校验 Metadata v1 接受的全部语义字段组合。 */
bool ucn_realtime_envelope_is_valid(
    const ucn_realtime_envelope_t *envelope)
{
    if (envelope == NULL ||
        envelope->uncertainty_class > UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN) {
        return false;
    }

    if (envelope->mode == UCN_REALTIME_MODE_LOCAL_STAMP) {
        return envelope->uncertainty_class ==
                   UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN &&
               !envelope->domain_time_valid && !envelope->source_holdover &&
               envelope->clock_domain_id == 0U &&
               envelope->domain_generation == 0U;
    }

    if (!mode_uses_shared_domain(envelope->mode)) {
        return false;
    }

    return envelope->uncertainty_class <=
               UCN_REALTIME_UNCERTAINTY_CLASS_MAX_KNOWN &&
           envelope->domain_time_valid && envelope->clock_domain_id != 0U &&
           envelope->clock_domain_id <= UCN_REALTIME_CLOCK_DOMAIN_ID_MAX &&
           envelope->domain_generation != 0U &&
           envelope->domain_generation <=
               UCN_REALTIME_DOMAIN_GENERATION_MAX;
}

/* EN: Encodes one valid semantic value into the canonical 16-byte form.
 * 中文：把一个合法语义值编码为规范 16 字节格式。 */
ucn_result_t ucn_realtime_envelope_encode(
    const ucn_realtime_envelope_t *envelope,
    uint8_t output[UCN_REALTIME_ENVELOPE_WIRE_BYTES])
{
    uint8_t encoded[UCN_REALTIME_ENVELOPE_WIRE_BYTES];
    uint8_t quality_flags;

    if (envelope == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ucn_realtime_envelope_is_valid(envelope)) {
        return UCN_ERR_ARGUMENT;
    }

    (void)memset(encoded, 0, sizeof(encoded));
    encoded[VERSION_MODE_OFFSET] =
        (uint8_t)((UCN_REALTIME_ENVELOPE_VERSION << VERSION_SHIFT) |
                  envelope->mode);
    quality_flags = (uint8_t)(envelope->uncertainty_class <<
                              UNCERTAINTY_CLASS_SHIFT);
    if (envelope->sample_capture_hardware) {
        quality_flags |= SAMPLE_CAPTURE_HARDWARE_FLAG;
    }
    if (envelope->domain_time_valid) {
        quality_flags |= DOMAIN_TIME_VALID_FLAG;
    }
    if (envelope->source_holdover) {
        quality_flags |= SOURCE_HOLDOVER_FLAG;
    }
    encoded[QUALITY_FLAGS_OFFSET] = quality_flags;
    write_u16_be(&encoded[CLOCK_DOMAIN_ID_OFFSET], envelope->clock_domain_id);
    write_u32_be(&encoded[DOMAIN_GENERATION_OFFSET],
                 envelope->domain_generation);
    write_u64_be(&encoded[CAPTURE_TIME_OFFSET], envelope->capture_time_us);

    (void)memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

/* EN: Strictly decodes one canonical 16-byte Metadata v1 value.
 * 中文：严格解码一个规范 16 字节 Metadata v1 值。 */
ucn_result_t ucn_realtime_envelope_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_realtime_envelope_t *output)
{
    ucn_realtime_envelope_t decoded;
    uint8_t version_mode;
    uint8_t quality_flags;

    if (input == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length != UCN_REALTIME_ENVELOPE_WIRE_BYTES) {
        return UCN_ERR_MALFORMED;
    }

    version_mode = input[VERSION_MODE_OFFSET];
    if ((version_mode & VERSION_MASK) !=
        (uint8_t)(UCN_REALTIME_ENVELOPE_VERSION << VERSION_SHIFT)) {
        return UCN_ERR_VERSION;
    }
    if ((version_mode & VERSION_MODE_RESERVED_MASK) != 0U) {
        return UCN_ERR_MALFORMED;
    }

    quality_flags = input[QUALITY_FLAGS_OFFSET];
    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.mode = version_mode & MODE_MASK;
    decoded.uncertainty_class =
        (uint8_t)(quality_flags >> UNCERTAINTY_CLASS_SHIFT);
    decoded.sample_capture_hardware =
        (quality_flags & SAMPLE_CAPTURE_HARDWARE_FLAG) != 0U;
    decoded.domain_time_valid =
        (quality_flags & DOMAIN_TIME_VALID_FLAG) != 0U;
    decoded.source_holdover =
        (quality_flags & SOURCE_HOLDOVER_FLAG) != 0U;
    decoded.clock_domain_id = read_u16_be(&input[CLOCK_DOMAIN_ID_OFFSET]);
    decoded.domain_generation = read_u32_be(&input[DOMAIN_GENERATION_OFFSET]);
    decoded.capture_time_us = read_u64_be(&input[CAPTURE_TIME_OFFSET]);

    if (!ucn_realtime_envelope_is_valid(&decoded)) {
        return UCN_ERR_MALFORMED;
    }

    *output = decoded;
    return UCN_OK;
}

/* EN: Rounds a sender uncertainty upper bound up to its binary class.
 * 中文：把发送端误差上界向上舍入为二进制等级。 */
ucn_result_t ucn_realtime_uncertainty_class_encode(
    bool known,
    uint64_t upper_bound_us,
    uint8_t *output_class)
{
    uint8_t encoded_class = 0U;
    uint64_t class_upper_bound = 1U;

    if (output_class == NULL) {
        return UCN_ERR_ARGUMENT;
    }

    if (!known || upper_bound_us > (UINT64_C(1) << 30U)) {
        encoded_class = UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN;
    } else {
        if (upper_bound_us == 0U) {
            upper_bound_us = 1U;
        }
        while (class_upper_bound < upper_bound_us &&
               encoded_class < UCN_REALTIME_UNCERTAINTY_CLASS_MAX_KNOWN) {
            class_upper_bound <<= 1U;
            ++encoded_class;
        }
    }

    *output_class = encoded_class;
    return UCN_OK;
}

/* EN: Converts one binary uncertainty class back to its conservative bound.
 * 中文：把二进制误差等级还原为保守上界。 */
ucn_result_t ucn_realtime_uncertainty_class_decode(
    uint8_t uncertainty_class,
    bool *known,
    uint32_t *upper_bound_us)
{
    bool decoded_known;
    uint32_t decoded_upper_bound;

    if (known == NULL || upper_bound_us == NULL ||
        uncertainty_class > UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN) {
        return UCN_ERR_ARGUMENT;
    }

    if (uncertainty_class == UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN) {
        decoded_known = false;
        decoded_upper_bound = 0U;
    } else {
        decoded_known = true;
        decoded_upper_bound = UINT32_C(1) << uncertainty_class;
    }
    *known = decoded_known;
    *upper_bound_us = decoded_upper_bound;
    return UCN_OK;
}

/* EN: Aggregates the complete Metadata-v1 synchronization error budget.
 * 中文：聚合 Metadata-v1 的完整同步误差预算。 */
ucn_result_t ucn_realtime_uncertainty_aggregate(
    const ucn_realtime_uncertainty_components_t *components,
    bool path_asymmetry_known,
    uint32_t path_asymmetry_bound_us,
    uint32_t *output_bound_us)
{
    uint64_t total;

    if (components == NULL || output_bound_us == NULL ||
        !path_asymmetry_known || path_asymmetry_bound_us == 0U ||
        components->known_mask !=
            UCN_REALTIME_UNCERTAINTY_SYNC_REQUIRED_MASK ||
        components->timer_resolution_bound_us == 0U ||
        components->link_timestamp_capture_bound_us == 0U ||
        components->filter_residual_bound_us == 0U ||
        components->arithmetic_rounding_bound_us == 0U) {
        return UCN_ERR_ARGUMENT;
    }

    total = (uint64_t)components->timer_resolution_bound_us +
            components->link_timestamp_capture_bound_us +
            components->filter_residual_bound_us +
            components->arithmetic_rounding_bound_us +
            path_asymmetry_bound_us;
    if (total > UINT32_MAX) {
        return UCN_ERR_TOO_LARGE;
    }
    *output_bound_us = (uint32_t)total;
    return UCN_OK;
}
