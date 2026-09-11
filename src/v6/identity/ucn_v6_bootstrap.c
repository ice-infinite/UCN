#include "../internal/ucn_v6_bootstrap_private.h"

#include "ucn/v6/ucn_v6_config.h"

#include <string.h>

typedef char ucn_v6_bootstrap_storage_size_check[
    sizeof(ucn_v6_bootstrap_owner_t) <= UCN_V6_BOOTSTRAP_OWNER_STORAGE_BYTES ?
        1 : -1];

static bool callback_result_is_declared(ucn_v6_result_t result)
{
    return result <= UCN_V6_OK && result >= UCN_V6_ERR_CANCELLED;
}

static bool owner_is_valid(const ucn_v6_bootstrap_owner_t *owner)
{
    return owner != NULL && owner->initialized &&
           owner->magic == UCN_V6_BOOTSTRAP_OWNER_MAGIC &&
           owner->schema == UCN_V6_STORAGE_LAYOUT &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->verifier.authorize_event != NULL &&
           owner->callback_gate != NULL &&
           ucn_v6_callback_gate_violation_count(owner->callback_gate) !=
               UINT64_MAX &&
           owner->canary == UCN_V6_BOOTSTRAP_OWNER_CANARY;
}

static ucn_v6_result_t callback_scope_finish(
    ucn_v6_callback_gate_t *gate,
    const void *owner,
    uint64_t violations_before,
    ucn_v6_result_t result)
{
    bool clean;

    if (gate == NULL || owner == NULL || !gate->initialized ||
        gate->lock == NULL || gate->unlock == NULL ||
        violations_before == UINT64_MAX) {
        return UCN_V6_ERR_STATE;
    }
    gate->lock(gate->context);
    clean = gate->active && gate->active_owner == owner &&
            gate->violation_count == violations_before &&
            gate->violation_count != UINT64_MAX;
    if (gate->active && gate->active_owner == owner) {
        gate->active = false;
        gate->active_owner = NULL;
    }
    gate->unlock(gate->context);
    return clean && callback_result_is_declared(result) ? result :
                                                         UCN_V6_ERR_STATE;
}

static bool callback_reentry_is_blocked(
    const ucn_v6_bootstrap_owner_t *owner)
{
    ucn_v6_result_t result;

    if (!ucn_v6_callback_gate_is_active(owner->callback_gate)) {
        return false;
    }
    /* Record the violation in the shared Gate so an outer verifier call
     * cannot hide a nested API attempt by discarding its return code. */
    result = ucn_v6_callback_gate_try_enter(owner->callback_gate, owner);
    if (result == UCN_V6_OK &&
        ucn_v6_callback_gate_leave(owner->callback_gate, owner) !=
            UCN_V6_OK) {
        return true;
    }
    return true;
}

static bool principal_equal(
    const ucn_v6_principal_t *left,
    const ucn_v6_principal_t *right)
{
    return memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
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
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool evidence_is_valid(
    const ucn_v6_bootstrap_evidence_t *evidence)
{
    return evidence != NULL && evidence->length != 0U &&
           evidence->length <= UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES &&
           bytes_nonzero(evidence->bytes, evidence->length) &&
           bytes_zero(evidence->bytes + evidence->length,
                      UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES -
                          evidence->length);
}

static bool flow_is_valid(ucn_v6_bootstrap_flow_t flow)
{
    return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ||
           flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH;
}

static bool key_is_valid(const ucn_v6_bootstrap_key_t *key)
{
    return key != NULL && key->ingress_link_id != 0U &&
           key->ingress_link_id <= UCN_V6_LINK_ID_MAX &&
           key->ingress_link_generation != 0U &&
           key->ingress_link_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           key->local_peer_discriminator != 0U &&
           ucn_v6_principal_is_valid(&key->identity_digest) &&
           key->transaction_id != 0U &&
           key->transaction_id <= UCN_V6_SERIAL64_ROTATION_THRESHOLD;
}

static bool key_equal(
    const ucn_v6_bootstrap_key_t *left,
    const ucn_v6_bootstrap_key_t *right)
{
    return left->ingress_link_id == right->ingress_link_id &&
           left->ingress_link_generation == right->ingress_link_generation &&
           left->local_peer_discriminator == right->local_peer_discriminator &&
           left->transaction_id == right->transaction_id &&
           principal_equal(&left->identity_digest, &right->identity_digest);
}

static bool transcript_is_valid(
    const ucn_v6_bootstrap_transcript_t *transcript)
{
    return transcript != NULL &&
           transcript->protocol_version == UCN_V6_PROTOCOL_VERSION &&
           transcript->bootstrap_header_contract != 0U &&
           flow_is_valid(transcript->flow) &&
           ucn_v6_principal_is_valid(
               &transcript->joining_device_principal) &&
           ucn_v6_principal_is_valid(
               &transcript->joining_device_identity_digest) &&
           ucn_v6_principal_is_valid(&transcript->authority_principal) &&
           transcript->authority_generation != 0U &&
           transcript->authority_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->device_nonce != 0U &&
           transcript->authority_nonce != 0U &&
           transcript->transaction_id != 0U &&
           transcript->transaction_id <= UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           transcript->lease_freshness_challenge_nonce != 0U &&
           transcript->realm_id != 0U && transcript->realm_id != UINT32_MAX &&
           transcript->proposed_address != 0U &&
           transcript->proposed_address != UINT32_MAX &&
           transcript->address_binding_generation != 0U &&
           transcript->address_binding_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->authority_address != 0U &&
           transcript->authority_address != UINT32_MAX &&
           transcript->authority_binding_generation != 0U &&
           transcript->authority_binding_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->selected_link_instance_id != 0U &&
           transcript->selected_link_instance_id <= UCN_V6_LINK_ID_MAX &&
           bytes_nonzero(transcript->binding_lease_id,
                         sizeof(transcript->binding_lease_id)) &&
           transcript->binding_lease_duration_us != 0U &&
           transcript->authority_lease_sequence != 0U &&
           transcript->authority_lease_sequence <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           transcript->authority_lease_duration_us != 0U &&
           transcript->freshness_max_remaining_lease_us != 0U &&
           transcript->freshness_max_remaining_lease_us <=
               transcript->authority_lease_duration_us &&
           bytes_nonzero(transcript->durable_fence_token,
                         sizeof(transcript->durable_fence_token)) &&
           bytes_nonzero(transcript->allocation_high_water_digest,
                         sizeof(transcript->allocation_high_water_digest)) &&
           bytes_nonzero(transcript->quorum_config_digest,
                         sizeof(transcript->quorum_config_digest)) &&
           bytes_nonzero(transcript->signer_set_digest,
                         sizeof(transcript->signer_set_digest)) &&
           bytes_nonzero(transcript->threshold_proof_digest,
                         sizeof(transcript->threshold_proof_digest)) &&
           bytes_nonzero(transcript->freshness_proof_transcript_hash,
                         sizeof(transcript->freshness_proof_transcript_hash)) &&
           transcript->authority_signer_count != 0U &&
           transcript->authority_quorum_threshold != 0U &&
           transcript->authority_quorum_threshold <=
               transcript->authority_signer_count &&
           (transcript->binding_mode == UCN_V6_ADDRESS_STATIC ||
            transcript->binding_mode == UCN_V6_ADDRESS_LEASED ||
            transcript->binding_mode == UCN_V6_ADDRESS_SELF_PROPOSED) &&
           transcript->selected_hop_suite != 0U &&
           transcript->selected_hop_key_id != 0U &&
           transcript->selected_hop_key_generation != 0U &&
           transcript->selected_hop_key_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           (transcript->selected_e2e_mode == UCN_V6_E2E_AUTH_ONLY ||
            transcript->selected_e2e_mode == UCN_V6_E2E_AEAD) &&
           transcript->selected_e2e_suite != 0U &&
           transcript->selected_e2e_key_id != 0U &&
           transcript->selected_e2e_key_generation != 0U &&
           transcript->selected_e2e_key_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->selected_session_generation != 0U &&
           transcript->selected_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->selected_link_instance_generation != 0U &&
           transcript->selected_link_instance_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_nonzero(transcript->prior_messages_hash,
                         sizeof(transcript->prior_messages_hash));
}

static bool optional_principal_is_valid(
    const ucn_v6_principal_t *principal)
{
    return !bytes_nonzero(principal->bytes, sizeof(principal->bytes)) ||
           ucn_v6_principal_is_valid(principal);
}

static bool transcript_common_is_valid(
    const ucn_v6_bootstrap_transcript_t *transcript)
{
    return transcript != NULL &&
           transcript->protocol_version == UCN_V6_PROTOCOL_VERSION &&
           transcript->bootstrap_header_contract != 0U &&
           flow_is_valid(transcript->flow) &&
           optional_principal_is_valid(
               &transcript->joining_device_principal) &&
           ucn_v6_principal_is_valid(
               &transcript->joining_device_identity_digest) &&
           optional_principal_is_valid(&transcript->authority_principal) &&
           transcript->device_nonce != 0U &&
           transcript->transaction_id != 0U &&
           transcript->transaction_id <= UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           transcript->lease_freshness_challenge_nonce != 0U &&
           transcript->selected_link_instance_id != 0U &&
           transcript->selected_link_instance_id <= UCN_V6_LINK_ID_MAX &&
           transcript->selected_link_instance_generation != 0U &&
           transcript->selected_link_instance_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_nonzero(transcript->prior_messages_hash,
                         sizeof(transcript->prior_messages_hash));
}

static bool transcript_authority_fields_are_valid(
    const ucn_v6_bootstrap_transcript_t *transcript)
{
    return ucn_v6_principal_is_valid(&transcript->authority_principal) &&
           transcript->authority_generation != 0U &&
           transcript->authority_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->authority_nonce != 0U &&
           transcript->realm_id != 0U && transcript->realm_id != UINT32_MAX &&
           transcript->authority_address != 0U &&
           transcript->authority_address != UINT32_MAX &&
           transcript->authority_binding_generation != 0U &&
           transcript->authority_binding_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->authority_lease_sequence != 0U &&
           transcript->authority_lease_sequence <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           transcript->authority_lease_duration_us != 0U &&
           transcript->freshness_max_remaining_lease_us != 0U &&
           transcript->freshness_max_remaining_lease_us <=
               transcript->authority_lease_duration_us &&
           bytes_nonzero(transcript->durable_fence_token,
                         sizeof(transcript->durable_fence_token)) &&
           bytes_nonzero(transcript->allocation_high_water_digest,
                         sizeof(transcript->allocation_high_water_digest)) &&
           bytes_nonzero(transcript->quorum_config_digest,
                         sizeof(transcript->quorum_config_digest)) &&
           bytes_nonzero(transcript->signer_set_digest,
                         sizeof(transcript->signer_set_digest)) &&
           bytes_nonzero(transcript->threshold_proof_digest,
                         sizeof(transcript->threshold_proof_digest)) &&
           bytes_nonzero(transcript->freshness_proof_transcript_hash,
                         sizeof(transcript->freshness_proof_transcript_hash)) &&
           transcript->authority_signer_count != 0U &&
           transcript->authority_quorum_threshold != 0U &&
           transcript->authority_quorum_threshold <=
               transcript->authority_signer_count;
}

static bool transcript_device_fields_are_valid(
    const ucn_v6_bootstrap_transcript_t *transcript)
{
    return ucn_v6_principal_is_valid(
               &transcript->joining_device_principal) &&
           transcript->selected_hop_suite != 0U &&
           transcript->selected_hop_key_id != 0U &&
           transcript->selected_hop_key_generation != 0U &&
           transcript->selected_hop_key_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           (transcript->selected_e2e_mode == UCN_V6_E2E_AUTH_ONLY ||
            transcript->selected_e2e_mode == UCN_V6_E2E_AEAD) &&
           transcript->selected_e2e_suite != 0U &&
           transcript->selected_e2e_key_id != 0U &&
           transcript->selected_e2e_key_generation != 0U &&
           transcript->selected_e2e_key_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           transcript->selected_session_generation != 0U &&
           transcript->selected_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool transcript_address_fields_are_valid(
    const ucn_v6_bootstrap_transcript_t *transcript)
{
    return transcript->proposed_address != 0U &&
           transcript->proposed_address != UINT32_MAX &&
           transcript->address_binding_generation != 0U &&
           transcript->address_binding_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_nonzero(transcript->binding_lease_id,
                         sizeof(transcript->binding_lease_id)) &&
           transcript->binding_lease_duration_us != 0U &&
           (transcript->binding_mode == UCN_V6_ADDRESS_STATIC ||
            transcript->binding_mode == UCN_V6_ADDRESS_LEASED ||
            transcript->binding_mode == UCN_V6_ADDRESS_SELF_PROPOSED);
}

static bool transcript_is_valid_for_phase(
    const ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_phase_t phase)
{
    if (!transcript_common_is_valid(transcript)) return false;
    if (phase >= UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED &&
        !transcript_authority_fields_are_valid(transcript)) {
        return false;
    }
    if (phase >= UCN_V6_BOOTSTRAP_DEVICE_VERIFIED &&
        !transcript_device_fields_are_valid(transcript)) {
        return false;
    }
    if (transcript->flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
        phase >= UCN_V6_BOOTSTRAP_ADDRESS_OFFERED &&
        !transcript_address_fields_are_valid(transcript)) {
        return false;
    }
    if (phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE) {
        return transcript_is_valid(transcript);
    }
    return phase >= UCN_V6_BOOTSTRAP_COOKIE_VERIFIED &&
           phase <= UCN_V6_BOOTSTRAP_DEVICE_COMMITTED;
}

static bool scalar_extends_u64(uint64_t previous, uint64_t next)
{
    return previous == 0U || previous == next;
}

static bool scalar_extends_u32(uint32_t previous, uint32_t next)
{
    return previous == 0U || previous == next;
}

static bool scalar_extends_u16(uint16_t previous, uint16_t next)
{
    return previous == 0U || previous == next;
}

static bool scalar_extends_u8(uint8_t previous, uint8_t next)
{
    return previous == 0U || previous == next;
}

static bool bytes_extend(const uint8_t *previous, const uint8_t *next,
                         size_t length)
{
    return !bytes_nonzero(previous, length) ||
           memcmp(previous, next, length) == 0;
}

static bool principal_extends(const ucn_v6_principal_t *previous,
                              const ucn_v6_principal_t *next)
{
    return bytes_extend(previous->bytes, next->bytes,
                        sizeof(previous->bytes));
}

/* Every semantic field may be introduced exactly once as the transcript is
 * assembled in causal message order. Once nonzero it is immutable. The
 * rolling prior_messages_hash is intentionally excluded: each verified
 * event replaces it with the hash of its newly extended prefix.
 * 每个语义字段只能按消息因果顺序从零补齐一次，非零后永久不可修改。滚动
 * prior_messages_hash 有意不参与此前缀比较：每个已验证事件都用扩展后前缀
 * 的 Hash 替换它。 */
static bool transcript_extends(
    const ucn_v6_bootstrap_transcript_t *previous,
    const ucn_v6_bootstrap_transcript_t *next)
{
    return previous->protocol_version == next->protocol_version &&
           previous->bootstrap_header_contract ==
               next->bootstrap_header_contract &&
           previous->flow == next->flow &&
           principal_extends(&previous->joining_device_principal,
                             &next->joining_device_principal) &&
           principal_extends(&previous->joining_device_identity_digest,
                             &next->joining_device_identity_digest) &&
           principal_extends(&previous->authority_principal,
                             &next->authority_principal) &&
           scalar_extends_u32(previous->authority_generation,
                              next->authority_generation) &&
           scalar_extends_u64(previous->device_nonce, next->device_nonce) &&
           scalar_extends_u64(previous->authority_nonce,
                              next->authority_nonce) &&
           scalar_extends_u64(previous->transaction_id,
                              next->transaction_id) &&
           scalar_extends_u64(previous->lease_freshness_challenge_nonce,
                              next->lease_freshness_challenge_nonce) &&
           scalar_extends_u32(previous->realm_id, next->realm_id) &&
           scalar_extends_u32(previous->proposed_address,
                              next->proposed_address) &&
           scalar_extends_u32(previous->address_binding_generation,
                              next->address_binding_generation) &&
           scalar_extends_u32(previous->authority_address,
                              next->authority_address) &&
           scalar_extends_u32(previous->authority_binding_generation,
                              next->authority_binding_generation) &&
           scalar_extends_u16(previous->selected_link_instance_id,
                              next->selected_link_instance_id) &&
           bytes_extend(previous->binding_lease_id, next->binding_lease_id,
                        sizeof(previous->binding_lease_id)) &&
           scalar_extends_u64(previous->binding_lease_duration_us,
                              next->binding_lease_duration_us) &&
           scalar_extends_u64(previous->authority_lease_sequence,
                              next->authority_lease_sequence) &&
           scalar_extends_u64(previous->authority_lease_duration_us,
                              next->authority_lease_duration_us) &&
           scalar_extends_u64(previous->freshness_max_remaining_lease_us,
                              next->freshness_max_remaining_lease_us) &&
           bytes_extend(previous->durable_fence_token,
                        next->durable_fence_token,
                        sizeof(previous->durable_fence_token)) &&
           bytes_extend(previous->allocation_high_water_digest,
                        next->allocation_high_water_digest,
                        sizeof(previous->allocation_high_water_digest)) &&
           bytes_extend(previous->quorum_config_digest,
                        next->quorum_config_digest,
                        sizeof(previous->quorum_config_digest)) &&
           bytes_extend(previous->signer_set_digest,
                        next->signer_set_digest,
                        sizeof(previous->signer_set_digest)) &&
           bytes_extend(previous->threshold_proof_digest,
                        next->threshold_proof_digest,
                        sizeof(previous->threshold_proof_digest)) &&
           bytes_extend(previous->freshness_proof_transcript_hash,
                        next->freshness_proof_transcript_hash,
                        sizeof(previous->freshness_proof_transcript_hash)) &&
           scalar_extends_u16(previous->authority_signer_count,
                              next->authority_signer_count) &&
           scalar_extends_u16(previous->authority_quorum_threshold,
                              next->authority_quorum_threshold) &&
           scalar_extends_u8(previous->binding_mode, next->binding_mode) &&
           scalar_extends_u8(previous->selected_hop_suite,
                             next->selected_hop_suite) &&
           scalar_extends_u16(previous->selected_hop_key_id,
                              next->selected_hop_key_id) &&
           scalar_extends_u32(previous->selected_hop_key_generation,
                              next->selected_hop_key_generation) &&
           scalar_extends_u8(previous->selected_e2e_mode,
                             next->selected_e2e_mode) &&
           scalar_extends_u8(previous->selected_e2e_suite,
                             next->selected_e2e_suite) &&
           scalar_extends_u16(previous->selected_e2e_key_id,
                              next->selected_e2e_key_id) &&
           scalar_extends_u32(previous->selected_e2e_key_generation,
                              next->selected_e2e_key_generation) &&
           scalar_extends_u32(previous->selected_session_generation,
                              next->selected_session_generation) &&
           scalar_extends_u32(previous->selected_link_instance_generation,
                              next->selected_link_instance_generation);
}

static void bootstrap_put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void bootstrap_put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void bootstrap_put_u64(uint8_t *output, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static uint16_t bootstrap_get_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static uint32_t bootstrap_get_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
           ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | input[3];
}

static uint64_t bootstrap_get_u64(const uint8_t *input)
{
    uint64_t value = 0U;
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | input[index];
    }
    return value;
}

static size_t bootstrap_transcript_write(
    const ucn_v6_bootstrap_transcript_t *transcript, uint8_t *encoded)
{
    size_t offset = 0U;
#define WRITE_BYTES(field_)                                                   \
    do {                                                                      \
        memcpy(&encoded[offset], (field_), sizeof(field_));                  \
        offset += sizeof(field_);                                             \
    } while (0)
    encoded[offset++] = transcript->protocol_version;
    bootstrap_put_u16(&encoded[offset], transcript->bootstrap_header_contract);
    offset += 2U;
    encoded[offset++] = 1U;
    encoded[offset++] = (uint8_t)transcript->flow;
    WRITE_BYTES(transcript->joining_device_principal.bytes);
    WRITE_BYTES(transcript->joining_device_identity_digest.bytes);
    WRITE_BYTES(transcript->authority_principal.bytes);
    bootstrap_put_u32(&encoded[offset], transcript->authority_generation);
    offset += 4U;
    bootstrap_put_u64(&encoded[offset], transcript->device_nonce);
    offset += 8U;
    bootstrap_put_u64(&encoded[offset], transcript->authority_nonce);
    offset += 8U;
    bootstrap_put_u64(&encoded[offset], transcript->transaction_id);
    offset += 8U;
    bootstrap_put_u64(&encoded[offset],
                      transcript->lease_freshness_challenge_nonce);
    offset += 8U;
    bootstrap_put_u32(&encoded[offset], transcript->realm_id);
    offset += 4U;
    bootstrap_put_u32(&encoded[offset], transcript->proposed_address);
    offset += 4U;
    bootstrap_put_u32(&encoded[offset],
                      transcript->address_binding_generation);
    offset += 4U;
    bootstrap_put_u32(&encoded[offset], transcript->authority_address);
    offset += 4U;
    bootstrap_put_u32(&encoded[offset],
                      transcript->authority_binding_generation);
    offset += 4U;
    bootstrap_put_u16(&encoded[offset], transcript->selected_link_instance_id);
    offset += 2U;
    WRITE_BYTES(transcript->binding_lease_id);
    bootstrap_put_u64(&encoded[offset], transcript->binding_lease_duration_us);
    offset += 8U;
    bootstrap_put_u64(&encoded[offset], transcript->authority_lease_sequence);
    offset += 8U;
    bootstrap_put_u64(&encoded[offset], transcript->authority_lease_duration_us);
    offset += 8U;
    bootstrap_put_u64(&encoded[offset],
                      transcript->freshness_max_remaining_lease_us);
    offset += 8U;
    WRITE_BYTES(transcript->durable_fence_token);
    WRITE_BYTES(transcript->allocation_high_water_digest);
    WRITE_BYTES(transcript->quorum_config_digest);
    WRITE_BYTES(transcript->signer_set_digest);
    WRITE_BYTES(transcript->threshold_proof_digest);
    WRITE_BYTES(transcript->freshness_proof_transcript_hash);
    bootstrap_put_u16(&encoded[offset], transcript->authority_signer_count);
    offset += 2U;
    bootstrap_put_u16(&encoded[offset], transcript->authority_quorum_threshold);
    offset += 2U;
    encoded[offset++] = transcript->binding_mode;
    encoded[offset++] = transcript->selected_hop_suite;
    bootstrap_put_u16(&encoded[offset], transcript->selected_hop_key_id);
    offset += 2U;
    bootstrap_put_u32(&encoded[offset],
                      transcript->selected_hop_key_generation);
    offset += 4U;
    encoded[offset++] = transcript->selected_e2e_mode;
    encoded[offset++] = transcript->selected_e2e_suite;
    bootstrap_put_u16(&encoded[offset], transcript->selected_e2e_key_id);
    offset += 2U;
    bootstrap_put_u32(&encoded[offset],
                      transcript->selected_e2e_key_generation);
    offset += 4U;
    bootstrap_put_u32(&encoded[offset],
                      transcript->selected_session_generation);
    offset += 4U;
    bootstrap_put_u32(&encoded[offset],
                      transcript->selected_link_instance_generation);
    offset += 4U;
    WRITE_BYTES(transcript->prior_messages_hash);
#undef WRITE_BYTES
    return offset;
}

static size_t bootstrap_transcript_read(
    const uint8_t *input, ucn_v6_bootstrap_transcript_t *transcript)
{
    size_t offset = 0U;
#define READ_BYTES(field_)                                                    \
    do {                                                                      \
        memcpy((field_), &input[offset], sizeof(field_));                    \
        offset += sizeof(field_);                                             \
    } while (0)
    transcript->protocol_version = input[offset++];
    transcript->bootstrap_header_contract = bootstrap_get_u16(&input[offset]);
    offset += 2U;
    ++offset;
    transcript->flow = (ucn_v6_bootstrap_flow_t)input[offset++];
    READ_BYTES(transcript->joining_device_principal.bytes);
    READ_BYTES(transcript->joining_device_identity_digest.bytes);
    READ_BYTES(transcript->authority_principal.bytes);
    transcript->authority_generation = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->device_nonce = bootstrap_get_u64(&input[offset]);
    offset += 8U;
    transcript->authority_nonce = bootstrap_get_u64(&input[offset]);
    offset += 8U;
    transcript->transaction_id = bootstrap_get_u64(&input[offset]);
    offset += 8U;
    transcript->lease_freshness_challenge_nonce =
        bootstrap_get_u64(&input[offset]);
    offset += 8U;
    transcript->realm_id = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->proposed_address = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->address_binding_generation = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->authority_address = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->authority_binding_generation = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->selected_link_instance_id = bootstrap_get_u16(&input[offset]);
    offset += 2U;
    READ_BYTES(transcript->binding_lease_id);
    transcript->binding_lease_duration_us = bootstrap_get_u64(&input[offset]);
    offset += 8U;
    transcript->authority_lease_sequence = bootstrap_get_u64(&input[offset]);
    offset += 8U;
    transcript->authority_lease_duration_us = bootstrap_get_u64(&input[offset]);
    offset += 8U;
    transcript->freshness_max_remaining_lease_us =
        bootstrap_get_u64(&input[offset]);
    offset += 8U;
    READ_BYTES(transcript->durable_fence_token);
    READ_BYTES(transcript->allocation_high_water_digest);
    READ_BYTES(transcript->quorum_config_digest);
    READ_BYTES(transcript->signer_set_digest);
    READ_BYTES(transcript->threshold_proof_digest);
    READ_BYTES(transcript->freshness_proof_transcript_hash);
    transcript->authority_signer_count = bootstrap_get_u16(&input[offset]);
    offset += 2U;
    transcript->authority_quorum_threshold = bootstrap_get_u16(&input[offset]);
    offset += 2U;
    transcript->binding_mode = input[offset++];
    transcript->selected_hop_suite = input[offset++];
    transcript->selected_hop_key_id = bootstrap_get_u16(&input[offset]);
    offset += 2U;
    transcript->selected_hop_key_generation = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->selected_e2e_mode = input[offset++];
    transcript->selected_e2e_suite = input[offset++];
    transcript->selected_e2e_key_id = bootstrap_get_u16(&input[offset]);
    offset += 2U;
    transcript->selected_e2e_key_generation = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->selected_session_generation = bootstrap_get_u32(&input[offset]);
    offset += 4U;
    transcript->selected_link_instance_generation =
        bootstrap_get_u32(&input[offset]);
    offset += 4U;
    READ_BYTES(transcript->prior_messages_hash);
#undef READ_BYTES
    return offset;
}

ucn_v6_result_t ucn_v6_bootstrap_transcript_encode(
    const ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_phase_t expected_phase,
    uint8_t output[UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES])
{
    uint8_t encoded[UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES];
    if (output == NULL ||
        !transcript_is_valid_for_phase(transcript, expected_phase) ||
        ucn_v6_memory_ranges_overlap(
            transcript, sizeof(*transcript), output, sizeof(encoded))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    if (bootstrap_transcript_write(transcript, encoded) != sizeof(encoded)) {
        return UCN_V6_ERR_STATE;
    }
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_transcript_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_phase_t expected_phase,
    ucn_v6_bootstrap_transcript_t *transcript)
{
    ucn_v6_bootstrap_transcript_t decoded;
    if (input == NULL || transcript == NULL ||
        input_length != UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES ||
        input[3] != 1U ||
        ucn_v6_memory_ranges_overlap(input, input_length, transcript,
                                     sizeof(*transcript))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&decoded, 0, sizeof(decoded));
    if (bootstrap_transcript_read(input, &decoded) != input_length ||
        !transcript_is_valid_for_phase(&decoded, expected_phase)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *transcript = decoded;
    return UCN_V6_OK;
}

static bool bootstrap_hello_is_valid(const ucn_v6_bootstrap_hello_t *hello)
{
    return hello != NULL && flow_is_valid(hello->flow) &&
           ucn_v6_principal_is_valid(&hello->identity_digest) &&
           hello->device_nonce != 0U && hello->transaction_id != 0U &&
           hello->transaction_id <= UCN_V6_SERIAL64_ROTATION_THRESHOLD;
}

static bool bootstrap_cookie_challenge_is_valid(
    const ucn_v6_bootstrap_cookie_challenge_t *challenge)
{
    return challenge != NULL && flow_is_valid(challenge->flow) &&
           challenge->transaction_id != 0U &&
           challenge->transaction_id <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           challenge->cookie_time_bucket != 0U &&
           challenge->cookie_time_bucket <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           challenge->cookie_length != 0U &&
           challenge->cookie_length <= UCN_V6_BOOTSTRAP_COOKIE_MAX_BYTES &&
           bytes_nonzero(challenge->cookie, challenge->cookie_length) &&
           bytes_zero(challenge->cookie + challenge->cookie_length,
                      UCN_V6_BOOTSTRAP_COOKIE_MAX_BYTES -
                          challenge->cookie_length);
}

static bool bootstrap_hello_cookie_is_valid(
    const ucn_v6_bootstrap_hello_cookie_t *hello_cookie)
{
    return hello_cookie != NULL && flow_is_valid(hello_cookie->flow) &&
           ucn_v6_principal_is_valid(&hello_cookie->identity_digest) &&
           hello_cookie->device_nonce != 0U &&
           hello_cookie->transaction_id != 0U &&
           hello_cookie->transaction_id <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
           hello_cookie->lease_freshness_challenge_nonce != 0U &&
           hello_cookie->selected_link_instance_id != 0U &&
           hello_cookie->selected_link_instance_id <= UCN_V6_LINK_ID_MAX &&
           hello_cookie->selected_link_instance_generation != 0U &&
           hello_cookie->selected_link_instance_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           bytes_nonzero(hello_cookie->prior_messages_hash,
                         sizeof(hello_cookie->prior_messages_hash)) &&
           evidence_is_valid(&hello_cookie->cookie_evidence);
}

ucn_v6_result_t ucn_v6_bootstrap_hello_encode(
    const ucn_v6_bootstrap_hello_t *hello,
    uint8_t output[UCN_V6_BOOTSTRAP_HELLO_BYTES])
{
    uint8_t encoded[UCN_V6_BOOTSTRAP_HELLO_BYTES];
    if (!bootstrap_hello_is_valid(hello) || output == NULL ||
        ucn_v6_memory_ranges_overlap(hello, sizeof(*hello), output,
                                     sizeof(encoded))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = 1U;
    encoded[1] = (uint8_t)hello->flow;
    memcpy(&encoded[4], hello->identity_digest.bytes, 16U);
    bootstrap_put_u64(&encoded[20], hello->device_nonce);
    bootstrap_put_u64(&encoded[28], hello->transaction_id);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_hello_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_hello_t *hello)
{
    ucn_v6_bootstrap_hello_t decoded;
    if (input == NULL || hello == NULL ||
        input_length != UCN_V6_BOOTSTRAP_HELLO_BYTES ||
        ucn_v6_memory_ranges_overlap(input, input_length, hello,
                                     sizeof(*hello))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (input[0] != 1U || input[2] != 0U || input[3] != 0U ||
        !bytes_zero(&input[36], 4U)) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.flow = (ucn_v6_bootstrap_flow_t)input[1];
    memcpy(decoded.identity_digest.bytes, &input[4], 16U);
    decoded.device_nonce = bootstrap_get_u64(&input[20]);
    decoded.transaction_id = bootstrap_get_u64(&input[28]);
    if (!bootstrap_hello_is_valid(&decoded)) return UCN_V6_ERR_MALFORMED;
    *hello = decoded;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_cookie_challenge_encode(
    const ucn_v6_bootstrap_cookie_challenge_t *challenge,
    uint8_t output[UCN_V6_BOOTSTRAP_COOKIE_CHALLENGE_BYTES])
{
    uint8_t encoded[UCN_V6_BOOTSTRAP_COOKIE_CHALLENGE_BYTES];
    if (!bootstrap_cookie_challenge_is_valid(challenge) || output == NULL ||
        ucn_v6_memory_ranges_overlap(challenge, sizeof(*challenge), output,
                                     sizeof(encoded))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = 1U;
    encoded[1] = (uint8_t)challenge->flow;
    encoded[2] = challenge->cookie_length;
    bootstrap_put_u64(&encoded[4], challenge->transaction_id);
    bootstrap_put_u32(&encoded[12], challenge->cookie_time_bucket);
    memcpy(&encoded[16], challenge->cookie, challenge->cookie_length);
    memcpy(output, encoded, sizeof(encoded));
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_cookie_challenge_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_cookie_challenge_t *challenge)
{
    ucn_v6_bootstrap_cookie_challenge_t decoded;
    if (input == NULL || challenge == NULL ||
        input_length != UCN_V6_BOOTSTRAP_COOKIE_CHALLENGE_BYTES ||
        ucn_v6_memory_ranges_overlap(input, input_length, challenge,
                                     sizeof(*challenge))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (input[0] != 1U || input[3] != 0U ||
        !bytes_zero(&input[32], 8U)) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.flow = (ucn_v6_bootstrap_flow_t)input[1];
    decoded.cookie_length = input[2];
    decoded.transaction_id = bootstrap_get_u64(&input[4]);
    decoded.cookie_time_bucket = bootstrap_get_u32(&input[12]);
    if (decoded.cookie_length <= UCN_V6_BOOTSTRAP_COOKIE_MAX_BYTES) {
        memcpy(decoded.cookie, &input[16], decoded.cookie_length);
        if (!bytes_zero(&input[16 + decoded.cookie_length],
                        UCN_V6_BOOTSTRAP_COOKIE_MAX_BYTES -
                            decoded.cookie_length)) {
            return UCN_V6_ERR_MALFORMED;
        }
    }
    if (!bootstrap_cookie_challenge_is_valid(&decoded)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *challenge = decoded;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_hello_cookie_encode(
    const ucn_v6_bootstrap_hello_cookie_t *hello_cookie,
    uint8_t *output, size_t output_capacity, size_t *output_length)
{
    uint8_t encoded[UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES +
                    UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES];
    size_t length;
    if (!bootstrap_hello_cookie_is_valid(hello_cookie) || output == NULL ||
        output_length == NULL ||
        output_capacity < UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES +
                              hello_cookie->cookie_evidence.length ||
        ucn_v6_memory_ranges_overlap(hello_cookie, sizeof(*hello_cookie),
                                     output, output_capacity) ||
        ucn_v6_memory_ranges_overlap(output, output_capacity, output_length,
                                     sizeof(*output_length)) ||
        ucn_v6_memory_ranges_overlap(hello_cookie, sizeof(*hello_cookie),
                                     output_length,
                                     sizeof(*output_length))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = 1U;
    encoded[1] = (uint8_t)hello_cookie->flow;
    encoded[2] = (uint8_t)hello_cookie->cookie_evidence.length;
    memcpy(&encoded[4], hello_cookie->identity_digest.bytes, 16U);
    bootstrap_put_u64(&encoded[20], hello_cookie->device_nonce);
    bootstrap_put_u64(&encoded[28], hello_cookie->transaction_id);
    bootstrap_put_u64(&encoded[36],
                      hello_cookie->lease_freshness_challenge_nonce);
    bootstrap_put_u16(&encoded[44],
                      hello_cookie->selected_link_instance_id);
    bootstrap_put_u32(&encoded[46],
                      hello_cookie->selected_link_instance_generation);
    memcpy(&encoded[50], hello_cookie->prior_messages_hash, 32U);
    memcpy(&encoded[UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES],
           hello_cookie->cookie_evidence.bytes,
           hello_cookie->cookie_evidence.length);
    length = UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES +
             hello_cookie->cookie_evidence.length;
    memcpy(output, encoded, length);
    *output_length = length;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_hello_cookie_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_hello_cookie_t *hello_cookie)
{
    ucn_v6_bootstrap_hello_cookie_t decoded;
    uint8_t evidence_length;
    if (input == NULL || hello_cookie == NULL ||
        input_length < UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES + 1U ||
        input_length > UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES +
                           UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES ||
        ucn_v6_memory_ranges_overlap(input, input_length, hello_cookie,
                                     sizeof(*hello_cookie))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    evidence_length = input[2];
    if (input[0] != 1U || input[3] != 0U || evidence_length == 0U ||
        input_length != UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES +
                            evidence_length) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.flow = (ucn_v6_bootstrap_flow_t)input[1];
    decoded.cookie_evidence.length = evidence_length;
    memcpy(decoded.identity_digest.bytes, &input[4], 16U);
    decoded.device_nonce = bootstrap_get_u64(&input[20]);
    decoded.transaction_id = bootstrap_get_u64(&input[28]);
    decoded.lease_freshness_challenge_nonce =
        bootstrap_get_u64(&input[36]);
    decoded.selected_link_instance_id = bootstrap_get_u16(&input[44]);
    decoded.selected_link_instance_generation =
        bootstrap_get_u32(&input[46]);
    memcpy(decoded.prior_messages_hash, &input[50], 32U);
    memcpy(decoded.cookie_evidence.bytes,
           &input[UCN_V6_BOOTSTRAP_HELLO_COOKIE_FIXED_BYTES],
           evidence_length);
    if (!bootstrap_hello_cookie_is_valid(&decoded)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *hello_cookie = decoded;
    return UCN_V6_OK;
}

static bool bootstrap_event_matches_phase(
    ucn_v6_bootstrap_event_t event, ucn_v6_bootstrap_flow_t flow,
    ucn_v6_bootstrap_phase_t phase)
{
    switch (event) {
    case UCN_V6_BOOTSTRAP_EVENT_COOKIE:
        return phase == UCN_V6_BOOTSTRAP_COOKIE_VERIFIED;
    case UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF:
        return phase == UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED;
    case UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF:
        return phase == UCN_V6_BOOTSTRAP_DEVICE_VERIFIED;
    case UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER:
        return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
               phase == UCN_V6_BOOTSTRAP_ADDRESS_OFFERED;
    case UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT:
        return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
               phase == UCN_V6_BOOTSTRAP_DEVICE_COMMITTED;
    case UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE:
        return phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE;
    case UCN_V6_BOOTSTRAP_EVENT_ABORT:
        return phase >= UCN_V6_BOOTSTRAP_COOKIE_VERIFIED &&
               phase <= UCN_V6_BOOTSTRAP_DEVICE_COMMITTED;
    default:
        return false;
    }
}

uint16_t ucn_v6_bootstrap_event_opcode(ucn_v6_bootstrap_event_t event)
{
    switch (event) {
    case UCN_V6_BOOTSTRAP_EVENT_COOKIE:
        return UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_HELLO_COOKIE;
    case UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF:
        return UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_IDENTITY_CHALLENGE;
    case UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF:
        return UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_IDENTITY_RESPONSE;
    case UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER:
        return UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_ADDRESS_OFFER;
    case UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT:
        return UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_DEVICE_COMMIT;
    case UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE:
        return UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_FINAL_COMMIT;
    case UCN_V6_BOOTSTRAP_EVENT_ABORT:
        return UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_ABORT;
    default:
        return 0U;
    }
}

static ucn_v6_bootstrap_event_t bootstrap_event_from_opcode(uint16_t opcode)
{
    switch (opcode) {
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_HELLO_COOKIE:
        return UCN_V6_BOOTSTRAP_EVENT_COOKIE;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_IDENTITY_CHALLENGE:
        return UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_IDENTITY_RESPONSE:
        return UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_ADDRESS_OFFER:
        return UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_DEVICE_COMMIT:
        return UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_FINAL_COMMIT:
        return UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE;
    case UCN_V6_PROTOCOL_OPCODE_BOOTSTRAP_ABORT:
        return UCN_V6_BOOTSTRAP_EVENT_ABORT;
    default:
        return (ucn_v6_bootstrap_event_t)0;
    }
}

ucn_v6_result_t ucn_v6_bootstrap_logical_encode(
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_phase_t expected_phase,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_bootstrap_evidence_t *evidence,
    uint8_t *output, size_t output_capacity, size_t *output_length)
{
    uint8_t encoded[UCN_V6_BOOTSTRAP_LOGICAL_MAX_BYTES];
    size_t length;
    ucn_v6_result_t result;

    if (event == UCN_V6_BOOTSTRAP_EVENT_COOKIE ||
        transcript == NULL || !evidence_is_valid(evidence) || output == NULL ||
        output_length == NULL ||
        !bootstrap_event_matches_phase(event, transcript->flow,
                                       expected_phase) ||
        output_capacity < UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES +
                              1U + evidence->length ||
        ucn_v6_memory_ranges_overlap(
            transcript, sizeof(*transcript), output, output_capacity) ||
        ucn_v6_memory_ranges_overlap(
            evidence, sizeof(*evidence), output, output_capacity) ||
        ucn_v6_memory_ranges_overlap(
            output, output_capacity, output_length, sizeof(*output_length)) ||
        ucn_v6_memory_ranges_overlap(
            transcript, sizeof(*transcript), output_length,
            sizeof(*output_length)) ||
        ucn_v6_memory_ranges_overlap(
            evidence, sizeof(*evidence), output_length,
            sizeof(*output_length))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = ucn_v6_bootstrap_transcript_encode(
        transcript, expected_phase, encoded);
    if (result != UCN_V6_OK) return result;
    length = UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES + 1U +
             evidence->length;
    encoded[UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES] = evidence->length;
    memcpy(&encoded[UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES + 1U],
           evidence->bytes, evidence->length);
    memcpy(output, encoded, length);
    *output_length = length;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_logical_decode(
    uint16_t protocol_opcode,
    ucn_v6_bootstrap_phase_t expected_phase,
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_evidence_t *evidence)
{
    ucn_v6_bootstrap_transcript_t decoded_transcript;
    ucn_v6_bootstrap_evidence_t decoded_evidence;
    ucn_v6_bootstrap_event_t event =
        bootstrap_event_from_opcode(protocol_opcode);
    uint8_t evidence_length;
    ucn_v6_result_t result;

    if (input == NULL || transcript == NULL || evidence == NULL ||
        event == (ucn_v6_bootstrap_event_t)0 ||
        event == UCN_V6_BOOTSTRAP_EVENT_COOKIE ||
        input_length < UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES + 2U ||
        input_length > UCN_V6_BOOTSTRAP_LOGICAL_MAX_BYTES ||
        ucn_v6_memory_ranges_overlap(input, input_length, transcript,
                                     sizeof(*transcript)) ||
        ucn_v6_memory_ranges_overlap(input, input_length, evidence,
                                     sizeof(*evidence)) ||
        ucn_v6_memory_ranges_overlap(transcript, sizeof(*transcript),
                                     evidence, sizeof(*evidence))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    evidence_length =
        input[UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES];
    if (evidence_length == 0U ||
        evidence_length > UCN_V6_BOOTSTRAP_EVIDENCE_MAX_BYTES ||
        input_length != UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES + 1U +
                            evidence_length) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded_transcript, 0, sizeof(decoded_transcript));
    result = ucn_v6_bootstrap_transcript_decode(
        input, UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES, expected_phase,
        &decoded_transcript);
    if (result != UCN_V6_OK ||
        !bootstrap_event_matches_phase(event, decoded_transcript.flow,
                                       expected_phase)) {
        return result != UCN_V6_OK ? result : UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded_evidence, 0, sizeof(decoded_evidence));
    decoded_evidence.length = evidence_length;
    memcpy(decoded_evidence.bytes,
           &input[UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES + 1U],
           evidence_length);
    if (!evidence_is_valid(&decoded_evidence)) return UCN_V6_ERR_MALFORMED;
    *transcript = decoded_transcript;
    *evidence = decoded_evidence;
    return UCN_V6_OK;
}

static bool bootstrap_fragment_is_valid(
    const ucn_v6_bootstrap_fragment_t *fragment)
{
    uint16_t expected_offset;
    uint16_t expected_length;
    uint8_t expected_count;

    if (fragment == NULL || !flow_is_valid(fragment->flow) ||
        fragment->phase < UCN_V6_BOOTSTRAP_COOKIE_VERIFIED ||
        fragment->phase > UCN_V6_BOOTSTRAP_FINAL_DURABLE ||
        fragment->total_length <
            UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES + 2U ||
        fragment->total_length > UCN_V6_BOOTSTRAP_LOGICAL_MAX_BYTES ||
        fragment->transaction_id == 0U ||
        fragment->transaction_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        !ucn_v6_principal_is_valid(&fragment->identity_digest)) {
        return false;
    }
    expected_count = (uint8_t)((fragment->total_length +
                                UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES - 1U) /
                               UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES);
    if (expected_count == 0U ||
        expected_count > UCN_V6_BOOTSTRAP_MAX_FRAGMENTS ||
        fragment->fragment_count != expected_count ||
        fragment->fragment_index >= expected_count) {
        return false;
    }
    expected_offset = (uint16_t)((uint16_t)fragment->fragment_index *
                                 UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES);
    expected_length = (uint16_t)(fragment->total_length - expected_offset);
    if (expected_length > UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES) {
        expected_length = UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES;
    }
    return fragment->fragment_offset == expected_offset &&
           fragment->fragment_length == expected_length &&
           bytes_zero(fragment->data + expected_length,
                      UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES -
                          expected_length);
}

ucn_v6_result_t ucn_v6_bootstrap_fragment_encode(
    const ucn_v6_bootstrap_fragment_t *fragment,
    uint8_t *output, size_t output_capacity, size_t *output_length)
{
    uint8_t encoded[UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES +
                    UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES];
    size_t length;

    if (!bootstrap_fragment_is_valid(fragment) || output == NULL ||
        output_length == NULL ||
        output_capacity < UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES +
                              fragment->fragment_length ||
        ucn_v6_memory_ranges_overlap(fragment, sizeof(*fragment), output,
                                     output_capacity) ||
        ucn_v6_memory_ranges_overlap(output, output_capacity, output_length,
                                     sizeof(*output_length)) ||
        ucn_v6_memory_ranges_overlap(fragment, sizeof(*fragment),
                                     output_length,
                                     sizeof(*output_length))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = 1U;
    encoded[1] = (uint8_t)fragment->flow;
    encoded[2] = (uint8_t)fragment->phase;
    encoded[3] = fragment->fragment_index;
    encoded[4] = fragment->fragment_count;
    encoded[5] = 0U;
    bootstrap_put_u16(&encoded[6], fragment->total_length);
    bootstrap_put_u16(&encoded[8], fragment->fragment_offset);
    bootstrap_put_u16(&encoded[10], fragment->fragment_length);
    bootstrap_put_u64(&encoded[12], fragment->transaction_id);
    memcpy(&encoded[20], fragment->identity_digest.bytes, 16U);
    memcpy(&encoded[UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES], fragment->data,
           fragment->fragment_length);
    length = UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES +
             fragment->fragment_length;
    memcpy(output, encoded, length);
    *output_length = length;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_fragment_decode(
    const uint8_t *input, size_t input_length,
    ucn_v6_bootstrap_fragment_t *fragment)
{
    ucn_v6_bootstrap_fragment_t decoded;

    if (input == NULL || fragment == NULL ||
        input_length < UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES + 1U ||
        input_length > UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES +
                           UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES ||
        ucn_v6_memory_ranges_overlap(input, input_length, fragment,
                                     sizeof(*fragment))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (input[0] != 1U || input[5] != 0U) {
        return UCN_V6_ERR_MALFORMED;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.flow = (ucn_v6_bootstrap_flow_t)input[1];
    decoded.phase = (ucn_v6_bootstrap_phase_t)input[2];
    decoded.fragment_index = input[3];
    decoded.fragment_count = input[4];
    decoded.total_length = bootstrap_get_u16(&input[6]);
    decoded.fragment_offset = bootstrap_get_u16(&input[8]);
    decoded.fragment_length = bootstrap_get_u16(&input[10]);
    decoded.transaction_id = bootstrap_get_u64(&input[12]);
    memcpy(decoded.identity_digest.bytes, &input[20], 16U);
    if (decoded.fragment_length == 0U ||
        input_length != UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES +
                            decoded.fragment_length ||
        decoded.fragment_length > UCN_V6_BOOTSTRAP_FRAGMENT_DATA_BYTES) {
        return UCN_V6_ERR_MALFORMED;
    }
    memcpy(decoded.data, &input[UCN_V6_BOOTSTRAP_FRAGMENT_HEADER_BYTES],
           decoded.fragment_length);
    if (!bootstrap_fragment_is_valid(&decoded)) {
        return UCN_V6_ERR_MALFORMED;
    }
    *fragment = decoded;
    return UCN_V6_OK;
}

static bool bootstrap_fragment_matches_key(
    const ucn_v6_bootstrap_fragment_t *fragment,
    const ucn_v6_bootstrap_key_t *key)
{
    return fragment->flow != (ucn_v6_bootstrap_flow_t)0 &&
           fragment->transaction_id == key->transaction_id &&
           principal_equal(&fragment->identity_digest,
                           &key->identity_digest);
}

static uint8_t bootstrap_complete_mask(uint8_t fragment_count)
{
    return (uint8_t)((UINT8_C(1) << fragment_count) - UINT8_C(1));
}

ucn_v6_result_t ucn_v6_bootstrap_reassembly_accept(
    ucn_v6_bootstrap_reassembly_t *reassembly,
    uint16_t protocol_opcode,
    const ucn_v6_bootstrap_key_t *key,
    uint64_t pending_deadline_us,
    uint64_t now_us,
    const ucn_v6_bootstrap_fragment_t *fragment,
    bool *complete)
{
    ucn_v6_bootstrap_event_t event =
        bootstrap_event_from_opcode(protocol_opcode);
    uint8_t bit;
    bool already_received;

    if (reassembly == NULL || complete == NULL || !key_is_valid(key) ||
        !bootstrap_fragment_is_valid(fragment) ||
        event == (ucn_v6_bootstrap_event_t)0 ||
        event == UCN_V6_BOOTSTRAP_EVENT_COOKIE ||
        pending_deadline_us == 0U || now_us >= pending_deadline_us ||
        !bootstrap_event_matches_phase(event, fragment->flow,
                                       fragment->phase) ||
        !bootstrap_fragment_matches_key(fragment, key) ||
        ucn_v6_memory_ranges_overlap(reassembly, sizeof(*reassembly),
                                     fragment, sizeof(*fragment)) ||
        ucn_v6_memory_ranges_overlap(reassembly, sizeof(*reassembly),
                                     complete, sizeof(*complete)) ||
        ucn_v6_memory_ranges_overlap(fragment, sizeof(*fragment), complete,
                                     sizeof(*complete))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!reassembly->occupied) {
        if (fragment->fragment_index != 0U) {
            return UCN_V6_ERR_NOT_FOUND;
        }
    } else if (reassembly->protocol_opcode != protocol_opcode ||
               reassembly->phase != fragment->phase ||
               !key_equal(&reassembly->key, key) ||
               reassembly->total_length != fragment->total_length ||
               reassembly->fragment_count != fragment->fragment_count ||
               reassembly->deadline_us != pending_deadline_us) {
        return UCN_V6_ERR_STATE;
    }
    bit = (uint8_t)(UINT8_C(1) << fragment->fragment_index);
    already_received = reassembly->occupied &&
                       (reassembly->received_mask & bit) != 0U;
    if (already_received &&
        memcmp(&reassembly->logical[fragment->fragment_offset],
               fragment->data, fragment->fragment_length) != 0) {
        return UCN_V6_ERR_REPLAY;
    }
    if (!reassembly->occupied) {
        memset(reassembly, 0, sizeof(*reassembly));
        reassembly->occupied = true;
        reassembly->protocol_opcode = protocol_opcode;
        reassembly->phase = fragment->phase;
        reassembly->key = *key;
        reassembly->total_length = fragment->total_length;
        reassembly->fragment_count = fragment->fragment_count;
        reassembly->deadline_us = pending_deadline_us;
    }
    if (!already_received) {
        memcpy(&reassembly->logical[fragment->fragment_offset],
               fragment->data, fragment->fragment_length);
        reassembly->received_mask =
            (uint8_t)(reassembly->received_mask | bit);
    }
    *complete = reassembly->received_mask ==
                bootstrap_complete_mask(reassembly->fragment_count);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_reassembly_borrow(
    const ucn_v6_bootstrap_reassembly_t *reassembly,
    const uint8_t **logical, size_t *logical_length)
{
    if (reassembly == NULL || logical == NULL || logical_length == NULL ||
        ucn_v6_memory_ranges_overlap(reassembly, sizeof(*reassembly),
                                     logical, sizeof(*logical)) ||
        ucn_v6_memory_ranges_overlap(reassembly, sizeof(*reassembly),
                                     logical_length,
                                     sizeof(*logical_length)) ||
        ucn_v6_memory_ranges_overlap(logical, sizeof(*logical),
                                     logical_length,
                                     sizeof(*logical_length))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!reassembly->occupied || reassembly->fragment_count == 0U ||
        reassembly->fragment_count > UCN_V6_BOOTSTRAP_MAX_FRAGMENTS ||
        reassembly->received_mask !=
            bootstrap_complete_mask(reassembly->fragment_count) ||
        reassembly->total_length <
            UCN_V6_BOOTSTRAP_TRANSCRIPT_CANONICAL_BYTES + 2U ||
        reassembly->total_length > UCN_V6_BOOTSTRAP_LOGICAL_MAX_BYTES) {
        return UCN_V6_ERR_STATE;
    }
    *logical = reassembly->logical;
    *logical_length = reassembly->total_length;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_reassembly_reset(
    ucn_v6_bootstrap_reassembly_t *reassembly)
{
    if (reassembly == NULL) return UCN_V6_ERR_ARGUMENT;
    memset(reassembly, 0, sizeof(*reassembly));
    return UCN_V6_OK;
}

static bool transcript_equal(
    const ucn_v6_bootstrap_transcript_t *left,
    const ucn_v6_bootstrap_transcript_t *right)
{
    return left->protocol_version == right->protocol_version &&
           left->bootstrap_header_contract ==
               right->bootstrap_header_contract &&
           left->flow == right->flow &&
           principal_equal(&left->joining_device_principal,
                           &right->joining_device_principal) &&
           principal_equal(&left->joining_device_identity_digest,
                           &right->joining_device_identity_digest) &&
           principal_equal(&left->authority_principal,
                           &right->authority_principal) &&
           left->authority_generation == right->authority_generation &&
           left->device_nonce == right->device_nonce &&
           left->authority_nonce == right->authority_nonce &&
           left->transaction_id == right->transaction_id &&
           left->lease_freshness_challenge_nonce ==
               right->lease_freshness_challenge_nonce &&
           left->realm_id == right->realm_id &&
           left->proposed_address == right->proposed_address &&
           left->address_binding_generation ==
               right->address_binding_generation &&
           left->authority_address == right->authority_address &&
           left->authority_binding_generation ==
               right->authority_binding_generation &&
           left->selected_link_instance_id ==
               right->selected_link_instance_id &&
           memcmp(left->binding_lease_id, right->binding_lease_id,
                  sizeof(left->binding_lease_id)) == 0 &&
           left->binding_lease_duration_us ==
               right->binding_lease_duration_us &&
           left->authority_lease_sequence ==
               right->authority_lease_sequence &&
           left->authority_lease_duration_us ==
               right->authority_lease_duration_us &&
           left->freshness_max_remaining_lease_us ==
               right->freshness_max_remaining_lease_us &&
           memcmp(left->durable_fence_token, right->durable_fence_token,
                  sizeof(left->durable_fence_token)) == 0 &&
           memcmp(left->allocation_high_water_digest,
                  right->allocation_high_water_digest,
                  sizeof(left->allocation_high_water_digest)) == 0 &&
           memcmp(left->quorum_config_digest, right->quorum_config_digest,
                  sizeof(left->quorum_config_digest)) == 0 &&
           memcmp(left->signer_set_digest, right->signer_set_digest,
                  sizeof(left->signer_set_digest)) == 0 &&
           memcmp(left->threshold_proof_digest,
                  right->threshold_proof_digest,
                  sizeof(left->threshold_proof_digest)) == 0 &&
           memcmp(left->freshness_proof_transcript_hash,
                  right->freshness_proof_transcript_hash,
                  sizeof(left->freshness_proof_transcript_hash)) == 0 &&
           left->authority_signer_count == right->authority_signer_count &&
           left->authority_quorum_threshold ==
               right->authority_quorum_threshold &&
           left->binding_mode == right->binding_mode &&
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
               right->selected_session_generation &&
           left->selected_link_instance_generation ==
               right->selected_link_instance_generation &&
           memcmp(left->prior_messages_hash, right->prior_messages_hash,
                  sizeof(left->prior_messages_hash)) == 0;
}

static bool optional_binding_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right)
{
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return ucn_v6_binding_key_equal(left, right);
}

static ucn_v6_result_t authorize_event(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_event_t event,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    uint64_t now_us,
    const ucn_v6_bootstrap_evidence_t *evidence)
{
    ucn_v6_bootstrap_key_t key_copy;
    ucn_v6_bootstrap_transcript_t transcript_copy;
    ucn_v6_binding_key_t binding_copy;
    ucn_v6_bootstrap_evidence_t evidence_copy;
    const ucn_v6_binding_key_t *binding_argument = NULL;
    uint64_t violations_before;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    if (!evidence_is_valid(evidence)) {
        return UCN_V6_ERR_SECURITY;
    }
    key_copy = *key;
    transcript_copy = *transcript;
    evidence_copy = *evidence;
    if (existing_binding != NULL) {
        binding_copy = *existing_binding;
        binding_argument = &binding_copy;
    }
    violations_before = ucn_v6_callback_gate_violation_count(
        owner->callback_gate);
    if (violations_before == UINT64_MAX ||
        ucn_v6_callback_gate_try_enter(owner->callback_gate, owner) !=
            UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    result = owner->verifier.authorize_event(
        owner->verifier.context, event, flow, &key_copy, &transcript_copy,
        binding_argument, now_us, &evidence_copy);
    leave_result = callback_scope_finish(
        owner->callback_gate, owner, violations_before, result);
    if (leave_result == UCN_V6_ERR_STATE && result == UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    return leave_result == UCN_V6_OK ? UCN_V6_OK : UCN_V6_ERR_SECURITY;
}

static ucn_v6_bootstrap_pending_t *pending_array(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow)
{
    return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ? owner->join_pending :
                                                owner->reauth_pending;
}

static const ucn_v6_bootstrap_pending_t *pending_array_const(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow)
{
    return flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ? owner->join_pending :
                                                owner->reauth_pending;
}

static ucn_v6_bootstrap_pending_t *find_pending(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key)
{
    size_t index;
    ucn_v6_bootstrap_pending_t *slots = pending_array(owner, flow);

    for (index = 0U; index < owner->config.max_pending; ++index) {
        if (slots[index].occupied && key_equal(&slots[index].key, key)) {
            return &slots[index];
        }
    }
    return NULL;
}

static ucn_v6_bootstrap_link_budget_t *find_budget(
    ucn_v6_bootstrap_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation)
{
    size_t index;
    ucn_v6_bootstrap_link_budget_t *empty = NULL;

    for (index = 0U; index < UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS; ++index) {
        if (owner->budgets[index].occupied &&
            owner->budgets[index].ingress_link_id == link_id &&
            owner->budgets[index].ingress_link_generation == link_generation) {
            return &owner->budgets[index];
        }
        if (!owner->budgets[index].occupied && empty == NULL) {
            empty = &owner->budgets[index];
        }
    }
    return empty;
}

static bool budget_has_pending(
    const ucn_v6_bootstrap_owner_t *owner,
    const ucn_v6_bootstrap_link_budget_t *budget)
{
    const ucn_v6_bootstrap_pending_t *arrays[2];
    size_t array_index;
    size_t slot_index;

    arrays[0] = owner->join_pending;
    arrays[1] = owner->reauth_pending;
    for (array_index = 0U; array_index < 2U; ++array_index) {
        for (slot_index = 0U; slot_index < owner->config.max_pending;
             ++slot_index) {
            if (arrays[array_index][slot_index].occupied &&
                arrays[array_index][slot_index].key.ingress_link_id ==
                    budget->ingress_link_id &&
                arrays[array_index][slot_index]
                        .key.ingress_link_generation ==
                    budget->ingress_link_generation) {
                return true;
            }
        }
    }
    return false;
}

static void pending_resource_usage(
    const ucn_v6_bootstrap_owner_t *owner,
    const ucn_v6_bootstrap_key_t *key,
    size_t *global_count,
    size_t *link_count,
    size_t *peer_count)
{
    const ucn_v6_bootstrap_pending_t *arrays[2];
    size_t array_index;
    size_t slot_index;

    *global_count = 0U;
    *link_count = 0U;
    *peer_count = 0U;
    arrays[0] = owner->join_pending;
    arrays[1] = owner->reauth_pending;
    for (array_index = 0U; array_index < 2U; ++array_index) {
        for (slot_index = 0U; slot_index < owner->config.max_pending;
             ++slot_index) {
            const ucn_v6_bootstrap_pending_t *pending =
                &arrays[array_index][slot_index];
            if (!pending->occupied) {
                continue;
            }
            ++(*global_count);
            if (pending->key.ingress_link_id != key->ingress_link_id ||
                pending->key.ingress_link_generation !=
                key->ingress_link_generation) {
                continue;
            }
            ++(*link_count);
            if (pending->key.local_peer_discriminator ==
                    key->local_peer_discriminator &&
                principal_equal(&pending->key.identity_digest,
                                &key->identity_digest)) {
                ++(*peer_count);
            }
        }
    }
}

ucn_v6_result_t ucn_v6_bootstrap_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const struct ucn_v6_feature_manifest *manifest,
    const ucn_v6_bootstrap_config_t *config,
    const ucn_v6_bootstrap_verifier_ops_t *verifier,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_bootstrap_owner_t **owner_out)
{
    ucn_v6_bootstrap_owner_t *owner;
    ucn_v6_result_t result;

    if (owner_out == NULL || config == NULL || verifier == NULL ||
        verifier->authorize_event == NULL || callback_gate == NULL ||
        ucn_v6_callback_gate_violation_count(callback_gate) == UINT64_MAX ||
        ucn_v6_callback_gate_is_active(callback_gate) ||
        config->max_pending == 0U ||
        config->max_pending > UCN_V6_BOOTSTRAP_MAX_PENDING ||
        config->max_pending_per_link == 0U ||
        config->max_pending_per_link > config->max_pending ||
        config->token_burst == 0U || config->tokens_per_second == 0U ||
        config->pending_timeout_us == 0U) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(
        (const ucn_v6_feature_manifest_t *)manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(*owner),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    if (ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     owner_out, sizeof(*owner_out)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     manifest,
                                     sizeof(ucn_v6_feature_manifest_t)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     config, sizeof(*config)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     verifier, sizeof(*verifier)) ||
        ucn_v6_memory_ranges_overlap(storage, storage_bytes,
                                     callback_gate,
                                     sizeof(*callback_gate)) ||
        ucn_v6_memory_ranges_overlap(
            storage, storage_bytes, verifier->context,
            verifier->context != NULL ? 1U : 0U) ||
        ucn_v6_memory_ranges_overlap(
            storage, storage_bytes, callback_gate->context,
            callback_gate->context != NULL ? 1U : 0U)) {
        return UCN_V6_ERR_CONFIG;
    }
    owner = (ucn_v6_bootstrap_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_BOOTSTRAP_OWNER_MAGIC;
    owner->schema = UCN_V6_STORAGE_LAYOUT;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->config = *config;
    owner->verifier = *verifier;
    owner->callback_gate = callback_gate;
    owner->initialized = true;
    owner->canary = UCN_V6_BOOTSTRAP_OWNER_CANARY;
    *owner_out = owner;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_admit_initial_hello(
    ucn_v6_bootstrap_owner_t *owner,
    uint16_t ingress_link_id,
    uint32_t ingress_link_generation,
    uint64_t now_us,
    size_t request_bytes,
    size_t response_bytes)
{
    ucn_v6_bootstrap_link_budget_t *budget;
    uint64_t elapsed_seconds;
    uint64_t missing_tokens;
    uint64_t seconds_to_full;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (ingress_link_id == 0U || ingress_link_id > UCN_V6_LINK_ID_MAX ||
        ingress_link_generation == 0U ||
        ingress_link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        request_bytes == 0U ||
        response_bytes > request_bytes) {
        return UCN_V6_ERR_ARGUMENT;
    }
    budget = find_budget(owner, ingress_link_id, ingress_link_generation);
    if (budget == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (!budget->occupied) {
        budget->occupied = true;
        budget->ingress_link_id = ingress_link_id;
        budget->ingress_link_generation = ingress_link_generation;
        budget->tokens = owner->config.token_burst;
        budget->last_refill_us = now_us;
        budget->last_activity_us = now_us;
    } else {
        if (now_us < budget->last_refill_us) {
            return UCN_V6_ERR_STATE;
        }
        elapsed_seconds = (now_us - budget->last_refill_us) /
                          UINT64_C(1000000);
        if (elapsed_seconds != 0U) {
            missing_tokens = (uint64_t)owner->config.token_burst -
                             (uint64_t)budget->tokens;
            seconds_to_full =
                (missing_tokens + owner->config.tokens_per_second - 1U) /
                owner->config.tokens_per_second;
            if (elapsed_seconds >= seconds_to_full) {
                budget->tokens = owner->config.token_burst;
            } else {
                budget->tokens = (uint8_t)(
                    (uint64_t)budget->tokens +
                    elapsed_seconds * owner->config.tokens_per_second);
            }
            if (elapsed_seconds > UINT64_MAX / UINT64_C(1000000)) {
                budget->last_refill_us = now_us;
            } else {
                budget->last_refill_us +=
                    elapsed_seconds * UINT64_C(1000000);
            }
        }
    }
    if (budget->tokens == 0U) {
        return UCN_V6_ERR_ACCESS;
    }
    --budget->tokens;
    budget->last_activity_us = now_us;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_open_after_cookie(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    const ucn_v6_binding_key_t *existing_binding,
    const ucn_v6_bootstrap_evidence_t *cookie_evidence,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t *slots;
    ucn_v6_bootstrap_pending_t *duplicate;
    size_t index;
    size_t global_count;
    size_t link_count = 0U;
    size_t peer_count;
    ucn_v6_bootstrap_pending_t *empty = NULL;
    uint64_t deadline;
    ucn_v6_result_t result;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!flow_is_valid(flow) ||
        !key_is_valid(key) ||
        !transcript_is_valid_for_phase(
            transcript, UCN_V6_BOOTSTRAP_COOKIE_VERIFIED) ||
        transcript->flow != flow ||
        !principal_equal(&key->identity_digest,
                         &transcript->joining_device_identity_digest) ||
        key->transaction_id != transcript->transaction_id ||
        key->ingress_link_id != transcript->selected_link_instance_id ||
        key->ingress_link_generation !=
            transcript->selected_link_instance_generation) {
        return UCN_V6_ERR_SECURITY;
    }
    if (flow == UCN_V6_BOOTSTRAP_FLOW_JOIN) {
        if (existing_binding != NULL &&
            ucn_v6_binding_key_is_valid(existing_binding)) {
            return UCN_V6_ERR_STATE;
        }
    } else {
        if (!ucn_v6_binding_key_is_valid(existing_binding) ||
            existing_binding->realm_id != transcript->realm_id ||
            existing_binding->node_address != transcript->proposed_address ||
            existing_binding->binding_generation !=
                transcript->address_binding_generation) {
            return UCN_V6_ERR_STATE;
        }
    }

    duplicate = find_pending(owner, flow, key);
    if (duplicate != NULL) {
        if (!transcript_equal(&duplicate->transcript, transcript) ||
            !optional_binding_equal(
                flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH ?
                    &duplicate->existing_binding : NULL,
                existing_binding)) {
            return UCN_V6_ERR_REPLAY;
        }
        return authorize_event(
            owner, UCN_V6_BOOTSTRAP_EVENT_COOKIE, flow, key, transcript,
            existing_binding, now_us, cookie_evidence);
    }
    if (now_us > UINT64_MAX - owner->config.pending_timeout_us) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    deadline = now_us + owner->config.pending_timeout_us;
    if (deadline == 0U) {
        return UCN_V6_ERR_EXHAUSTED;
    }

    pending_resource_usage(owner, key, &global_count, &link_count,
                           &peer_count);
    if (global_count >= owner->config.max_pending ||
        link_count >= owner->config.max_pending_per_link ||
        peer_count != 0U) {
        return UCN_V6_ERR_NO_SPACE;
    }

    slots = pending_array(owner, flow);
    for (index = 0U; index < owner->config.max_pending; ++index) {
        if (!slots[index].occupied && empty == NULL) {
            empty = &slots[index];
        }
    }
    if (empty == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }

    result = authorize_event(owner, UCN_V6_BOOTSTRAP_EVENT_COOKIE, flow, key,
                             transcript, existing_binding, now_us,
                             cookie_evidence);
    if (result != UCN_V6_OK) {
        return result;
    }

    memset(empty, 0, sizeof(*empty));
    empty->occupied = true;
    empty->flow = flow;
    empty->phase = UCN_V6_BOOTSTRAP_COOKIE_VERIFIED;
    empty->key = *key;
    empty->transcript = *transcript;
    if (existing_binding != NULL) {
        empty->existing_binding = *existing_binding;
    }
    empty->challenge_started_local_us = now_us;
    empty->deadline_us = deadline;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_advance(
    ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    ucn_v6_bootstrap_event_t event,
    const ucn_v6_bootstrap_evidence_t *evidence,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t *pending;
    ucn_v6_bootstrap_phase_t next_phase;
    ucn_v6_result_t result;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!flow_is_valid(flow) ||
        !key_is_valid(key) || transcript == NULL ||
        transcript->flow != flow) {
        return UCN_V6_ERR_ARGUMENT;
    }
    pending = find_pending(owner, flow, key);
    if (pending == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (now_us >= pending->deadline_us) {
        return UCN_V6_ERR_TIMEOUT;
    }
    if (event == UCN_V6_BOOTSTRAP_EVENT_ABORT) {
        if (pending->phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE ||
            pending->phase == UCN_V6_BOOTSTRAP_ABORTED) {
            return UCN_V6_ERR_STATE;
        }
        if (!transcript_equal(&pending->transcript, transcript)) {
            return UCN_V6_ERR_REPLAY;
        }
        result = authorize_event(
            owner, event, flow, key, transcript,
            flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH ?
                &pending->existing_binding : NULL,
            now_us, evidence);
        if (result != UCN_V6_OK) {
            return result;
        }
        pending->phase = UCN_V6_BOOTSTRAP_ABORTED;
        return UCN_V6_OK;
    }

    next_phase = pending->phase;
    if (event == UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF &&
        pending->phase == UCN_V6_BOOTSTRAP_COOKIE_VERIFIED) {
        next_phase = UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED;
    } else if (event == UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF &&
               pending->phase == UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED) {
        next_phase = UCN_V6_BOOTSTRAP_DEVICE_VERIFIED;
    } else if (flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
               event == UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER &&
               pending->phase == UCN_V6_BOOTSTRAP_DEVICE_VERIFIED) {
        next_phase = UCN_V6_BOOTSTRAP_ADDRESS_OFFERED;
    } else if (flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
               event == UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT &&
               pending->phase == UCN_V6_BOOTSTRAP_ADDRESS_OFFERED) {
        next_phase = UCN_V6_BOOTSTRAP_DEVICE_COMMITTED;
    } else if (flow == UCN_V6_BOOTSTRAP_FLOW_JOIN &&
               event == UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE &&
               pending->phase == UCN_V6_BOOTSTRAP_DEVICE_COMMITTED) {
        next_phase = UCN_V6_BOOTSTRAP_FINAL_DURABLE;
    } else if (flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH &&
               event == UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE &&
               pending->phase == UCN_V6_BOOTSTRAP_DEVICE_VERIFIED) {
        next_phase = UCN_V6_BOOTSTRAP_FINAL_DURABLE;
    } else if ((event == UCN_V6_BOOTSTRAP_EVENT_AUTHORITY_PROOF &&
                pending->phase == UCN_V6_BOOTSTRAP_AUTHORITY_VERIFIED) ||
               (event == UCN_V6_BOOTSTRAP_EVENT_DEVICE_PROOF &&
                pending->phase == UCN_V6_BOOTSTRAP_DEVICE_VERIFIED) ||
               (event == UCN_V6_BOOTSTRAP_EVENT_ADDRESS_OFFER &&
                pending->phase == UCN_V6_BOOTSTRAP_ADDRESS_OFFERED) ||
               (event == UCN_V6_BOOTSTRAP_EVENT_DEVICE_COMMIT &&
                pending->phase == UCN_V6_BOOTSTRAP_DEVICE_COMMITTED) ||
               (event == UCN_V6_BOOTSTRAP_EVENT_FINAL_DURABLE &&
                pending->phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE)) {
        next_phase = pending->phase;
    } else {
        return UCN_V6_ERR_STATE;
    }
    if (next_phase == pending->phase) {
        if (!transcript_equal(&pending->transcript, transcript)) {
            return UCN_V6_ERR_REPLAY;
        }
    } else if (!transcript_is_valid_for_phase(transcript, next_phase) ||
               !transcript_extends(&pending->transcript, transcript)) {
        return UCN_V6_ERR_REPLAY;
    }
    result = authorize_event(
        owner, event, flow, key, transcript,
        flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH ?
            &pending->existing_binding : NULL,
        now_us, evidence);
    if (result != UCN_V6_OK) {
        return result;
    }
    if (next_phase != pending->phase) {
        pending->transcript = *transcript;
    }
    pending->phase = next_phase;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_bootstrap_copy_pending(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    ucn_v6_bootstrap_pending_t *pending)
{
    const ucn_v6_bootstrap_pending_t *slots;
    size_t index;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!flow_is_valid(flow) ||
        !key_is_valid(key) || pending == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slots = pending_array_const(owner, flow);
    for (index = 0U; index < owner->config.max_pending; ++index) {
        if (slots[index].occupied && key_equal(&slots[index].key, key)) {
            *pending = slots[index];
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_bootstrap_validate_final(
    const ucn_v6_bootstrap_owner_t *owner,
    ucn_v6_bootstrap_flow_t flow,
    const ucn_v6_bootstrap_key_t *key,
    const ucn_v6_bootstrap_transcript_t *transcript,
    uint64_t now_us)
{
    const ucn_v6_bootstrap_pending_t *slots;
    size_t index;

    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (callback_reentry_is_blocked(owner)) {
        return UCN_V6_ERR_STATE;
    }
    if (!flow_is_valid(flow) ||
        !key_is_valid(key) || !transcript_is_valid(transcript) ||
        transcript->flow != flow) {
        return UCN_V6_ERR_ARGUMENT;
    }
    slots = pending_array_const(owner, flow);
    for (index = 0U; index < owner->config.max_pending; ++index) {
        if (!slots[index].occupied || !key_equal(&slots[index].key, key)) {
            continue;
        }
        if (!transcript_equal(&slots[index].transcript, transcript)) {
            return UCN_V6_ERR_REPLAY;
        }
        if (now_us >= slots[index].deadline_us) {
            return UCN_V6_ERR_TIMEOUT;
        }
        return slots[index].phase == UCN_V6_BOOTSTRAP_FINAL_DURABLE ?
                   UCN_V6_OK : UCN_V6_ERR_STATE;
    }
    return UCN_V6_ERR_NOT_FOUND;
}

size_t ucn_v6_bootstrap_expire(
    ucn_v6_bootstrap_owner_t *owner,
    uint64_t now_us)
{
    ucn_v6_bootstrap_pending_t *arrays[2];
    size_t array_index;
    size_t slot_index;
    size_t expired = 0U;

    if (!owner_is_valid(owner) || callback_reentry_is_blocked(owner)) {
        return 0U;
    }
    arrays[0] = owner->join_pending;
    arrays[1] = owner->reauth_pending;
    for (array_index = 0U; array_index < 2U; ++array_index) {
        for (slot_index = 0U; slot_index < owner->config.max_pending;
             ++slot_index) {
            if (arrays[array_index][slot_index].occupied &&
                now_us >= arrays[array_index][slot_index].deadline_us) {
                memset(&arrays[array_index][slot_index], 0,
                       sizeof(arrays[array_index][slot_index]));
                ++expired;
            }
        }
    }
    for (slot_index = 0U;
         slot_index < UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS;
         ++slot_index) {
        ucn_v6_bootstrap_link_budget_t *budget =
            &owner->budgets[slot_index];
        uint64_t seconds_to_full =
            ((uint64_t)owner->config.token_burst +
             (uint64_t)owner->config.tokens_per_second - 1U) /
            (uint64_t)owner->config.tokens_per_second;
        uint64_t refill_idle_us =
            seconds_to_full > UINT64_MAX / UINT64_C(1000000) ?
                UINT64_MAX : seconds_to_full * UINT64_C(1000000);
        uint64_t reclaim_idle_us =
            owner->config.pending_timeout_us > refill_idle_us ?
                owner->config.pending_timeout_us : refill_idle_us;
        if (budget->occupied && !budget_has_pending(owner, budget) &&
            now_us >= budget->last_activity_us &&
            now_us - budget->last_activity_us >=
                reclaim_idle_us) {
            memset(budget, 0, sizeof(*budget));
        }
    }
    return expired;
}

size_t ucn_v6_bootstrap_invalidate_link(
    ucn_v6_bootstrap_owner_t *owner,
    uint16_t link_id,
    uint32_t link_generation)
{
    ucn_v6_bootstrap_pending_t *arrays[2];
    size_t array_index;
    size_t slot_index;
    size_t removed = 0U;

    if (!owner_is_valid(owner) || callback_reentry_is_blocked(owner) ||
        link_id == 0U || link_id > UCN_V6_LINK_ID_MAX ||
        link_generation == 0U ||
        link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return 0U;
    }
    arrays[0] = owner->join_pending;
    arrays[1] = owner->reauth_pending;
    for (array_index = 0U; array_index < 2U; ++array_index) {
        for (slot_index = 0U; slot_index < owner->config.max_pending;
             ++slot_index) {
            ucn_v6_bootstrap_pending_t *pending =
                &arrays[array_index][slot_index];
            if (pending->occupied &&
                pending->key.ingress_link_id == link_id &&
                pending->key.ingress_link_generation == link_generation) {
                memset(pending, 0, sizeof(*pending));
                ++removed;
            }
        }
    }
    for (slot_index = 0U;
         slot_index < UCN_V6_BOOTSTRAP_MAX_BUDGET_LINKS; ++slot_index) {
        ucn_v6_bootstrap_link_budget_t *budget =
            &owner->budgets[slot_index];
        if (budget->occupied && budget->ingress_link_id == link_id &&
            budget->ingress_link_generation == link_generation) {
            memset(budget, 0, sizeof(*budget));
        }
    }
    return removed;
}
