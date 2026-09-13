#include "internal/ucn_checked.h"

#include <limits.h>

ucn_result_t ucn_i_u32_allocate_first(bool never_allocated,
                                      uint32_t *value_out)
{
    if (value_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!never_allocated) {
        return UCN_ERR_STATE;
    }
    *value_out = UINT32_C(1);
    return UCN_OK;
}

ucn_result_t ucn_i_u32_checked_next(uint32_t current,
                                    uint32_t *value_out)
{
    if (value_out == NULL || current == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (current == UINT32_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *value_out = current + UINT32_C(1);
    return UCN_OK;
}

ucn_result_t ucn_i_u64_allocate_first(bool never_allocated,
                                      uint64_t *value_out)
{
    if (value_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!never_allocated) {
        return UCN_ERR_STATE;
    }
    *value_out = UINT64_C(1);
    return UCN_OK;
}

ucn_result_t ucn_i_u64_checked_next(uint64_t current,
                                    uint64_t *value_out)
{
    if (value_out == NULL || current == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (current == UINT64_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *value_out = current + UINT64_C(1);
    return UCN_OK;
}

ucn_result_t ucn_i_deadline_from_duration_us(uint64_t now_us,
                                             uint64_t duration_us,
                                             uint64_t *deadline_out)
{
    if (deadline_out == NULL || duration_us == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    if (now_us > UINT64_MAX - duration_us) {
        return UCN_ERR_EXHAUSTED;
    }
    *deadline_out = now_us + duration_us;
    return UCN_OK;
}

bool ucn_i_deadline_expired_us(uint64_t now_us, uint64_t deadline_us)
{
    return deadline_us == 0U || now_us >= deadline_us;
}

ucn_result_t ucn_i_size_add(size_t left, size_t right, size_t *value_out)
{
    if (value_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (left > SIZE_MAX - right) {
        return UCN_ERR_EXHAUSTED;
    }
    *value_out = left + right;
    return UCN_OK;
}

ucn_result_t ucn_i_size_multiply(size_t left, size_t right,
                                 size_t *value_out)
{
    if (value_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (left != 0U && right > SIZE_MAX / left) {
        return UCN_ERR_EXHAUSTED;
    }
    *value_out = left * right;
    return UCN_OK;
}

bool ucn_i_handle_matches(const ucn_handle_t *handle,
                          uint32_t runtime_instance,
                          uint16_t owner_instance,
                          uint16_t slot_limit,
                          ucn_object_kind_t expected_kind)
{
    return handle != NULL && handle->reserved_zero == 0U &&
           ((expected_kind >= UCN_OBJECT_KIND_SEND &&
             expected_kind <= UCN_OBJECT_KIND_TIME_DOMAIN) ||
            expected_kind == UCN_OBJECT_KIND_PERSISTENCE) &&
           handle->object_kind == expected_kind &&
           runtime_instance != 0U &&
           handle->runtime_instance == runtime_instance &&
           owner_instance != 0U && handle->owner_instance == owner_instance &&
           slot_limit != 0U && handle->slot < slot_limit &&
           handle->generation != 0U;
}

bool ucn_i_ranges_overlap(const void *left, size_t left_size,
                          const void *right, size_t right_size)
{
    uintptr_t left_start;
    uintptr_t right_start;
    uintptr_t left_end;
    uintptr_t right_end;

    if (left == NULL || right == NULL || left_size == 0U || right_size == 0U) {
        return false;
    }
    left_start = (uintptr_t)left;
    right_start = (uintptr_t)right;
    if (left_start > UINTPTR_MAX - (left_size - 1U) ||
        right_start > UINTPTR_MAX - (right_size - 1U)) {
        return true;
    }
    left_end = left_start + left_size - 1U;
    right_end = right_start + right_size - 1U;
    return left_start <= right_end && right_start <= left_end;
}
