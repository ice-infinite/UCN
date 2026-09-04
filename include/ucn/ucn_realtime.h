#ifndef UCN_REALTIME_H
#define UCN_REALTIME_H

/* Optional UCN-Extended Realtime Metadata v1 codec.
 *
 * This header defines only the fixed 16-byte Envelope value/codec boundary.
 * It does not enable a Time Domain, synchronize clocks, apply Endpoint
 * policy, or alter the Core frame and Node storage layouts.  Products opt in
 * by explicitly linking the separate ucn_realtime archive. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_REALTIME_ENVELOPE_VERSION ((uint8_t)1U)
#define UCN_REALTIME_ENVELOPE_WIRE_BYTES ((size_t)16U)
#define UCN_REALTIME_CLOCK_DOMAIN_ID_MAX ((uint16_t)0xFFFEU)
#define UCN_REALTIME_DOMAIN_GENERATION_MAX UINT32_C(0x7FFFFFFF)
#define UCN_REALTIME_UNCERTAINTY_CLASS_MAX_KNOWN ((uint8_t)30U)
#define UCN_REALTIME_UNCERTAINTY_CLASS_UNKNOWN ((uint8_t)31U)

#define UCN_REALTIME_UNCERTAINTY_TIMER_RESOLUTION ((uint8_t)0x01U)
#define UCN_REALTIME_UNCERTAINTY_LINK_CAPTURE ((uint8_t)0x02U)
#define UCN_REALTIME_UNCERTAINTY_FILTER_RESIDUAL ((uint8_t)0x04U)
#define UCN_REALTIME_UNCERTAINTY_ARITHMETIC_ROUNDING ((uint8_t)0x08U)
#define UCN_REALTIME_UNCERTAINTY_SYNC_REQUIRED_MASK ((uint8_t)0x0FU)

typedef char ucn_realtime_envelope_wire_size_must_be_16[
    UCN_REALTIME_ENVELOPE_WIRE_BYTES == 16U ? 1 : -1];

/* Mode zero means that an Endpoint carries no Envelope at all.  It is not a
 * valid encoded 16-byte object. / 模式零表示该 Endpoint 完全不携带
 * Envelope，不能编码成 16 字节全零对象。 */
typedef uint8_t ucn_realtime_mode_t;
enum {
    UCN_REALTIME_MODE_NONE = 0U,
    UCN_REALTIME_MODE_LOCAL_STAMP = 1U,
    UCN_REALTIME_MODE_SYNCED_STAMP = 2U,
    UCN_REALTIME_MODE_DEADLINE = 3U
};

/* Semantic value object; it is intentionally not a packed Wire struct.
 * 语义值对象刻意不使用 packed，Wire 始终由显式大端 Codec 生成。 */
typedef struct ucn_realtime_envelope {
    ucn_realtime_mode_t mode;
    uint8_t uncertainty_class;
    bool sample_capture_hardware;
    bool domain_time_valid;
    bool source_holdover;
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint64_t capture_time_us;
} ucn_realtime_envelope_t;

/* Every component is an independently proven upper bound.  A zero value is
 * not accepted for v1 because real MCU clocks, capture paths, filters and
 * integer arithmetic all have a non-zero bound. / 每个分量都是独立证明的
 * 上界；v1 不接受零值，因为真实 MCU 时钟、打点、滤波和整数运算均有
 * 非零误差。 */
typedef struct ucn_realtime_uncertainty_components {
    uint32_t timer_resolution_bound_us;
    uint32_t link_timestamp_capture_bound_us;
    uint32_t filter_residual_bound_us;
    uint32_t arithmetic_rounding_bound_us;
    uint8_t known_mask;
} ucn_realtime_uncertainty_components_t;

/* EN: Validates the complete Metadata-v1 semantic combination.
 * 中文：校验 Metadata-v1 的完整语义组合。 */
bool ucn_realtime_envelope_is_valid(
    const ucn_realtime_envelope_t *envelope);

/* EN: Encodes one valid object to the fixed 16-byte network order.  Failure
 * leaves the complete output unchanged.
 * 中文：把合法对象编码为固定 16 字节网络序；失败时完整 output 不写回。 */
ucn_result_t ucn_realtime_envelope_encode(
    const ucn_realtime_envelope_t *envelope,
    uint8_t output[UCN_REALTIME_ENVELOPE_WIRE_BYTES]);

/* EN: Strictly decodes exactly 16 bytes.  Failure leaves the complete output
 * object unchanged.
 * 中文：严格解码恰好 16 字节；失败时完整输出对象不写回。 */
ucn_result_t ucn_realtime_envelope_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_realtime_envelope_t *output);

/* EN: Conservatively rounds a sender uncertainty upper bound upward to the
 * smallest binary class. Unknown or values above 2^30 map to class 31.
 * 中文：把发送端误差上界向上舍入到最小二进制等级；unknown 或超过
 * 2^30 的值映射为等级 31。 */
ucn_result_t ucn_realtime_uncertainty_class_encode(
    bool known,
    uint64_t upper_bound_us,
    uint8_t *output_class);

/* EN: Decodes class 0..30 to its exact binary upper bound; class 31 reports
 * known=false and upper_bound_us=0. Failure leaves both outputs unchanged.
 * 中文：等级 0..30 解码为精确二进制上界；等级 31 返回 known=false 和
 * upper_bound_us=0；失败时两个输出都不写回。 */
ucn_result_t ucn_realtime_uncertainty_class_decode(
    uint8_t uncertainty_class,
    bool *known,
    uint32_t *upper_bound_us);

/* EN: Adds every required sync uncertainty component and the independently
 * proven path-asymmetry bound. Unknown, zero, extra-mask and overflow inputs
 * fail without writing output.
 * 中文：累加全部同步误差分量及独立证明的路径非对称上界；未知、零值、
 * 非法掩码或溢出均失败且不写 output。 */
ucn_result_t ucn_realtime_uncertainty_aggregate(
    const ucn_realtime_uncertainty_components_t *components,
    bool path_asymmetry_known,
    uint32_t path_asymmetry_bound_us,
    uint32_t *output_bound_us);

#ifdef __cplusplus
}
#endif

#endif
