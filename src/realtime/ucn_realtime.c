#include "internal/ucn_realtime.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool object_zero(const void *object, size_t bytes)
{
    const uint8_t *value = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < bytes; ++index) {
        if (value[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

ucn_result_t ucn_i_realtime_owner_init(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_config_t *config)
{
    if (owner == NULL || config == NULL ||
        !object_zero(owner, sizeof(*owner)) ||
        config->runtime_instance == 0U || config->owner_instance == 0U ||
        config->reserved_zero != 0U || !lock_valid(&config->state_lock) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             config->state_lock.context, 1U)) {
        return UCN_ERR_ARGUMENT;
    }
    owner->magic = UCN_I_REALTIME_MAGIC;
    owner->runtime_instance = config->runtime_instance;
    owner->schema = UCN_I_REALTIME_SCHEMA;
    owner->owner_instance = config->owner_instance;
    owner->state_lock = config->state_lock;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_owner_destroy(ucn_i_realtime_owner_t *owner)
{
    ucn_i_lock_ops_t lock;
    ucn_result_t result;

    if (owner == NULL || owner->magic != UCN_I_REALTIME_MAGIC ||
        owner->schema != UCN_I_REALTIME_SCHEMA ||
        !lock_valid(&owner->state_lock)) {
        return UCN_ERR_STATE;
    }
    lock = owner->state_lock;
    result = lock.enter(lock.context);
    if (result != UCN_OK) {
        return result;
    }
    memset(owner, 0, sizeof(*owner));
    lock.leave(lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_local_stamp(
    uint64_t capture_time_us, bool hardware_capture,
    ucn_i_realtime_envelope_t *envelope_out)
{
    ucn_i_realtime_envelope_t envelope;

    if (envelope_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&envelope, 0, sizeof(envelope));
    envelope.capture_time_us = capture_time_us;
    envelope.mode = UCN_I_REALTIME_LOCAL_STAMP;
    envelope.uncertainty_class = UCN_I_REALTIME_UNCERTAINTY_UNKNOWN;
    envelope.sample_capture_hardware = hardware_capture ? 1U : 0U;
    *envelope_out = envelope;
    return UCN_OK;
}
