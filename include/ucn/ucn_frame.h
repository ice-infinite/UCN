#ifndef UCN_FRAME_H
#define UCN_FRAME_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t ucn_frame_encoded_size(const ucn_frame_t *frame);
/* Maximum payload that fits UCN_MAX_FRAME_BYTES for a valid flag set.  This
 * accounts for the 32/36/40 B header and the optional E2E authentication tag.
 * Unknown flags return 0. */
size_t ucn_frame_max_payload(uint8_t flags);
size_t ucn_frame_e2e_aad_size(void);
ucn_result_t ucn_frame_write_e2e_aad(const ucn_frame_t *frame,
                                     uint8_t *output,
                                     size_t output_capacity,
                                     size_t *output_length);
ucn_result_t ucn_frame_encode(const ucn_frame_t *frame,
                              uint8_t *output,
                              size_t output_capacity,
                              size_t *output_length);
ucn_result_t ucn_frame_decode(const uint8_t *input,
                              size_t input_length,
                              ucn_frame_t *frame);
uint16_t ucn_crc16_ccitt(const uint8_t *data, size_t length, uint16_t seed);

#ifdef __cplusplus
}
#endif

#endif
