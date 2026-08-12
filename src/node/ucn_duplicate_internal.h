#ifndef UCN_DUPLICATE_INTERNAL_H
#define UCN_DUPLICATE_INTERNAL_H

#include <string.h>

#include "ucn/ucn_node_storage.h"
#include "ucn/ucn_time.h"

static ucn_result_t ucn_duplicate_accept_frame(ucn_node_t *node,
                                                const ucn_frame_t *frame)
{
    ucn_duplicate_source_window_t *free_slot = NULL;
    size_t index;

    for (index = 0U; index < UCN_DUPLICATE_SOURCE_WINDOWS; ++index) {
        ucn_duplicate_source_window_t *slot = &node->duplicate_windows[index];

        if (slot->valid && slot->source == frame->source &&
            slot->session_id == frame->session_id) {
            const int32_t delta =
                (int32_t)(frame->sequence - slot->highest_sequence);

            if (delta > 0) {
                if ((uint32_t)delta >= UCN_DUPLICATE_WINDOW_BITS) {
                    slot->received_bitmap = (ucn_duplicate_bitmap_t)1U;
                } else {
                    slot->received_bitmap =
                        (ucn_duplicate_bitmap_t)((slot->received_bitmap <<
                                                  (uint32_t)delta) |
                                                 (ucn_duplicate_bitmap_t)1U);
                }
                slot->highest_sequence = frame->sequence;
                slot->last_observed_ms = node->now_ms;
                return UCN_OK;
            }
            if (delta == 0) {
                node->stats.duplicate_frames_dropped++;
                return UCN_ERR_REPLAY;
            }
            {
                const uint32_t age =
                    (uint32_t)(slot->highest_sequence - frame->sequence);
                ucn_duplicate_bitmap_t bit;

                if (age >= UCN_DUPLICATE_WINDOW_BITS) {
                    node->stats.duplicate_frames_dropped++;
                    return UCN_ERR_REPLAY;
                }
                bit = (ucn_duplicate_bitmap_t)1U << age;
                if ((slot->received_bitmap & bit) != 0U) {
                    node->stats.duplicate_frames_dropped++;
                    return UCN_ERR_REPLAY;
                }
                slot->received_bitmap |= bit;
                slot->last_observed_ms = node->now_ms;
                return UCN_OK;
            }
        }
        if (!slot->valid ||
            ucn_elapsed_at_least(node->now_ms, slot->last_observed_ms,
                                 UCN_DUPLICATE_SOURCE_TIMEOUT_MS)) {
            if (free_slot == NULL) {
                free_slot = slot;
            }
        }
    }

    if (free_slot == NULL) {
        node->stats.duplicate_source_window_full++;
        return UCN_ERR_NO_SPACE;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->valid = true;
    free_slot->source = frame->source;
    free_slot->session_id = frame->session_id;
    free_slot->highest_sequence = frame->sequence;
    free_slot->received_bitmap = (ucn_duplicate_bitmap_t)1U;
    free_slot->last_observed_ms = node->now_ms;
    return UCN_OK;
}

#endif
