#include "service/ucn_service_private.h"

#include <string.h>

static const uint8_t schedule[] = {
    0U, 0U, 0U, 0U, 0U, 0U,
    1U, 1U, 1U,
    2U, 2U,
    3U
};

static bool handle_valid(ucn_handle_t handle)
{
    return handle.runtime_instance != 0U && handle.owner_instance != 0U &&
           handle.generation != 0U && handle.object_kind != 0U &&
           handle.reserved_zero == 0U;
}

static bool handle_equal(ucn_handle_t left, ucn_handle_t right)
{
    return left.runtime_instance == right.runtime_instance &&
           left.owner_instance == right.owner_instance &&
           left.slot == right.slot && left.generation == right.generation &&
           left.object_kind == right.object_kind &&
           left.reserved_zero == right.reserved_zero;
}

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool item_valid(const ucn_i_service_qos_item_t *item)
{
    if (item == NULL || !handle_valid(item->sendable) ||
        item->traffic_class >= UCN_I_SERVICE_CLASS_COUNT ||
        item->latest > 1U || item->control_authorized > 1U ||
        item->reserved_zero != 0U || item->enqueue_order != 0U ||
        item->source_quota_key == 0U ||
        item->flow_quota_key == 0U ||
        (item->traffic_class == 0U && item->control_authorized == 0U)) {
        return false;
    }
    if (item->latest != 0U &&
        !bytes_nonzero(item->latest_key, sizeof(item->latest_key))) {
        return false;
    }
    if (item->latest == 0U &&
        bytes_nonzero(item->latest_key, sizeof(item->latest_key))) {
        return false;
    }
    return true;
}

static ucn_result_t next_generation(uint16_t current, uint16_t *next_out)
{
    if (next_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (current == UINT16_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    *next_out = (uint16_t)(current + 1U);
    return UCN_OK;
}

static bool latest_equal(const ucn_i_service_qos_item_t *left,
                         const ucn_i_service_qos_item_t *right)
{
    return left->latest != 0U && right->latest != 0U &&
           left->traffic_class == right->traffic_class &&
           left->source_quota_key == right->source_quota_key &&
           left->flow_quota_key == right->flow_quota_key &&
           memcmp(left->latest_key, right->latest_key,
                  sizeof(left->latest_key)) == 0;
}

ucn_result_t ucn_i_service_qos_enqueue(ucn_i_service_owner_t *owner,
                                       const ucn_i_service_qos_item_t *item,
                                       ucn_handle_t *qos_handle_out,
                                       ucn_handle_t *superseded_out)
{
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_SERVICE_QOS_COUNT;
    size_t replace_index = UCN_I_SERVICE_QOS_COUNT;
    size_t source_count = 0U;
    size_t flow_count = 0U;
    size_t latest_count = 0U;
    uint16_t generation;
    ucn_i_service_qos_item_t frozen;

    if (!item_valid(item) || qos_handle_out == NULL ||
        superseded_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_SERVICE_QOS_COUNT; ++index) {
        const ucn_i_service_qos_record_t *record = &owner->qos[index];

        if (record->occupied != 0U &&
            handle_equal(record->item.sendable, item->sendable)) {
            ucn_i_service_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        if (record->occupied != 0U && record->selected == 0U &&
            record->submitted == 0U &&
            latest_equal(&record->item, item)) {
            replace_index = index;
        }
        if (record->occupied == 0U && free_index == UCN_I_SERVICE_QOS_COUNT) {
            free_index = index;
        }
    }
    for (index = 0U; index < UCN_I_SERVICE_QOS_COUNT; ++index) {
        const ucn_i_service_qos_record_t *record = &owner->qos[index];

        if (record->occupied == 0U || index == replace_index) {
            continue;
        }
        if (record->item.source_quota_key == item->source_quota_key) {
            ++source_count;
        }
        if (record->item.flow_quota_key == item->flow_quota_key) {
            ++flow_count;
        }
        if (record->item.latest != 0U) {
            ++latest_count;
        }
    }
    if (source_count >= UCN_I_SERVICE_SOURCE_QUOTA ||
        flow_count >= UCN_I_SERVICE_FLOW_QUOTA ||
        (item->latest != 0U && latest_count >= UCN_I_SERVICE_LATEST_COUNT)) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    index = replace_index != UCN_I_SERVICE_QOS_COUNT ?
                replace_index : free_index;
    if (index == UCN_I_SERVICE_QOS_COUNT) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (owner->next_enqueue_order == UINT64_MAX ||
        next_generation(owner->qos[index].generation, &generation) != UCN_OK) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    memset(superseded_out, 0, sizeof(*superseded_out));
    if (replace_index != UCN_I_SERVICE_QOS_COUNT) {
        *superseded_out = owner->qos[index].item.sendable;
    }
    frozen = *item;
    frozen.enqueue_order = owner->next_enqueue_order;
    ++owner->next_enqueue_order;
    memset(&owner->qos[index], 0, sizeof(owner->qos[index]));
    owner->qos[index].item = frozen;
    owner->qos[index].generation = generation;
    owner->qos[index].occupied = 1U;
    *qos_handle_out = ucn_i_service_p_handle(owner, (uint16_t)index,
                                              generation,
                                              UCN_I_SERVICE_QOS_KIND);
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

static bool eligible(const ucn_i_service_qos_record_t *record,
                     uint8_t traffic_class,
                     uint64_t now_us)
{
    return record->occupied != 0U && record->selected == 0U &&
           record->submitted == 0U &&
           record->item.traffic_class == traffic_class &&
           (record->item.deadline_us == 0U || now_us < record->item.deadline_us);
}

ucn_result_t ucn_i_service_qos_pick(ucn_i_service_owner_t *owner,
                                    uint64_t now_us,
                                    ucn_handle_t *qos_handle_out,
                                    ucn_handle_t *sendable_out)
{
    ucn_result_t result;
    size_t schedule_step;

    if (qos_handle_out == NULL || sendable_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (schedule_step = 0U; schedule_step < sizeof(schedule); ++schedule_step) {
        uint8_t traffic_class = schedule[owner->schedule_cursor];
        uint16_t start = owner->class_cursor[traffic_class];
        size_t offset;

        owner->schedule_cursor =
            (uint8_t)((owner->schedule_cursor + 1U) % sizeof(schedule));
        for (offset = 0U; offset < UCN_I_SERVICE_QOS_COUNT; ++offset) {
            uint16_t slot = (uint16_t)((start + offset) %
                                       UCN_I_SERVICE_QOS_COUNT);
            ucn_i_service_qos_record_t *record = &owner->qos[slot];

            if (eligible(record, traffic_class, now_us)) {
                owner->class_cursor[traffic_class] =
                    (uint16_t)((slot + 1U) % UCN_I_SERVICE_QOS_COUNT);
                record->selected = 1U;
                *qos_handle_out = ucn_i_service_p_handle(
                    owner, slot, record->generation, UCN_I_SERVICE_QOS_KIND);
                *sendable_out = record->item.sendable;
                ucn_i_service_p_unlock(owner);
                return UCN_OK;
            }
        }
        owner->class_cursor[traffic_class] =
            (uint16_t)((start + 1U) % UCN_I_SERVICE_QOS_COUNT);
    }
    ucn_i_service_p_unlock(owner);
    return UCN_ERR_NOT_FOUND;
}

ucn_result_t ucn_i_service_qos_note_submitted(ucn_i_service_owner_t *owner,
                                              ucn_handle_t qos_handle)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_qos_record_t *record;

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_qos(owner, qos_handle);
    if (record == NULL || record->selected == 0U ||
        record->submitted != 0U) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->submitted = 1U;
    record->selected = 0U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_qos_release_pick(ucn_i_service_owner_t *owner,
                                            ucn_handle_t qos_handle)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_qos_record_t *record;

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_qos(owner, qos_handle);
    if (record == NULL || record->selected == 0U ||
        record->submitted != 0U) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->selected = 0U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_qos_remove(ucn_i_service_owner_t *owner,
                                      ucn_handle_t qos_handle)
{
    ucn_result_t result = ucn_i_service_p_lock(owner);
    ucn_i_service_qos_record_t *record;

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_qos(owner, qos_handle);
    if (record == NULL) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    if (record->selected != 0U) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    record->occupied = 0U;
    record->selected = 0U;
    record->submitted = 0U;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_service_qos_view(ucn_i_service_owner_t *owner,
                                    ucn_handle_t qos_handle,
                                    ucn_i_service_qos_view_t *view_out)
{
    ucn_result_t result;
    ucn_i_service_qos_record_t *record;

    if (view_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_service_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_service_p_qos(owner, qos_handle);
    if (record == NULL) {
        ucn_i_service_p_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    memset(view_out, 0, sizeof(*view_out));
    view_out->item = record->item;
    view_out->selected = record->selected;
    view_out->submitted = record->submitted;
    ucn_i_service_p_unlock(owner);
    return UCN_OK;
}
