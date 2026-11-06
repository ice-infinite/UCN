#include "internal/ucn_realtime.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression_) do { if (!(expression_)) return __LINE__; } while (0)

typedef struct test_lock { uint8_t held; } test_lock_t;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = context;
    if (lock != NULL) lock->held = 0U;
}

static const uint8_t local_golden[16] = {
    0x11, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
};
static const uint8_t synced_golden[16] = {
    0x12, 0x56, 0x12, 0x34, 0x01, 0x02, 0x03, 0x04,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
};
static const uint8_t deadline_golden[16] = {
    0x13, 0x5B, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static int check_vector(const ucn_i_realtime_envelope_t *value,
                        const uint8_t golden[16])
{
    ucn_i_realtime_envelope_t decoded;
    uint8_t encoded[16];
    memset(encoded, 0xA5, sizeof(encoded));
    memset(&decoded, 0xA5, sizeof(decoded));
    CHECK(ucn_i_realtime_envelope_encode(value, encoded) == UCN_OK);
    CHECK(memcmp(encoded, golden, 16U) == 0);
    CHECK(ucn_i_realtime_envelope_decode(golden, 16U, &decoded) == UCN_OK);
    CHECK(memcmp(&decoded, value, sizeof(decoded)) == 0);
    return 0;
}

static int test_golden(void)
{
    ucn_i_realtime_envelope_t value;
    int result;
    memset(&value, 0, sizeof(value));
    value.capture_time_us = UINT64_C(0x0102030405060708);
    value.mode = UCN_I_REALTIME_LOCAL_STAMP;
    value.uncertainty_class = 31U;
    value.sample_capture_hardware = 1U;
    result = check_vector(&value, local_golden);
    if (result != 0) return result;
    memset(&value, 0, sizeof(value));
    value.capture_time_us = UINT64_C(0x1122334455667788);
    value.domain_generation = UINT32_C(0x01020304);
    value.clock_domain_id = UINT16_C(0x1234);
    value.mode = UCN_I_REALTIME_SYNCED_STAMP;
    value.uncertainty_class = 10U;
    value.sample_capture_hardware = 1U;
    value.domain_time_valid = 1U;
    result = check_vector(&value, synced_golden);
    if (result != 0) return result;
    memset(&value, 0, sizeof(value));
    value.capture_time_us = UINT64_MAX;
    value.domain_generation = UCN_I_REALTIME_DOMAIN_GENERATION_MAX;
    value.clock_domain_id = UCN_I_REALTIME_DOMAIN_ID_MAX;
    value.mode = UCN_I_REALTIME_DEADLINE;
    value.uncertainty_class = 11U;
    value.domain_time_valid = 1U;
    value.source_holdover = 1U;
    return check_vector(&value, deadline_golden);
}

static int test_negative_no_write(void)
{
    ucn_i_realtime_envelope_t value;
    ucn_i_realtime_envelope_t sentinel;
    uint8_t output[16];
    uint8_t output_sentinel[16];
    uint8_t malformed[16];
    memset(&value, 0, sizeof(value));
    value.mode = UCN_I_REALTIME_LOCAL_STAMP;
    value.uncertainty_class = 31U;
    memset(output, 0xA5, sizeof(output));
    memcpy(output_sentinel, output, sizeof(output));
    value.clock_domain_id = 1U;
    CHECK(ucn_i_realtime_envelope_encode(&value, output) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(output, output_sentinel, sizeof(output)) == 0);
    memset(&sentinel, 0xA5, sizeof(sentinel));
    value = sentinel;
    memcpy(malformed, synced_golden, sizeof(malformed));
    malformed[0] = 0x22U;
    CHECK(ucn_i_realtime_envelope_decode(malformed, sizeof(malformed),
                                          &value) == UCN_ERR_UNSUPPORTED);
    CHECK(memcmp(&value, &sentinel, sizeof(value)) == 0);
    memcpy(malformed, synced_golden, sizeof(malformed));
    malformed[1] = 0xFEU;
    CHECK(ucn_i_realtime_envelope_decode(malformed, sizeof(malformed),
                                          &value) == UCN_ERR_MALFORMED);
    CHECK(memcmp(&value, &sentinel, sizeof(value)) == 0);
    CHECK(ucn_i_realtime_envelope_decode(synced_golden, 15U,
                                          &value) == UCN_ERR_MALFORMED);
    CHECK(memcmp(&value, &sentinel, sizeof(value)) == 0);
    return 0;
}

static int test_uncertainty(void)
{
    static const uint64_t inputs[] = {0U, 1U, 2U, 1024U, 1025U,
                                      UINT64_C(1) << 30};
    static const uint8_t outputs[] = {0U, 0U, 1U, 10U, 11U, 30U};
    ucn_i_realtime_uncertainty_t components;
    uint32_t bound;
    uint32_t decoded;
    uint8_t value_class;
    bool known;
    size_t index;
    for (index = 0U; index < sizeof(inputs) / sizeof(inputs[0]); ++index) {
        CHECK(ucn_i_realtime_uncertainty_class_encode(
                  true, inputs[index], &value_class) == UCN_OK);
        CHECK(value_class == outputs[index]);
    }
    CHECK(ucn_i_realtime_uncertainty_class_encode(
              false, 1U, &value_class) == UCN_OK && value_class == 31U);
    CHECK(ucn_i_realtime_uncertainty_class_encode(
              true, (UINT64_C(1) << 30) + 1U, &value_class) == UCN_OK &&
          value_class == 31U);
    CHECK(ucn_i_realtime_uncertainty_class_decode(
              11U, &known, &decoded) == UCN_OK && known && decoded == 2048U);
    CHECK(ucn_i_realtime_uncertainty_class_decode(
              31U, &known, &decoded) == UCN_OK && !known && decoded == 0U);
    memset(&components, 0, sizeof(components));
    components.known_mask = UCN_I_REALTIME_KNOWN_ALL;
    components.timer_resolution_bound_us = 1U;
    components.link_timestamp_capture_bound_us = 2U;
    components.filter_residual_bound_us = 3U;
    components.arithmetic_rounding_bound_us = 4U;
    components.sample_capture_bound_us = 5U;
    components.path_asymmetry_bound_us = 6U;
    CHECK(ucn_i_realtime_uncertainty_aggregate(&components, &bound) == UCN_OK);
    CHECK(bound == 21U);
    components.known_mask &= (uint8_t)~UCN_I_REALTIME_KNOWN_ASYMMETRY;
    bound = UINT32_C(0xA5A5A5A5);
    CHECK(ucn_i_realtime_uncertainty_aggregate(&components, &bound) ==
          UCN_ERR_STATE);
    CHECK(bound == UINT32_C(0xA5A5A5A5));
    return 0;
}

static int test_time_control_codecs(void)
{
    static const uint8_t key_golden[UCN_I_REALTIME_TIME_KEY_BYTES] = {
        0x12U, 0x34U, 0x00U, 0x00U,
        0x01U, 0x02U, 0x03U, 0x04U,
        0x05U, 0x06U, 0x07U, 0x08U
    };
    ucn_i_realtime_time_key_t key;
    ucn_i_realtime_time_key_t decoded_key;
    ucn_i_realtime_time_follow_up_t follow_up;
    ucn_i_realtime_time_follow_up_t decoded_follow_up;
    ucn_i_realtime_time_delay_response_t response;
    ucn_i_realtime_time_delay_response_t decoded_response;
    ucn_i_realtime_time_abort_t abort;
    ucn_i_realtime_time_abort_t decoded_abort;
    uint8_t key_bytes[UCN_I_REALTIME_TIME_KEY_BYTES];
    uint8_t follow_up_bytes[UCN_I_REALTIME_TIME_FOLLOW_UP_BYTES];
    uint8_t response_bytes[UCN_I_REALTIME_TIME_DELAY_RESPONSE_BYTES];
    uint8_t abort_bytes[UCN_I_REALTIME_TIME_SYNC_ABORT_BYTES];

    memset(&key, 0, sizeof(key));
    key.clock_domain_id = UINT16_C(0x1234);
    key.domain_generation = UINT32_C(0x01020304);
    key.sync_sequence = UINT32_C(0x05060708);
    CHECK(ucn_i_realtime_time_key_encode(&key, key_bytes) == UCN_OK);
    CHECK(memcmp(key_bytes, key_golden, sizeof(key_golden)) == 0);
    memset(&decoded_key, 0, sizeof(decoded_key));
    CHECK(ucn_i_realtime_time_key_decode(key_bytes, &decoded_key) == UCN_OK);
    CHECK(memcmp(&decoded_key, &key, sizeof(key)) == 0);

    memset(&follow_up, 0, sizeof(follow_up));
    follow_up.key = key;
    follow_up.t1_master_tx_us = UINT64_C(0x1112131415161718);
    follow_up.t1_uncertainty_us = UINT32_C(0x01020304);
    CHECK(ucn_i_realtime_time_follow_up_encode(
              &follow_up, follow_up_bytes) == UCN_OK);
    CHECK(follow_up_bytes[12] == 0x11U && follow_up_bytes[19] == 0x18U &&
          follow_up_bytes[20] == 0x01U && follow_up_bytes[23] == 0x04U);
    memset(&decoded_follow_up, 0, sizeof(decoded_follow_up));
    CHECK(ucn_i_realtime_time_follow_up_decode(
              follow_up_bytes, &decoded_follow_up) == UCN_OK);
    CHECK(memcmp(&decoded_follow_up, &follow_up,
                 sizeof(follow_up)) == 0);

    memset(&response, 0, sizeof(response));
    response.key = key;
    response.t4_master_rx_us = UINT64_C(0x2122232425262728);
    response.t4_uncertainty_us = UINT32_C(0x05060708);
    CHECK(ucn_i_realtime_time_delay_response_encode(
              &response, response_bytes) == UCN_OK);
    memset(&decoded_response, 0, sizeof(decoded_response));
    CHECK(ucn_i_realtime_time_delay_response_decode(
              response_bytes, &decoded_response) == UCN_OK);
    CHECK(memcmp(&decoded_response, &response, sizeof(response)) == 0);

    memset(&abort, 0, sizeof(abort));
    abort.key = key;
    abort.reason = UCN_I_REALTIME_ABORT_TIMEOUT;
    CHECK(ucn_i_realtime_time_abort_encode(&abort, abort_bytes) == UCN_OK);
    CHECK(abort_bytes[12] == UCN_I_REALTIME_ABORT_TIMEOUT &&
          abort_bytes[13] == 0U && abort_bytes[15] == 0U);
    memset(&decoded_abort, 0, sizeof(decoded_abort));
    CHECK(ucn_i_realtime_time_abort_decode(abort_bytes,
                                            &decoded_abort) == UCN_OK);
    CHECK(memcmp(&decoded_abort, &abort, sizeof(abort)) == 0);
    abort_bytes[13] = 1U;
    memset(&decoded_abort, 0xA5, sizeof(decoded_abort));
    CHECK(ucn_i_realtime_time_abort_decode(abort_bytes,
                                            &decoded_abort) ==
          UCN_ERR_MALFORMED);
    CHECK(((const uint8_t *)&decoded_abort)[0] == 0xA5U);
    return 0;
}

static int test_owner_lifecycle(void)
{
    ucn_i_realtime_owner_t owner;
    ucn_i_realtime_owner_t sentinel;
    ucn_i_realtime_config_t config;
    test_lock_t lock;

    memset(&owner, 0, sizeof(owner));
    memset(&config, 0, sizeof(config));
    memset(&lock, 0, sizeof(lock));
    config.runtime_instance = 1U;
    config.owner_instance = 8U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &lock;
    config.state_lock.enter = lock_enter;
    config.state_lock.leave = lock_leave;
    CHECK(ucn_i_realtime_owner_init(&owner, &config) == UCN_OK);
    sentinel = owner;
    CHECK(ucn_i_realtime_owner_init(&owner, &config) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&owner, &sentinel, sizeof(owner)) == 0);
    lock.held = 1U;
    CHECK(ucn_i_realtime_owner_destroy(&owner) == UCN_ERR_STATE);
    CHECK(memcmp(&owner, &sentinel, sizeof(owner)) == 0);
    lock.held = 0U;
    CHECK(ucn_i_realtime_owner_destroy(&owner) == UCN_OK);
    CHECK(lock.held == 0U);
    memset(&sentinel, 0, sizeof(sentinel));
    CHECK(memcmp(&owner, &sentinel, sizeof(owner)) == 0);
    return 0;
}

int main(void)
{
    int result = test_golden();
    if (result == 0) result = test_negative_no_write();
    if (result == 0) result = test_uncertainty();
    if (result == 0) result = test_time_control_codecs();
    if (result == 0) result = test_owner_lifecycle();
    if (result != 0) fprintf(stderr, "realtime codec failed: %d\n", result);
    return result;
}
