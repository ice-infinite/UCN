#include "internal/ucn_service.h"
#include "internal/ucn_checked.h"

#include <string.h>

static void put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void put_u64(uint8_t *output, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - 8U * index));
    }
}

static uint16_t get_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint64_t get_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
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

static bool bytes_zero(const uint8_t *bytes, size_t length)
{
    return !bytes_nonzero(bytes, length);
}

static bool wire_key_valid(const ucn_i_service_operation_wire_key_t *value)
{
    return value != NULL && value->operation_id != 0U &&
           value->service_id != 0U && value->service_id != UINT16_MAX &&
           value->service_opcode != UINT16_MAX &&
           bytes_nonzero(value->request_digest,
                         sizeof(value->request_digest));
}

static void wire_key_write(const ucn_i_service_operation_wire_key_t *value,
                           uint8_t *output)
{
    put_u64(output, value->operation_id);
    put_u16(output + 8U, value->service_id);
    put_u16(output + 10U, value->service_opcode);
    memcpy(output + 12U, value->request_digest,
           sizeof(value->request_digest));
}

static void wire_key_read(const uint8_t *input,
                          ucn_i_service_operation_wire_key_t *value)
{
    value->operation_id = get_u64(input);
    value->service_id = get_u16(input + 8U);
    value->service_opcode = get_u16(input + 10U);
    memcpy(value->request_digest, input + 12U,
           sizeof(value->request_digest));
}

ucn_result_t ucn_i_service_operation_wire_key_encode(
    const ucn_i_service_operation_wire_key_t *value,
    uint8_t output[UCN_I_SERVICE_OPERATION_WIRE_KEY_BYTES])
{
    uint8_t encoded[UCN_I_SERVICE_OPERATION_WIRE_KEY_BYTES];
    if (!wire_key_valid(value) || output == NULL ||
        ucn_i_ranges_overlap(value, sizeof(*value), output,
                             UCN_I_SERVICE_OPERATION_WIRE_KEY_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    wire_key_write(value, encoded);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_wire_key_decode(
    const uint8_t input[UCN_I_SERVICE_OPERATION_WIRE_KEY_BYTES],
    ucn_i_service_operation_wire_key_t *value_out)
{
    ucn_i_service_operation_wire_key_t value;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_SERVICE_OPERATION_WIRE_KEY_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    wire_key_read(input, &value);
    if (!wire_key_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

static bool terminal_phase_valid(uint8_t phase)
{
    return phase == UCN_I_SERVICE_OPERATION_COMMITTED_RESULT ||
           phase == UCN_I_SERVICE_OPERATION_ABORTED_NO_EFFECT ||
           phase == UCN_I_SERVICE_OPERATION_IN_DOUBT ||
           phase == UCN_I_SERVICE_OPERATION_TOMBSTONED;
}

ucn_result_t ucn_i_service_operation_receipt_encode(
    const ucn_i_service_operation_receipt_body_t *value,
    uint8_t output[UCN_I_SERVICE_OPERATION_RECEIPT_BYTES])
{
    uint8_t encoded[UCN_I_SERVICE_OPERATION_RECEIPT_BYTES] = {0};
    if (value == NULL || output == NULL || !wire_key_valid(&value->key) ||
        !bytes_nonzero(value->result_digest,
                       sizeof(value->result_digest)) ||
        !terminal_phase_valid(value->terminal_phase) ||
        !bytes_zero(value->reserved_zero, sizeof(value->reserved_zero)) ||
        ucn_i_ranges_overlap(value, sizeof(*value), output,
                             UCN_I_SERVICE_OPERATION_RECEIPT_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    wire_key_write(&value->key, encoded);
    memcpy(encoded + UCN_I_SERVICE_OPERATION_WIRE_KEY_BYTES,
           value->result_digest, sizeof(value->result_digest));
    encoded[44] = value->terminal_phase;
    memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_receipt_decode(
    const uint8_t input[UCN_I_SERVICE_OPERATION_RECEIPT_BYTES],
    ucn_i_service_operation_receipt_body_t *value_out)
{
    ucn_i_service_operation_receipt_body_t value;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_SERVICE_OPERATION_RECEIPT_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    wire_key_read(input, &value.key);
    memcpy(value.result_digest,
           input + UCN_I_SERVICE_OPERATION_WIRE_KEY_BYTES,
           sizeof(value.result_digest));
    value.terminal_phase = input[44];
    memcpy(value.reserved_zero, input + 45U,
           sizeof(value.reserved_zero));
    if (!wire_key_valid(&value.key) ||
        !bytes_nonzero(value.result_digest, sizeof(value.result_digest)) ||
        !terminal_phase_valid(value.terminal_phase) ||
        !bytes_zero(value.reserved_zero, sizeof(value.reserved_zero))) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_cancel_encode(
    const ucn_i_service_operation_cancel_body_t *value,
    uint8_t output[UCN_I_SERVICE_OPERATION_CANCEL_BYTES])
{
    uint8_t encoded[UCN_I_SERVICE_OPERATION_CANCEL_BYTES] = {0};
    if (value == NULL || output == NULL || !wire_key_valid(&value->key) ||
        value->reason < UCN_I_SERVICE_CANCEL_CALLER ||
        value->reason > UCN_I_SERVICE_CANCEL_SHUTDOWN ||
        !bytes_zero(value->reserved_zero, sizeof(value->reserved_zero)) ||
        ucn_i_ranges_overlap(value, sizeof(*value), output,
                             UCN_I_SERVICE_OPERATION_CANCEL_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    wire_key_write(&value->key, encoded);
    encoded[28] = value->reason;
    memcpy(output, encoded, sizeof(encoded));
    return UCN_OK;
}

ucn_result_t ucn_i_service_operation_cancel_decode(
    const uint8_t input[UCN_I_SERVICE_OPERATION_CANCEL_BYTES],
    ucn_i_service_operation_cancel_body_t *value_out)
{
    ucn_i_service_operation_cancel_body_t value;
    if (input == NULL || value_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_SERVICE_OPERATION_CANCEL_BYTES,
                             value_out, sizeof(*value_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    wire_key_read(input, &value.key);
    value.reason = input[28];
    memcpy(value.reserved_zero, input + 29U,
           sizeof(value.reserved_zero));
    if (!wire_key_valid(&value.key) ||
        value.reason < UCN_I_SERVICE_CANCEL_CALLER ||
        value.reason > UCN_I_SERVICE_CANCEL_SHUTDOWN ||
        !bytes_zero(value.reserved_zero, sizeof(value.reserved_zero))) {
        return UCN_ERR_MALFORMED;
    }
    *value_out = value;
    return UCN_OK;
}
