#include "internal/ucn_admission.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_ADMISSION_MAGIC UINT32_C(0x55434144)
#define UCN_I_ADMISSION_PROVIDER_API UINT16_C(1)
#define UCN_I_ADMISSION_CALLBACK_COOKIE UINT16_C(0x0601)
#define UCN_I_ADMISSION_CALLBACK_VERIFY UINT16_C(0x0602)
#define UCN_I_ADMISSION_CALLBACK_EVENT UINT16_C(0x0603)
#define UCN_I_ADMISSION_PROTOCOL_VERSION UINT8_C(6)

static bool hello_cookie_valid(
    const ucn_i_admission_hello_cookie_t *hello_cookie);

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

static bool object_zero(const void *object, size_t bytes)
{
    return bytes_zero((const uint8_t *)object, bytes);
}

static bool lock_valid(const ucn_i_lock_ops_t *lock)
{
    return lock != NULL && lock->struct_size == sizeof(*lock) &&
           lock->api_version == UCN_I_LOCK_OPS_VERSION &&
           lock->context != NULL && lock->enter != NULL &&
           lock->leave != NULL;
}

static ucn_result_t owner_lock(ucn_i_admission_owner_t *owner)
{
    if (owner == NULL || owner->magic != UCN_I_ADMISSION_MAGIC ||
        owner->schema != UCN_I_ADMISSION_SCHEMA ||
        !lock_valid(&owner->state_lock)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

static void owner_unlock(ucn_i_admission_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static bool link_valid(ucn_i_admission_link_t link)
{
    return link.id != 0U && link.id != UINT16_MAX &&
           link.generation != 0U && link.reserved_zero == 0U;
}

static bool key_valid(const ucn_i_admission_key_t *key)
{
    return key != NULL && link_valid(key->link) &&
           key->transaction_id != 0U &&
           key->local_peer_discriminator != 0U &&
           bytes_nonzero(key->identity_digest,
                         sizeof(key->identity_digest));
}

static bool hello_valid(const ucn_i_admission_hello_t *hello)
{
    return hello != NULL && hello->device_nonce != 0U &&
           hello->transaction_id != 0U &&
           bytes_nonzero(hello->identity_digest,
                         sizeof(hello->identity_digest));
}

static void put16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (uint8_t)(value >> 8U);
    bytes[offset + 1U] = (uint8_t)value;
}

static void put32(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (uint8_t)(value >> 24U);
    bytes[offset + 1U] = (uint8_t)(value >> 16U);
    bytes[offset + 2U] = (uint8_t)(value >> 8U);
    bytes[offset + 3U] = (uint8_t)value;
}

static void put64(uint8_t *bytes, size_t offset, uint64_t value)
{
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[offset + index] =
            (uint8_t)(value >> (uint8_t)((7U - index) * 8U));
    }
}

static uint16_t get16(const uint8_t *bytes, size_t offset)
{
    return (uint16_t)(((uint16_t)bytes[offset] << 8U) |
                      bytes[offset + 1U]);
}

static uint32_t get32(const uint8_t *bytes, size_t offset)
{
    return ((uint32_t)bytes[offset] << 24U) |
           ((uint32_t)bytes[offset + 1U] << 16U) |
           ((uint32_t)bytes[offset + 2U] << 8U) |
           bytes[offset + 3U];
}

static uint64_t get64(const uint8_t *bytes, size_t offset)
{
    uint64_t value = 0U;
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

ucn_result_t ucn_i_admission_hello_encode(
    const ucn_i_admission_hello_t *hello,
    uint8_t output[UCN_I_ADMISSION_HELLO_BYTES])
{
    uint8_t bytes[UCN_I_ADMISSION_HELLO_BYTES];

    if (!hello_valid(hello) || output == NULL ||
        ucn_i_ranges_overlap(hello, sizeof(*hello), output,
                             UCN_I_ADMISSION_HELLO_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(bytes, 0, sizeof(bytes));
    bytes[0] = 1U;
    bytes[1] = 1U;
    memcpy(&bytes[4], hello->identity_digest, 16U);
    put64(bytes, 20U, hello->device_nonce);
    put64(bytes, 28U, hello->transaction_id);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_admission_hello_decode(
    const uint8_t input[UCN_I_ADMISSION_HELLO_BYTES],
    ucn_i_admission_hello_t *hello_out)
{
    ucn_i_admission_hello_t hello;

    if (input == NULL || hello_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_ADMISSION_HELLO_BYTES,
                             hello_out, sizeof(*hello_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (input[0] != 1U || input[1] != 1U || input[2] != 0U ||
        input[3] != 0U || !bytes_zero(&input[36], 4U)) {
        return UCN_ERR_MALFORMED;
    }
    memset(&hello, 0, sizeof(hello));
    memcpy(hello.identity_digest, &input[4], 16U);
    hello.device_nonce = get64(input, 20U);
    hello.transaction_id = get64(input, 28U);
    if (!hello_valid(&hello)) {
        return UCN_ERR_MALFORMED;
    }
    *hello_out = hello;
    return UCN_OK;
}

static bool cookie_challenge_valid(
    const ucn_i_admission_cookie_challenge_t *challenge)
{
    size_t length;

    if (challenge == NULL ||
        challenge->transaction_id == 0U ||
        challenge->cookie_time_bucket == 0U ||
        challenge->cookie_bytes == 0U ||
        challenge->cookie_bytes > UCN_I_ADMISSION_COOKIE_BYTES ||
        !bytes_zero(challenge->reserved_zero,
                    sizeof(challenge->reserved_zero))) {
        return false;
    }
    length = challenge->cookie_bytes;
    return bytes_nonzero(challenge->cookie, length) &&
           bytes_zero(&challenge->cookie[length],
                      UCN_I_ADMISSION_COOKIE_BYTES - length);
}

ucn_result_t ucn_i_admission_cookie_challenge_encode(
    const ucn_i_admission_cookie_challenge_t *challenge,
    uint8_t output[UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES])
{
    uint8_t bytes[UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES];

    if (!cookie_challenge_valid(challenge) || output == NULL ||
        ucn_i_ranges_overlap(challenge, sizeof(*challenge), output,
                             UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(bytes, 0, sizeof(bytes));
    bytes[0] = 1U;
    bytes[1] = 1U;
    bytes[2] = challenge->cookie_bytes;
    put64(bytes, 4U, challenge->transaction_id);
    put32(bytes, 12U, challenge->cookie_time_bucket);
    memcpy(&bytes[16], challenge->cookie, 16U);
    memcpy(output, bytes, sizeof(bytes));
    return UCN_OK;
}

ucn_result_t ucn_i_admission_cookie_challenge_decode(
    const uint8_t input[UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES],
    ucn_i_admission_cookie_challenge_t *challenge_out)
{
    ucn_i_admission_cookie_challenge_t challenge;

    if (input == NULL || challenge_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES,
                             challenge_out, sizeof(*challenge_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (input[0] != 1U || input[1] != 1U || input[3] != 0U ||
        !bytes_zero(&input[32], 8U)) {
        return UCN_ERR_MALFORMED;
    }
    memset(&challenge, 0, sizeof(challenge));
    challenge.cookie_bytes = input[2];
    challenge.transaction_id = get64(input, 4U);
    challenge.cookie_time_bucket = get32(input, 12U);
    memcpy(challenge.cookie, &input[16], 16U);
    if (!cookie_challenge_valid(&challenge)) {
        return UCN_ERR_MALFORMED;
    }
    *challenge_out = challenge;
    return UCN_OK;
}

ucn_result_t ucn_i_admission_hello_cookie_encode(
    const ucn_i_admission_hello_cookie_t *hello_cookie,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    uint8_t bytes[UCN_I_ADMISSION_HELLO_COOKIE_MAX_BYTES];
    size_t length;

    if (!hello_cookie_valid(hello_cookie) || output == NULL ||
        output_bytes == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    length = UCN_I_ADMISSION_HELLO_COOKIE_FIXED_BYTES +
             hello_cookie->cookie_bytes;
    if (output_capacity < length ||
        ucn_i_ranges_overlap(hello_cookie, sizeof(*hello_cookie), output,
                             output_capacity) ||
        ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                             sizeof(*output_bytes))) {
        return output_capacity < length ? UCN_ERR_NO_SPACE :
                                          UCN_ERR_ARGUMENT;
    }
    memset(bytes, 0, sizeof(bytes));
    bytes[0] = 1U;
    bytes[1] = 1U;
    bytes[2] = hello_cookie->cookie_bytes;
    memcpy(&bytes[4], hello_cookie->key.identity_digest, 16U);
    put64(bytes, 20U, hello_cookie->device_nonce);
    put64(bytes, 28U, hello_cookie->key.transaction_id);
    put64(bytes, 36U, hello_cookie->lease_freshness_challenge_nonce);
    put16(bytes, 44U, hello_cookie->key.link.id);
    put32(bytes, 46U, hello_cookie->key.link.generation);
    memcpy(&bytes[50], hello_cookie->prior_messages_hash, 32U);
    memcpy(&bytes[82], hello_cookie->cookie, hello_cookie->cookie_bytes);
    memcpy(output, bytes, length);
    *output_bytes = length;
    return UCN_OK;
}

ucn_result_t ucn_i_admission_hello_cookie_decode(
    const uint8_t *input,
    size_t input_bytes,
    uint32_t local_peer_discriminator,
    ucn_i_admission_hello_cookie_t *hello_cookie_out)
{
    ucn_i_admission_hello_cookie_t value;
    size_t length;

    if (input == NULL || hello_cookie_out == NULL ||
        local_peer_discriminator == 0U ||
        ucn_i_ranges_overlap(input, input_bytes, hello_cookie_out,
                             sizeof(*hello_cookie_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_bytes < UCN_I_ADMISSION_HELLO_COOKIE_FIXED_BYTES + 1U ||
        input_bytes > UCN_I_ADMISSION_HELLO_COOKIE_MAX_BYTES ||
        input[0] != 1U || input[1] != 1U || input[3] != 0U) {
        return UCN_ERR_MALFORMED;
    }
    length = input[2];
    if (length == 0U || length > UCN_I_ADMISSION_COOKIE_BYTES ||
        input_bytes != UCN_I_ADMISSION_HELLO_COOKIE_FIXED_BYTES + length) {
        return UCN_ERR_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.key.local_peer_discriminator = local_peer_discriminator;
    memcpy(value.key.identity_digest, &input[4], 16U);
    value.device_nonce = get64(input, 20U);
    value.key.transaction_id = get64(input, 28U);
    value.lease_freshness_challenge_nonce = get64(input, 36U);
    value.key.link.id = get16(input, 44U);
    value.key.link.generation = get32(input, 46U);
    memcpy(value.prior_messages_hash, &input[50], 32U);
    value.cookie_bytes = (uint8_t)length;
    memcpy(value.cookie, &input[82], length);
    if (!hello_cookie_valid(&value)) {
        return UCN_ERR_MALFORMED;
    }
    *hello_cookie_out = value;
    return UCN_OK;
}

static bool evidence_valid(const ucn_i_admission_evidence_t *evidence)
{
    size_t length;

    if (evidence == NULL || evidence->length == 0U ||
        evidence->length > UCN_I_ADMISSION_EVIDENCE_BYTES ||
        !bytes_zero(evidence->reserved_zero,
                    sizeof(evidence->reserved_zero))) {
        return false;
    }
    length = evidence->length;
    return bytes_nonzero(evidence->bytes, length) &&
           bytes_zero(&evidence->bytes[length],
                      UCN_I_ADMISSION_EVIDENCE_BYTES - length);
}

static bool hello_cookie_valid(
    const ucn_i_admission_hello_cookie_t *hello_cookie)
{
    size_t length;

    if (hello_cookie == NULL || !key_valid(&hello_cookie->key) ||
        hello_cookie->device_nonce == 0U ||
        hello_cookie->lease_freshness_challenge_nonce == 0U ||
        hello_cookie->cookie_bytes == 0U ||
        hello_cookie->cookie_bytes > UCN_I_ADMISSION_COOKIE_BYTES ||
        !bytes_zero(hello_cookie->reserved_zero,
                    sizeof(hello_cookie->reserved_zero)) ||
        !bytes_nonzero(hello_cookie->prior_messages_hash,
                       sizeof(hello_cookie->prior_messages_hash))) {
        return false;
    }
    length = hello_cookie->cookie_bytes;
    return bytes_nonzero(hello_cookie->cookie, length) &&
           bytes_zero(&hello_cookie->cookie[length],
                      UCN_I_ADMISSION_COOKIE_BYTES - length);
}

static bool principal_valid(const uint8_t principal[16])
{
    return bytes_nonzero(principal, 16U);
}

static bool transcript_base_valid(
    const ucn_i_admission_transcript_t *transcript)
{
    return transcript != NULL &&
           transcript->protocol_version == UCN_I_ADMISSION_PROTOCOL_VERSION &&
           transcript->bootstrap_header_contract == 1U &&
           bytes_zero(transcript->reserved_zero,
                      sizeof(transcript->reserved_zero)) &&
           bytes_nonzero(transcript->joining_device_identity_digest, 16U) &&
           transcript->device_nonce != 0U &&
           transcript->transaction_id != 0U &&
           transcript->lease_freshness_challenge_nonce != 0U &&
           transcript->selected_link_id != 0U &&
           transcript->selected_link_id != UINT16_MAX &&
           transcript->selected_link_generation != 0U &&
           bytes_nonzero(transcript->prior_messages_hash, 32U);
}

static bool authority_fields_valid(
    const ucn_i_admission_transcript_t *value)
{
    return principal_valid(value->authority_principal) &&
           value->authority_generation != 0U &&
           value->authority_nonce != 0U && value->realm_id != 0U &&
           value->realm_id != UINT32_MAX && value->authority_address != 0U &&
           value->authority_address != UINT32_MAX &&
           value->authority_binding_generation != 0U &&
           value->authority_lease_sequence != 0U &&
           value->authority_lease_duration_us != 0U &&
           value->freshness_max_remaining_lease_us != 0U &&
           value->freshness_max_remaining_lease_us <=
               value->authority_lease_duration_us &&
           bytes_nonzero(value->durable_fence_token, 16U) &&
           bytes_nonzero(value->allocation_high_water_digest, 16U) &&
           bytes_nonzero(value->quorum_config_digest, 32U) &&
           bytes_nonzero(value->signer_set_digest, 32U) &&
           bytes_nonzero(value->threshold_proof_digest, 32U) &&
           bytes_nonzero(value->freshness_proof_transcript_hash, 32U) &&
           value->authority_signer_count != 0U &&
           value->authority_quorum_threshold != 0U &&
           value->authority_quorum_threshold <= value->authority_signer_count;
}

static bool authority_fields_zero(
    const ucn_i_admission_transcript_t *value)
{
    return bytes_zero(value->authority_principal, 16U) &&
           value->authority_generation == 0U && value->authority_nonce == 0U &&
           value->realm_id == 0U && value->authority_address == 0U &&
           value->authority_binding_generation == 0U &&
           value->authority_lease_sequence == 0U &&
           value->authority_lease_duration_us == 0U &&
           value->freshness_max_remaining_lease_us == 0U &&
           bytes_zero(value->durable_fence_token, 16U) &&
           bytes_zero(value->allocation_high_water_digest, 16U) &&
           bytes_zero(value->quorum_config_digest, 32U) &&
           bytes_zero(value->signer_set_digest, 32U) &&
           bytes_zero(value->threshold_proof_digest, 32U) &&
           bytes_zero(value->freshness_proof_transcript_hash, 32U) &&
           value->authority_signer_count == 0U &&
           value->authority_quorum_threshold == 0U;
}

static bool device_fields_valid(
    const ucn_i_admission_transcript_t *value)
{
    return principal_valid(value->joining_device_principal) &&
           value->selected_hop_suite != 0U &&
           value->selected_hop_key_id != 0U &&
           value->selected_hop_key_generation != 0U &&
           (value->selected_e2e_mode == 1U ||
            value->selected_e2e_mode == 2U) &&
           value->selected_e2e_suite != 0U &&
           value->selected_e2e_key_id != 0U &&
           value->selected_e2e_key_generation != 0U &&
           value->selected_session_generation != 0U;
}

static bool device_fields_zero(const ucn_i_admission_transcript_t *value)
{
    return bytes_zero(value->joining_device_principal, 16U) &&
           value->selected_hop_suite == 0U &&
           value->selected_hop_key_id == 0U &&
           value->selected_hop_key_generation == 0U &&
           value->selected_e2e_mode == 0U &&
           value->selected_e2e_suite == 0U &&
           value->selected_e2e_key_id == 0U &&
           value->selected_e2e_key_generation == 0U &&
           value->selected_session_generation == 0U;
}

static bool address_fields_valid(
    const ucn_i_admission_transcript_t *value)
{
    return value->proposed_address != 0U &&
           value->proposed_address != UINT32_MAX &&
           value->address_binding_generation != 0U &&
           bytes_nonzero(value->binding_lease_id, 16U) &&
           value->binding_lease_duration_us != 0U &&
           value->binding_mode >= 1U && value->binding_mode <= 3U;
}

static bool address_fields_zero(const ucn_i_admission_transcript_t *value)
{
    return value->proposed_address == 0U &&
           value->address_binding_generation == 0U &&
           bytes_zero(value->binding_lease_id, 16U) &&
           value->binding_lease_duration_us == 0U &&
           value->binding_mode == 0U;
}

static bool transcript_phase_valid(
    const ucn_i_admission_transcript_t *value,
    ucn_i_admission_phase_t phase)
{
    bool authority;
    bool device;
    bool address;

    if (!transcript_base_valid(value)) {
        return false;
    }
    authority = authority_fields_valid(value);
    device = device_fields_valid(value);
    address = address_fields_valid(value);
    if (phase == UCN_I_ADMISSION_COOKIE_VERIFIED) {
        return !authority && !device && !address &&
               authority_fields_zero(value) && device_fields_zero(value) &&
               address_fields_zero(value);
    }
    if (phase == UCN_I_ADMISSION_AUTHORITY_VERIFIED) {
        return authority && !device && !address && device_fields_zero(value) &&
               address_fields_zero(value);
    }
    if (phase == UCN_I_ADMISSION_DEVICE_VERIFIED) {
        return authority && device && !address && address_fields_zero(value);
    }
    if (phase == UCN_I_ADMISSION_ADDRESS_OFFERED ||
        phase == UCN_I_ADMISSION_DEVICE_COMMITTED ||
        phase == UCN_I_ADMISSION_FINAL_DURABLE) {
        return authority && device && address;
    }
    return false;
}

static bool common_equal(const ucn_i_admission_transcript_t *left,
                         const ucn_i_admission_transcript_t *right)
{
    return left->protocol_version == right->protocol_version &&
           left->bootstrap_header_contract ==
               right->bootstrap_header_contract &&
           memcmp(left->joining_device_identity_digest,
                  right->joining_device_identity_digest, 16U) == 0 &&
           left->device_nonce == right->device_nonce &&
           left->transaction_id == right->transaction_id &&
           left->lease_freshness_challenge_nonce ==
               right->lease_freshness_challenge_nonce &&
           left->selected_link_id == right->selected_link_id &&
           left->selected_link_generation == right->selected_link_generation &&
           memcmp(left->prior_messages_hash,
                  right->prior_messages_hash, 32U) == 0;
}

static bool authority_equal(const ucn_i_admission_transcript_t *left,
                            const ucn_i_admission_transcript_t *right)
{
    return memcmp(left->authority_principal,
                  right->authority_principal, 16U) == 0 &&
           left->authority_generation == right->authority_generation &&
           left->authority_nonce == right->authority_nonce &&
           left->realm_id == right->realm_id &&
           left->authority_address == right->authority_address &&
           left->authority_binding_generation ==
               right->authority_binding_generation &&
           left->authority_lease_sequence == right->authority_lease_sequence &&
           left->authority_lease_duration_us ==
               right->authority_lease_duration_us &&
           left->freshness_max_remaining_lease_us ==
               right->freshness_max_remaining_lease_us &&
           memcmp(left->durable_fence_token,
                  right->durable_fence_token, 16U) == 0 &&
           memcmp(left->allocation_high_water_digest,
                  right->allocation_high_water_digest, 16U) == 0 &&
           memcmp(left->quorum_config_digest,
                  right->quorum_config_digest, 32U) == 0 &&
           memcmp(left->signer_set_digest,
                  right->signer_set_digest, 32U) == 0 &&
           memcmp(left->threshold_proof_digest,
                  right->threshold_proof_digest, 32U) == 0 &&
           memcmp(left->freshness_proof_transcript_hash,
                  right->freshness_proof_transcript_hash, 32U) == 0 &&
           left->authority_signer_count == right->authority_signer_count &&
           left->authority_quorum_threshold ==
               right->authority_quorum_threshold;
}

static bool device_equal(const ucn_i_admission_transcript_t *left,
                         const ucn_i_admission_transcript_t *right)
{
    return memcmp(left->joining_device_principal,
                  right->joining_device_principal, 16U) == 0 &&
           left->selected_hop_suite == right->selected_hop_suite &&
           left->selected_hop_key_id == right->selected_hop_key_id &&
           left->selected_hop_key_generation ==
               right->selected_hop_key_generation &&
           left->selected_e2e_mode == right->selected_e2e_mode &&
           left->selected_e2e_suite == right->selected_e2e_suite &&
           left->selected_e2e_key_id == right->selected_e2e_key_id &&
           left->selected_e2e_key_generation ==
               right->selected_e2e_key_generation &&
           left->selected_session_generation ==
               right->selected_session_generation;
}

static bool semantic_equal(const ucn_i_admission_transcript_t *left,
                           const ucn_i_admission_transcript_t *right)
{
    return common_equal(left, right) && authority_equal(left, right) &&
           device_equal(left, right) &&
           left->proposed_address == right->proposed_address &&
           left->address_binding_generation ==
               right->address_binding_generation &&
           memcmp(left->binding_lease_id,
                  right->binding_lease_id, 16U) == 0 &&
           left->binding_lease_duration_us ==
               right->binding_lease_duration_us &&
           left->binding_mode == right->binding_mode;
}

static bool transition_valid(const ucn_i_admission_transcript_t *previous,
                             const ucn_i_admission_transcript_t *next,
                             ucn_i_admission_event_t event)
{
    if (!common_equal(previous, next)) {
        return false;
    }
    if (event == UCN_I_ADMISSION_EVENT_AUTHORITY_PROOF) {
        return authority_fields_zero(previous) && authority_fields_valid(next) &&
               device_fields_zero(previous) && device_fields_zero(next) &&
               address_fields_zero(previous) && address_fields_zero(next);
    }
    if (event == UCN_I_ADMISSION_EVENT_DEVICE_PROOF) {
        return authority_equal(previous, next) &&
               device_fields_zero(previous) && device_fields_valid(next) &&
               address_fields_zero(previous) && address_fields_zero(next);
    }
    if (event == UCN_I_ADMISSION_EVENT_ADDRESS_OFFER) {
        return authority_equal(previous, next) && device_equal(previous, next) &&
               address_fields_zero(previous) && address_fields_valid(next);
    }
    if (event == UCN_I_ADMISSION_EVENT_DEVICE_COMMIT ||
        event == UCN_I_ADMISSION_EVENT_FINAL_DURABLE) {
        return semantic_equal(previous, next);
    }
    return event == UCN_I_ADMISSION_EVENT_ABORT;
}

static void clear_callback(ucn_i_admission_owner_t *owner)
{
    owner->callback_active = 0U;
    owner->callback_event = 0U;
    owner->callback_now_us = 0U;
    owner->callback_cookie_bucket = 0U;
    owner->callback_cookie_bytes = 0U;
    memset(&owner->callback_claim, 0, sizeof(owner->callback_claim));
    memset(&owner->callback_hello, 0, sizeof(owner->callback_hello));
    memset(&owner->callback_hello_cookie, 0,
           sizeof(owner->callback_hello_cookie));
    memset(&owner->callback_key, 0, sizeof(owner->callback_key));
    memset(&owner->callback_transcript, 0,
           sizeof(owner->callback_transcript));
    memset(&owner->callback_evidence, 0, sizeof(owner->callback_evidence));
    memset(&owner->callback_binding, 0, sizeof(owner->callback_binding));
    memset(owner->callback_cookie, 0, sizeof(owner->callback_cookie));
}

static bool handle_matches(const ucn_i_admission_owner_t *owner,
                           ucn_i_admission_handle_t handle,
                           uint16_t *index_out)
{
    uint16_t index;
    const ucn_i_admission_slot_t *slot;

    if (handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance || handle.slot == 0U ||
        handle.slot > UCN_I_ADMISSION_PENDING_COUNT ||
        handle.slot_generation == 0U || handle.transaction_id == 0U) {
        return false;
    }
    index = (uint16_t)(handle.slot - 1U);
    slot = &owner->pending[index];
    if (slot->occupied == 0U ||
        slot->slot_generation != handle.slot_generation ||
        slot->key.transaction_id != handle.transaction_id) {
        return false;
    }
    if (index_out != NULL) {
        *index_out = index;
    }
    return true;
}

static ucn_i_admission_handle_t make_handle(
    const ucn_i_admission_owner_t *owner,
    uint16_t index)
{
    ucn_i_admission_handle_t handle;

    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = (uint16_t)(index + 1U);
    handle.slot_generation = owner->pending[index].slot_generation;
    handle.transaction_id = owner->pending[index].key.transaction_id;
    return handle;
}

static ucn_result_t next_claim(ucn_i_admission_owner_t *owner,
                               uint16_t operation_kind)
{
    uint32_t operation_id = owner->next_operation_id;

    if (operation_id == 0U || operation_id == UINT32_MAX) {
        return UCN_ERR_EXHAUSTED;
    }
    owner->next_operation_id++;
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = operation_id;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = operation_kind;
    return UCN_OK;
}

static ucn_result_t consume_budget(ucn_i_admission_owner_t *owner,
                                   ucn_i_admission_link_t link,
                                   uint64_t now_us)
{
    uint16_t index;
    uint16_t empty = UINT16_MAX;
    ucn_i_admission_link_budget_t *budget = NULL;

    for (index = 0U; index < UCN_LINK_COUNT; ++index) {
        if (owner->budgets[index].occupied != 0U &&
            memcmp(&owner->budgets[index].link, &link, sizeof(link)) == 0) {
            budget = &owner->budgets[index];
            break;
        }
        if (empty == UINT16_MAX && owner->budgets[index].occupied == 0U) {
            empty = index;
        }
    }
    if (budget == NULL) {
        if (empty == UINT16_MAX) {
            return UCN_ERR_NO_SPACE;
        }
        budget = &owner->budgets[empty];
        memset(budget, 0, sizeof(*budget));
        budget->link = link;
        budget->last_refill_us = now_us;
        budget->tokens = owner->token_burst;
        budget->occupied = 1U;
    } else if (now_us < budget->last_refill_us) {
        return UCN_ERR_STATE;
    } else {
        uint64_t seconds = (now_us - budget->last_refill_us) / UINT64_C(1000000);

        if (seconds != 0U) {
            uint64_t addition =
                seconds > UINT64_MAX / owner->tokens_per_second ?
                    UINT64_MAX : seconds * owner->tokens_per_second;
            uint64_t tokens = budget->tokens + addition;

            if (addition > UINT64_MAX - budget->tokens ||
                tokens > owner->token_burst) {
                tokens = owner->token_burst;
            }
            budget->tokens = (uint8_t)tokens;
            budget->last_refill_us += seconds * UINT64_C(1000000);
        }
    }
    if (budget->tokens == 0U) {
        return UCN_ERR_ACCESS;
    }
    budget->tokens--;
    return UCN_OK;
}

static ucn_result_t call_provider(ucn_i_admission_owner_t *owner,
                                  uint16_t callback_kind)
{
    ucn_result_t result;
    ucn_result_t leave_result;

    result = ucn_i_callback_gate_enter(owner->provider_gate,
                                       &owner->callback_claim);
    if (result != UCN_OK) {
        return result;
    }
    if (callback_kind == UCN_I_ADMISSION_CALLBACK_COOKIE) {
        result = owner->provider.issue_cookie(
            owner->provider.context, &owner->callback_hello,
            owner->callback_key.link, owner->callback_cookie_bucket,
            owner->callback_cookie, &owner->callback_cookie_bytes);
    } else if (callback_kind == UCN_I_ADMISSION_CALLBACK_VERIFY) {
        result = owner->provider.verify_cookie(
            owner->provider.context, &owner->callback_hello_cookie);
    } else {
        result = owner->provider.authorize_event(
            owner->provider.context, owner->callback_event,
            &owner->callback_key, &owner->callback_transcript,
            owner->callback_now_us, &owner->callback_evidence);
    }
    leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                              &owner->callback_claim);
    return leave_result == UCN_OK ? result : UCN_ERR_STATE;
}

ucn_result_t ucn_i_admission_owner_init(
    ucn_i_admission_owner_t *owner,
    const ucn_i_admission_config_t *config)
{
    ucn_result_t result;

    if (owner == NULL || config == NULL ||
        config->struct_size != sizeof(*config) ||
        config->api_version != UCN_API_VERSION ||
        config->runtime_instance == 0U || config->realm_id == 0U ||
        config->realm_id == UINT32_MAX || config->owner_instance == 0U ||
        config->identity_owner_instance == 0U ||
        config->persistence_owner_instance == 0U ||
        config->max_pending_per_link == 0U ||
        config->max_pending_per_link >
            UCN_I_ADMISSION_MAX_PENDING_PER_LINK ||
        config->token_burst == 0U || config->tokens_per_second == 0U ||
        config->pending_timeout_us == 0U || config->reserved_zero != 0U ||
        config->provider.struct_size != sizeof(config->provider) ||
        config->provider.api_version != UCN_I_ADMISSION_PROVIDER_API ||
        config->provider.context == NULL ||
        config->provider.issue_cookie == NULL ||
        config->provider.verify_cookie == NULL ||
        config->provider.authorize_event == NULL ||
        !lock_valid(&config->state_lock) || config->provider_gate == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config->provider_gate,
                             sizeof(*config->provider_gate)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             config->state_lock.context, 1U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             config->provider.context, 1U)) {
        return UCN_ERR_CONFIG;
    }
    result = config->state_lock.enter(config->state_lock.context);
    if (result != UCN_OK) {
        return result;
    }
    if (!object_zero(owner, sizeof(*owner))) {
        config->state_lock.leave(config->state_lock.context);
        return UCN_ERR_STATE;
    }
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_I_ADMISSION_MAGIC;
    owner->schema = UCN_I_ADMISSION_SCHEMA;
    owner->runtime_instance = config->runtime_instance;
    owner->realm_id = config->realm_id;
    owner->owner_instance = config->owner_instance;
    owner->identity_owner_instance = config->identity_owner_instance;
    owner->persistence_owner_instance = config->persistence_owner_instance;
    owner->max_pending_per_link = config->max_pending_per_link;
    owner->token_burst = config->token_burst;
    owner->tokens_per_second = config->tokens_per_second;
    owner->pending_timeout_us = config->pending_timeout_us;
    owner->next_operation_id = 1U;
    owner->provider = config->provider;
    owner->state_lock = config->state_lock;
    owner->provider_gate = config->provider_gate;
    config->state_lock.leave(config->state_lock.context);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_issue_cookie(
    ucn_i_admission_owner_t *owner,
    const ucn_i_admission_hello_t *hello,
    ucn_i_admission_link_t link,
    uint64_t now_us,
    uint32_t request_bytes,
    uint32_t response_bytes,
    uint32_t cookie_time_bucket,
    ucn_i_admission_cookie_challenge_t *challenge_out)
{
    ucn_i_admission_cookie_challenge_t challenge;
    ucn_result_t result;

    if (owner == NULL || !hello_valid(hello) || !link_valid(link) ||
        now_us == 0U || request_bytes == 0U || response_bytes == 0U ||
        response_bytes > request_bytes || cookie_time_bucket == 0U ||
        challenge_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), hello,
                             sizeof(*hello)) ||
        ucn_i_ranges_overlap(hello, sizeof(*hello), challenge_out,
                             sizeof(*challenge_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), challenge_out,
                             sizeof(*challenge_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = consume_budget(owner, link, now_us);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return result;
    }
    owner->callback_active = 1U;
    owner->callback_hello = *hello;
    owner->callback_key.link = link;
    owner->callback_cookie_bucket = cookie_time_bucket;
    result = next_claim(owner, UCN_I_ADMISSION_CALLBACK_COOKIE);
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    owner_unlock(owner);
    result = call_provider(owner, UCN_I_ADMISSION_CALLBACK_COOKIE);
    if (owner_lock(owner) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (owner->callback_active == 0U) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    if (owner->callback_cookie_bytes == 0U ||
        owner->callback_cookie_bytes > UCN_I_ADMISSION_COOKIE_BYTES ||
        !bytes_nonzero(owner->callback_cookie,
                       owner->callback_cookie_bytes) ||
        !bytes_zero(&owner->callback_cookie[owner->callback_cookie_bytes],
                    UCN_I_ADMISSION_COOKIE_BYTES -
                        owner->callback_cookie_bytes)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    memset(&challenge, 0, sizeof(challenge));
    challenge.transaction_id = owner->callback_hello.transaction_id;
    challenge.cookie_time_bucket = owner->callback_cookie_bucket;
    challenge.cookie_bytes = owner->callback_cookie_bytes;
    memcpy(challenge.cookie, owner->callback_cookie,
           sizeof(challenge.cookie));
    clear_callback(owner);
    *challenge_out = challenge;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_open_after_cookie(
    ucn_i_admission_owner_t *owner,
    const ucn_i_admission_hello_cookie_t *hello_cookie,
    const ucn_i_admission_transcript_t *transcript,
    uint64_t now_us,
    ucn_i_admission_handle_t *handle_out)
{
    uint16_t index;
    uint16_t empty = UINT16_MAX;
    uint16_t per_link = 0U;
    uint64_t deadline;
    uint32_t generation;
    ucn_result_t result;

    if (owner == NULL || !hello_cookie_valid(hello_cookie) ||
        !transcript_phase_valid(transcript,
                                UCN_I_ADMISSION_COOKIE_VERIFIED) ||
        now_us == 0U || handle_out == NULL ||
        memcmp(hello_cookie->key.identity_digest,
               transcript->joining_device_identity_digest, 16U) != 0 ||
        hello_cookie->key.transaction_id != transcript->transaction_id ||
        hello_cookie->key.link.id != transcript->selected_link_id ||
        hello_cookie->key.link.generation !=
            transcript->selected_link_generation ||
        hello_cookie->device_nonce != transcript->device_nonce ||
        hello_cookie->lease_freshness_challenge_nonce !=
            transcript->lease_freshness_challenge_nonce ||
        memcmp(hello_cookie->prior_messages_hash,
               transcript->prior_messages_hash, 32U) != 0 ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), hello_cookie,
                             sizeof(*hello_cookie)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), transcript,
                             sizeof(*transcript)) ||
        ucn_i_ranges_overlap(hello_cookie, sizeof(*hello_cookie),
                             handle_out, sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(transcript, sizeof(*transcript),
                             handle_out, sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->callback_active = 1U;
    owner->callback_hello_cookie = *hello_cookie;
    owner->callback_key = hello_cookie->key;
    owner->callback_transcript = *transcript;
    result = next_claim(owner, UCN_I_ADMISSION_CALLBACK_VERIFY);
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    owner_unlock(owner);
    result = call_provider(owner, UCN_I_ADMISSION_CALLBACK_VERIFY);
    if (owner_lock(owner) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (owner->callback_active == 0U || result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result == UCN_OK ? UCN_ERR_STATE : result;
    }
    for (index = 0U; index < UCN_I_ADMISSION_PENDING_COUNT; ++index) {
        ucn_i_admission_slot_t *slot = &owner->pending[index];

        if (slot->occupied != 0U &&
            memcmp(&slot->key, &owner->callback_hello_cookie.key,
                   sizeof(slot->key)) == 0) {
            if (memcmp(&slot->transcript, &owner->callback_transcript,
                       sizeof(owner->callback_transcript)) != 0 ||
                now_us >= slot->deadline_us) {
                clear_callback(owner);
                owner_unlock(owner);
                return UCN_ERR_REPLAY;
            }
            *handle_out = make_handle(owner, index);
            clear_callback(owner);
            owner_unlock(owner);
            return UCN_OK;
        }
        if (slot->occupied == 0U && empty == UINT16_MAX) {
            empty = index;
        }
        if (slot->occupied != 0U &&
            memcmp(&slot->key.link, &owner->callback_hello_cookie.key.link,
                   sizeof(slot->key.link)) == 0) {
            per_link++;
        }
    }
    if (empty == UINT16_MAX || per_link >= owner->max_pending_per_link) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    result = ucn_i_deadline_from_duration_us(
        now_us, owner->pending_timeout_us, &deadline);
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    generation = owner->pending[empty].slot_generation + 1U;
    if (generation == 0U) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    memset(&owner->pending[empty], 0, sizeof(owner->pending[empty]));
    owner->pending[empty].occupied = 1U;
    owner->pending[empty].phase = UCN_I_ADMISSION_COOKIE_VERIFIED;
    owner->pending[empty].slot_generation = generation;
    owner->pending[empty].key = owner->callback_hello_cookie.key;
    owner->pending[empty].transcript = owner->callback_transcript;
    owner->pending[empty].challenge_started_local_us = now_us;
    owner->pending[empty].deadline_us = deadline;
    *handle_out = make_handle(owner, empty);
    clear_callback(owner);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_advance(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    ucn_i_admission_event_t event,
    const ucn_i_admission_transcript_t *transcript,
    const ucn_i_admission_evidence_t *evidence,
    uint64_t now_us)
{
    uint16_t index;
    uint8_t expected;
    ucn_result_t result;

    if (owner == NULL || transcript == NULL || !evidence_valid(evidence) ||
        now_us == 0U || event < UCN_I_ADMISSION_EVENT_AUTHORITY_PROOF ||
        event > UCN_I_ADMISSION_EVENT_ABORT ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), transcript,
                             sizeof(*transcript)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), evidence,
                             sizeof(*evidence))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (owner->pending[index].phase == UCN_I_ADMISSION_FINAL_DURABLE ||
        owner->pending[index].phase == UCN_I_ADMISSION_ABORTED) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (now_us < owner->pending[index].challenge_started_local_us ||
        now_us >= owner->pending[index].deadline_us) {
        owner_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    if (event == UCN_I_ADMISSION_EVENT_ABORT) {
        expected = UCN_I_ADMISSION_ABORTED;
    } else if (owner->pending[index].phase == UCN_I_ADMISSION_COOKIE_VERIFIED &&
               event == UCN_I_ADMISSION_EVENT_AUTHORITY_PROOF) {
        expected = UCN_I_ADMISSION_AUTHORITY_VERIFIED;
    } else if (owner->pending[index].phase ==
                   UCN_I_ADMISSION_AUTHORITY_VERIFIED &&
               event == UCN_I_ADMISSION_EVENT_DEVICE_PROOF) {
        expected = UCN_I_ADMISSION_DEVICE_VERIFIED;
    } else if (owner->pending[index].phase ==
                   UCN_I_ADMISSION_DEVICE_VERIFIED &&
               event == UCN_I_ADMISSION_EVENT_ADDRESS_OFFER) {
        expected = UCN_I_ADMISSION_ADDRESS_OFFERED;
    } else if (owner->pending[index].phase ==
                   UCN_I_ADMISSION_ADDRESS_OFFERED &&
               event == UCN_I_ADMISSION_EVENT_DEVICE_COMMIT) {
        expected = UCN_I_ADMISSION_DEVICE_COMMITTED;
    } else {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (event != UCN_I_ADMISSION_EVENT_ABORT &&
        (!transcript_phase_valid(transcript, expected) ||
         !transition_valid(&owner->pending[index].transcript,
                           transcript, event))) {
        owner_unlock(owner);
        return UCN_ERR_REPLAY;
    }
    if (event == UCN_I_ADMISSION_EVENT_ABORT &&
        memcmp(transcript, &owner->pending[index].transcript,
               sizeof(*transcript)) != 0) {
        owner_unlock(owner);
        return UCN_ERR_REPLAY;
    }
    owner->callback_active = 1U;
    owner->callback_event = event;
    owner->callback_key = owner->pending[index].key;
    owner->callback_transcript = *transcript;
    owner->callback_evidence = *evidence;
    owner->callback_now_us = now_us;
    result = next_claim(owner, UCN_I_ADMISSION_CALLBACK_EVENT);
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    owner_unlock(owner);
    result = call_provider(owner, UCN_I_ADMISSION_CALLBACK_EVENT);
    if (owner_lock(owner) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (owner->callback_active == 0U ||
        !handle_matches(owner, handle, &index) || result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result == UCN_OK ? UCN_ERR_STATE : result;
    }
    owner->pending[index].phase = expected;
    if (event != UCN_I_ADMISSION_EVENT_ABORT) {
        owner->pending[index].transcript = owner->callback_transcript;
    }
    clear_callback(owner);
    owner_unlock(owner);
    return UCN_OK;
}

static bool binding_matches(
    const ucn_i_admission_owner_t *owner,
    const ucn_i_admission_slot_t *slot,
    const ucn_i_admission_binding_view_t *binding,
    uint64_t now_us)
{
    uint64_t expected_deadline;
    const ucn_i_admission_transcript_t *transcript = &slot->transcript;

    if (binding == NULL || binding->runtime_instance != owner->runtime_instance ||
        binding->identity_owner_instance != owner->identity_owner_instance ||
        binding->persistence_owner_instance !=
            owner->persistence_owner_instance ||
        binding->realm_id != owner->realm_id ||
        binding->realm_id != transcript->realm_id ||
        binding->address != transcript->proposed_address ||
        binding->binding_generation !=
            transcript->address_binding_generation ||
        binding->authority_generation != transcript->authority_generation ||
        binding->link_generation != transcript->selected_link_generation ||
        memcmp(binding->principal, transcript->joining_device_principal,
               sizeof(binding->principal)) != 0 ||
        binding->record_generation == 0U ||
        binding->foundation_transaction_id == 0U ||
        binding->witness_generation != binding->record_generation ||
        binding->schema_id != UCN_I_IDENTITY_BINDING_SCHEMA_ID ||
        binding->schema_version !=
            UCN_I_IDENTITY_BINDING_RECORD_SCHEMA ||
        !bytes_nonzero(binding->body_digest, sizeof(binding->body_digest)) ||
        ucn_i_deadline_from_duration_us(
            slot->challenge_started_local_us,
            transcript->binding_lease_duration_us,
            &expected_deadline) != UCN_OK ||
        binding->local_deadline_us != expected_deadline ||
        now_us < slot->challenge_started_local_us ||
        now_us >= binding->local_deadline_us || now_us >= slot->deadline_us) {
        return false;
    }
    return true;
}

ucn_result_t ucn_i_admission_finalize(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    const ucn_i_admission_transcript_t *transcript,
    const ucn_i_admission_evidence_t *evidence,
    const ucn_i_admission_binding_view_t *binding,
    uint64_t now_us,
    ucn_i_admitted_view_t *admitted_out)
{
    uint16_t index;
    ucn_result_t result;

    if (owner == NULL || transcript == NULL || !evidence_valid(evidence) ||
        binding == NULL || admitted_out == NULL || now_us == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), transcript,
                             sizeof(*transcript)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), evidence,
                             sizeof(*evidence)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), binding,
                             sizeof(*binding)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), admitted_out,
                             sizeof(*admitted_out)) ||
        ucn_i_ranges_overlap(transcript, sizeof(*transcript), admitted_out,
                             sizeof(*admitted_out)) ||
        ucn_i_ranges_overlap(evidence, sizeof(*evidence), admitted_out,
                             sizeof(*admitted_out)) ||
        ucn_i_ranges_overlap(binding, sizeof(*binding), admitted_out,
                             sizeof(*admitted_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &index) ||
        owner->pending[index].phase != UCN_I_ADMISSION_DEVICE_COMMITTED ||
        !transcript_phase_valid(transcript,
                                UCN_I_ADMISSION_FINAL_DURABLE) ||
        !transition_valid(&owner->pending[index].transcript, transcript,
                          UCN_I_ADMISSION_EVENT_FINAL_DURABLE) ||
        !binding_matches(owner, &owner->pending[index], binding, now_us)) {
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    owner->callback_active = 1U;
    owner->callback_event = UCN_I_ADMISSION_EVENT_FINAL_DURABLE;
    owner->callback_key = owner->pending[index].key;
    owner->callback_transcript = *transcript;
    owner->callback_evidence = *evidence;
    owner->callback_binding = *binding;
    owner->callback_now_us = now_us;
    result = next_claim(owner, UCN_I_ADMISSION_CALLBACK_EVENT);
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    owner_unlock(owner);
    result = call_provider(owner, UCN_I_ADMISSION_CALLBACK_EVENT);
    if (owner_lock(owner) != UCN_OK) {
        return UCN_ERR_STATE;
    }
    if (owner->callback_active == 0U ||
        !handle_matches(owner, handle, &index) || result != UCN_OK ||
        !binding_matches(owner, &owner->pending[index],
                         &owner->callback_binding, now_us)) {
        clear_callback(owner);
        owner_unlock(owner);
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    memset(&owner->pending[index].admitted, 0,
           sizeof(owner->pending[index].admitted));
    owner->pending[index].admitted.binding = owner->callback_binding;
    owner->pending[index].admitted.link = owner->pending[index].key.link;
    owner->pending[index].admitted.session_generation =
        owner->callback_transcript.selected_session_generation;
    owner->pending[index].admitted.admission_generation =
        owner->pending[index].slot_generation;
    owner->pending[index].phase = UCN_I_ADMISSION_FINAL_DURABLE;
    owner->pending[index].transcript = owner->callback_transcript;
    *admitted_out = owner->pending[index].admitted;
    clear_callback(owner);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_pending_get(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    ucn_i_admission_pending_view_t *view_out)
{
    uint16_t index;
    ucn_i_admission_pending_view_t view;
    ucn_result_t result;

    if (owner == NULL || view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&view, 0, sizeof(view));
    view.key = owner->pending[index].key;
    view.challenge_started_local_us =
        owner->pending[index].challenge_started_local_us;
    view.deadline_us = owner->pending[index].deadline_us;
    view.phase = owner->pending[index].phase;
    *view_out = view;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_admitted_get(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    uint64_t now_us,
    ucn_i_admitted_view_t *view_out)
{
    uint16_t index;
    ucn_result_t result;

    if (owner == NULL || view_out == NULL || now_us == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &index) ||
        owner->pending[index].phase != UCN_I_ADMISSION_FINAL_DURABLE) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (now_us >= owner->pending[index].admitted.binding.local_deadline_us) {
        owner_unlock(owner);
        return UCN_ERR_ACCESS;
    }
    *view_out = owner->pending[index].admitted;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_session_requirement_get(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle,
    uint64_t now_us,
    ucn_i_admission_session_requirement_t *requirement_out)
{
    const ucn_i_admission_transcript_t *transcript;
    uint16_t index;
    ucn_result_t result;

    if (owner == NULL || requirement_out == NULL || now_us == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &index) ||
        owner->pending[index].phase != UCN_I_ADMISSION_FINAL_DURABLE ||
        now_us >= owner->pending[index].admitted.binding.local_deadline_us) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    transcript = &owner->pending[index].transcript;
    if (!transcript_phase_valid(transcript,
                                UCN_I_ADMISSION_FINAL_DURABLE) ||
        !bytes_nonzero(transcript->freshness_proof_transcript_hash,
                       UCN_I_ADMISSION_DIGEST_BYTES)) {
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    /* Every validation and failure point is complete before writing the
     * caller-owned result. Avoid a full Requirement automatic object on the
     * MCU task stack. */
    memset(requirement_out, 0, sizeof(*requirement_out));
    requirement_out->admitted = owner->pending[index].admitted;
    requirement_out->transaction_id = transcript->transaction_id;
    requirement_out->absolute_deadline_us =
        owner->pending[index].admitted.binding.local_deadline_us;
    requirement_out->runtime_instance = owner->runtime_instance;
    requirement_out->realm_id = owner->realm_id;
    requirement_out->authority_address = transcript->authority_address;
    requirement_out->authority_binding_generation =
        transcript->authority_binding_generation;
    requirement_out->authority_generation = transcript->authority_generation;
    requirement_out->hop_key_generation =
        transcript->selected_hop_key_generation;
    requirement_out->e2e_key_generation =
        transcript->selected_e2e_key_generation;
    requirement_out->admission_owner_instance = owner->owner_instance;
    requirement_out->identity_owner_instance = owner->identity_owner_instance;
    requirement_out->persistence_owner_instance =
        owner->persistence_owner_instance;
    requirement_out->hop_key_id = transcript->selected_hop_key_id;
    requirement_out->e2e_key_id = transcript->selected_e2e_key_id;
    requirement_out->hop_suite = transcript->selected_hop_suite;
    requirement_out->e2e_mode = transcript->selected_e2e_mode;
    requirement_out->e2e_suite = transcript->selected_e2e_suite;
    memcpy(requirement_out->authority_principal,
           transcript->authority_principal,
           sizeof(requirement_out->authority_principal));
    memcpy(requirement_out->authority_freshness_transcript_hash,
           transcript->freshness_proof_transcript_hash,
           sizeof(requirement_out->authority_freshness_transcript_hash));
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_expire(
    ucn_i_admission_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *expired_out)
{
    uint16_t index;
    uint16_t inspected = 0U;
    uint16_t expired = 0U;
    ucn_result_t result;

    if (owner == NULL || now_us == 0U || budget == 0U ||
        inspected_out == NULL || expired_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), expired_out,
                             sizeof(*expired_out)) ||
        ucn_i_ranges_overlap(inspected_out, sizeof(*inspected_out),
                             expired_out, sizeof(*expired_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    index = owner->maintenance_cursor;
    while (inspected < budget &&
           inspected < UCN_I_ADMISSION_PENDING_COUNT) {
        ucn_i_admission_slot_t *slot = &owner->pending[index];

        inspected++;
        if (slot->occupied != 0U &&
            slot->phase != UCN_I_ADMISSION_FINAL_DURABLE &&
            slot->phase != UCN_I_ADMISSION_ABORTED &&
            now_us >= slot->deadline_us) {
            slot->phase = UCN_I_ADMISSION_ABORTED;
            expired++;
        }
        index = (uint16_t)((index + 1U) % UCN_I_ADMISSION_PENDING_COUNT);
    }
    owner->maintenance_cursor = index;
    *inspected_out = inspected;
    *expired_out = expired;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_retire(
    ucn_i_admission_owner_t *owner,
    ucn_i_admission_handle_t handle)
{
    uint16_t index;
    uint32_t generation;
    ucn_result_t result;

    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &index) ||
        (owner->pending[index].phase != UCN_I_ADMISSION_FINAL_DURABLE &&
         owner->pending[index].phase != UCN_I_ADMISSION_ABORTED)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    generation = owner->pending[index].slot_generation;
    memset(&owner->pending[index], 0, sizeof(owner->pending[index]));
    owner->pending[index].slot_generation = generation;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_owner_destroy(
    ucn_i_admission_owner_t *owner)
{
    uint16_t index;
    ucn_i_lock_ops_t lock;
    ucn_result_t result;

    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_ADMISSION_PENDING_COUNT; ++index) {
        if (owner->pending[index].occupied != 0U) {
            owner_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    lock = owner->state_lock;
    memset(owner, 0, sizeof(*owner));
    lock.leave(lock.context);
    return UCN_OK;
}
