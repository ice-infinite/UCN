#ifndef UCN_FRAME_H
#define UCN_FRAME_H

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucn_wire_profile_descriptor {
    ucn_wire_profile_t profile;
    uint8_t wire_code;
    uint8_t address_bytes;
    uint8_t payload_length_bytes;
    uint8_t route_epoch_bytes;
    uint8_t path_id_bytes;
    uint8_t route_cost_bytes;
    uint8_t max_hops;
    uint32_t max_wire_value;
    uint32_t max_node_id;
} ucn_wire_profile_descriptor_t;

const ucn_wire_profile_descriptor_t *ucn_wire_profile_get_descriptor(
    ucn_wire_profile_t profile);
size_t ucn_frame_header_size_for_profile(ucn_wire_profile_t profile,
                                         uint8_t flags);
size_t ucn_frame_encoded_size(const ucn_frame_t *frame);
/* Default-compatible W3 query.  New code should use the profile-aware form. */
size_t ucn_frame_max_payload(uint8_t flags);
size_t ucn_frame_max_payload_for_profile(ucn_wire_profile_t profile,
                                         uint8_t flags);
/* Select the smallest official profile up to maximum_profile that can
 * represent the frame and fit mtu (0 means only UCN_MAX_FRAME_BYTES).  The
 * input wire_profile and auth_tag pointer are ignored for selection; a
 * protected frame still reserves the fixed E2E tag bytes. */
ucn_result_t ucn_frame_select_min_wire_profile(
    const ucn_frame_t *frame,
    ucn_wire_profile_t maximum_profile,
    size_t mtu,
    ucn_wire_profile_t *selected_profile);
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
