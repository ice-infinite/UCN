#ifndef UCN_WIRE_H
#define UCN_WIRE_H

#include "ucn/ucn_core.h"

#define UCN_I_C1_COMMON_BYTES 3U
#define UCN_I_C1_FIXED_AFTER_ADDRESSES 6U

typedef struct ucn_i_c1_frame {
    const uint8_t *payload;
    size_t payload_bytes;
    uint32_t source_address;
    uint32_t destination_address;
    uint32_t origin_sequence;
    uint16_t service_id;
    uint8_t traffic_class;
    uint8_t hop_limit;
} ucn_i_c1_frame_t;

ucn_result_t ucn_i_c1_header_bytes(uint8_t address_width,
                                   size_t *header_bytes_out);
ucn_result_t ucn_i_c1_encoded_size(uint8_t address_width,
                                   size_t payload_bytes,
                                   size_t *encoded_bytes_out);
ucn_result_t ucn_i_c1_encode(const ucn_i_c1_frame_t *frame,
                             uint8_t address_width,
                             uint8_t *output,
                             size_t output_capacity,
                             size_t *output_bytes);
ucn_result_t ucn_i_c1_encode_in_place(ucn_i_c1_frame_t *frame,
                                      uint8_t address_width,
                                      uint8_t *payload_then_frame,
                                      size_t capacity,
                                      size_t *output_bytes);
ucn_result_t ucn_i_c1_decode(const uint8_t *input,
                             size_t input_bytes,
                             uint8_t address_width,
                             ucn_i_c1_frame_t *frame_out);

#endif
