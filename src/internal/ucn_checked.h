#ifndef UCN_CHECKED_H
#define UCN_CHECKED_H

#include "ucn/ucn_types.h"

/* Internal common primitives. They own no business state and never write an
 * output on failure. */
ucn_result_t ucn_i_u32_allocate_first(bool never_allocated,
                                      uint32_t *value_out);
ucn_result_t ucn_i_u32_checked_next(uint32_t current,
                                    uint32_t *value_out);
ucn_result_t ucn_i_u64_allocate_first(bool never_allocated,
                                      uint64_t *value_out);
ucn_result_t ucn_i_u64_checked_next(uint64_t current,
                                    uint64_t *value_out);
ucn_result_t ucn_i_deadline_from_duration_us(uint64_t now_us,
                                             uint64_t duration_us,
                                             uint64_t *deadline_out);
bool ucn_i_deadline_expired_us(uint64_t now_us, uint64_t deadline_us);
ucn_result_t ucn_i_size_add(size_t left, size_t right, size_t *value_out);
ucn_result_t ucn_i_size_multiply(size_t left, size_t right,
                                 size_t *value_out);
bool ucn_i_handle_matches(const ucn_handle_t *handle,
                          uint32_t runtime_instance,
                          uint16_t owner_instance,
                          uint16_t slot_limit,
                          ucn_object_kind_t expected_kind);
bool ucn_i_ranges_overlap(const void *left, size_t left_size,
                          const void *right, size_t right_size);

#endif
