#include "internal/ucn_security.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_SECURITY_MAGIC UINT32_C(0x5543534F)
#define UCN_I_SECURITY_PROVIDER_API UINT16_C(1)
#define UCN_I_SECURITY_CALLBACK_VERIFY UINT16_C(0x0501)
#define UCN_I_SECURITY_CALLBACK_PROTECT UINT16_C(0x0502)
#define UCN_I_SECURITY_CALLBACK_OPEN UINT16_C(0x0503)

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

static uint32_t address_limit(uint8_t width)
{
    if (width == 4U) {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << (width * 8U)) - UINT32_C(1);
}

static bool binding_valid(const ucn_i_security_binding_t *binding,
                          uint8_t width)
{
    uint32_t limit;

    if (binding == NULL || width == 0U || width > 4U) {
        return false;
    }
    limit = address_limit(width);
    return binding->address != 0U && binding->address < limit &&
           binding->binding_generation != 0U &&
           bytes_nonzero(binding->principal,
                         UCN_I_SECURITY_PRINCIPAL_BYTES);
}

static bool selector_zero(const ucn_i_security_key_selector_t *selector)
{
    static const ucn_i_security_key_selector_t zero = {0};

    return memcmp(selector, &zero, sizeof(zero)) == 0;
}

static bool selector_equal(const ucn_i_security_key_selector_t *left,
                           const ucn_i_security_key_selector_t *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static bool origin_selector_valid(
    const ucn_i_security_key_selector_t *selector,
    uint8_t origin_level)
{
    uint8_t expected_suite = origin_level == UCN_I_SECURITY_AUTHENTICATED ?
                                 1U :
                                 selector->suite_id;

    return selector->reserved_zero == 0U && selector->key_id != 0U &&
           selector->key_id != UINT16_MAX && selector->key_generation != 0U &&
           ((origin_level == UCN_I_SECURITY_AUTHENTICATED &&
             expected_suite == 1U && selector->suite_id == 1U) ||
            (origin_level == UCN_I_SECURITY_CONFIDENTIAL &&
             (selector->suite_id == 2U || selector->suite_id == 3U)));
}

static bool hop_selector_valid(
    const ucn_i_security_key_selector_t *selector)
{
    return selector->reserved_zero == 0U && selector->suite_id == 16U &&
           selector->key_id != 0U && selector->key_id != UINT16_MAX &&
           selector->key_generation != 0U;
}

static bool candidate_valid(const ucn_i_security_candidate_t *candidate,
                            uint8_t expected_width,
                            uint64_t now_us)
{
    if (candidate == NULL || candidate->reserved_zero != 0U ||
        candidate->address_width != expected_width || now_us == 0U ||
        candidate->expires_at_us == 0U || now_us >= candidate->expires_at_us ||
        !binding_valid(&candidate->local, expected_width) ||
        !binding_valid(&candidate->peer, expected_width) ||
        candidate->local.address == candidate->peer.address ||
        memcmp(candidate->local.principal, candidate->peer.principal,
               UCN_I_SECURITY_PRINCIPAL_BYTES) == 0 ||
        candidate->link_generation == 0U ||
        candidate->session_generation == 0U ||
        candidate->policy_generation == 0U ||
        !bytes_nonzero(candidate->origin_fingerprint,
                       UCN_I_SECURITY_FINGERPRINT_BYTES) ||
        !bytes_nonzero(candidate->hop_fingerprint,
                       UCN_I_SECURITY_FINGERPRINT_BYTES) ||
        !bytes_nonzero(candidate->transcript_digest,
                       UCN_I_SECURITY_TRANSCRIPT_BYTES) ||
        (candidate->origin_level != UCN_I_SECURITY_AUTHENTICATED &&
         candidate->origin_level != UCN_I_SECURITY_CONFIDENTIAL) ||
        !origin_selector_valid(&candidate->origin_tx,
                               candidate->origin_level) ||
        !origin_selector_valid(&candidate->origin_rx,
                               candidate->origin_level) ||
        selector_equal(&candidate->origin_tx, &candidate->origin_rx)) {
        return false;
    }
    if (candidate->hop_profile == UCN_I_HOP_PROFILE_H0 ||
        candidate->hop_profile == UCN_I_HOP_PROFILE_H2) {
        return selector_zero(&candidate->hop_tx) &&
               selector_zero(&candidate->hop_rx);
    }
    if (candidate->hop_profile == UCN_I_HOP_PROFILE_H1) {
        return hop_selector_valid(&candidate->hop_tx) &&
               hop_selector_valid(&candidate->hop_rx) &&
               !selector_equal(&candidate->hop_tx, &candidate->hop_rx);
    }
    return false;
}

static bool owner_valid(const ucn_i_security_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_I_SECURITY_MAGIC &&
           owner->schema == UCN_I_SECURITY_SCHEMA &&
           owner->runtime_instance != 0U && owner->owner_instance != 0U &&
           owner->persistence_business_owner_instance != 0U &&
           owner->provider_gate != NULL;
}

static ucn_result_t owner_lock(ucn_i_security_owner_t *owner)
{
    if (!owner_valid(owner)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

static void owner_unlock(ucn_i_security_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static void write_be64(uint8_t *out, uint64_t value)
{
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        out[index] = (uint8_t)(value >> ((7U - index) * 8U));
    }
}

static void write_address_be(uint8_t *out, uint32_t value, uint8_t bytes)
{
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        uint8_t shift = (uint8_t)((bytes - index - 1U) * 8U);

        out[index] = (uint8_t)(value >> shift);
    }
}

static size_t c1_header_bytes(uint8_t address_width)
{
    return 9U + (2U * (size_t)address_width);
}

static uint16_t read_be16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8U) | in[1]);
}

static uint32_t read_be32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) |
           ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
}

static uint64_t read_be64(const uint8_t *in)
{
    uint64_t value = 0U;
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | in[index];
    }
    return value;
}

static uint32_t read_address_be(const uint8_t *in, uint8_t bytes)
{
    uint32_t value = 0U;
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        value = (value << 8U) | in[index];
    }
    return value;
}

static void encode_binding(const ucn_i_security_binding_t *binding,
                           uint8_t *output)
{
    write_be32(output, binding->address);
    write_be32(output + 4U, binding->binding_generation);
    memcpy(output + 8U, binding->principal,
           UCN_I_SECURITY_PRINCIPAL_BYTES);
}

static void encode_selector(const ucn_i_security_key_selector_t *selector,
                            uint8_t *output)
{
    memset(output, 0, 8U);
    output[0] = selector->suite_id;
    write_be16(output + 2U, selector->key_id);
    write_be32(output + 4U, selector->key_generation);
}

static void decode_binding(const uint8_t *input,
                           ucn_i_security_binding_t *binding)
{
    binding->address = read_be32(input);
    binding->binding_generation = read_be32(input + 4U);
    memcpy(binding->principal, input + 8U,
           UCN_I_SECURITY_PRINCIPAL_BYTES);
}

static void decode_selector(const uint8_t *input,
                            ucn_i_security_key_selector_t *selector)
{
    selector->suite_id = input[0];
    selector->reserved_zero = input[1];
    selector->key_id = read_be16(input + 2U);
    selector->key_generation = read_be32(input + 4U);
}

static uint64_t transition_fingerprint(const uint8_t *bytes, size_t length)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    size_t index;

    for (index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(0x00000100000001B3);
    }
    return hash;
}

ucn_result_t ucn_i_security_record_encode(
    uint32_t realm_id,
    const ucn_i_security_candidate_t *candidate,
    uint8_t output[UCN_I_SECURITY_RECORD_BYTES])
{
    if (output == NULL || realm_id == 0U ||
        !candidate_valid(candidate, candidate == NULL ? 0U :
                                      candidate->address_width,
                         1U) ||
        ucn_i_ranges_overlap(candidate, sizeof(*candidate), output,
                             UCN_I_SECURITY_RECORD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(output, 0, UCN_I_SECURITY_RECORD_BYTES);
    memcpy(output, "UC6S", 4U);
    write_be16(output + 4U, UCN_I_SECURITY_RECORD_SCHEMA);
    output[6] = candidate->address_width;
    output[7] = candidate->origin_level;
    output[8] = candidate->hop_profile;
    write_be32(output + 12U, realm_id);
    encode_binding(&candidate->local, output + 16U);
    encode_binding(&candidate->peer, output + 40U);
    write_be32(output + 64U, candidate->link_generation);
    write_be32(output + 68U, candidate->session_generation);
    write_be32(output + 72U, candidate->policy_generation);
    write_be64(output + 76U, candidate->expires_at_us);
    encode_selector(&candidate->origin_tx, output + 84U);
    encode_selector(&candidate->origin_rx, output + 92U);
    encode_selector(&candidate->hop_tx, output + 100U);
    encode_selector(&candidate->hop_rx, output + 108U);
    memcpy(output + 116U, candidate->origin_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    memcpy(output + 132U, candidate->hop_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    memcpy(output + 148U, candidate->transcript_digest,
           UCN_I_SECURITY_TRANSCRIPT_BYTES);
    write_be32(output + 180U, candidate->hop_tx.key_generation);
    write_be32(output + 184U, candidate->hop_rx.key_generation);
    return UCN_OK;
}

ucn_result_t ucn_i_security_record_decode(
    const uint8_t input[UCN_I_SECURITY_RECORD_BYTES],
    ucn_i_security_codec_workspace_t *workspace,
    uint32_t *realm_id_out,
    ucn_i_security_candidate_t *candidate_out)
{
    ucn_i_security_candidate_t *candidate;
    uint32_t realm_id;

    if (input == NULL || workspace == NULL || realm_id_out == NULL ||
        candidate_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_SECURITY_RECORD_BYTES, workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(input, UCN_I_SECURITY_RECORD_BYTES, realm_id_out,
                             sizeof(*realm_id_out)) ||
        ucn_i_ranges_overlap(input, UCN_I_SECURITY_RECORD_BYTES, candidate_out,
                             sizeof(*candidate_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), realm_id_out,
                             sizeof(*realm_id_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), candidate_out,
                             sizeof(*candidate_out)) ||
        ucn_i_ranges_overlap(realm_id_out, sizeof(*realm_id_out),
                             candidate_out, sizeof(*candidate_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (memcmp(input, "UC6S", 4U) != 0 ||
        read_be16(input + 4U) != UCN_I_SECURITY_RECORD_SCHEMA ||
        !bytes_zero(input + 9U, 3U) || !bytes_zero(input + 188U, 4U)) {
        return UCN_ERR_MALFORMED;
    }
    candidate = &workspace->candidate;
    memset(candidate, 0, sizeof(*candidate));
    candidate->address_width = input[6];
    candidate->origin_level = input[7];
    candidate->hop_profile = input[8];
    realm_id = read_be32(input + 12U);
    decode_binding(input + 16U, &candidate->local);
    decode_binding(input + 40U, &candidate->peer);
    candidate->link_generation = read_be32(input + 64U);
    candidate->session_generation = read_be32(input + 68U);
    candidate->policy_generation = read_be32(input + 72U);
    candidate->expires_at_us = read_be64(input + 76U);
    decode_selector(input + 84U, &candidate->origin_tx);
    decode_selector(input + 92U, &candidate->origin_rx);
    decode_selector(input + 100U, &candidate->hop_tx);
    decode_selector(input + 108U, &candidate->hop_rx);
    memcpy(candidate->origin_fingerprint, input + 116U,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    memcpy(candidate->hop_fingerprint, input + 132U,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    memcpy(candidate->transcript_digest, input + 148U,
           UCN_I_SECURITY_TRANSCRIPT_BYTES);
    if (realm_id == 0U ||
        !candidate_valid(candidate, candidate->address_width, 1U) ||
        read_be32(input + 180U) != candidate->hop_tx.key_generation ||
        read_be32(input + 184U) != candidate->hop_rx.key_generation) {
        return UCN_ERR_MALFORMED;
    }
    *realm_id_out = realm_id;
    *candidate_out = *candidate;
    return UCN_OK;
}

static bool rules_valid(const ucn_i_security_config_t *config)
{
    uint16_t left;
    uint16_t right;

    if (config->domain_rules == NULL || config->domain_rule_count == 0U ||
        config->domain_rule_count > UCN_I_SECURITY_SESSION_COUNT) {
        return false;
    }
    for (left = 0U; left < config->domain_rule_count; ++left) {
        if (config->domain_rules[left].domain_id == 0U ||
            !bytes_nonzero(config->domain_rules[left].peer_principal,
                           UCN_I_SECURITY_PRINCIPAL_BYTES)) {
            return false;
        }
        for (right = (uint16_t)(left + 1U);
             right < config->domain_rule_count; ++right) {
            if (config->domain_rules[left].domain_id ==
                    config->domain_rules[right].domain_id ||
                memcmp(config->domain_rules[left].peer_principal,
                       config->domain_rules[right].peer_principal,
                       UCN_I_SECURITY_PRINCIPAL_BYTES) == 0) {
                return false;
            }
        }
    }
    if (config->acl_rules == NULL || config->acl_rule_count == 0U ||
        config->replay_reservation_lifetime_us == 0U) {
        return false;
    }
    for (left = 0U; left < config->acl_rule_count; ++left) {
        const ucn_i_security_acl_rule_t *rule = &config->acl_rules[left];

        if (!bytes_nonzero(rule->peer_principal,
                           UCN_I_SECURITY_PRINCIPAL_BYTES) ||
            !bytes_nonzero(rule->context_fingerprint,
                           UCN_I_SECURITY_FINGERPRINT_BYTES) ||
            rule->peer_binding_generation == 0U || rule->service_id == 0U ||
            rule->service_id == UINT16_MAX ||
            (rule->direction != UCN_I_SECURITY_ACCESS_INBOUND &&
             rule->direction != UCN_I_SECURITY_ACCESS_OUTBOUND) ||
            !bytes_zero(rule->reserved_zero, sizeof(rule->reserved_zero))) {
            return false;
        }
        for (right = (uint16_t)(left + 1U);
             right < config->acl_rule_count; ++right) {
            if (memcmp(rule, &config->acl_rules[right], sizeof(*rule)) == 0) {
                return false;
            }
        }
    }
    return true;
}

ucn_result_t ucn_i_security_owner_init(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_config_t *config)
{
    ucn_result_t lock_result;

    if (owner == NULL || config == NULL ||
        config->struct_size != sizeof(*config) ||
        config->api_version != UCN_API_VERSION ||
        config->runtime_instance == 0U || config->realm_id == 0U ||
        config->owner_instance == 0U ||
        config->persistence_business_owner_instance == 0U ||
        config->address_width == 0U || config->address_width > 4U ||
        !rules_valid(config) ||
        config->provider.struct_size != sizeof(config->provider) ||
        config->provider.api_version != UCN_I_SECURITY_PROVIDER_API ||
        config->provider.verify_session == NULL ||
        config->provider.protect_origin == NULL ||
        config->provider.open_origin == NULL ||
        config->provider.protect_hop == NULL ||
        config->provider.verify_hop == NULL ||
        config->state_lock.struct_size != sizeof(config->state_lock) ||
        config->state_lock.api_version != UCN_I_LOCK_OPS_VERSION ||
        config->state_lock.enter == NULL || config->state_lock.leave == NULL ||
        config->provider_gate == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config->domain_rules,
                             sizeof(config->domain_rules[0]) *
                                 config->domain_rule_count) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config->acl_rules,
                             sizeof(config->acl_rules[0]) *
                                 config->acl_rule_count) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config->provider_gate,
                             sizeof(*config->provider_gate)) ||
        (config->state_lock.context != NULL &&
         ucn_i_ranges_overlap(owner, sizeof(*owner),
                              config->state_lock.context, 1U)) ||
        (config->provider.context != NULL &&
         ucn_i_ranges_overlap(owner, sizeof(*owner),
                               config->provider.context, 1U))) {
        return UCN_ERR_CONFIG;
    }
    lock_result = config->state_lock.enter(config->state_lock.context);
    if (lock_result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    /* The external state lock is deliberately acquired before inspecting the
     * caller-owned storage.  This makes recursive/concurrent init serialize
     * with every live Owner operation without reading an initialization flag
     * from an uninitialized object. */
    if (!bytes_zero((const uint8_t *)owner, sizeof(*owner))) {
        config->state_lock.leave(config->state_lock.context);
        return UCN_ERR_STATE;
    }
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_I_SECURITY_MAGIC;
    owner->runtime_instance = config->runtime_instance;
    owner->realm_id = config->realm_id;
    owner->next_operation_id = 1U;
    owner->schema = UCN_I_SECURITY_SCHEMA;
    owner->owner_instance = config->owner_instance;
    owner->persistence_business_owner_instance =
        config->persistence_business_owner_instance;
    owner->address_width = config->address_width;
    owner->domain_rules = config->domain_rules;
    owner->domain_rule_count = config->domain_rule_count;
    owner->acl_rules = config->acl_rules;
    owner->acl_rule_count = config->acl_rule_count;
    owner->replay_reservation_lifetime_us =
        config->replay_reservation_lifetime_us;
    owner->provider = config->provider;
    owner->state_lock = config->state_lock;
    owner->provider_gate = config->provider_gate;
    config->state_lock.leave(config->state_lock.context);
    return UCN_OK;
}

static uint16_t find_rule(const ucn_i_security_owner_t *owner,
                          const uint8_t principal[UCN_I_SECURITY_PRINCIPAL_BYTES])
{
    uint16_t index;

    for (index = 0U; index < owner->domain_rule_count; ++index) {
        if (memcmp(owner->domain_rules[index].peer_principal, principal,
                   UCN_I_SECURITY_PRINCIPAL_BYTES) == 0) {
            return index;
        }
    }
    return UINT16_MAX;
}

static uint16_t find_free_slot(const ucn_i_security_owner_t *owner)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        if (owner->sessions[index].occupied == 0U) {
            return index;
        }
    }
    return UINT16_MAX;
}

static bool handle_matches(const ucn_i_security_owner_t *owner,
                           ucn_i_security_handle_t handle,
                           uint16_t *slot_out)
{
    uint16_t slot = handle.slot;

    if (slot >= UCN_I_SECURITY_SESSION_COUNT ||
        handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        owner->sessions[slot].occupied == 0U ||
        owner->sessions[slot].slot_generation != handle.slot_generation ||
        owner->sessions[slot].candidate.session_generation !=
            handle.session_generation) {
        return false;
    }
    *slot_out = slot;
    return true;
}

static bool persistence_handle_valid(ucn_handle_t handle)
{
    return handle.runtime_instance != 0U && handle.owner_instance != 0U &&
           handle.generation != 0U &&
           handle.object_kind == UCN_OBJECT_KIND_PERSISTENCE &&
           handle.reserved_zero == 0U;
}

static void clear_callback(ucn_i_security_owner_t *owner)
{
    owner->callback_active = 0U;
    owner->callback_slot = 0U;
    owner->callback_proof_bytes = 0U;
    memset(&owner->callback_claim, 0, sizeof(owner->callback_claim));
    memset(&owner->callback_candidate, 0, sizeof(owner->callback_candidate));
    memset(owner->callback_proof, 0, sizeof(owner->callback_proof));
}

ucn_result_t ucn_i_security_prepare_static_session(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_candidate_t *candidate,
    const uint8_t *proof,
    size_t proof_bytes,
    const ucn_i_security_durability_base_t *durability,
    uint64_t now_us,
    ucn_i_security_handle_t *handle_out)
{
    ucn_i_security_handle_t handle;
    ucn_i_security_slot_t *slot;
    uint16_t rule_index;
    uint16_t slot_index;
    bool peer_session_found = false;
    uint32_t next_generation;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;

    if (candidate == NULL || proof == NULL || proof_bytes == 0U ||
        proof_bytes > UCN_I_SECURITY_PROOF_BYTES || durability == NULL ||
        handle_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), candidate,
                             sizeof(*candidate)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), proof, proof_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(candidate, sizeof(*candidate), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(proof, proof_bytes, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !candidate_valid(candidate, owner->address_width, now_us) ||
        durability->reserved_zero != 0U || durability->domain_id == 0U ||
        durability->next_transaction_id == 0U ||
        durability->absolute_deadline_us == 0U ||
        now_us >= durability->absolute_deadline_us ||
        durability->domain_generation == 0U ||
        durability->volatile_continuation == 0U ||
        durability->prior_session_generation >= UINT32_MAX ||
        candidate->session_generation !=
            durability->prior_session_generation + UINT64_C(1)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    rule_index = find_rule(owner, candidate->peer.principal);
    slot_index = find_free_slot(owner);
    if (rule_index == UINT16_MAX ||
        owner->domain_rules[rule_index].domain_id != durability->domain_id) {
        owner_unlock(owner);
        return UCN_ERR_ACCESS;
    }
    if (slot_index == UINT16_MAX || owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    {
        uint16_t index;

        for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
            const ucn_i_security_slot_t *existing = &owner->sessions[index];

            if (existing->occupied != 0U &&
                memcmp(existing->candidate.peer.principal,
                       candidate->peer.principal,
                       UCN_I_SECURITY_PRINCIPAL_BYTES) == 0) {
                if (peer_session_found ||
                    existing->phase != UCN_I_SECURITY_SESSION_ACTIVE) {
                    owner_unlock(owner);
                    return UCN_ERR_STATE;
                }
                peer_session_found = true;
                if (existing->candidate.session_generation >=
                    candidate->session_generation) {
                    owner_unlock(owner);
                    return UCN_ERR_REPLAY;
                }
            }
        }
    }
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_proof_bytes = (uint16_t)proof_bytes;
    owner->callback_candidate = *candidate;
    memcpy(owner->callback_proof, proof, proof_bytes);
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = UCN_I_SECURITY_CALLBACK_VERIFY;
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        result = owner->provider.verify_session(
            owner->provider.context, &owner->callback_candidate,
            owner->callback_proof, owner->callback_proof_bytes);
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, candidate,
               sizeof(*candidate)) != 0) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    slot = &owner->sessions[slot_index];
    next_generation = slot->slot_generation + 1U;
    if (next_generation == 0U) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    memset(slot, 0, sizeof(*slot));
    slot->candidate = owner->callback_candidate;
    slot->durability = *durability;
    slot->slot_generation = next_generation;
    result = ucn_i_security_record_encode(owner->realm_id, &slot->candidate,
                                           slot->body);
    if (result != UCN_OK) {
        memset(slot, 0, sizeof(*slot));
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    slot->transition_fingerprint = transition_fingerprint(
        slot->body, UCN_I_SECURITY_RECORD_BYTES);
    slot->occupied = 1U;
    slot->phase = UCN_I_SECURITY_SESSION_AWAITING_DURABILITY;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = slot_index;
    handle.slot_generation = slot->slot_generation;
    handle.session_generation = slot->candidate.session_generation;
    clear_callback(owner);
    *handle_out = handle;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_requirement_get(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_i_security_requirement_view_t *requirement_out)
{
    ucn_i_security_requirement_view_t view;
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (requirement_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_AWAITING_DURABILITY) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&view, 0, sizeof(view));
    view.canonical_body = slot->body;
    view.domain_id = slot->durability.domain_id;
    view.foundation_transaction_id = slot->durability.next_transaction_id;
    view.expected_record_generation =
        slot->durability.expected_record_generation;
    view.absolute_deadline_us = slot->durability.absolute_deadline_us;
    view.transition_fingerprint = slot->transition_fingerprint;
    view.runtime_instance = owner->runtime_instance;
    view.body_bytes = UCN_I_SECURITY_RECORD_BYTES;
    view.volatile_continuation = slot->durability.volatile_continuation;
    view.caller_owner_instance =
        owner->persistence_business_owner_instance;
    view.domain_generation = slot->durability.domain_generation;
    view.schema_id = UCN_I_SECURITY_RECORD_SCHEMA_ID;
    view.schema_version = UCN_I_SECURITY_RECORD_SCHEMA;
    view.operation_kind = UCN_I_SECURITY_OPERATION_KIND;
    memcpy(view.expected_body_digest,
           slot->durability.expected_body_digest,
           sizeof(view.expected_body_digest));
    *requirement_out = view;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_bind_persistence(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_handle_t persistence_handle,
    const uint8_t expected_published_digest[16])
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (!persistence_handle_valid(persistence_handle) ||
        expected_published_digest == NULL ||
        !bytes_nonzero(expected_published_digest, 16U) || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             expected_published_digest, 16U)) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_AWAITING_DURABILITY) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (slot->persistence_bound != 0U) {
        bool exact = memcmp(&slot->persistence_handle, &persistence_handle,
                            sizeof(persistence_handle)) == 0 &&
                     memcmp(slot->expected_published_digest,
                            expected_published_digest, 16U) == 0;

        owner_unlock(owner);
        return exact ? UCN_OK : UCN_ERR_STATE;
    }
    slot->persistence_handle = persistence_handle;
    memcpy(slot->expected_published_digest, expected_published_digest, 16U);
    slot->persistence_bound = 1U;
    owner_unlock(owner);
    return UCN_OK;
}

static bool current_facts_match(
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_current_facts_t *facts)
{
    return facts != NULL && facts->now_us != 0U &&
           facts->now_us < slot->candidate.expires_at_us &&
           facts->now_us < slot->durability.absolute_deadline_us &&
           memcmp(&facts->local, &slot->candidate.local,
                  sizeof(facts->local)) == 0 &&
           memcmp(&facts->peer, &slot->candidate.peer,
                  sizeof(facts->peer)) == 0 &&
           facts->link_generation == slot->candidate.link_generation &&
           facts->policy_generation == slot->candidate.policy_generation;
}

static bool acl_rule_matches(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *slot,
    const uint8_t context_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES],
    uint16_t service_id,
    uint16_t protocol_opcode,
    uint8_t direction)
{
    uint16_t index;

    for (index = 0U; index < owner->acl_rule_count; ++index) {
        const ucn_i_security_acl_rule_t *rule = &owner->acl_rules[index];

        if (memcmp(rule->peer_principal,
                   slot->candidate.peer.principal,
                   UCN_I_SECURITY_PRINCIPAL_BYTES) == 0 &&
            rule->peer_binding_generation ==
                slot->candidate.peer.binding_generation &&
            memcmp(rule->context_fingerprint, context_fingerprint,
                   UCN_I_SECURITY_FINGERPRINT_BYTES) == 0 &&
            rule->service_id == service_id &&
            rule->protocol_opcode == protocol_opcode &&
            rule->direction == direction) {
            return true;
        }
    }
    return false;
}

static bool durability_proof_matches(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_durability_proof_t *proof)
{
    uint64_t expected_generation;

    if (proof == NULL || slot->persistence_bound == 0U ||
        slot->durability.expected_record_generation == UINT64_MAX) {
        return false;
    }
    expected_generation = slot->durability.expected_record_generation + 1U;
    return memcmp(&proof->persistence_handle, &slot->persistence_handle,
                  sizeof(proof->persistence_handle)) == 0 &&
           proof->domain_id == slot->durability.domain_id &&
           proof->record_generation == expected_generation &&
           proof->foundation_transaction_id ==
               slot->durability.next_transaction_id &&
           proof->witness_generation == proof->record_generation &&
           proof->transition_fingerprint == slot->transition_fingerprint &&
           proof->runtime_instance == owner->runtime_instance &&
           proof->body_bytes == UCN_I_SECURITY_RECORD_BYTES &&
           proof->volatile_continuation ==
               slot->durability.volatile_continuation &&
           proof->caller_owner_instance ==
               owner->persistence_business_owner_instance &&
           proof->domain_generation == slot->durability.domain_generation &&
           proof->schema_id == UCN_I_SECURITY_RECORD_SCHEMA_ID &&
           proof->schema_version == UCN_I_SECURITY_RECORD_SCHEMA &&
           proof->operation_kind == UCN_I_SECURITY_OPERATION_KIND &&
           proof->reserved_zero == 0U &&
           proof->persistence_owner_instance ==
               slot->persistence_handle.owner_instance &&
           memcmp(proof->body_digest, slot->expected_published_digest,
                  sizeof(proof->body_digest)) == 0;
}

ucn_result_t ucn_i_security_activate(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    const ucn_i_security_durability_proof_t *proof,
    const ucn_i_security_current_facts_t *facts)
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    uint16_t index;
    ucn_result_t result;

    if (proof == NULL || facts == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), proof,
                             sizeof(*proof)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_AWAITING_DURABILITY ||
        !current_facts_match(slot, facts) ||
        !durability_proof_matches(owner, slot, proof)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        const ucn_i_security_slot_t *existing = &owner->sessions[index];

        if (index != slot_index && existing->occupied != 0U &&
            existing->phase == UCN_I_SECURITY_SESSION_ACTIVE &&
            memcmp(existing->candidate.peer.principal,
                   slot->candidate.peer.principal,
                   UCN_I_SECURITY_PRINCIPAL_BYTES) == 0 &&
            existing->candidate.session_generation >=
                slot->candidate.session_generation) {
            owner_unlock(owner);
            return UCN_ERR_REPLAY;
        }
    }
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        ucn_i_security_slot_t *existing = &owner->sessions[index];

        if (index != slot_index && existing->occupied != 0U &&
            existing->phase == UCN_I_SECURITY_SESSION_ACTIVE &&
            memcmp(existing->candidate.peer.principal,
                   slot->candidate.peer.principal,
                   UCN_I_SECURITY_PRINCIPAL_BYTES) == 0) {
            existing->phase = UCN_I_SECURITY_SESSION_FENCED;
        }
    }
    slot->phase = UCN_I_SECURITY_SESSION_ACTIVE;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_authorize(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_access_request_t *request,
    const ucn_i_security_current_facts_t *facts)
{
    const ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (request == NULL || facts == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts)) ||
        request->service_id == 0U || request->service_id == UINT16_MAX ||
        (request->direction != UCN_I_SECURITY_ACCESS_INBOUND &&
         request->direction != UCN_I_SECURITY_ACCESS_OUTBOUND) ||
        !bytes_zero(request->reserved_zero, sizeof(request->reserved_zero))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, request->session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (acl_rule_matches(owner, slot, request->context_fingerprint,
                         request->service_id, request->protocol_opcode,
                         request->direction)) {
        owner_unlock(owner);
        return UCN_OK;
    }
    owner_unlock(owner);
    return UCN_ERR_ACCESS;
}

static bool c1_origin_material_matches(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_c1_origin_material_t *material)
{
    const ucn_i_security_binding_t *source;
    const ucn_i_security_binding_t *destination;
    uint8_t origin_security;

    if (material->payload_bytes > UINT32_MAX ||
        material->origin_sequence == 0U || material->service_id == 0U ||
        material->service_id == UINT16_MAX ||
        !bytes_zero(material->reserved_zero,
                    sizeof(material->reserved_zero)) ||
        material->common_header[0] != UINT8_C(0x61) ||
        (material->common_header[1] & UINT8_C(0x3F)) != 0U ||
        (material->common_header[2] & UINT8_C(0x3F)) == 0U) {
        return false;
    }
    origin_security = (uint8_t)(material->common_header[2] >> 6U);
    if (origin_security != slot->candidate.origin_level) {
        return false;
    }
    if (material->direction == UCN_I_SECURITY_ACCESS_OUTBOUND) {
        source = &slot->candidate.local;
        destination = &slot->candidate.peer;
    } else if (material->direction == UCN_I_SECURITY_ACCESS_INBOUND) {
        source = &slot->candidate.peer;
        destination = &slot->candidate.local;
    } else {
        return false;
    }
    return material->source_address == source->address &&
           material->destination_address == destination->address &&
           acl_rule_matches(owner, slot, slot->candidate.origin_fingerprint,
                            material->service_id,
                            material->protocol_opcode,
                            material->direction);
}

static ucn_result_t c1_origin_material_lock(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    uint16_t *slot_index_out)
{
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, material->session, slot_index_out) ||
        owner->sessions[*slot_index_out].phase !=
            UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(&owner->sessions[*slot_index_out],
                             &material->facts) ||
        !c1_origin_material_matches(owner,
                                    &owner->sessions[*slot_index_out],
                                    material)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

static void c1_origin_aad_encode(
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_c1_origin_material_t *material,
    uint8_t output[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES])
{
    static const uint8_t domain[] = "UCN6-ORIGIN-V1";
    const ucn_i_security_binding_t *source;
    const ucn_i_security_binding_t *destination;
    size_t offset = 0U;

    if (material->direction == UCN_I_SECURITY_ACCESS_OUTBOUND) {
        source = &slot->candidate.local;
        destination = &slot->candidate.peer;
    } else {
        source = &slot->candidate.peer;
        destination = &slot->candidate.local;
    }
    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    write_be32(output + offset, UINT32_C(54));
    offset += 4U;
    memcpy(output + offset, material->common_header, 3U);
    output[offset + 2U] &= UINT8_C(0xC0);
    offset += 3U;
    output[offset++] = UINT8_C(1);
    memcpy(output + offset, source->principal,
           UCN_I_SECURITY_PRINCIPAL_BYTES);
    offset += UCN_I_SECURITY_PRINCIPAL_BYTES;
    write_be32(output + offset, source->binding_generation);
    offset += 4U;
    memcpy(output + offset, destination->principal,
           UCN_I_SECURITY_PRINCIPAL_BYTES);
    offset += UCN_I_SECURITY_PRINCIPAL_BYTES;
    write_be32(output + offset, destination->binding_generation);
    offset += 4U;
    write_be16(output + offset, material->service_id);
    offset += 2U;
    write_be32(output + offset, material->origin_sequence);
    offset += 4U;
    write_be32(output + offset, (uint32_t)material->payload_bytes);
}

static void sequence_nonce_input_encode(
    const ucn_i_security_slot_t *slot,
    uint32_t origin_sequence,
    uint8_t output[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES])
{
    static const uint8_t domain[] = "UCN6-NONCE-SEQ-V1";
    size_t offset = 0U;

    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    memcpy(output + offset, slot->candidate.origin_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    offset += UCN_I_SECURITY_FINGERPRINT_BYTES;
    write_be32(output + offset, origin_sequence);
}

ucn_result_t ucn_i_security_c1_origin_aad_build(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    uint8_t output[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES])
{
    uint8_t encoded[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES];
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || material == NULL || output == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), material,
                             sizeof(*material)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                             UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES) ||
        ucn_i_ranges_overlap(material, sizeof(*material), output,
                             UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = c1_origin_material_lock(owner, material, &slot_index);
    if (result != UCN_OK) {
        return result;
    }
    c1_origin_aad_encode(&owner->sessions[slot_index], material, encoded);
    memcpy(output, encoded, sizeof(encoded));
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_sequence_nonce_input_build(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    uint8_t output[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES])
{
    uint8_t encoded[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES];
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || material == NULL || output == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), material,
                             sizeof(*material)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                             UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES) ||
        ucn_i_ranges_overlap(material, sizeof(*material), output,
                             UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = c1_origin_material_lock(owner, material, &slot_index);
    if (result != UCN_OK) {
        return result;
    }
    sequence_nonce_input_encode(&owner->sessions[slot_index],
                                material->origin_sequence, encoded);
    memcpy(output, encoded, sizeof(encoded));
    owner_unlock(owner);
    return UCN_OK;
}

static void tx_reservation_clear(
    ucn_i_security_tx_reservation_t *reservation)
{
    uint32_t generation = reservation->generation;

    memset(reservation, 0, sizeof(*reservation));
    reservation->generation = generation;
}

static bool tx_handle_matches(
    const ucn_i_security_slot_t *slot,
    ucn_i_security_tx_handle_t handle)
{
    return handle.reservation_generation != 0U &&
           slot->origin_tx_reservation.valid != 0U &&
           handle.reservation_generation ==
               slot->origin_tx_reservation.generation &&
           handle.origin_sequence ==
               slot->origin_tx_reservation.origin_sequence &&
           handle.deadline_us == slot->origin_tx_reservation.deadline_us;
}

ucn_result_t ucn_i_security_tx_reserve(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_access_request_t *request,
    const ucn_i_security_current_facts_t *facts,
    ucn_i_security_tx_handle_t *handle_out)
{
    ucn_i_security_tx_handle_t handle;
    ucn_i_security_slot_t *slot;
    uint64_t deadline;
    uint16_t slot_index;
    uint32_t generation;
    uint32_t sequence;
    ucn_result_t result;

    if (owner == NULL || request == NULL || facts == NULL ||
        handle_out == NULL || request->direction !=
            UCN_I_SECURITY_ACCESS_OUTBOUND ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, request->session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts) ||
        !acl_rule_matches(owner, slot, request->context_fingerprint,
                          request->service_id, request->protocol_opcode,
                          request->direction)) {
        owner_unlock(owner);
        return UCN_ERR_ACCESS;
    }
    if (slot->origin_tx_reservation.valid != 0U) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (slot->origin_tx_highest_reserved == UINT32_MAX ||
        facts->now_us > UINT64_MAX - owner->replay_reservation_lifetime_us) {
        owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    generation = slot->origin_tx_reservation.generation + 1U;
    if (generation == 0U) {
        owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    sequence = slot->origin_tx_highest_reserved + 1U;
    deadline = facts->now_us + owner->replay_reservation_lifetime_us;
    memset(&slot->origin_tx_reservation, 0,
           sizeof(slot->origin_tx_reservation));
    slot->origin_tx_reservation.deadline_us = deadline;
    memcpy(slot->origin_tx_reservation.context_fingerprint,
           request->context_fingerprint,
           sizeof(slot->origin_tx_reservation.context_fingerprint));
    slot->origin_tx_reservation.origin_sequence = sequence;
    slot->origin_tx_reservation.generation = generation;
    slot->origin_tx_reservation.service_id = request->service_id;
    slot->origin_tx_reservation.protocol_opcode = request->protocol_opcode;
    slot->origin_tx_reservation.direction = request->direction;
    slot->origin_tx_reservation.valid = 1U;
    slot->origin_tx_highest_reserved = sequence;
    memset(&handle, 0, sizeof(handle));
    handle.session = request->session;
    handle.deadline_us = deadline;
    handle.origin_sequence = sequence;
    handle.reservation_generation = generation;
    *handle_out = handle;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_tx_commit(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t handle,
    const ucn_i_security_current_facts_t *facts)
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || facts == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle.session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts) ||
        !tx_handle_matches(slot, handle) ||
        facts->now_us >= handle.deadline_us) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    tx_reservation_clear(&slot->origin_tx_reservation);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_tx_abort(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t handle)
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle.session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (!tx_handle_matches(slot, handle)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    tx_reservation_clear(&slot->origin_tx_reservation);
    owner_unlock(owner);
    return UCN_OK;
}

static bool replay_is_committed(const ucn_i_security_replay_window_t *window,
                                uint64_t sequence)
{
    uint64_t distance;

    if (window->highest_committed == 0U ||
        sequence > window->highest_committed) {
        return false;
    }
    distance = window->highest_committed - sequence;
    return distance >= 64U ||
           (window->committed_bitmap & (UINT64_C(1) << distance)) != 0U;
}

static bool replay_is_inflight(const ucn_i_security_replay_window_t *window,
                               uint64_t sequence)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_SECURITY_REPLAY_RESERVATION_COUNT;
         ++index) {
        if (window->reservations[index].valid != 0U &&
            window->reservations[index].sequence == sequence) {
            return true;
        }
    }
    return false;
}

static uint16_t replay_free_slot(const ucn_i_security_replay_window_t *window)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_SECURITY_REPLAY_RESERVATION_COUNT;
         ++index) {
        if (window->reservations[index].valid == 0U) {
            return index;
        }
    }
    return UINT16_MAX;
}

static bool replay_handle_matches(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *session_slot,
    ucn_i_security_replay_handle_t handle,
    uint16_t *reservation_slot_out)
{
    uint16_t index = handle.reservation_slot;

    if (!bytes_zero(handle.reserved_zero, sizeof(handle.reserved_zero)) ||
        handle.has_hop > 1U ||
        (handle.has_hop == 0U &&
         (handle.hop_sequence != 0U ||
          handle.hop_reservation_generation != 0U ||
          handle.hop_reservation_slot != 0U)) ||
        index >= UCN_I_SECURITY_REPLAY_RESERVATION_COUNT ||
        session_slot->origin_replay.reservations[index].valid == 0U ||
        session_slot->origin_replay.reservations[index].generation !=
            handle.reservation_generation ||
        session_slot->origin_replay.reservations[index].sequence !=
            handle.sequence) {
        return false;
    }
    (void)owner;
    *reservation_slot_out = index;
    return true;
}

static bool hop_replay_handle_matches(
    const ucn_i_security_slot_t *session_slot,
    ucn_i_security_replay_handle_t handle,
    uint16_t *reservation_slot_out)
{
    uint16_t index = handle.hop_reservation_slot;

    if (handle.has_hop != 1U ||
        index >= UCN_I_SECURITY_REPLAY_RESERVATION_COUNT ||
        session_slot->hop_replay.reservations[index].valid == 0U ||
        session_slot->hop_replay.reservations[index].generation !=
            handle.hop_reservation_generation ||
        session_slot->hop_replay.reservations[index].sequence !=
            handle.hop_sequence) {
        return false;
    }
    *reservation_slot_out = index;
    return true;
}

static ucn_result_t hop_replay_reserve_locked(
    ucn_i_security_owner_t *owner,
    ucn_i_security_slot_t *slot,
    const ucn_i_security_current_facts_t *facts,
    uint32_t sequence,
    const uint8_t tag_digest[16],
    ucn_i_security_replay_handle_t *handle)
{
    ucn_i_security_replay_reservation_t *reservation;
    uint16_t reservation_slot;
    uint32_t generation;

    if (sequence == 0U ||
        replay_is_committed(&slot->hop_replay, sequence)) {
        return UCN_ERR_REPLAY;
    }
    if (replay_is_inflight(&slot->hop_replay, sequence)) {
        return UCN_ERR_STATE;
    }
    reservation_slot = replay_free_slot(&slot->hop_replay);
    if (reservation_slot == UINT16_MAX ||
        facts->now_us > UINT64_MAX - owner->replay_reservation_lifetime_us) {
        return reservation_slot == UINT16_MAX ? UCN_ERR_NO_SPACE :
                                                UCN_ERR_EXHAUSTED;
    }
    reservation = &slot->hop_replay.reservations[reservation_slot];
    generation = reservation->generation + 1U;
    if (generation == 0U) {
        return UCN_ERR_EXHAUSTED;
    }
    memset(reservation, 0, sizeof(*reservation));
    reservation->sequence = sequence;
    reservation->deadline_us = facts->now_us +
                                owner->replay_reservation_lifetime_us;
    memcpy(reservation->aad_digest, tag_digest, 16U);
    memcpy(reservation->payload_digest, tag_digest, 16U);
    reservation->generation = generation;
    reservation->valid = 1U;
    handle->has_hop = 1U;
    handle->hop_sequence = sequence;
    handle->hop_reservation_generation = generation;
    handle->hop_reservation_slot = reservation_slot;
    return UCN_OK;
}

static ucn_result_t replay_reserve_locked(
    ucn_i_security_owner_t *owner,
    ucn_i_security_slot_t *slot,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    uint64_t sequence,
    const uint8_t aad_digest[16],
    const uint8_t payload_digest[16],
    ucn_i_security_replay_handle_t *handle_out)
{
    ucn_i_security_replay_handle_t handle;
    ucn_i_security_replay_reservation_t *reservation;
    uint64_t deadline;
    uint16_t reservation_slot;
    uint32_t generation;

    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts)) {
        return UCN_ERR_STATE;
    }
    if (replay_is_committed(&slot->origin_replay, sequence)) {
        return UCN_ERR_REPLAY;
    }
    if (replay_is_inflight(&slot->origin_replay, sequence)) {
        return UCN_ERR_STATE;
    }
    reservation_slot = replay_free_slot(&slot->origin_replay);
    if (reservation_slot == UINT16_MAX || facts->now_us == 0U ||
        owner->replay_reservation_lifetime_us == 0U ||
        facts->now_us > UINT64_MAX - owner->replay_reservation_lifetime_us) {
        return reservation_slot == UINT16_MAX ? UCN_ERR_NO_SPACE :
                                                UCN_ERR_EXHAUSTED;
    }
    reservation = &slot->origin_replay.reservations[reservation_slot];
    generation = reservation->generation + 1U;
    if (generation == 0U) {
        return UCN_ERR_EXHAUSTED;
    }
    deadline = facts->now_us + owner->replay_reservation_lifetime_us;
    memset(reservation, 0, sizeof(*reservation));
    reservation->sequence = sequence;
    reservation->deadline_us = deadline;
    memcpy(reservation->aad_digest, aad_digest, 16U);
    memcpy(reservation->payload_digest, payload_digest, 16U);
    reservation->generation = generation;
    reservation->valid = 1U;
    memset(&handle, 0, sizeof(handle));
    handle.session = session;
    handle.sequence = sequence;
    handle.reservation_generation = generation;
    handle.reservation_slot = reservation_slot;
    *handle_out = handle;
    return UCN_OK;
}

ucn_result_t ucn_i_security_replay_reserve(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    uint64_t sequence,
    const uint8_t aad_digest[16],
    const uint8_t payload_digest[16],
    ucn_i_security_replay_handle_t *handle_out)
{
    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    ucn_result_t result;

    if (facts == NULL || sequence == 0U || aad_digest == NULL ||
        payload_digest == NULL || handle_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), aad_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(aad_digest, 16U, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(payload_digest, 16U, handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    result = replay_reserve_locked(owner, slot, session, facts, sequence,
                                   aad_digest, payload_digest, handle_out);
    owner_unlock(owner);
    return result;
}

static void replay_reservation_clear(
    ucn_i_security_replay_reservation_t *reservation);

static bool protect_ranges_valid(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    size_t plaintext_bytes = material == NULL ? 0U : material->payload_bytes;

    return owner != NULL && material != NULL && workspace != NULL &&
           output != NULL && output_bytes != NULL &&
           (plaintext != NULL || plaintext_bytes == 0U) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), material,
                                 sizeof(*material)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext,
                                 plaintext_bytes) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(material, sizeof(*material), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(material, sizeof(*material), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(material, sizeof(*material), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                                 sizeof(*output_bytes));
}

static void hop_aad_encode(
    const uint8_t *packet_before_hop,
    size_t packet_before_hop_bytes,
    uint32_t hop_sequence,
    const uint8_t hop_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES],
    uint8_t *output,
    size_t *output_bytes)
{
    static const uint8_t domain[] = "UCN6-HOP-V1";
    size_t offset = 0U;

    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    write_be32(output + offset,
               (uint32_t)(packet_before_hop_bytes + 4U +
                          UCN_I_SECURITY_FINGERPRINT_BYTES));
    offset += 4U;
    memcpy(output + offset, packet_before_hop, packet_before_hop_bytes);
    offset += packet_before_hop_bytes;
    write_be32(output + offset, hop_sequence);
    offset += 4U;
    memcpy(output + offset, hop_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    offset += UCN_I_SECURITY_FINGERPRINT_BYTES;
    *output_bytes = offset;
}

ucn_result_t ucn_i_security_c1_protect(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t tx,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    ucn_i_security_slot_t *slot;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;
    uint16_t slot_index;

    if (!protect_ranges_valid(owner, material, plaintext, workspace,
                              output, output_capacity, output_bytes) ||
        material->direction != UCN_I_SECURITY_ACCESS_OUTBOUND ||
        memcmp(&tx.session, &material->session,
               sizeof(tx.session)) != 0 ||
        tx.origin_sequence != material->origin_sequence) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, tx.session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    memset(workspace, 0, sizeof(*workspace));
    workspace->protect_control.header_bytes =
        c1_header_bytes(owner->address_width);
    workspace->protect_control.hop_trailer_bytes =
        slot->candidate.hop_profile ==
                            UCN_I_HOP_PROFILE_H1 ?
                            UCN_I_SECURITY_HOP_TRAILER_BYTES : 0U;
    if (ucn_i_size_add(workspace->protect_control.header_bytes,
                       material->payload_bytes,
                       &workspace->protect_control.packet_bytes) != UCN_OK ||
        ucn_i_size_add(workspace->protect_control.packet_bytes,
                       UCN_I_SECURITY_ORIGIN_TAG_BYTES,
                       &workspace->protect_control.packet_bytes) != UCN_OK ||
        ucn_i_size_add(workspace->protect_control.packet_bytes,
                       workspace->protect_control.hop_trailer_bytes,
                       &workspace->protect_control.packet_bytes) != UCN_OK ||
        workspace->protect_control.packet_bytes > UCN_ADAPTER_FRAME_BYTES ||
        workspace->protect_control.packet_bytes > output_capacity) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        (slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H0 &&
         slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1) ||
        !tx_handle_matches(slot, tx) ||
        memcmp(slot->origin_tx_reservation.context_fingerprint,
               slot->candidate.origin_fingerprint,
               UCN_I_SECURITY_FINGERPRINT_BYTES) != 0 ||
        slot->origin_tx_reservation.service_id != material->service_id ||
        slot->origin_tx_reservation.protocol_opcode !=
            material->protocol_opcode ||
        slot->origin_tx_reservation.direction != material->direction ||
        material->payload_bytes > UCN_ADAPTER_FRAME_BYTES ||
        !current_facts_match(slot, &material->facts) ||
        !c1_origin_material_matches(owner, slot, material) ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return (slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H0 ||
                slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H1) ?
                   UCN_ERR_STATE : UCN_ERR_UNSUPPORTED;
    }
    memcpy(workspace->packet, material->common_header, 3U);
    workspace->protect_control.offset = 3U;
    write_address_be(workspace->packet + workspace->protect_control.offset,
                     material->source_address, owner->address_width);
    workspace->protect_control.offset += owner->address_width;
    write_address_be(workspace->packet + workspace->protect_control.offset,
                     material->destination_address, owner->address_width);
    workspace->protect_control.offset += owner->address_width;
    write_be16(workspace->packet + workspace->protect_control.offset,
               material->service_id);
    workspace->protect_control.offset += 2U;
    write_be32(workspace->packet + workspace->protect_control.offset,
               material->origin_sequence);
    workspace->protect_control.offset += 4U;
    if (workspace->protect_control.offset !=
        workspace->protect_control.header_bytes) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (material->payload_bytes != 0U) {
        memcpy(workspace->plaintext, plaintext, material->payload_bytes);
    }
    c1_origin_aad_encode(slot, material, workspace->origin_aad);
    if (slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL) {
        sequence_nonce_input_encode(slot, material->origin_sequence,
                                    workspace->nonce_input);
    }
    memset(&workspace->origin_protect, 0,
           sizeof(workspace->origin_protect));
    workspace->origin_protect.selector = slot->candidate.origin_tx;
    workspace->origin_protect.nonce_input =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            workspace->nonce_input : NULL;
    workspace->origin_protect.nonce_input_bytes =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES : 0U;
    workspace->origin_protect.aad = workspace->origin_aad;
    workspace->origin_protect.aad_bytes =
        UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES;
    workspace->origin_protect.plaintext = workspace->plaintext;
    workspace->origin_protect.payload_bytes = material->payload_bytes;
    workspace->origin_protect.protected_payload =
        workspace->packet + workspace->protect_control.header_bytes;
    workspace->origin_protect.origin_tag =
        workspace->packet + workspace->protect_control.header_bytes +
        material->payload_bytes;
    workspace->protect_control.packet_before_hop_bytes =
        workspace->protect_control.header_bytes + material->payload_bytes +
        UCN_I_SECURITY_ORIGIN_TAG_BYTES;
    if (slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H1) {
        if (slot->hop_tx_highest_reserved == UINT32_MAX) {
            owner_unlock(owner);
            return UCN_ERR_EXHAUSTED;
        }
        workspace->protect_control.hop_sequence =
            slot->hop_tx_highest_reserved + 1U;
        slot->hop_tx_highest_reserved =
            workspace->protect_control.hop_sequence;
        write_be32(workspace->packet +
                       workspace->protect_control.packet_before_hop_bytes,
                   workspace->protect_control.hop_sequence);
    }
    workspace->candidate = slot->candidate;
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_candidate = workspace->candidate;
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = UCN_I_SECURITY_CALLBACK_PROTECT;
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        workspace->protect_control.provider_invoked = 1U;
        result = owner->provider.protect_origin(
            owner->provider.context, &workspace->origin_protect);
        if (result == UCN_OK &&
            workspace->protect_control.hop_trailer_bytes != 0U) {
            memset(&workspace->hop_protect, 0,
                   sizeof(workspace->hop_protect));
            workspace->hop_protect.selector = workspace->candidate.hop_tx;
            workspace->hop_protect.aad = workspace->hop_aad;
            hop_aad_encode(
                workspace->packet,
                workspace->protect_control.packet_before_hop_bytes,
                workspace->protect_control.hop_sequence,
                workspace->candidate.hop_fingerprint,
                workspace->hop_aad, &workspace->hop_protect.aad_bytes);
            workspace->hop_protect.hop_tag =
                workspace->packet +
                workspace->protect_control.packet_before_hop_bytes + 4U;
            result = owner->provider.protect_hop(
                owner->provider.context, &workspace->hop_protect);
        }
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        !current_facts_match(slot, &material->facts) ||
        !tx_handle_matches(slot, tx)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (workspace->protect_control.provider_invoked != 0U) {
        tx_reservation_clear(&slot->origin_tx_reservation);
    }
    if (result == UCN_OK &&
        workspace->candidate.origin_level ==
            UCN_I_SECURITY_AUTHENTICATED &&
        material->payload_bytes != 0U &&
        memcmp(workspace->packet + workspace->protect_control.header_bytes,
               workspace->plaintext,
               material->payload_bytes) != 0) {
        result = UCN_ERR_SECURITY;
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    memcpy(output, workspace->packet,
           workspace->protect_control.packet_bytes);
    *output_bytes = workspace->protect_control.packet_bytes;
    clear_callback(owner);
    owner_unlock(owner);
    return UCN_OK;
}

static bool open_ranges_valid(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_access_request_t *access,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_replay_handle_t *replay_out)
{
    return owner != NULL && facts != NULL && access != NULL &&
           packet != NULL && workspace != NULL && plaintext_output != NULL &&
           plaintext_bytes != NULL && replay_out != NULL &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                                 sizeof(*facts)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), access,
                                 sizeof(*access)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), packet,
                                 packet_bytes) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(access, sizeof(*access), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(access, sizeof(*access), plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(access, sizeof(*access), plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(access, sizeof(*access), replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                                 plaintext_output, plaintext_capacity) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                                 plaintext_bytes, sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                                 plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                                 replay_out, sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(plaintext_bytes, sizeof(*plaintext_bytes),
                                 replay_out, sizeof(*replay_out));
}

ucn_result_t ucn_i_security_c1_open(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_access_request_t *access,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_replay_handle_t *replay_out)
{
    ucn_i_security_slot_t *slot;
    const uint8_t *origin_tag;
    const uint8_t *hop_tag = NULL;
    size_t header_bytes;
    size_t payload_bytes;
    size_t hop_aad_bytes = 0U;
    size_t hop_trailer_bytes;
    size_t packet_before_hop_bytes;
    size_t offset;
    uint16_t slot_index;
    uint32_t hop_sequence = 0U;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;

    if (!open_ranges_valid(owner, facts, access, packet, packet_bytes,
                           workspace, plaintext_output, plaintext_capacity,
                           plaintext_bytes, replay_out) ||
        access->direction != UCN_I_SECURITY_ACCESS_INBOUND ||
        memcmp(&session, &access->session, sizeof(session)) != 0 ||
        packet_bytes > UCN_ADAPTER_FRAME_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H0 &&
        slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1) {
        owner_unlock(owner);
        return UCN_ERR_UNSUPPORTED;
    }
    hop_trailer_bytes = slot->candidate.hop_profile ==
                            UCN_I_HOP_PROFILE_H1 ?
                            UCN_I_SECURITY_HOP_TRAILER_BYTES : 0U;
    header_bytes = c1_header_bytes(owner->address_width);
    if (packet_bytes < header_bytes + UCN_I_SECURITY_ORIGIN_TAG_BYTES +
                           hop_trailer_bytes) {
        owner_unlock(owner);
        return UCN_ERR_MALFORMED;
    }
    payload_bytes = packet_bytes - header_bytes -
                    UCN_I_SECURITY_ORIGIN_TAG_BYTES - hop_trailer_bytes;
    if (payload_bytes > plaintext_capacity) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    memcpy(workspace->packet, packet, packet_bytes);
    memset(&workspace->material, 0, sizeof(workspace->material));
    workspace->material.session = session;
    workspace->material.facts = *facts;
    workspace->material.payload_bytes = payload_bytes;
    workspace->material.common_header[0] = workspace->packet[0];
    workspace->material.common_header[1] = workspace->packet[1];
    workspace->material.common_header[2] = workspace->packet[2];
    offset = 3U;
    workspace->material.source_address =
        read_address_be(workspace->packet + offset, owner->address_width);
    offset += owner->address_width;
    workspace->material.destination_address =
        read_address_be(workspace->packet + offset, owner->address_width);
    offset += owner->address_width;
    workspace->material.service_id = read_be16(workspace->packet + offset);
    offset += 2U;
    workspace->material.origin_sequence =
        read_be32(workspace->packet + offset);
    workspace->material.protocol_opcode = access->protocol_opcode;
    workspace->material.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    if (offset + 4U != header_bytes ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        access->protocol_opcode != 0U ||
        access->service_id == 0U || access->service_id == UINT16_MAX ||
        access->service_id != workspace->material.service_id ||
        !bytes_zero(access->reserved_zero,
                    sizeof(access->reserved_zero)) ||
        memcmp(access->context_fingerprint,
               slot->candidate.origin_fingerprint,
               UCN_I_SECURITY_FINGERPRINT_BYTES) != 0 ||
        !current_facts_match(slot, facts) ||
        !c1_origin_material_matches(owner, slot, &workspace->material) ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    c1_origin_aad_encode(slot, &workspace->material,
                         workspace->origin_aad);
    if (slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL) {
        sequence_nonce_input_encode(slot,
                                    workspace->material.origin_sequence,
                                    workspace->nonce_input);
    }
    origin_tag = workspace->packet + header_bytes + payload_bytes;
    packet_before_hop_bytes = header_bytes + payload_bytes +
                              UCN_I_SECURITY_ORIGIN_TAG_BYTES;
    if (hop_trailer_bytes != 0U) {
        hop_sequence = read_be32(workspace->packet +
                                 packet_before_hop_bytes);
        if (hop_sequence == 0U) {
            owner_unlock(owner);
            return UCN_ERR_MALFORMED;
        }
        hop_tag = workspace->packet + packet_before_hop_bytes + 4U;
        hop_aad_encode(workspace->packet, packet_before_hop_bytes,
                       hop_sequence, slot->candidate.hop_fingerprint,
                       workspace->hop_aad, &hop_aad_bytes);
    }
    memset(&workspace->origin_open, 0, sizeof(workspace->origin_open));
    workspace->origin_open.selector = slot->candidate.origin_rx;
    workspace->origin_open.nonce_input =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            workspace->nonce_input : NULL;
    workspace->origin_open.nonce_input_bytes =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES : 0U;
    workspace->origin_open.aad = workspace->origin_aad;
    workspace->origin_open.aad_bytes =
        UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES;
    workspace->origin_open.protected_payload =
        workspace->packet + header_bytes;
    workspace->origin_open.origin_tag = origin_tag;
    workspace->origin_open.payload_bytes = payload_bytes;
    workspace->origin_open.plaintext = workspace->plaintext;
    workspace->candidate = slot->candidate;
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_candidate = workspace->candidate;
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = UCN_I_SECURITY_CALLBACK_OPEN;
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        if (hop_trailer_bytes != 0U) {
            memset(&workspace->hop_verify, 0,
                   sizeof(workspace->hop_verify));
            workspace->hop_verify.selector = workspace->candidate.hop_rx;
            workspace->hop_verify.aad = workspace->hop_aad;
            workspace->hop_verify.aad_bytes = hop_aad_bytes;
            workspace->hop_verify.hop_tag = hop_tag;
            result = owner->provider.verify_hop(owner->provider.context,
                                                 &workspace->hop_verify);
        } else {
            result = UCN_OK;
        }
        if (result == UCN_OK) {
            result = owner->provider.open_origin(owner->provider.context,
                                                  &workspace->origin_open);
        }
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        !current_facts_match(slot, facts) ||
        !c1_origin_material_matches(owner, slot, &workspace->material)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    if (workspace->candidate.origin_level ==
            UCN_I_SECURITY_AUTHENTICATED &&
        payload_bytes != 0U &&
        memcmp(workspace->plaintext, workspace->packet + header_bytes,
               payload_bytes) != 0) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    memcpy(workspace->digest, origin_tag,
           UCN_I_SECURITY_ORIGIN_TAG_BYTES);
    result = replay_reserve_locked(owner, slot, session, facts,
                                   workspace->material.origin_sequence,
                                   workspace->digest, workspace->digest,
                                   &workspace->replay);
    if (result == UCN_OK && hop_trailer_bytes != 0U) {
        memset(workspace->digest + UCN_I_SECURITY_ORIGIN_TAG_BYTES,
               0, UCN_I_SECURITY_ORIGIN_TAG_BYTES);
        memcpy(workspace->digest + UCN_I_SECURITY_ORIGIN_TAG_BYTES,
               hop_tag, UCN_I_SECURITY_HOP_TAG_BYTES);
        result = hop_replay_reserve_locked(owner, slot, facts,
            hop_sequence,
            workspace->digest + UCN_I_SECURITY_ORIGIN_TAG_BYTES,
            &workspace->replay);
        if (result != UCN_OK) {
            replay_reservation_clear(
                &slot->origin_replay.reservations[
                    workspace->replay.reservation_slot]);
        }
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    if (payload_bytes != 0U) {
        memcpy(plaintext_output, workspace->plaintext, payload_bytes);
    }
    *plaintext_bytes = payload_bytes;
    *replay_out = workspace->replay;
    clear_callback(owner);
    owner_unlock(owner);
    return UCN_OK;
}

static void replay_reservation_clear(
    ucn_i_security_replay_reservation_t *reservation)
{
    uint32_t generation = reservation->generation;

    memset(reservation, 0, sizeof(*reservation));
    reservation->generation = generation;
}

static void replay_window_commit(ucn_i_security_replay_window_t *window,
                                 uint64_t sequence)
{
    uint64_t distance;

    if (window->highest_committed == 0U ||
        sequence > window->highest_committed) {
        distance = window->highest_committed == 0U ? 64U :
            sequence - window->highest_committed;
        window->committed_bitmap = distance >= 64U ? 0U :
            window->committed_bitmap << distance;
        window->highest_committed = sequence;
        window->committed_bitmap |= UINT64_C(1);
    } else {
        distance = window->highest_committed - sequence;
        window->committed_bitmap |= UINT64_C(1) << distance;
    }
}

ucn_result_t ucn_i_security_replay_commit(
    ucn_i_security_owner_t *owner,
    ucn_i_security_replay_handle_t handle,
    const ucn_i_security_current_facts_t *facts)
{
    ucn_i_security_replay_reservation_t *reservation;
    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    uint16_t reservation_slot;
    uint16_t hop_reservation_slot = 0U;
    ucn_result_t result;

    if (facts == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle.session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    if (!replay_handle_matches(owner, slot, handle, &reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    reservation = &slot->origin_replay.reservations[reservation_slot];
    if (handle.has_hop != 0U &&
        !hop_replay_handle_matches(slot, handle, &hop_reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts) ||
        facts->now_us >= reservation->deadline_us ||
        (handle.has_hop != 0U &&
         facts->now_us >= slot->hop_replay
                              .reservations[hop_reservation_slot]
                              .deadline_us) ||
        replay_is_committed(&slot->origin_replay, handle.sequence)) {
        replay_reservation_clear(reservation);
        if (handle.has_hop != 0U) {
            replay_reservation_clear(
                &slot->hop_replay.reservations[hop_reservation_slot]);
        }
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (handle.has_hop != 0U &&
        replay_is_committed(&slot->hop_replay, handle.hop_sequence)) {
        replay_reservation_clear(reservation);
        replay_reservation_clear(
            &slot->hop_replay.reservations[hop_reservation_slot]);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    replay_window_commit(&slot->origin_replay, handle.sequence);
    if (handle.has_hop != 0U) {
        replay_window_commit(&slot->hop_replay, handle.hop_sequence);
        replay_reservation_clear(
            &slot->hop_replay.reservations[hop_reservation_slot]);
    }
    replay_reservation_clear(reservation);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_replay_abort(
    ucn_i_security_owner_t *owner,
    ucn_i_security_replay_handle_t handle)
{
    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    uint16_t reservation_slot;
    uint16_t hop_reservation_slot = 0U;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle.session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    if (!replay_handle_matches(owner, slot, handle, &reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (handle.has_hop != 0U &&
        !hop_replay_handle_matches(slot, handle, &hop_reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    replay_reservation_clear(
        &slot->origin_replay.reservations[reservation_slot]);
    if (handle.has_hop != 0U) {
        replay_reservation_clear(
            &slot->hop_replay.reservations[hop_reservation_slot]);
    }
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_replay_maintain(
    ucn_i_security_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *expired_out)
{
    uint16_t inspected = 0U;
    uint16_t expired = 0U;
    uint16_t per_session = (uint16_t)(
        (2U * UCN_I_SECURITY_REPLAY_RESERVATION_COUNT) + 1U);
    uint16_t total = (uint16_t)(UCN_I_SECURITY_SESSION_COUNT * per_session);
    ucn_result_t result;

    if (now_us == 0U || budget == 0U || inspected_out == NULL ||
        expired_out == NULL || owner == NULL ||
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
    while (inspected < budget && inspected < total) {
        uint16_t flat = owner->replay_maintenance_cursor;
        uint16_t session_index = (uint16_t)(flat / per_session);
        uint16_t reservation_index = (uint16_t)(flat % per_session);

        if (reservation_index == 0U) {
            ucn_i_security_tx_reservation_t *tx =
                &owner->sessions[session_index].origin_tx_reservation;

            if (tx->valid != 0U && now_us >= tx->deadline_us) {
                tx_reservation_clear(tx);
                expired++;
            }
        } else if (reservation_index <=
                   UCN_I_SECURITY_REPLAY_RESERVATION_COUNT) {
            ucn_i_security_replay_reservation_t *reservation =
                &owner->sessions[session_index]
                     .origin_replay.reservations[reservation_index - 1U];

            if (reservation->valid != 0U &&
                now_us >= reservation->deadline_us) {
                replay_reservation_clear(reservation);
                expired++;
            }
        } else {
            uint16_t hop_index = (uint16_t)(
                reservation_index -
                UCN_I_SECURITY_REPLAY_RESERVATION_COUNT - 1U);
            ucn_i_security_replay_reservation_t *reservation =
                &owner->sessions[session_index]
                     .hop_replay.reservations[hop_index];

            if (reservation->valid != 0U &&
                now_us >= reservation->deadline_us) {
                replay_reservation_clear(reservation);
                expired++;
            }
        }
        owner->replay_maintenance_cursor = (uint16_t)((flat + 1U) % total);
        inspected++;
    }
    *inspected_out = inspected;
    *expired_out = expired;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_session_get(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_i_security_session_view_t *view_out)
{
    ucn_i_security_session_view_t view;
    const ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (view_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    memset(&view, 0, sizeof(view));
    view.peer = slot->candidate.peer;
    view.expires_at_us = slot->candidate.expires_at_us;
    view.link_generation = slot->candidate.link_generation;
    view.session_generation = slot->candidate.session_generation;
    view.policy_generation = slot->candidate.policy_generation;
    view.origin_level = slot->candidate.origin_level;
    view.hop_profile = slot->candidate.hop_profile;
    view.phase = slot->phase;
    *view_out = view;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_fence(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle)
{
    uint16_t slot_index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->sessions[slot_index].phase = UCN_I_SECURITY_SESSION_FENCED;
    memset(&owner->sessions[slot_index].origin_tx_reservation, 0,
           sizeof(owner->sessions[slot_index].origin_tx_reservation));
    memset(&owner->sessions[slot_index].origin_replay, 0,
           sizeof(owner->sessions[slot_index].origin_replay));
    memset(&owner->sessions[slot_index].hop_replay, 0,
           sizeof(owner->sessions[slot_index].hop_replay));
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_retire_fenced(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle)
{
    uint32_t slot_generation;
    uint16_t slot_index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, handle, &slot_index) ||
        owner->sessions[slot_index].phase !=
            UCN_I_SECURITY_SESSION_FENCED) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot_generation = owner->sessions[slot_index].slot_generation;
    memset(&owner->sessions[slot_index], 0,
           sizeof(owner->sessions[slot_index]));
    owner->sessions[slot_index].slot_generation = slot_generation;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_owner_destroy(ucn_i_security_owner_t *owner)
{
    uint16_t index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        if (owner->sessions[index].occupied != 0U) {
            owner_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    owner_unlock(owner);
    memset(owner, 0, sizeof(*owner));
    return UCN_OK;
}
