#include "../internal/ucn_v6_security_private.h"

#include <string.h>

#define UCN_V6_BOOTSTRAP_CANONICAL_BYTES ((size_t)196U)
#define UCN_V6_ACL_CANONICAL_BYTES ((size_t)92U)

typedef char ucn_v6_security_storage_size_check[
    sizeof(ucn_v6_security_manager_t) <= UCN_V6_SECURITY_MANAGER_STORAGE_BYTES ?
        1 : -1];

static void put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void put_u64(uint8_t *output, uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool principal_equal(
    const ucn_v6_principal_t *left,
    const ucn_v6_principal_t *right)
{
    return memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool binding_equal(
    const ucn_v6_binding_key_t *left,
    const ucn_v6_binding_key_t *right)
{
    return left->realm_id == right->realm_id &&
           left->node_address == right->node_address &&
           left->binding_generation == right->binding_generation;
}

static bool bootstrap_transcript_is_valid(
    const ucn_v6_bootstrap_transcript_t *transcript)
{
    return transcript != NULL &&
           transcript->protocol_version == UCN_V6_PROTOCOL_VERSION &&
           transcript->bootstrap_header_contract != 0U &&
           (transcript->flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ||
            transcript->flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH) &&
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
           !bytes_are_zero(transcript->binding_lease_id,
                           sizeof(transcript->binding_lease_id)) &&
           transcript->binding_lease_duration_us != 0U &&
           transcript->authority_lease_sequence != 0U &&
           transcript->authority_lease_sequence <=
               UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
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
           !bytes_are_zero(transcript->prior_messages_hash,
                           sizeof(transcript->prior_messages_hash));
}

static bool suite_is_valid(uint8_t suite_id, ucn_v6_e2e_mode_t mode)
{
    return ((mode == UCN_V6_E2E_NONE || mode == UCN_V6_E2E_AUTH_ONLY) &&
            suite_id == UCN_V6_SUITE_HMAC_SHA256_128) ||
           (mode == UCN_V6_E2E_AEAD &&
            (suite_id == UCN_V6_SUITE_AES_GCM_128 ||
             suite_id == UCN_V6_SUITE_CHACHA20_POLY1305));
}

static bool selector_is_valid(
    const ucn_v6_key_selector_t *selector,
    ucn_v6_e2e_mode_t mode)
{
    return selector != NULL && selector->suite_id != 0U &&
           selector->key_id != 0U && selector->key_generation != 0U &&
           selector->key_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           suite_is_valid(selector->suite_id, mode);
}

static bool selector_is_zero(const ucn_v6_key_selector_t *selector)
{
    return selector->suite_id == 0U && selector->key_id == 0U &&
           selector->key_generation == 0U;
}

static bool selector_equal(
    const ucn_v6_key_selector_t *left,
    const ucn_v6_key_selector_t *right)
{
    return left->suite_id == right->suite_id &&
           left->key_id == right->key_id &&
           left->key_generation == right->key_generation;
}

static bool manager_storage_is_valid(
    const ucn_v6_security_manager_t *manager)
{
    return manager != NULL && manager->initialized &&
           manager->magic == UCN_V6_SECURITY_MANAGER_MAGIC &&
           manager->schema == UCN_V6_STORAGE_LAYOUT &&
           manager->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           manager->canary == UCN_V6_SECURITY_MANAGER_CANARY;
}

static bool crypto_ops_are_valid(const ucn_v6_security_crypto_ops_t *crypto)
{
    return crypto != NULL && crypto->verify_proof != NULL &&
           crypto->verify_tag != NULL &&
           crypto->compute_tag != NULL && crypto->seal_aead != NULL &&
           crypto->open_aead != NULL;
}

static bool store_ops_are_valid(const ucn_v6_security_store_ops_t *store)
{
    return store != NULL && store->load_generation_witness != NULL &&
           store->reserve_generation_witness != NULL &&
           store->load != NULL && store->submit != NULL;
}

static bool replay_is_valid(const ucn_v6_replay_window_t *window)
{
    if (!window->initialized) {
        return window->highest_sequence == 0U && window->seen_bitmap == 0U &&
               window->durable_reserved_through == 0U;
    }
    return window->highest_sequence != 0U &&
           window->durable_reserved_through >= window->highest_sequence &&
           window->durable_reserved_through <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           (window->seen_bitmap & UINT64_C(1)) != 0U;
}

static bool session_is_valid(
    const ucn_v6_security_session_record_t *session,
    uint32_t realm_id,
    const ucn_v6_principal_t *local_principal)
{
    if (!session->occupied) {
        return bytes_are_zero((const uint8_t *)session, sizeof(*session));
    }
    return ((session->admitted && !session->revoked &&
             !session->requires_reauth) ||
            (!session->admitted && session->revoked &&
             !session->requires_reauth) ||
            (!session->admitted && !session->revoked &&
             session->requires_reauth)) &&
           ucn_v6_principal_is_valid(&session->peer_principal) &&
           ucn_v6_binding_key_is_valid(&session->local_binding) &&
           ucn_v6_binding_key_is_valid(&session->peer_binding) &&
           session->local_binding.realm_id == realm_id &&
           session->peer_binding.realm_id == realm_id &&
            session->session_generation != 0U &&
            session->session_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
            session->link_instance_generation != 0U &&
            (session->bootstrap_flow == UCN_V6_BOOTSTRAP_FLOW_JOIN ||
             session->bootstrap_flow == UCN_V6_BOOTSTRAP_FLOW_REAUTH) &&
            session->bootstrap_transaction_id != 0U &&
            session->bootstrap_transaction_id <=
                UCN_V6_SERIAL64_ROTATION_THRESHOLD &&
            session->bootstrap_device_nonce != 0U &&
            session->bootstrap_authority_nonce != 0U &&
            session->bootstrap_freshness_nonce != 0U &&
            !bytes_are_zero(session->bootstrap_prior_messages_hash,
                            sizeof(session->bootstrap_prior_messages_hash)) &&
            session->authority_epoch.authority_generation != 0U &&
           session->authority_epoch.quorum_proven &&
           session->authority_epoch.durable &&
           !bytes_are_zero(session->authority_epoch.durable_fence_token,
                           sizeof(session->authority_epoch.durable_fence_token)) &&
           !bytes_are_zero(
               session->authority_epoch.allocation_high_water_digest,
               sizeof(session->authority_epoch.allocation_high_water_digest)) &&
           ucn_v6_binding_key_is_valid(
               &session->joining_binding_certificate.binding) &&
           session->joining_binding_certificate.binding.realm_id == realm_id &&
           session->joining_binding_certificate.authority_generation ==
               session->authority_epoch.authority_generation &&
           session->joining_binding_certificate.authority_lease_sequence ==
               session->authority_epoch.lease_sequence &&
           session->joining_binding_certificate.lease_duration_us != 0U &&
           !bytes_are_zero(session->joining_binding_certificate.lease_id,
                           sizeof(session->joining_binding_certificate.lease_id)) &&
           principal_equal(
               &session->joining_binding_certificate.authority_principal,
               &session->authority_epoch.authority_principal) &&
           ((principal_equal(
                  &session->joining_binding_certificate.device_principal,
                  local_principal) &&
             binding_equal(&session->joining_binding_certificate.binding,
                           &session->local_binding) &&
             principal_equal(&session->peer_principal,
                             &session->authority_epoch.authority_principal)) ||
            (principal_equal(
                  &session->joining_binding_certificate.device_principal,
                  &session->peer_principal) &&
             binding_equal(&session->joining_binding_certificate.binding,
                           &session->peer_binding) &&
             principal_equal(local_principal,
                             &session->authority_epoch.authority_principal))) &&
           session->local_lease_deadline_us != 0U &&
           selector_is_valid(&session->hop_current, UCN_V6_E2E_NONE) &&
           (session->e2e_mode == UCN_V6_E2E_AUTH_ONLY ||
            session->e2e_mode == UCN_V6_E2E_AEAD) &&
           selector_is_valid(&session->e2e_current, session->e2e_mode) &&
           ((selector_is_zero(&session->hop_previous) &&
             session->hop_previous_deadline_us == 0U) ||
            (selector_is_valid(&session->hop_previous, UCN_V6_E2E_NONE) &&
             session->hop_previous_deadline_us != 0U)) &&
           ((selector_is_zero(&session->e2e_previous) &&
             session->e2e_previous_deadline_us == 0U) ||
            (selector_is_valid(&session->e2e_previous, session->e2e_mode) &&
             session->e2e_previous_deadline_us != 0U)) &&
           replay_is_valid(&session->hop_replay_current) &&
           replay_is_valid(&session->hop_replay_previous) &&
           replay_is_valid(&session->e2e_replay_current) &&
           replay_is_valid(&session->e2e_replay_previous) &&
           session->tx_next_sequence != 0U &&
           session->tx_next_sequence <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           session->tx_reserved_through <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool group_policy_is_valid(
    const ucn_v6_group_policy_slot_t *group)
{
    if (group->state == UCN_V6_GROUP_SLOT_NEVER_ACTIVATED) {
        return bytes_are_zero((const uint8_t *)group, sizeof(*group));
    }
    return (group->state == UCN_V6_GROUP_SLOT_ACTIVE ||
            group->state == UCN_V6_GROUP_SLOT_RETIRED) &&
           group->group_id != 0U &&
           group->group_id <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           group->group_generation != 0U &&
           group->group_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           ucn_v6_principal_is_valid(&group->owner_principal);
}

static bool group_key_is_valid(
    const ucn_v6_group_key_slot_t *key,
    const ucn_v6_group_policy_slot_t *group)
{
    if (key->state == UCN_V6_GROUP_KEY_NEVER_ACTIVATED) {
        return bytes_are_zero((const uint8_t *)key, sizeof(*key));
    }
    if ((key->state != UCN_V6_GROUP_KEY_ACTIVE &&
         key->state != UCN_V6_GROUP_KEY_RETIRED) ||
        group->state == UCN_V6_GROUP_SLOT_NEVER_ACTIVATED ||
        key->group_id != group->group_id ||
        key->group_generation != group->group_generation ||
        key->key_id == 0U || key->suite_id == 0U ||
        !suite_is_valid(key->suite_id, UCN_V6_E2E_NONE) ||
        key->current_generation == 0U ||
        key->current_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        !replay_is_valid(&key->current_replay) ||
        !replay_is_valid(&key->previous_replay)) {
        return false;
    }
    if ((key->state == UCN_V6_GROUP_KEY_ACTIVE &&
         (key->tx_next_sequence == 0U ||
          key->tx_next_sequence > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
          key->tx_reserved_through > UCN_V6_SERIAL_ROTATION_THRESHOLD)) ||
        (key->state == UCN_V6_GROUP_KEY_RETIRED &&
         (key->tx_next_sequence != 0U ||
          key->tx_reserved_through != 0U))) {
        return false;
    }
    if (key->previous_generation == 0U) {
        return key->previous_deadline_us == 0U;
    }
    return key->state == UCN_V6_GROUP_KEY_ACTIVE &&
           key->previous_generation < key->current_generation &&
           key->previous_deadline_us != 0U;
}

static const ucn_v6_group_key_slot_t *find_group_key_const(
    const ucn_v6_security_snapshot_t *snapshot,
    uint32_t group_id,
    uint32_t group_generation,
    uint16_t key_id,
    uint32_t key_generation)
{
    size_t group_index;
    size_t key_index;
    for (group_index = 0U;
         group_index < UCN_V6_CONFIG_STATIC_GROUP_SLOTS; ++group_index) {
        for (key_index = 0U;
             key_index < UCN_V6_CONFIG_GROUP_KEY_SLOTS; ++key_index) {
            const ucn_v6_group_key_slot_t *key =
                &snapshot->group_keys[group_index][key_index];
            if (key->state == UCN_V6_GROUP_KEY_ACTIVE &&
                key->group_id == group_id &&
                key->group_generation == group_generation &&
                key->key_id == key_id &&
                (key->current_generation == key_generation ||
                 key->previous_generation == key_generation)) {
                return key;
            }
        }
    }
    return NULL;
}

static bool group_replay_source_is_valid(
    const ucn_v6_group_replay_source_t *source,
    const ucn_v6_security_snapshot_t *snapshot)
{
    if (!source->occupied) {
        return bytes_are_zero((const uint8_t *)source, sizeof(*source));
    }
    return ucn_v6_binding_key_is_valid(&source->claimed_source) &&
           source->claimed_source.realm_id == snapshot->realm_id &&
           source->claimed_session_generation != 0U &&
           source->claimed_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           replay_is_valid(&source->replay) &&
           find_group_key_const(snapshot, source->group_id,
                                source->group_generation, source->key_id,
                                source->key_generation) != NULL;
}

static bool acl_key_is_valid(const ucn_v6_acl_key_t *key, uint32_t realm_id)
{
    uint32_t frame_type = (uint32_t)key->frame_type;
    uint32_t traffic = (uint32_t)key->traffic_class;
    uint32_t delivery = (uint32_t)key->delivery_guarantee;
    uint32_t interaction = (uint32_t)key->interaction_role;
    bool data;

    if (!ucn_v6_principal_is_valid(&key->device_principal) ||
        !ucn_v6_binding_key_is_valid(&key->source_binding) ||
        !ucn_v6_binding_key_is_valid(&key->destination_binding) ||
        key->source_binding.realm_id != realm_id ||
        key->destination_binding.realm_id != realm_id ||
        key->session_generation == 0U ||
        key->session_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        frame_type < UCN_V6_FRAME_CONTROL ||
        frame_type > UCN_V6_FRAME_DIAGNOSTIC ||
        traffic > UCN_V6_TRAFFIC_Q3 ||
        delivery > UCN_V6_DELIVERY_RELIABLE ||
        interaction > UCN_V6_INTERACTION_ERROR ||
        (key->direction != UCN_V6_SECURITY_INBOUND &&
         key->direction != UCN_V6_SECURITY_OUTBOUND)) {
        return false;
    }
    data = key->frame_type == UCN_V6_FRAME_DATA;
    if (data) {
        if (key->protocol_opcode != 0U || key->source_endpoint == 0U ||
            key->destination_endpoint == 0U ||
            key->source_endpoint == UINT16_MAX ||
            key->destination_endpoint == UINT16_MAX) {
            return false;
        }
    } else if (key->protocol_opcode == 0U || key->source_endpoint != 0U ||
               key->destination_endpoint != 0U ||
               key->interaction_role != UCN_V6_INTERACTION_ONE_WAY) {
        return false;
    }
    if (key->operation_id_policy == UCN_V6_OPERATION_ID_NONE) {
        return key->exact_operation_id == 0U &&
               (!data || key->interaction_role == UCN_V6_INTERACTION_ONE_WAY);
    }
    if (key->operation_id_policy == UCN_V6_OPERATION_ID_ANY_NONZERO) {
        return key->exact_operation_id == 0U && data &&
               key->interaction_role != UCN_V6_INTERACTION_ONE_WAY;
    }
    return key->operation_id_policy == UCN_V6_OPERATION_ID_EXACT && data &&
           key->interaction_role != UCN_V6_INTERACTION_ONE_WAY &&
           key->exact_operation_id != 0U &&
           key->exact_operation_id <= UCN_V6_SERIAL64_ROTATION_THRESHOLD;
}

static bool snapshot_is_valid(
    const ucn_v6_security_snapshot_t *snapshot,
    uint32_t realm_id,
    const ucn_v6_principal_t *local_principal,
    bool allow_factory)
{
    size_t index;
    size_t right;
    size_t key_index;
    size_t occupied_sessions = 0U;

    if (snapshot == NULL || snapshot->magic != UCN_V6_SECURITY_SNAPSHOT_MAGIC ||
        snapshot->schema != UCN_V6_SECURITY_SNAPSHOT_SCHEMA ||
        snapshot->session_count != UCN_V6_CONFIG_SECURITY_SESSIONS ||
        snapshot->snapshot_generation >
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        (!allow_factory && snapshot->snapshot_generation == 0U) ||
        snapshot->realm_id != realm_id ||
        !principal_equal(&snapshot->local_principal, local_principal)) {
        return false;
    }
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        if (!session_is_valid(&snapshot->sessions[index], realm_id,
                              local_principal)) {
            return false;
        }
        if (snapshot->sessions[index].occupied) {
            ++occupied_sessions;
            if (!snapshot->local_binding_valid ||
                !binding_equal(&snapshot->sessions[index].local_binding,
                               &snapshot->local_binding)) {
                return false;
            }
            for (right = index + 1U;
                 right < UCN_V6_CONFIG_SECURITY_SESSIONS; ++right) {
                if (snapshot->sessions[right].occupied &&
                    binding_equal(&snapshot->sessions[index].peer_binding,
                                  &snapshot->sessions[right].peer_binding) &&
                    snapshot->sessions[index].session_generation ==
                        snapshot->sessions[right].session_generation) {
                    return false;
                }
            }
        }
    }
    if ((snapshot->local_binding_valid &&
         (!ucn_v6_binding_key_is_valid(&snapshot->local_binding) ||
          snapshot->local_binding.realm_id != realm_id)) ||
        (!snapshot->local_binding_valid &&
         !bytes_are_zero((const uint8_t *)&snapshot->local_binding,
                         sizeof(snapshot->local_binding))) ||
        (occupied_sessions != 0U && !snapshot->local_binding_valid)) {
        return false;
    }
    for (index = 0U; index < UCN_V6_CONFIG_ACL_ENTRIES; ++index) {
        const ucn_v6_acl_entry_t *entry = &snapshot->acl_entries[index];
        if (!entry->occupied) {
            if (!bytes_are_zero((const uint8_t *)entry, sizeof(*entry))) {
                return false;
            }
        } else if (!acl_key_is_valid(&entry->key, realm_id)) {
            return false;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_STATIC_GROUP_SLOTS; ++index) {
        const ucn_v6_group_policy_slot_t *group = &snapshot->groups[index];
        if (!group_policy_is_valid(group)) {
            return false;
        }
        if (group->state != UCN_V6_GROUP_SLOT_NEVER_ACTIVATED) {
            for (right = index + 1U;
                 right < UCN_V6_CONFIG_STATIC_GROUP_SLOTS; ++right) {
                if (snapshot->groups[right].state !=
                        UCN_V6_GROUP_SLOT_NEVER_ACTIVATED &&
                    snapshot->groups[right].group_id == group->group_id) {
                    return false;
                }
            }
        }
        for (key_index = 0U;
             key_index < UCN_V6_CONFIG_GROUP_KEY_SLOTS; ++key_index) {
            if (!group_key_is_valid(
                    &snapshot->group_keys[index][key_index], group)) {
                return false;
            }
        }
    }
    for (index = 0U;
         index < UCN_V6_CONFIG_GROUP_REPLAY_SOURCES; ++index) {
        if (!group_replay_source_is_valid(
                &snapshot->group_replay_sources[index], snapshot)) {
            return false;
        }
    }
    return true;
}

static bool replay_equal(
    const ucn_v6_replay_window_t *left,
    const ucn_v6_replay_window_t *right)
{
    return left->highest_sequence == right->highest_sequence &&
           left->seen_bitmap == right->seen_bitmap &&
           left->durable_reserved_through ==
               right->durable_reserved_through &&
           left->initialized == right->initialized;
}

static bool session_equal(
    const ucn_v6_security_session_record_t *left,
    const ucn_v6_security_session_record_t *right)
{
    return left->occupied == right->occupied &&
           left->admitted == right->admitted &&
           left->revoked == right->revoked &&
           left->requires_reauth == right->requires_reauth &&
           principal_equal(&left->peer_principal, &right->peer_principal) &&
           binding_equal(&left->local_binding, &right->local_binding) &&
           binding_equal(&left->peer_binding, &right->peer_binding) &&
           left->session_generation == right->session_generation &&
           left->link_instance_generation ==
               right->link_instance_generation &&
           left->bootstrap_flow == right->bootstrap_flow &&
           left->bootstrap_transaction_id ==
               right->bootstrap_transaction_id &&
           left->bootstrap_device_nonce == right->bootstrap_device_nonce &&
           left->bootstrap_authority_nonce ==
               right->bootstrap_authority_nonce &&
           left->bootstrap_freshness_nonce ==
               right->bootstrap_freshness_nonce &&
           memcmp(left->bootstrap_prior_messages_hash,
                  right->bootstrap_prior_messages_hash,
                  sizeof(left->bootstrap_prior_messages_hash)) == 0 &&
           left->e2e_mode == right->e2e_mode &&
           left->authority_epoch.authority_generation ==
               right->authority_epoch.authority_generation &&
           left->authority_epoch.lease_sequence ==
               right->authority_epoch.lease_sequence &&
           left->authority_epoch.lease_duration_us ==
               right->authority_epoch.lease_duration_us &&
           left->authority_epoch.quorum_proven ==
               right->authority_epoch.quorum_proven &&
           left->authority_epoch.durable == right->authority_epoch.durable &&
           principal_equal(&left->authority_epoch.authority_principal,
                           &right->authority_epoch.authority_principal) &&
           memcmp(left->authority_epoch.durable_fence_token,
                  right->authority_epoch.durable_fence_token,
                  sizeof(left->authority_epoch.durable_fence_token)) == 0 &&
           memcmp(left->authority_epoch.allocation_high_water_digest,
                  right->authority_epoch.allocation_high_water_digest,
                  sizeof(left->authority_epoch.allocation_high_water_digest)) ==
               0 &&
           principal_equal(
               &left->joining_binding_certificate.device_principal,
               &right->joining_binding_certificate.device_principal) &&
           principal_equal(
               &left->joining_binding_certificate.authority_principal,
               &right->joining_binding_certificate.authority_principal) &&
           binding_equal(&left->joining_binding_certificate.binding,
                         &right->joining_binding_certificate.binding) &&
           left->joining_binding_certificate.authority_generation ==
               right->joining_binding_certificate.authority_generation &&
           memcmp(left->joining_binding_certificate.lease_id,
                  right->joining_binding_certificate.lease_id,
                  sizeof(left->joining_binding_certificate.lease_id)) == 0 &&
           left->joining_binding_certificate.lease_duration_us ==
               right->joining_binding_certificate.lease_duration_us &&
           left->joining_binding_certificate.authority_lease_sequence ==
               right->joining_binding_certificate.authority_lease_sequence &&
           left->joining_binding_certificate.mode ==
               right->joining_binding_certificate.mode &&
           left->local_lease_deadline_us == right->local_lease_deadline_us &&
           selector_equal(&left->hop_current, &right->hop_current) &&
           selector_equal(&left->hop_previous, &right->hop_previous) &&
           left->hop_previous_deadline_us ==
               right->hop_previous_deadline_us &&
           selector_equal(&left->e2e_current, &right->e2e_current) &&
           selector_equal(&left->e2e_previous, &right->e2e_previous) &&
           left->e2e_previous_deadline_us ==
               right->e2e_previous_deadline_us &&
           replay_equal(&left->hop_replay_current,
                        &right->hop_replay_current) &&
           replay_equal(&left->hop_replay_previous,
                        &right->hop_replay_previous) &&
           replay_equal(&left->e2e_replay_current,
                        &right->e2e_replay_current) &&
           replay_equal(&left->e2e_replay_previous,
                        &right->e2e_replay_previous) &&
           left->tx_next_sequence == right->tx_next_sequence &&
           left->tx_reserved_through == right->tx_reserved_through;
}

static bool acl_key_fields_equal(
    const ucn_v6_acl_key_t *left,
    const ucn_v6_acl_key_t *right)
{
    return principal_equal(&left->device_principal,
                           &right->device_principal) &&
           binding_equal(&left->source_binding, &right->source_binding) &&
           binding_equal(&left->destination_binding,
                         &right->destination_binding) &&
           left->session_generation == right->session_generation &&
           left->source_endpoint == right->source_endpoint &&
           left->destination_endpoint == right->destination_endpoint &&
           left->frame_type == right->frame_type &&
           left->protocol_opcode == right->protocol_opcode &&
           left->traffic_class == right->traffic_class &&
           left->delivery_guarantee == right->delivery_guarantee &&
           left->interaction_role == right->interaction_role &&
           left->operation_id_policy == right->operation_id_policy &&
           left->exact_operation_id == right->exact_operation_id &&
           left->direction == right->direction;
}

static bool group_policy_equal(
    const ucn_v6_group_policy_slot_t *left,
    const ucn_v6_group_policy_slot_t *right)
{
    return left->state == right->state && left->group_id == right->group_id &&
           left->group_generation == right->group_generation &&
           principal_equal(&left->owner_principal, &right->owner_principal);
}

static bool group_key_equal(
    const ucn_v6_group_key_slot_t *left,
    const ucn_v6_group_key_slot_t *right)
{
    return left->state == right->state && left->group_id == right->group_id &&
           left->group_generation == right->group_generation &&
           left->key_id == right->key_id &&
           left->suite_id == right->suite_id &&
           left->current_generation == right->current_generation &&
           left->previous_generation == right->previous_generation &&
           left->previous_deadline_us == right->previous_deadline_us &&
           replay_equal(&left->current_replay, &right->current_replay) &&
           replay_equal(&left->previous_replay, &right->previous_replay) &&
           left->tx_next_sequence == right->tx_next_sequence &&
           left->tx_reserved_through == right->tx_reserved_through;
}

static bool snapshot_equal(
    const ucn_v6_security_snapshot_t *left,
    const ucn_v6_security_snapshot_t *right)
{
    size_t index;
    size_t key_index;
    if (left->magic != right->magic || left->schema != right->schema ||
        left->session_count != right->session_count ||
        left->snapshot_generation != right->snapshot_generation ||
        left->realm_id != right->realm_id ||
        !principal_equal(&left->local_principal, &right->local_principal) ||
        left->local_binding_valid != right->local_binding_valid ||
        !binding_equal(&left->local_binding, &right->local_binding)) {
        return false;
    }
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        if (!session_equal(&left->sessions[index], &right->sessions[index])) {
            return false;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ACL_ENTRIES; ++index) {
        if (left->acl_entries[index].occupied !=
                right->acl_entries[index].occupied ||
            left->acl_entries[index].revoked !=
                right->acl_entries[index].revoked ||
            !acl_key_fields_equal(&left->acl_entries[index].key,
                                  &right->acl_entries[index].key)) {
            return false;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_STATIC_GROUP_SLOTS; ++index) {
        if (!group_policy_equal(&left->groups[index], &right->groups[index])) {
            return false;
        }
        for (key_index = 0U;
             key_index < UCN_V6_CONFIG_GROUP_KEY_SLOTS; ++key_index) {
            if (!group_key_equal(&left->group_keys[index][key_index],
                                 &right->group_keys[index][key_index])) {
                return false;
            }
        }
    }
    for (index = 0U;
         index < UCN_V6_CONFIG_GROUP_REPLAY_SOURCES; ++index) {
        const ucn_v6_group_replay_source_t *a =
            &left->group_replay_sources[index];
        const ucn_v6_group_replay_source_t *b =
            &right->group_replay_sources[index];
        if (a->occupied != b->occupied || a->group_id != b->group_id ||
            a->group_generation != b->group_generation ||
            a->key_id != b->key_id ||
            a->key_generation != b->key_generation ||
            !binding_equal(&a->claimed_source, &b->claimed_source) ||
            a->claimed_session_generation !=
                b->claimed_session_generation ||
            !replay_equal(&a->replay, &b->replay)) {
            return false;
        }
    }
    return true;
}

static void snapshot_make_factory(
    ucn_v6_security_snapshot_t *snapshot,
    uint32_t realm_id,
    const ucn_v6_principal_t *local_principal)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = UCN_V6_SECURITY_SNAPSHOT_MAGIC;
    snapshot->schema = UCN_V6_SECURITY_SNAPSHOT_SCHEMA;
    snapshot->session_count = UCN_V6_CONFIG_SECURITY_SESSIONS;
    snapshot->realm_id = realm_id;
    snapshot->local_principal = *local_principal;
}

static ucn_v6_result_t persist_candidate(
    ucn_v6_security_manager_t *manager,
    ucn_v6_security_snapshot_t *candidate)
{
    ucn_v6_security_snapshot_t verified;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;
    uint64_t verified_generation = 0U;

    if (manager->committed.snapshot_generation >=
        UCN_V6_SERIAL64_ROTATION_THRESHOLD) {
        manager->faulted = true;
        return UCN_V6_ERR_EXHAUSTED;
    }
    candidate->snapshot_generation =
        manager->committed.snapshot_generation + 1U;
    if (!snapshot_is_valid(candidate, manager->committed.realm_id,
                           &manager->committed.local_principal, false)) {
        return UCN_V6_ERR_STATE;
    }
    result = ucn_v6_callback_gate_try_enter(manager->callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = manager->store.reserve_generation_witness(
        manager->store.context, candidate->snapshot_generation);
    if (result == UCN_V6_OK) {
        result = manager->store.submit(manager->store.context, candidate);
    }
    if (result == UCN_V6_OK) {
        memset(&verified, 0, sizeof(verified));
        result = manager->store.load(manager->store.context, &verified);
        if (result == UCN_V6_OK) {
            result = manager->store.load_generation_witness(
                manager->store.context, &verified_generation);
        }
        if (result != UCN_V6_OK ||
            verified_generation != candidate->snapshot_generation ||
            !snapshot_is_valid(&verified, manager->committed.realm_id,
                               &manager->committed.local_principal, false) ||
            !snapshot_equal(candidate, &verified)) {
            result = UCN_V6_ERR_STATE;
        }
    }
    leave_result = ucn_v6_callback_gate_leave(manager->callback_gate, manager);
    if (result != UCN_V6_OK || leave_result != UCN_V6_OK) {
        manager->faulted = true;
        return result != UCN_V6_OK ? result : leave_result;
    }
    manager->committed = verified;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_security_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    uint32_t realm_id,
    const ucn_v6_principal_t *local_principal,
    const ucn_v6_security_store_ops_t *store,
    const ucn_v6_security_crypto_ops_t *crypto,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_security_manager_t **manager_out)
{
    ucn_v6_security_manager_t initialized;
    ucn_v6_security_snapshot_t loaded;
    ucn_v6_security_manager_t *manager;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;
    size_t index;
    size_t key_index;
    uint64_t generation_witness = 0U;
    ucn_v6_result_t witness_result;

    if (manager_out == NULL || realm_id == 0U || realm_id == UINT32_MAX ||
        !ucn_v6_principal_is_valid(local_principal) ||
        !store_ops_are_valid(store) || !crypto_ops_are_valid(crypto) ||
        callback_gate == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(*manager),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    manager = (ucn_v6_security_manager_t *)storage;
    result = ucn_v6_callback_gate_try_enter(callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    memset(&loaded, 0, sizeof(loaded));
    result = store->load(store->context, &loaded);
    witness_result = store->load_generation_witness(
        store->context, &generation_witness);
    if (result == UCN_V6_ERR_NOT_FOUND &&
        witness_result == UCN_V6_ERR_NOT_FOUND) {
        snapshot_make_factory(&loaded, realm_id, local_principal);
        result = UCN_V6_OK;
    } else if (result != UCN_V6_OK || witness_result != UCN_V6_OK ||
               generation_witness != loaded.snapshot_generation ||
               !snapshot_is_valid(&loaded, realm_id, local_principal,
                                  false)) {
        result = UCN_V6_ERR_STATE;
    }
    leave_result = ucn_v6_callback_gate_leave(callback_gate, manager);
    if (result != UCN_V6_OK || leave_result != UCN_V6_OK) {
        return result != UCN_V6_OK ? result : leave_result;
    }
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        ucn_v6_security_session_record_t *session = &loaded.sessions[index];
        if (session->occupied &&
            session->tx_next_sequence <= session->tx_reserved_through) {
            if (session->tx_reserved_through >=
                UCN_V6_SERIAL_ROTATION_THRESHOLD) {
                return UCN_V6_ERR_EXHAUSTED;
            }
            session->tx_next_sequence = session->tx_reserved_through + 1U;
        }
        if (session->occupied && !session->revoked) {
            session->admitted = false;
            session->requires_reauth = true;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_STATIC_GROUP_SLOTS; ++index) {
        for (key_index = 0U;
             key_index < UCN_V6_CONFIG_GROUP_KEY_SLOTS; ++key_index) {
            ucn_v6_group_key_slot_t *key =
                &loaded.group_keys[index][key_index];
            if (key->state == UCN_V6_GROUP_KEY_ACTIVE &&
                key->tx_next_sequence <= key->tx_reserved_through) {
                if (key->tx_reserved_through >=
                    UCN_V6_SERIAL_ROTATION_THRESHOLD) {
                    return UCN_V6_ERR_EXHAUSTED;
                }
                key->tx_next_sequence = key->tx_reserved_through + 1U;
            }
        }
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.magic = UCN_V6_SECURITY_MANAGER_MAGIC;
    initialized.schema = UCN_V6_STORAGE_LAYOUT;
    initialized.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    initialized.committed = loaded;
    initialized.store = *store;
    initialized.crypto = *crypto;
    initialized.callback_gate = callback_gate;
    initialized.initialized = true;
    initialized.canary = UCN_V6_SECURITY_MANAGER_CANARY;
    *manager = initialized;
    *manager_out = manager;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_security_write_bootstrap_transcript(
    const ucn_v6_bootstrap_transcript_t *transcript,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    uint8_t encoded[UCN_V6_BOOTSTRAP_CANONICAL_BYTES];
    size_t offset = 0U;

    if (transcript == NULL || output == NULL || output_length == NULL ||
        output_capacity < sizeof(encoded) ||
        !bootstrap_transcript_is_valid(transcript)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    encoded[offset++] = transcript->protocol_version;
    put_u16(&encoded[offset], transcript->bootstrap_header_contract);
    offset += 2U;
    encoded[offset++] = 1U; /* Frozen ordered-role contract v1. */
    encoded[offset++] = (uint8_t)transcript->flow;
    memcpy(&encoded[offset], transcript->joining_device_principal.bytes, 16U);
    offset += 16U;
    memcpy(&encoded[offset],
           transcript->joining_device_identity_digest.bytes, 16U);
    offset += 16U;
    memcpy(&encoded[offset], transcript->authority_principal.bytes, 16U);
    offset += 16U;
    put_u32(&encoded[offset], transcript->authority_generation);
    offset += 4U;
    put_u64(&encoded[offset], transcript->device_nonce);
    offset += 8U;
    put_u64(&encoded[offset], transcript->authority_nonce);
    offset += 8U;
    put_u64(&encoded[offset], transcript->transaction_id);
    offset += 8U;
    put_u64(&encoded[offset], transcript->lease_freshness_challenge_nonce);
    offset += 8U;
    put_u32(&encoded[offset], transcript->realm_id);
    offset += 4U;
    put_u32(&encoded[offset], transcript->proposed_address);
    offset += 4U;
    put_u32(&encoded[offset], transcript->address_binding_generation);
    offset += 4U;
    put_u32(&encoded[offset], transcript->authority_address);
    offset += 4U;
    put_u32(&encoded[offset], transcript->authority_binding_generation);
    offset += 4U;
    memcpy(&encoded[offset], transcript->binding_lease_id, 16U);
    offset += 16U;
    put_u64(&encoded[offset], transcript->binding_lease_duration_us);
    offset += 8U;
    put_u64(&encoded[offset], transcript->authority_lease_sequence);
    offset += 8U;
    encoded[offset++] = transcript->selected_hop_suite;
    put_u16(&encoded[offset], transcript->selected_hop_key_id);
    offset += 2U;
    put_u32(&encoded[offset], transcript->selected_hop_key_generation);
    offset += 4U;
    encoded[offset++] = transcript->selected_e2e_mode;
    encoded[offset++] = transcript->selected_e2e_suite;
    put_u16(&encoded[offset], transcript->selected_e2e_key_id);
    offset += 2U;
    put_u32(&encoded[offset], transcript->selected_e2e_key_generation);
    offset += 4U;
    put_u32(&encoded[offset], transcript->selected_session_generation);
    offset += 4U;
    put_u32(&encoded[offset], transcript->selected_link_instance_generation);
    offset += 4U;
    memcpy(&encoded[offset], transcript->prior_messages_hash, 32U);
    offset += 32U;
    if (offset != sizeof(encoded)) {
        return UCN_V6_ERR_STATE;
    }
    memcpy(output, encoded, sizeof(encoded));
    *output_length = sizeof(encoded);
    return UCN_V6_OK;
}

static bool authority_epoch_matches(
    const ucn_v6_join_commit_t *commit,
    uint64_t now_us)
{
    const ucn_v6_authority_epoch_t *epoch = &commit->authority_epoch;
    const ucn_v6_bootstrap_transcript_t *transcript = &commit->transcript;
    return ucn_v6_principal_is_valid(&epoch->authority_principal) &&
           principal_equal(&epoch->authority_principal,
                           &transcript->authority_principal) &&
           epoch->authority_generation == transcript->authority_generation &&
           epoch->authority_generation != 0U && epoch->quorum_proven &&
           epoch->durable &&
           !bytes_are_zero(epoch->durable_fence_token,
                           sizeof(epoch->durable_fence_token)) &&
           !bytes_are_zero(epoch->allocation_high_water_digest,
                           sizeof(epoch->allocation_high_water_digest)) &&
           epoch->lease_sequence ==
               transcript->authority_lease_sequence &&
           epoch->lease_duration_us >= transcript->binding_lease_duration_us &&
           ucn_v6_lease_deadline_is_live(
               now_us, commit->authority_local_lease_deadline_us);
}

static bool binding_certificate_matches(const ucn_v6_join_commit_t *commit)
{
    const ucn_v6_binding_certificate_t *certificate =
        &commit->joining_binding_certificate;
    const ucn_v6_bootstrap_transcript_t *transcript = &commit->transcript;
    return principal_equal(&certificate->device_principal,
                           &transcript->joining_device_principal) &&
           principal_equal(&certificate->authority_principal,
                           &transcript->authority_principal) &&
           certificate->binding.realm_id == transcript->realm_id &&
           certificate->binding.node_address ==
               transcript->proposed_address &&
           certificate->binding.binding_generation ==
               transcript->address_binding_generation &&
           certificate->authority_generation ==
               transcript->authority_generation &&
           memcmp(certificate->lease_id, transcript->binding_lease_id,
                  sizeof(certificate->lease_id)) == 0 &&
           certificate->lease_duration_us ==
               transcript->binding_lease_duration_us &&
           certificate->authority_lease_sequence ==
               transcript->authority_lease_sequence &&
           (certificate->mode == UCN_V6_ADDRESS_STATIC ||
            certificate->mode == UCN_V6_ADDRESS_LEASED ||
            certificate->mode == UCN_V6_ADDRESS_SELF_PROPOSED);
}

static bool binding_certificate_equal(
    const ucn_v6_binding_certificate_t *left,
    const ucn_v6_binding_certificate_t *right)
{
    return principal_equal(&left->device_principal,
                           &right->device_principal) &&
           principal_equal(&left->authority_principal,
                           &right->authority_principal) &&
           binding_equal(&left->binding, &right->binding) &&
           left->authority_generation == right->authority_generation &&
           memcmp(left->lease_id, right->lease_id,
                  sizeof(left->lease_id)) == 0 &&
           left->lease_duration_us == right->lease_duration_us &&
           left->authority_lease_sequence ==
               right->authority_lease_sequence &&
           left->mode == right->mode;
}

static bool authority_epoch_equal(
    const ucn_v6_authority_epoch_t *left,
    const ucn_v6_authority_epoch_t *right)
{
    return principal_equal(&left->authority_principal,
                           &right->authority_principal) &&
           left->authority_generation == right->authority_generation &&
           memcmp(left->durable_fence_token, right->durable_fence_token,
                  sizeof(left->durable_fence_token)) == 0 &&
           memcmp(left->allocation_high_water_digest,
                  right->allocation_high_water_digest,
                  sizeof(left->allocation_high_water_digest)) == 0 &&
           left->lease_sequence == right->lease_sequence &&
           left->lease_duration_us == right->lease_duration_us &&
           left->quorum_proven == right->quorum_proven &&
           left->durable == right->durable;
}

static bool session_matches_commit_exact(
    const ucn_v6_security_session_record_t *session,
    const ucn_v6_join_commit_t *commit,
    const ucn_v6_principal_t *peer)
{
    const ucn_v6_bootstrap_transcript_t *transcript = &commit->transcript;
    return session->occupied && session->admitted && !session->revoked &&
           !session->requires_reauth &&
           principal_equal(&session->peer_principal, peer) &&
           binding_equal(&session->local_binding, &commit->local_binding) &&
           binding_equal(&session->peer_binding, &commit->peer_binding) &&
           session->session_generation == commit->session_generation &&
           session->link_instance_generation ==
               commit->link_instance_generation &&
           session->bootstrap_flow == transcript->flow &&
           session->bootstrap_transaction_id == transcript->transaction_id &&
           session->bootstrap_device_nonce == transcript->device_nonce &&
           session->bootstrap_authority_nonce == transcript->authority_nonce &&
           session->bootstrap_freshness_nonce ==
               transcript->lease_freshness_challenge_nonce &&
           memcmp(session->bootstrap_prior_messages_hash,
                  transcript->prior_messages_hash,
                  sizeof(session->bootstrap_prior_messages_hash)) == 0 &&
           session->e2e_mode ==
               (ucn_v6_e2e_mode_t)transcript->selected_e2e_mode &&
           authority_epoch_equal(&session->authority_epoch,
                                 &commit->authority_epoch) &&
           binding_certificate_equal(
               &session->joining_binding_certificate,
               &commit->joining_binding_certificate) &&
           session->local_lease_deadline_us ==
               commit->authority_local_lease_deadline_us &&
           selector_equal(&session->hop_current, &commit->hop_selector) &&
           selector_equal(&session->e2e_current, &commit->e2e_selector);
}

static ucn_v6_security_session_record_t *find_session_slot(
    ucn_v6_security_snapshot_t *snapshot,
    const ucn_v6_principal_t *peer,
    bool allow_empty)
{
    size_t index;
    ucn_v6_security_session_record_t *empty = NULL;
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        if (snapshot->sessions[index].occupied &&
            principal_equal(&snapshot->sessions[index].peer_principal, peer)) {
            return &snapshot->sessions[index];
        }
        if (!snapshot->sessions[index].occupied && empty == NULL) {
            empty = &snapshot->sessions[index];
        }
    }
    return allow_empty ? empty : NULL;
}

ucn_v6_result_t ucn_v6_security_commit_join(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_bootstrap_owner_t *bootstrap_owner,
    const ucn_v6_bootstrap_key_t *bootstrap_key,
    uint64_t now_us,
    const ucn_v6_join_commit_t *commit)
{
    uint8_t canonical[UCN_V6_BOOTSTRAP_CANONICAL_BYTES];
    size_t canonical_length = 0U;
    ucn_v6_security_snapshot_t candidate;
    ucn_v6_security_session_record_t *slot;
    const ucn_v6_principal_t *peer;
    bool local_is_joining;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    if (!manager_storage_is_valid(manager) || manager->faulted ||
        bootstrap_owner == NULL || bootstrap_key == NULL ||
        commit == NULL || commit->device_proof == NULL ||
        commit->device_proof_length == 0U ||
        commit->device_proof_length > UCN_V6_SECURITY_PROOF_MAX_BYTES ||
        commit->authority_proof == NULL ||
        commit->authority_proof_length == 0U ||
        commit->authority_proof_length > UCN_V6_SECURITY_PROOF_MAX_BYTES ||
        !authority_epoch_matches(commit, now_us) ||
        !binding_certificate_matches(commit) ||
        !ucn_v6_binding_key_is_valid(&commit->local_binding) ||
        !ucn_v6_binding_key_is_valid(&commit->peer_binding) ||
        commit->local_binding.realm_id != manager->committed.realm_id ||
        commit->peer_binding.realm_id != manager->committed.realm_id ||
        commit->session_generation == 0U ||
        commit->session_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        commit->link_instance_generation == 0U ||
        commit->link_instance_generation >
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        commit->session_generation !=
            commit->transcript.selected_session_generation ||
        commit->link_instance_generation !=
            commit->transcript.selected_link_instance_generation ||
        !selector_is_valid(&commit->hop_selector, UCN_V6_E2E_NONE) ||
        !selector_is_valid(&commit->e2e_selector,
             (ucn_v6_e2e_mode_t)commit->transcript.selected_e2e_mode) ||
        commit->transcript.selected_hop_suite !=
            commit->hop_selector.suite_id ||
        commit->transcript.selected_hop_key_id !=
            commit->hop_selector.key_id ||
        commit->transcript.selected_hop_key_generation !=
            commit->hop_selector.key_generation ||
        commit->transcript.selected_e2e_suite !=
            commit->e2e_selector.suite_id ||
        commit->transcript.selected_e2e_key_id !=
            commit->e2e_selector.key_id ||
        commit->transcript.selected_e2e_key_generation !=
            commit->e2e_selector.key_generation) {
        return UCN_V6_ERR_SECURITY;
    }
    result = ucn_v6_bootstrap_validate_final(
        bootstrap_owner, commit->transcript.flow, bootstrap_key,
        &commit->transcript, now_us);
    if (result != UCN_V6_OK) {
        return result;
    }
    local_is_joining = principal_equal(
        &manager->committed.local_principal,
        &commit->transcript.joining_device_principal);
    if (local_is_joining) {
        if (!binding_equal(&commit->local_binding,
                           &commit->joining_binding_certificate.binding) ||
            commit->peer_binding.node_address !=
                commit->transcript.authority_address ||
            commit->peer_binding.binding_generation !=
                commit->transcript.authority_binding_generation) {
            return UCN_V6_ERR_STATE;
        }
        peer = &commit->transcript.authority_principal;
    } else {
        if (!principal_equal(&manager->committed.local_principal,
                             &commit->transcript.authority_principal) ||
            !binding_equal(&commit->peer_binding,
                           &commit->joining_binding_certificate.binding) ||
            commit->local_binding.node_address !=
                commit->transcript.authority_address ||
            commit->local_binding.binding_generation !=
                commit->transcript.authority_binding_generation) {
            return UCN_V6_ERR_STATE;
        }
        peer = &commit->transcript.joining_device_principal;
    }
    result = ucn_v6_security_write_bootstrap_transcript(
        &commit->transcript, canonical, sizeof(canonical), &canonical_length);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_callback_gate_try_enter(manager->callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = manager->crypto.verify_proof(
        manager->crypto.context, UCN_V6_PROOF_ADDRESS_AUTHORITY,
        &commit->transcript.authority_principal,
        canonical, canonical_length, commit->authority_proof,
        commit->authority_proof_length);
    if (result == UCN_V6_OK) {
        result = manager->crypto.verify_proof(
            manager->crypto.context, UCN_V6_PROOF_JOINING_DEVICE,
            &commit->transcript.joining_device_principal,
            canonical, canonical_length, commit->device_proof,
            commit->device_proof_length);
    }
    leave_result = ucn_v6_callback_gate_leave(manager->callback_gate, manager);
    if (result != UCN_V6_OK || leave_result != UCN_V6_OK) {
        return result != UCN_V6_OK ? result : leave_result;
    }

    candidate = manager->committed;
    if (candidate.local_binding_valid &&
        !binding_equal(&candidate.local_binding, &commit->local_binding)) {
        return UCN_V6_ERR_STATE;
    }
    candidate.local_binding_valid = true;
    candidate.local_binding = commit->local_binding;
    slot = find_session_slot(&candidate, peer, true);
    if (slot == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (slot->occupied) {
        if (slot->revoked) {
            return UCN_V6_ERR_ACCESS;
        }
        if (session_matches_commit_exact(slot, commit, peer)) {
            return UCN_V6_OK;
        }
        if (commit->transcript.flow != UCN_V6_BOOTSTRAP_FLOW_REAUTH ||
            !slot->requires_reauth ||
            !binding_certificate_equal(
                &slot->joining_binding_certificate,
                &commit->joining_binding_certificate)) {
            return UCN_V6_ERR_STATE;
        }
        if (!binding_equal(&slot->local_binding, &commit->local_binding) ||
            !binding_equal(&slot->peer_binding, &commit->peer_binding)) {
            return UCN_V6_ERR_REPLAY;
        }
        if (slot->session_generation >= commit->session_generation ||
            slot->session_generation == UCN_V6_SERIAL_ROTATION_THRESHOLD ||
            commit->session_generation != slot->session_generation + 1U) {
            return UCN_V6_ERR_REPLAY;
        }
    }
    if (!slot->occupied &&
        (commit->transcript.flow != UCN_V6_BOOTSTRAP_FLOW_JOIN ||
         commit->session_generation != 1U)) {
        return UCN_V6_ERR_REPLAY;
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->admitted = true;
    slot->requires_reauth = false;
    slot->peer_principal = *peer;
    slot->local_binding = commit->local_binding;
    slot->peer_binding = commit->peer_binding;
    slot->session_generation = commit->session_generation;
    slot->link_instance_generation = commit->link_instance_generation;
    slot->bootstrap_flow = commit->transcript.flow;
    slot->bootstrap_transaction_id = commit->transcript.transaction_id;
    slot->bootstrap_device_nonce = commit->transcript.device_nonce;
    slot->bootstrap_authority_nonce = commit->transcript.authority_nonce;
    slot->bootstrap_freshness_nonce =
        commit->transcript.lease_freshness_challenge_nonce;
    memcpy(slot->bootstrap_prior_messages_hash,
           commit->transcript.prior_messages_hash,
           sizeof(slot->bootstrap_prior_messages_hash));
    slot->e2e_mode =
        (ucn_v6_e2e_mode_t)commit->transcript.selected_e2e_mode;
    slot->authority_epoch = commit->authority_epoch;
    slot->joining_binding_certificate =
        commit->joining_binding_certificate;
    slot->local_lease_deadline_us =
        commit->authority_local_lease_deadline_us;
    slot->hop_current = commit->hop_selector;
    slot->e2e_current = commit->e2e_selector;
    slot->tx_next_sequence = 1U;
    return persist_candidate(manager, &candidate);
}

ucn_v6_result_t ucn_v6_security_require_reauth(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *peer_principal)
{
    ucn_v6_security_session_record_t *session;
    if (!manager_storage_is_valid(manager) || manager->faulted ||
        !ucn_v6_principal_is_valid(peer_principal) ||
        ucn_v6_callback_gate_is_active(manager->callback_gate)) {
        return UCN_V6_ERR_STATE;
    }
    session = find_session_slot(&manager->committed, peer_principal, false);
    if (session == NULL || session->revoked) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    session->admitted = false;
    session->requires_reauth = true;
    return UCN_V6_OK;
}

static bool acl_key_equal(
    const ucn_v6_acl_key_t *left,
    const ucn_v6_acl_key_t *right)
{
    return acl_key_fields_equal(left, right);
}

static ucn_v6_result_t write_acl_canonical(
    const ucn_v6_acl_entry_t *entry,
    uint8_t output[UCN_V6_ACL_CANONICAL_BYTES])
{
    size_t offset = 0U;
    if (entry == NULL || !entry->occupied) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(output, 0, UCN_V6_ACL_CANONICAL_BYTES);
    output[offset++] = 1U;
    output[offset++] = entry->revoked ? 1U : 0U;
    memcpy(&output[offset], entry->key.device_principal.bytes, 16U);
    offset += 16U;
#define PUT_BINDING(value_)                                                    \
    do {                                                                       \
        put_u32(&output[offset], (value_).realm_id); offset += 4U;             \
        put_u32(&output[offset], (value_).node_address); offset += 4U;         \
        put_u32(&output[offset], (value_).binding_generation); offset += 4U;   \
    } while (0)
    PUT_BINDING(entry->key.source_binding);
    PUT_BINDING(entry->key.destination_binding);
#undef PUT_BINDING
    put_u32(&output[offset], entry->key.session_generation);
    offset += 4U;
    put_u16(&output[offset], entry->key.source_endpoint);
    offset += 2U;
    put_u16(&output[offset], entry->key.destination_endpoint);
    offset += 2U;
    output[offset++] = (uint8_t)entry->key.frame_type;
    put_u16(&output[offset], entry->key.protocol_opcode);
    offset += 2U;
    output[offset++] = (uint8_t)entry->key.traffic_class;
    output[offset++] = (uint8_t)entry->key.delivery_guarantee;
    output[offset++] = (uint8_t)entry->key.interaction_role;
    output[offset++] = (uint8_t)entry->key.operation_id_policy;
    put_u64(&output[offset], entry->key.exact_operation_id);
    offset += 8U;
    output[offset++] = (uint8_t)entry->key.direction;
    return offset <= UCN_V6_ACL_CANONICAL_BYTES ? UCN_V6_OK : UCN_V6_ERR_STATE;
}

ucn_v6_result_t ucn_v6_security_set_acl(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_acl_entry_t *entry,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length)
{
    uint8_t canonical[UCN_V6_ACL_CANONICAL_BYTES];
    ucn_v6_security_snapshot_t candidate;
    ucn_v6_acl_entry_t *target = NULL;
    size_t index;
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;

    if (!manager_storage_is_valid(manager) || manager->faulted ||
        entry == NULL || !ucn_v6_principal_is_valid(admin_principal) ||
        !acl_key_is_valid(
            &entry->key, manager->committed.realm_id) ||
        admin_proof == NULL || admin_proof_length == 0U ||
        admin_proof_length > UCN_V6_SECURITY_PROOF_MAX_BYTES) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = write_acl_canonical(entry, canonical);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_callback_gate_try_enter(manager->callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = manager->crypto.verify_proof(
        manager->crypto.context, UCN_V6_PROOF_REALM_ADMIN,
        admin_principal, canonical, sizeof(canonical),
        admin_proof, admin_proof_length);
    leave_result = ucn_v6_callback_gate_leave(manager->callback_gate, manager);
    if (result != UCN_V6_OK || leave_result != UCN_V6_OK) {
        return result != UCN_V6_OK ? result : leave_result;
    }
    candidate = manager->committed;
    for (index = 0U; index < UCN_V6_CONFIG_ACL_ENTRIES; ++index) {
        if (candidate.acl_entries[index].occupied &&
            acl_key_equal(&candidate.acl_entries[index].key, &entry->key)) {
            target = &candidate.acl_entries[index];
            break;
        }
        if (!candidate.acl_entries[index].occupied && target == NULL) {
            target = &candidate.acl_entries[index];
        }
    }
    if (target == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(target, 0, sizeof(*target));
    *target = *entry;
    return persist_candidate(manager, &candidate);
}

static ucn_v6_result_t verify_admin_change(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *canonical,
    size_t canonical_length,
    const uint8_t *proof,
    size_t proof_length)
{
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;
    if (!ucn_v6_principal_is_valid(admin_principal) || canonical == NULL ||
        canonical_length == 0U || proof == NULL || proof_length == 0U ||
        proof_length > UCN_V6_SECURITY_PROOF_MAX_BYTES) {
        return UCN_V6_ERR_ARGUMENT;
    }
    result = ucn_v6_callback_gate_try_enter(manager->callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = manager->crypto.verify_proof(
        manager->crypto.context, UCN_V6_PROOF_REALM_ADMIN,
        admin_principal, canonical, canonical_length, proof, proof_length);
    leave_result = ucn_v6_callback_gate_leave(manager->callback_gate, manager);
    return result != UCN_V6_OK ? result : leave_result;
}

static void clear_group_replay_sources(
    ucn_v6_security_snapshot_t *snapshot,
    uint32_t group_id)
{
    size_t index;
    for (index = 0U;
         index < UCN_V6_CONFIG_GROUP_REPLAY_SOURCES; ++index) {
        if (snapshot->group_replay_sources[index].occupied &&
            snapshot->group_replay_sources[index].group_id == group_id) {
            memset(&snapshot->group_replay_sources[index], 0,
                   sizeof(snapshot->group_replay_sources[index]));
        }
    }
}

ucn_v6_result_t ucn_v6_security_set_group_policy(
    ucn_v6_security_manager_t *manager,
    size_t group_slot,
    const ucn_v6_group_policy_slot_t *policy,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length)
{
    uint8_t canonical[48U];
    ucn_v6_security_snapshot_t candidate;
    const ucn_v6_group_policy_slot_t *current;
    size_t index;
    ucn_v6_result_t result;

    if (!manager_storage_is_valid(manager) || manager->faulted ||
        group_slot >= UCN_V6_CONFIG_STATIC_GROUP_SLOTS || policy == NULL ||
        (policy->state != UCN_V6_GROUP_SLOT_ACTIVE &&
         policy->state != UCN_V6_GROUP_SLOT_RETIRED) ||
        !group_policy_is_valid(policy)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    current = &manager->committed.groups[group_slot];
    if (current->state == UCN_V6_GROUP_SLOT_RETIRED) {
        return UCN_V6_ERR_ACCESS;
    }
    if (current->state == UCN_V6_GROUP_SLOT_NEVER_ACTIVATED) {
        if (policy->state != UCN_V6_GROUP_SLOT_ACTIVE ||
            policy->group_generation != 1U) {
            return UCN_V6_ERR_STATE;
        }
    } else if (policy->group_id != current->group_id ||
               (policy->state == UCN_V6_GROUP_SLOT_ACTIVE &&
                (current->group_generation ==
                     UCN_V6_SERIAL_ROTATION_THRESHOLD ||
                 policy->group_generation !=
                     current->group_generation + 1U)) ||
               (policy->state == UCN_V6_GROUP_SLOT_RETIRED &&
                policy->group_generation != current->group_generation)) {
        return UCN_V6_ERR_STATE;
    }
    for (index = 0U; index < UCN_V6_CONFIG_STATIC_GROUP_SLOTS; ++index) {
        if (index != group_slot &&
            manager->committed.groups[index].state !=
                UCN_V6_GROUP_SLOT_NEVER_ACTIVATED &&
            manager->committed.groups[index].group_id == policy->group_id) {
            return UCN_V6_ERR_STATE;
        }
    }
    memset(canonical, 0, sizeof(canonical));
    canonical[0] = 2U;
    canonical[1] = (uint8_t)group_slot;
    canonical[2] = (uint8_t)policy->state;
    put_u32(&canonical[4], policy->group_id);
    put_u32(&canonical[8], policy->group_generation);
    memcpy(&canonical[12], policy->owner_principal.bytes, 16U);
    result = verify_admin_change(manager, admin_principal, canonical,
                                 sizeof(canonical), admin_proof,
                                 admin_proof_length);
    if (result != UCN_V6_OK) {
        return result;
    }
    candidate = manager->committed;
    candidate.groups[group_slot] = *policy;
    memset(candidate.group_keys[group_slot], 0,
           sizeof(candidate.group_keys[group_slot]));
    clear_group_replay_sources(&candidate, policy->group_id);
    return persist_candidate(manager, &candidate);
}

ucn_v6_result_t ucn_v6_security_set_group_key(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    size_t group_slot,
    size_t key_slot,
    const ucn_v6_group_key_slot_t *key,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length)
{
    uint8_t canonical[64U];
    ucn_v6_security_snapshot_t candidate;
    const ucn_v6_group_policy_slot_t *group;
    const ucn_v6_group_key_slot_t *current;
    ucn_v6_group_key_slot_t next;
    size_t index;
    ucn_v6_result_t result;

    if (!manager_storage_is_valid(manager) || manager->faulted || key == NULL ||
        group_slot >= UCN_V6_CONFIG_STATIC_GROUP_SLOTS ||
        key_slot >= UCN_V6_CONFIG_GROUP_KEY_SLOTS ||
        (key->state != UCN_V6_GROUP_KEY_ACTIVE &&
         key->state != UCN_V6_GROUP_KEY_RETIRED)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    group = &manager->committed.groups[group_slot];
    current = &manager->committed.group_keys[group_slot][key_slot];
    if (group->state != UCN_V6_GROUP_SLOT_ACTIVE ||
        key->group_id != group->group_id ||
        key->group_generation != group->group_generation ||
        current->state == UCN_V6_GROUP_KEY_RETIRED) {
        return UCN_V6_ERR_STATE;
    }
    memset(&next, 0, sizeof(next));
    if (current->state == UCN_V6_GROUP_KEY_NEVER_ACTIVATED) {
        if (key->state != UCN_V6_GROUP_KEY_ACTIVE ||
            key->current_generation != 1U || key->key_id == 0U ||
            !suite_is_valid(key->suite_id, UCN_V6_E2E_NONE)) {
            return UCN_V6_ERR_STATE;
        }
        next = *key;
        next.previous_generation = 0U;
        next.previous_deadline_us = 0U;
        memset(&next.current_replay, 0, sizeof(next.current_replay));
        memset(&next.previous_replay, 0, sizeof(next.previous_replay));
        next.tx_next_sequence = 1U;
        next.tx_reserved_through = 0U;
    } else if (key->key_id != current->key_id ||
               key->suite_id != current->suite_id ||
               key->group_id != current->group_id ||
               key->group_generation != current->group_generation) {
        return UCN_V6_ERR_STATE;
    } else if (key->state == UCN_V6_GROUP_KEY_RETIRED) {
        next = *current;
        next.state = UCN_V6_GROUP_KEY_RETIRED;
        next.previous_generation = 0U;
        next.previous_deadline_us = 0U;
        memset(&next.current_replay, 0, sizeof(next.current_replay));
        memset(&next.previous_replay, 0, sizeof(next.previous_replay));
        next.tx_next_sequence = 0U;
        next.tx_reserved_through = 0U;
    } else {
        if (current->current_generation ==
                UCN_V6_SERIAL_ROTATION_THRESHOLD ||
            key->current_generation != current->current_generation + 1U ||
            key->previous_deadline_us <= now_us) {
            return UCN_V6_ERR_STATE;
        }
        next = *current;
        next.previous_generation = current->current_generation;
        next.previous_deadline_us = key->previous_deadline_us;
        next.previous_replay = current->current_replay;
        next.current_generation = key->current_generation;
        memset(&next.current_replay, 0, sizeof(next.current_replay));
        next.tx_next_sequence = 1U;
        next.tx_reserved_through = 0U;
    }
    memset(canonical, 0, sizeof(canonical));
    canonical[0] = 3U;
    canonical[1] = (uint8_t)group_slot;
    canonical[2] = (uint8_t)key_slot;
    canonical[3] = (uint8_t)next.state;
    put_u32(&canonical[4], next.group_id);
    put_u32(&canonical[8], next.group_generation);
    put_u16(&canonical[12], next.key_id);
    canonical[14] = next.suite_id;
    put_u32(&canonical[16], next.current_generation);
    put_u32(&canonical[20], next.previous_generation);
    put_u64(&canonical[24], next.previous_deadline_us);
    result = verify_admin_change(manager, admin_principal, canonical,
                                 sizeof(canonical), admin_proof,
                                 admin_proof_length);
    if (result != UCN_V6_OK) {
        return result;
    }
    candidate = manager->committed;
    candidate.group_keys[group_slot][key_slot] = next;
    if (next.state == UCN_V6_GROUP_KEY_RETIRED) {
        for (index = 0U;
             index < UCN_V6_CONFIG_GROUP_REPLAY_SOURCES; ++index) {
            ucn_v6_group_replay_source_t *source =
                &candidate.group_replay_sources[index];
            if (source->occupied && source->group_id == next.group_id &&
                source->group_generation == next.group_generation &&
                source->key_id == next.key_id) {
                memset(source, 0, sizeof(*source));
            }
        }
    }
    return persist_candidate(manager, &candidate);
}

static ucn_v6_result_t deadline_add(
    uint64_t now_us,
    uint64_t duration_us,
    uint64_t *deadline_us)
{
    if (deadline_us == NULL || duration_us == 0U ||
        now_us > UINT64_MAX - duration_us) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *deadline_us = now_us + duration_us;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_security_rotate_session_keys(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    const ucn_v6_principal_t *peer_principal,
    const ucn_v6_key_selector_t *next_hop,
    const ucn_v6_key_selector_t *next_e2e,
    uint64_t previous_grace_us,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length)
{
    uint8_t canonical[80U];
    uint64_t deadline_us;
    ucn_v6_security_snapshot_t candidate;
    ucn_v6_security_session_record_t *session;
    ucn_v6_result_t result;

    if (!manager_storage_is_valid(manager) || manager->faulted ||
        !ucn_v6_principal_is_valid(peer_principal) || next_hop == NULL ||
        next_e2e == NULL ||
        deadline_add(now_us, previous_grace_us, &deadline_us) != UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = manager->committed;
    session = find_session_slot(&candidate, peer_principal, false);
    if (session == NULL || session->revoked || !session->admitted ||
        next_hop->suite_id != session->hop_current.suite_id ||
        next_hop->key_id != session->hop_current.key_id ||
        session->hop_current.key_generation ==
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        next_hop->key_generation !=
            session->hop_current.key_generation + 1U ||
        next_e2e->suite_id != session->e2e_current.suite_id ||
        next_e2e->key_id != session->e2e_current.key_id ||
        session->e2e_current.key_generation ==
            UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        next_e2e->key_generation !=
            session->e2e_current.key_generation + 1U ||
        !selector_is_valid(next_hop, UCN_V6_E2E_NONE) ||
        !selector_is_valid(next_e2e,
                           session->e2e_mode)) {
        return UCN_V6_ERR_STATE;
    }
    memset(canonical, 0, sizeof(canonical));
    canonical[0] = 4U;
    memcpy(&canonical[1], peer_principal->bytes, 16U);
    canonical[17] = next_hop->suite_id;
    put_u16(&canonical[18], next_hop->key_id);
    put_u32(&canonical[20], next_hop->key_generation);
    canonical[24] = next_e2e->suite_id;
    put_u16(&canonical[25], next_e2e->key_id);
    put_u32(&canonical[27], next_e2e->key_generation);
    put_u64(&canonical[31], deadline_us);
    put_u32(&canonical[39], session->session_generation);
    canonical[43] = session->e2e_mode;
    canonical[44] = session->hop_current.suite_id;
    put_u16(&canonical[45], session->hop_current.key_id);
    put_u32(&canonical[47], session->hop_current.key_generation);
    canonical[51] = session->e2e_current.suite_id;
    put_u16(&canonical[52], session->e2e_current.key_id);
    put_u32(&canonical[54], session->e2e_current.key_generation);
    result = verify_admin_change(manager, admin_principal, canonical,
                                 sizeof(canonical), admin_proof,
                                 admin_proof_length);
    if (result != UCN_V6_OK) {
        return result;
    }
    session->hop_previous = session->hop_current;
    session->hop_previous_deadline_us = deadline_us;
    session->hop_replay_previous = session->hop_replay_current;
    session->hop_current = *next_hop;
    memset(&session->hop_replay_current, 0,
           sizeof(session->hop_replay_current));
    session->e2e_previous = session->e2e_current;
    session->e2e_previous_deadline_us = deadline_us;
    session->e2e_replay_previous = session->e2e_replay_current;
    session->e2e_current = *next_e2e;
    memset(&session->e2e_replay_current, 0,
           sizeof(session->e2e_replay_current));
    return persist_candidate(manager, &candidate);
}

ucn_v6_result_t ucn_v6_security_revoke_session(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *peer_principal,
    const ucn_v6_principal_t *admin_principal,
    const uint8_t *admin_proof,
    size_t admin_proof_length)
{
    uint8_t canonical[32U];
    ucn_v6_security_snapshot_t candidate;
    ucn_v6_security_session_record_t *session;
    ucn_v6_result_t result;
    if (!manager_storage_is_valid(manager) || manager->faulted ||
        !ucn_v6_principal_is_valid(peer_principal)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    candidate = manager->committed;
    session = find_session_slot(&candidate, peer_principal, false);
    if (session == NULL || session->revoked) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    memset(canonical, 0, sizeof(canonical));
    canonical[0] = 5U;
    memcpy(&canonical[1], peer_principal->bytes, 16U);
    put_u32(&canonical[17], session->session_generation);
    result = verify_admin_change(manager, admin_principal, canonical,
                                 sizeof(canonical), admin_proof,
                                 admin_proof_length);
    if (result != UCN_V6_OK) {
        return result;
    }
    session->admitted = false;
    session->revoked = true;
    return persist_candidate(manager, &candidate);
}

static ucn_v6_security_session_record_t *find_session_by_principal(
    ucn_v6_security_snapshot_t *snapshot,
    const ucn_v6_principal_t *principal)
{
    return find_session_slot(snapshot, principal, false);
}

static ucn_v6_security_session_record_t *find_e2e_inbound_session(
    ucn_v6_security_snapshot_t *snapshot,
    const ucn_v6_frame_t *frame)
{
    ucn_v6_binding_key_t source;
    ucn_v6_binding_key_t destination;
    size_t index;
    source.realm_id = frame->realm_id;
    source.node_address = frame->source_address;
    source.binding_generation = frame->source_binding_generation;
    destination.realm_id = frame->realm_id;
    destination.node_address = frame->destination_address;
    destination.binding_generation = frame->destination_binding_generation;
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        ucn_v6_security_session_record_t *session = &snapshot->sessions[index];
        if (session->occupied && session->admitted && !session->revoked &&
            binding_equal(&session->peer_binding, &source) &&
            binding_equal(&session->local_binding, &destination) &&
            session->session_generation == frame->session_generation) {
            return session;
        }
    }
    return NULL;
}

static ucn_v6_security_session_record_t *find_e2e_outbound_session(
    ucn_v6_security_snapshot_t *snapshot,
    const ucn_v6_principal_t *peer,
    const ucn_v6_frame_t *frame)
{
    ucn_v6_security_session_record_t *session =
        find_session_by_principal(snapshot, peer);
    ucn_v6_binding_key_t source;
    ucn_v6_binding_key_t destination;
    if (session == NULL) {
        return NULL;
    }
    source.realm_id = frame->realm_id;
    source.node_address = frame->source_address;
    source.binding_generation = frame->source_binding_generation;
    destination.realm_id = frame->realm_id;
    destination.node_address = frame->destination_address;
    destination.binding_generation = frame->destination_binding_generation;
    return session->admitted && !session->revoked &&
           binding_equal(&session->local_binding, &source) &&
           binding_equal(&session->peer_binding, &destination) &&
           session->session_generation == frame->session_generation ?
               session : NULL;
}

static bool selector_matches_with_grace(
    const ucn_v6_key_selector_t *wire,
    const ucn_v6_key_selector_t *current,
    const ucn_v6_key_selector_t *previous,
    uint64_t previous_deadline_us,
    uint64_t now_us,
    bool *previous_selected)
{
    if (selector_equal(wire, current)) {
        *previous_selected = false;
        return true;
    }
    if (!selector_is_zero(previous) && selector_equal(wire, previous) &&
        now_us < previous_deadline_us) {
        *previous_selected = true;
        return true;
    }
    return false;
}

static ucn_v6_result_t replay_preview(
    const ucn_v6_replay_window_t *current,
    uint32_t sequence,
    ucn_v6_replay_window_t *next)
{
    uint32_t distance;
    uint64_t mask;
    if (current == NULL || next == NULL || sequence == 0U ||
        sequence > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        !replay_is_valid(current)) {
        return UCN_V6_ERR_REPLAY;
    }
    *next = *current;
    if (!current->initialized) {
        next->initialized = true;
        next->highest_sequence = sequence;
        next->seen_bitmap = UINT64_C(1);
    } else if (sequence > current->highest_sequence) {
        distance = sequence - current->highest_sequence;
        next->seen_bitmap = distance >= UCN_V6_SECURITY_REPLAY_BITS ?
                                UINT64_C(1) :
                                (current->seen_bitmap << distance) |
                                    UINT64_C(1);
        next->highest_sequence = sequence;
    } else {
        distance = current->highest_sequence - sequence;
        if (distance >= UCN_V6_SECURITY_REPLAY_BITS) {
            return UCN_V6_ERR_REPLAY;
        }
        mask = UINT64_C(1) << distance;
        if ((current->seen_bitmap & mask) != 0U) {
            return UCN_V6_ERR_REPLAY;
        }
        next->seen_bitmap |= mask;
    }
    if (next->durable_reserved_through < next->highest_sequence) {
        next->durable_reserved_through = next->highest_sequence;
    }
    return UCN_V6_OK;
}

static bool acl_entry_allows(
    const ucn_v6_acl_entry_t *entry,
    const ucn_v6_acl_key_t *request)
{
    const ucn_v6_acl_key_t *key = &entry->key;
    if (!entry->occupied || entry->revoked ||
        !principal_equal(&key->device_principal,
                         &request->device_principal) ||
        !binding_equal(&key->source_binding, &request->source_binding) ||
        !binding_equal(&key->destination_binding,
                       &request->destination_binding) ||
        key->session_generation != request->session_generation ||
        key->source_endpoint != request->source_endpoint ||
        key->destination_endpoint != request->destination_endpoint ||
        key->frame_type != request->frame_type ||
        key->protocol_opcode != request->protocol_opcode ||
        key->traffic_class != request->traffic_class ||
        key->delivery_guarantee != request->delivery_guarantee ||
        key->interaction_role != request->interaction_role ||
        key->direction != request->direction) {
        return false;
    }
    if (key->operation_id_policy == UCN_V6_OPERATION_ID_NONE) {
        return request->exact_operation_id == 0U;
    }
    if (key->operation_id_policy == UCN_V6_OPERATION_ID_ANY_NONZERO) {
        return request->exact_operation_id != 0U;
    }
    return key->operation_id_policy == UCN_V6_OPERATION_ID_EXACT &&
           key->exact_operation_id == request->exact_operation_id;
}

static void acl_request_from_frame(
    ucn_v6_acl_key_t *request,
    const ucn_v6_principal_t *principal,
    const ucn_v6_frame_t *frame,
    ucn_v6_security_direction_t direction)
{
    memset(request, 0, sizeof(*request));
    request->device_principal = *principal;
    request->source_binding.realm_id = frame->realm_id;
    request->source_binding.node_address = frame->source_address;
    request->source_binding.binding_generation =
        frame->source_binding_generation;
    request->destination_binding.realm_id = frame->realm_id;
    request->destination_binding.node_address = frame->destination_address;
    request->destination_binding.binding_generation =
        frame->destination_binding_generation;
    request->session_generation = frame->session_generation;
    request->source_endpoint = frame->message.source_endpoint;
    request->destination_endpoint = frame->message.destination_endpoint;
    request->frame_type = frame->frame_type;
    request->protocol_opcode = frame->frame_type == UCN_V6_FRAME_DATA ?
                                   0U : frame->protocol_opcode;
    request->traffic_class = frame->traffic_class;
    request->delivery_guarantee = frame->delivery_guarantee;
    request->interaction_role = frame->frame_type == UCN_V6_FRAME_DATA ?
                                    frame->message.interaction_role :
                                    UCN_V6_INTERACTION_ONE_WAY;
    request->exact_operation_id = frame->frame_type == UCN_V6_FRAME_DATA ?
                                      frame->message.operation_id : 0U;
    request->direction = direction;
}

static bool acl_allows(
    const ucn_v6_security_snapshot_t *snapshot,
    const ucn_v6_acl_key_t *request)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_ACL_ENTRIES; ++index) {
        if (acl_entry_allows(&snapshot->acl_entries[index], request)) {
            return true;
        }
    }
    return false;
}

static ucn_v6_result_t crypto_verify_tag(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *payload,
    size_t payload_length,
    const uint8_t tag[UCN_V6_SECURITY_TAG_BYTES])
{
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;
    result = ucn_v6_callback_gate_try_enter(manager->callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = manager->crypto.verify_tag(
        manager->crypto.context, selector, aad, aad_length, payload,
        payload_length, tag);
    leave_result = ucn_v6_callback_gate_leave(manager->callback_gate, manager);
    return result != UCN_V6_OK ? result : leave_result;
}

static ucn_v6_result_t crypto_compute_tag(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t tag[UCN_V6_SECURITY_TAG_BYTES])
{
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;
    result = ucn_v6_callback_gate_try_enter(manager->callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = manager->crypto.compute_tag(
        manager->crypto.context, selector, aad, aad_length, payload,
        payload_length, tag);
    leave_result = ucn_v6_callback_gate_leave(manager->callback_gate, manager);
    return result != UCN_V6_OK ? result : leave_result;
}

static ucn_v6_result_t crypto_open_aead(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *ciphertext,
    size_t ciphertext_length,
    const uint8_t tag[UCN_V6_SECURITY_TAG_BYTES],
    uint8_t *plaintext)
{
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;
    result = ucn_v6_callback_gate_try_enter(manager->callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = manager->crypto.open_aead(
        manager->crypto.context, selector, aad, aad_length, ciphertext,
        ciphertext_length, tag, plaintext);
    leave_result = ucn_v6_callback_gate_leave(manager->callback_gate, manager);
    return result != UCN_V6_OK ? result : leave_result;
}

static ucn_v6_result_t crypto_seal_aead(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_key_selector_t *selector,
    const uint8_t *aad,
    size_t aad_length,
    const uint8_t *plaintext,
    size_t plaintext_length,
    uint8_t *ciphertext,
    uint8_t tag[UCN_V6_SECURITY_TAG_BYTES])
{
    ucn_v6_result_t result;
    ucn_v6_result_t leave_result;
    result = ucn_v6_callback_gate_try_enter(manager->callback_gate, manager);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = manager->crypto.seal_aead(
        manager->crypto.context, selector, aad, aad_length, plaintext,
        plaintext_length, ciphertext, tag);
    leave_result = ucn_v6_callback_gate_leave(manager->callback_gate, manager);
    return result != UCN_V6_OK ? result : leave_result;
}

static ucn_v6_group_replay_source_t *find_group_replay_source(
    ucn_v6_security_snapshot_t *snapshot,
    const ucn_v6_frame_t *frame,
    bool allow_empty)
{
    size_t index;
    ucn_v6_group_replay_source_t *empty = NULL;
    for (index = 0U;
         index < UCN_V6_CONFIG_GROUP_REPLAY_SOURCES; ++index) {
        ucn_v6_group_replay_source_t *source =
            &snapshot->group_replay_sources[index];
        if (source->occupied && source->group_id == frame->group.group_id &&
            source->group_generation == frame->group.group_generation &&
            source->key_id == frame->group.key_id &&
            source->key_generation == frame->group.key_generation &&
            source->claimed_source.realm_id == frame->realm_id &&
            source->claimed_source.node_address == frame->source_address &&
            source->claimed_source.binding_generation ==
                frame->source_binding_generation &&
            source->claimed_session_generation == frame->session_generation) {
            return source;
        }
        if (!source->occupied && empty == NULL) {
            empty = source;
        }
    }
    return allow_empty ? empty : NULL;
}

static bool peer_discovery_contract_is_valid(const ucn_v6_frame_t *frame)
{
    if (frame == NULL || frame->frame_type != UCN_V6_FRAME_CONTROL ||
        frame->flags != (UCN_V6_FLAG_PEER_HOP_CONTEXT |
                         UCN_V6_FLAG_PROTOCOL_CONTEXT) ||
        frame->source_address == 0U || frame->destination_address == 0U ||
        frame->source_binding_generation == 0U ||
        frame->destination_binding_generation == 0U ||
        frame->session_generation == 0U || frame->hop_limit != 1U) {
        return false;
    }
    if (frame->protocol_opcode == UCN_V6_PROTOCOL_OPCODE_PEER_HELLO) {
        return frame->traffic_class == UCN_V6_TRAFFIC_Q1 &&
               frame->delivery_guarantee == UCN_V6_DELIVERY_LATEST;
    }
    if (frame->protocol_opcode == UCN_V6_PROTOCOL_OPCODE_CAPABILITY_QUERY) {
        return frame->traffic_class == UCN_V6_TRAFFIC_Q1 &&
               frame->delivery_guarantee == UCN_V6_DELIVERY_RELIABLE;
    }
    if (frame->protocol_opcode ==
        UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE) {
        return frame->traffic_class == UCN_V6_TRAFFIC_Q2 &&
               frame->delivery_guarantee == UCN_V6_DELIVERY_RELIABLE;
    }
    return false;
}

static ucn_v6_result_t reserve_tx_sequence(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *e2e_peer,
    uint32_t *sequence);

ucn_v6_result_t ucn_v6_security_open_frame(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    uint32_t ingress_link_instance_generation,
    const ucn_v6_principal_t *authenticated_peer_principal,
    const uint8_t *encoded_frame,
    size_t encoded_length,
    uint8_t *plaintext_storage,
    size_t plaintext_capacity,
    ucn_v6_security_open_result_t *result_out)
{
    ucn_v6_security_open_result_t opened;
    ucn_v6_security_snapshot_t candidate;
    ucn_v6_security_session_record_t *hop_session;
    ucn_v6_security_session_record_t *e2e_session;
    ucn_v6_group_replay_source_t *group_source;
    const ucn_v6_group_key_slot_t *group_key;
    const ucn_v6_key_selector_t *hop_selector;
    const ucn_v6_key_selector_t *e2e_selector;
    ucn_v6_key_selector_t group_selector;
    ucn_v6_key_selector_t wire_hop_selector;
    ucn_v6_key_selector_t wire_e2e_selector;
    ucn_v6_replay_window_t replay_next;
    ucn_v6_acl_key_t request;
    uint8_t aad[UCN_V6_CANONICAL_AAD_BYTES];
    size_t aad_length = 0U;
    size_t link_tag_offset;
    bool previous;
    bool local_target = false;
    ucn_v6_result_t rc;

    if (!manager_storage_is_valid(manager) || manager->faulted ||
        encoded_frame == NULL || result_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&opened, 0, sizeof(opened));
    rc = ucn_v6_wire_decode(encoded_frame, encoded_length, &opened.frame);
    if (rc != UCN_V6_OK || opened.frame.frame_type == UCN_V6_FRAME_BOOTSTRAP ||
        encoded_length < UCN_V6_SECURITY_TAG_BYTES + 4U) {
        return UCN_V6_ERR_MALFORMED;
    }
    candidate = manager->committed;
    link_tag_offset = encoded_length - UCN_V6_SECURITY_TAG_BYTES - 4U;
    if ((opened.frame.flags & UCN_V6_FLAG_GROUP_CONTEXT) != 0U) {
        group_key = find_group_key_const(
            &candidate, opened.frame.group.group_id,
            opened.frame.group.group_generation, opened.frame.group.key_id,
            opened.frame.group.key_generation);
        if (group_key == NULL) {
            return UCN_V6_ERR_SECURITY;
        }
        group_selector.suite_id = group_key->suite_id;
        group_selector.key_id = group_key->key_id;
        group_selector.key_generation = opened.frame.group.key_generation;
        if (opened.frame.group.key_generation ==
                group_key->previous_generation &&
            now_us >= group_key->previous_deadline_us) {
            return UCN_V6_ERR_SECURITY;
        }
        rc = crypto_verify_tag(manager, &group_selector, encoded_frame,
                               link_tag_offset, NULL, 0U,
                               opened.frame.link_tag);
        if (rc != UCN_V6_OK) {
            return rc;
        }
        group_source = find_group_replay_source(&candidate, &opened.frame, true);
        if (group_source == NULL) {
            return UCN_V6_ERR_NO_SPACE;
        }
        if (!group_source->occupied) {
            memset(group_source, 0, sizeof(*group_source));
            group_source->occupied = true;
            group_source->group_id = opened.frame.group.group_id;
            group_source->group_generation =
                opened.frame.group.group_generation;
            group_source->key_id = opened.frame.group.key_id;
            group_source->key_generation = opened.frame.group.key_generation;
            group_source->claimed_source.realm_id = opened.frame.realm_id;
            group_source->claimed_source.node_address =
                opened.frame.source_address;
            group_source->claimed_source.binding_generation =
                opened.frame.source_binding_generation;
            group_source->claimed_session_generation =
                opened.frame.session_generation;
        }
        rc = replay_preview(&group_source->replay,
                            opened.frame.packet_sequence, &replay_next);
        if (rc != UCN_V6_OK) {
            return rc;
        }
        group_source->replay = replay_next;
        rc = persist_candidate(manager, &candidate);
        if (rc != UCN_V6_OK) {
            return rc;
        }
        opened.group_discovery_only = true;
        *result_out = opened;
        return UCN_V6_OK;
    }
    if (authenticated_peer_principal == NULL ||
        !ucn_v6_principal_is_valid(authenticated_peer_principal)) {
        return UCN_V6_ERR_SECURITY;
    }
    hop_session = find_session_by_principal(
        &candidate, authenticated_peer_principal);
    wire_hop_selector.suite_id = opened.frame.peer_hop.suite_id;
    wire_hop_selector.key_id = opened.frame.peer_hop.key_id;
    wire_hop_selector.key_generation = opened.frame.peer_hop.key_generation;
    if (hop_session == NULL || !hop_session->admitted || hop_session->revoked ||
        now_us >= hop_session->local_lease_deadline_us ||
        hop_session->link_instance_generation !=
            ingress_link_instance_generation ||
        !selector_matches_with_grace(
            &wire_hop_selector,
            &hop_session->hop_current, &hop_session->hop_previous,
            hop_session->hop_previous_deadline_us, now_us, &previous)) {
        return UCN_V6_ERR_SECURITY;
    }
    hop_selector = previous ? &hop_session->hop_previous :
                              &hop_session->hop_current;
    rc = crypto_verify_tag(manager, hop_selector, encoded_frame,
                           link_tag_offset, NULL, 0U,
                           opened.frame.link_tag);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    rc = replay_preview(previous ? &hop_session->hop_replay_previous :
                                   &hop_session->hop_replay_current,
                        opened.frame.packet_sequence, &replay_next);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    if (previous) {
        hop_session->hop_replay_previous = replay_next;
    } else {
        hop_session->hop_replay_current = replay_next;
    }
    opened.authenticated_principal = hop_session->peer_principal;
    opened.ingress_peer_session.binding = hop_session->peer_binding;
    opened.ingress_peer_session.principal = hop_session->peer_principal;
    opened.ingress_peer_session.session_generation =
        hop_session->session_generation;
    opened.hop_authenticated = true;
    local_target = candidate.local_binding_valid &&
                   candidate.local_binding.realm_id == opened.frame.realm_id &&
                   candidate.local_binding.node_address ==
                       opened.frame.destination_address &&
                   candidate.local_binding.binding_generation ==
                       opened.frame.destination_binding_generation;
    e2e_session = local_target ?
        find_e2e_inbound_session(&candidate, &opened.frame) : NULL;
    if (local_target) {
        if ((opened.frame.flags & UCN_V6_FLAG_E2E_CONTEXT) == 0U &&
            peer_discovery_contract_is_valid(&opened.frame)) {
            rc = persist_candidate(manager, &candidate);
            if (rc != UCN_V6_OK) {
                return rc;
            }
            *result_out = opened;
            return UCN_V6_OK;
        }
        if (e2e_session == NULL) {
            return UCN_V6_ERR_SECURITY;
        }
        wire_e2e_selector.suite_id = opened.frame.e2e.suite_id;
        wire_e2e_selector.key_id = opened.frame.e2e.key_id;
        wire_e2e_selector.key_generation = opened.frame.e2e.key_generation;
        if ((opened.frame.flags & UCN_V6_FLAG_E2E_CONTEXT) == 0U ||
            !selector_matches_with_grace(
                &wire_e2e_selector,
                &e2e_session->e2e_current, &e2e_session->e2e_previous,
                e2e_session->e2e_previous_deadline_us, now_us, &previous) ||
            opened.frame.e2e.mode != e2e_session->e2e_mode ||
            (opened.frame.payload_length != 0U &&
             plaintext_storage == NULL) ||
            plaintext_capacity < opened.frame.payload_length) {
            return UCN_V6_ERR_SECURITY;
        }
        e2e_selector = previous ? &e2e_session->e2e_previous :
                                  &e2e_session->e2e_current;
        rc = ucn_v6_wire_write_canonical_aad(
            &opened.frame, aad, sizeof(aad), &aad_length);
        if (rc != UCN_V6_OK) {
            return rc;
        }
        if (opened.frame.e2e.mode == UCN_V6_E2E_AUTH_ONLY) {
            rc = crypto_verify_tag(manager, e2e_selector, aad, aad_length,
                                   opened.frame.payload,
                                   opened.frame.payload_length,
                                   opened.frame.e2e_tag);
            if (rc == UCN_V6_OK && opened.frame.payload_length != 0U) {
                memcpy(plaintext_storage, opened.frame.payload,
                       opened.frame.payload_length);
            }
        } else {
            rc = crypto_open_aead(manager, e2e_selector, aad, aad_length,
                                  opened.frame.payload,
                                  opened.frame.payload_length,
                                  opened.frame.e2e_tag, plaintext_storage);
        }
        if (rc != UCN_V6_OK) {
            return rc;
        }
        rc = replay_preview(previous ? &e2e_session->e2e_replay_previous :
                                       &e2e_session->e2e_replay_current,
                            opened.frame.packet_sequence, &replay_next);
        if (rc != UCN_V6_OK) {
            return rc;
        }
        if (previous) {
            e2e_session->e2e_replay_previous = replay_next;
        } else {
            e2e_session->e2e_replay_current = replay_next;
        }
        acl_request_from_frame(&request, &e2e_session->peer_principal,
                               &opened.frame, UCN_V6_SECURITY_INBOUND);
        if (!acl_allows(&candidate, &request)) {
            return UCN_V6_ERR_ACCESS;
        }
        opened.frame.payload = opened.frame.payload_length == 0U ?
                                   NULL : plaintext_storage;
        opened.authenticated_principal = e2e_session->peer_principal;
        opened.endpoint_authorized = true;
    }
    rc = persist_candidate(manager, &candidate);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    *result_out = opened;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_security_protect_peer_discovery(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    const ucn_v6_principal_t *peer_principal,
    ucn_v6_frame_t *frame,
    uint8_t *frame_work,
    size_t frame_work_capacity,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    ucn_v6_frame_t protected_frame;
    ucn_v6_security_session_record_t *session;
    size_t encoded_length = 0U;
    size_t link_tag_offset;
    uint32_t sequence;
    ucn_v6_result_t rc;

    if (!manager_storage_is_valid(manager) || manager->faulted ||
        !ucn_v6_principal_is_valid(peer_principal) || frame == NULL ||
        frame_work == NULL || output == NULL || output_length == NULL ||
        frame_work == output) {
        return UCN_V6_ERR_ARGUMENT;
    }
    protected_frame = *frame;
    session = find_session_by_principal(&manager->committed, peer_principal);
    if (session == NULL || !session->admitted || session->revoked ||
        session->requires_reauth || now_us >= session->local_lease_deadline_us ||
        !manager->committed.local_binding_valid ||
        !ucn_v6_binding_key_equal(&manager->committed.local_binding,
                                  &session->local_binding) ||
        protected_frame.realm_id != session->local_binding.realm_id ||
        protected_frame.source_address != session->local_binding.node_address ||
        protected_frame.source_binding_generation !=
            session->local_binding.binding_generation ||
        protected_frame.destination_address != session->peer_binding.node_address ||
        protected_frame.destination_binding_generation !=
            session->peer_binding.binding_generation ||
        protected_frame.session_generation != session->session_generation ||
        !peer_discovery_contract_is_valid(&protected_frame)) {
        return UCN_V6_ERR_SECURITY;
    }
    protected_frame.packet_sequence = 1U;
    protected_frame.peer_hop.suite_id = session->hop_current.suite_id;
    protected_frame.peer_hop.key_id = session->hop_current.key_id;
    protected_frame.peer_hop.key_generation =
        session->hop_current.key_generation;
    memset(protected_frame.link_tag, 0, sizeof(protected_frame.link_tag));
    rc = ucn_v6_wire_encoded_size(&protected_frame, &encoded_length);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    if (frame_work_capacity < encoded_length ||
        output_capacity < encoded_length) {
        return UCN_V6_ERR_NO_SPACE;
    }
    rc = reserve_tx_sequence(manager, peer_principal, &sequence);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    protected_frame.packet_sequence = sequence;
    rc = ucn_v6_wire_encode(&protected_frame, frame_work,
                            frame_work_capacity, &encoded_length);
    if (rc != UCN_V6_OK || encoded_length < 20U) {
        return rc != UCN_V6_OK ? rc : UCN_V6_ERR_STATE;
    }
    link_tag_offset = encoded_length - UCN_V6_SECURITY_TAG_BYTES - 4U;
    rc = crypto_compute_tag(manager, &session->hop_current, frame_work,
                            link_tag_offset, NULL, 0U,
                            protected_frame.link_tag);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    rc = ucn_v6_wire_encode(&protected_frame, frame_work,
                            frame_work_capacity, &encoded_length);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    memcpy(output, frame_work, encoded_length);
    *frame = protected_frame;
    *output_length = encoded_length;
    return UCN_V6_OK;
}

static ucn_v6_result_t reserve_tx_sequence(
    ucn_v6_security_manager_t *manager,
    const ucn_v6_principal_t *e2e_peer,
    uint32_t *sequence)
{
    ucn_v6_security_snapshot_t candidate;
    ucn_v6_security_session_record_t *session;
    uint32_t reserved;
    ucn_v6_result_t rc;
    candidate = manager->committed;
    session = find_session_by_principal(&candidate, e2e_peer);
    if (session == NULL || !session->admitted || session->revoked ||
        session->tx_next_sequence == 0U ||
        session->tx_next_sequence >= UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    if (session->tx_next_sequence > session->tx_reserved_through) {
        reserved = session->tx_next_sequence;
        if (reserved <= UCN_V6_SERIAL_ROTATION_THRESHOLD - 63U) {
            reserved += 63U;
        } else {
            reserved = UCN_V6_SERIAL_ROTATION_THRESHOLD;
        }
        session->tx_reserved_through = reserved;
        rc = persist_candidate(manager, &candidate);
        if (rc != UCN_V6_OK) {
            return rc;
        }
        session = find_session_by_principal(&manager->committed, e2e_peer);
    } else {
        session = find_session_by_principal(&manager->committed, e2e_peer);
    }
    if (session == NULL) {
        return UCN_V6_ERR_STATE;
    }
    *sequence = session->tx_next_sequence;
    ++session->tx_next_sequence;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_security_protect_frame(
    ucn_v6_security_manager_t *manager,
    uint64_t now_us,
    const ucn_v6_principal_t *next_hop_principal,
    const ucn_v6_principal_t *e2e_peer_principal,
    ucn_v6_frame_t *frame,
    uint8_t *payload_work,
    size_t payload_work_capacity,
    uint8_t *frame_work,
    size_t frame_work_capacity,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    ucn_v6_frame_t sealed;
    ucn_v6_security_session_record_t *hop_session;
    ucn_v6_security_session_record_t *e2e_session;
    ucn_v6_acl_key_t request;
    uint8_t aad[UCN_V6_CANONICAL_AAD_BYTES];
    size_t aad_length = 0U;
    size_t encoded_length = 0U;
    size_t link_tag_offset;
    uint32_t sequence;
    ucn_v6_result_t rc;

    if (!manager_storage_is_valid(manager) || manager->faulted ||
        !ucn_v6_principal_is_valid(next_hop_principal) ||
        !ucn_v6_principal_is_valid(e2e_peer_principal) || frame == NULL ||
        (frame->payload_length != 0U && payload_work == NULL) ||
        payload_work_capacity < frame->payload_length ||
        frame_work == NULL || output == NULL || output_length == NULL ||
        frame_work == output ||
        (frame->payload_length != 0U &&
         (payload_work == frame_work || payload_work == output))) {
        return UCN_V6_ERR_ARGUMENT;
    }
    sealed = *frame;
    if (!manager->committed.local_binding_valid ||
        manager->committed.local_binding.realm_id != sealed.realm_id ||
        manager->committed.local_binding.node_address !=
            sealed.source_address ||
        manager->committed.local_binding.binding_generation !=
            sealed.source_binding_generation) {
        return UCN_V6_ERR_STATE;
    }
    hop_session = find_session_by_principal(
        &manager->committed, next_hop_principal);
    e2e_session = find_e2e_outbound_session(
        &manager->committed, e2e_peer_principal, &sealed);
    if (hop_session == NULL || !hop_session->admitted || hop_session->revoked ||
        now_us >= hop_session->local_lease_deadline_us ||
        e2e_session == NULL ||
        now_us >= e2e_session->local_lease_deadline_us ||
        (sealed.flags & UCN_V6_FLAG_PEER_HOP_CONTEXT) == 0U ||
        (sealed.flags & UCN_V6_FLAG_GROUP_CONTEXT) != 0U ||
        (sealed.flags & UCN_V6_FLAG_E2E_CONTEXT) == 0U) {
        return UCN_V6_ERR_SECURITY;
    }
    acl_request_from_frame(&request, &manager->committed.local_principal,
                           &sealed, UCN_V6_SECURITY_OUTBOUND);
    if (!acl_allows(&manager->committed, &request)) {
        return UCN_V6_ERR_ACCESS;
    }
    sealed.packet_sequence = 1U;
    sealed.peer_hop.suite_id = hop_session->hop_current.suite_id;
    sealed.peer_hop.key_id = hop_session->hop_current.key_id;
    sealed.peer_hop.key_generation =
        hop_session->hop_current.key_generation;
    sealed.e2e.mode = e2e_session->e2e_mode;
    sealed.e2e.suite_id = e2e_session->e2e_current.suite_id;
    sealed.e2e.key_id = e2e_session->e2e_current.key_id;
    sealed.e2e.key_generation = e2e_session->e2e_current.key_generation;
    memset(sealed.e2e_tag, 0, sizeof(sealed.e2e_tag));
    memset(sealed.link_tag, 0, sizeof(sealed.link_tag));
    sealed.payload = frame->payload_length == 0U ? NULL : payload_work;
    rc = ucn_v6_wire_encoded_size(&sealed, &encoded_length);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    if (frame_work_capacity < encoded_length ||
        output_capacity < encoded_length) {
        return UCN_V6_ERR_NO_SPACE;
    }
    rc = reserve_tx_sequence(manager, e2e_peer_principal, &sequence);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    sealed.packet_sequence = sequence;
    rc = ucn_v6_wire_write_canonical_aad(
        &sealed, aad, sizeof(aad), &aad_length);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    if (sealed.e2e.mode == UCN_V6_E2E_AUTH_ONLY) {
        if (sealed.payload_length != 0U) {
            memcpy(payload_work, frame->payload, sealed.payload_length);
        }
        rc = crypto_compute_tag(manager, &e2e_session->e2e_current,
                                aad, aad_length, payload_work,
                                sealed.payload_length, sealed.e2e_tag);
    } else {
        rc = crypto_seal_aead(manager, &e2e_session->e2e_current,
                              aad, aad_length, frame->payload,
                              sealed.payload_length, payload_work,
                              sealed.e2e_tag);
    }
    if (rc != UCN_V6_OK) {
        return rc;
    }
    rc = ucn_v6_wire_encode(&sealed, frame_work, frame_work_capacity,
                            &encoded_length);
    if (rc != UCN_V6_OK || encoded_length < 20U) {
        return rc != UCN_V6_OK ? rc : UCN_V6_ERR_STATE;
    }
    link_tag_offset = encoded_length - UCN_V6_SECURITY_TAG_BYTES - 4U;
    rc = crypto_compute_tag(manager, &hop_session->hop_current, frame_work,
                            link_tag_offset, NULL, 0U, sealed.link_tag);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    rc = ucn_v6_wire_encode(&sealed, frame_work, frame_work_capacity,
                            &encoded_length);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    memcpy(output, frame_work, encoded_length);
    *frame = sealed;
    *output_length = encoded_length;
    return UCN_V6_OK;
}

static ucn_v6_result_t reserve_group_tx_sequence(
    ucn_v6_security_manager_t *manager,
    size_t group_slot,
    size_t key_slot,
    uint32_t *sequence)
{
    ucn_v6_security_snapshot_t candidate;
    ucn_v6_group_key_slot_t *key;
    uint32_t reserved;
    ucn_v6_result_t rc;
    candidate = manager->committed;
    key = &candidate.group_keys[group_slot][key_slot];
    if (key->state != UCN_V6_GROUP_KEY_ACTIVE ||
        key->tx_next_sequence == 0U ||
        key->tx_next_sequence >= UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    if (key->tx_next_sequence > key->tx_reserved_through) {
        reserved = key->tx_next_sequence;
        if (reserved <= UCN_V6_SERIAL_ROTATION_THRESHOLD - 63U) {
            reserved += 63U;
        } else {
            reserved = UCN_V6_SERIAL_ROTATION_THRESHOLD;
        }
        key->tx_reserved_through = reserved;
        rc = persist_candidate(manager, &candidate);
        if (rc != UCN_V6_OK) {
            return rc;
        }
        key = &manager->committed.group_keys[group_slot][key_slot];
    } else {
        key = &manager->committed.group_keys[group_slot][key_slot];
    }
    *sequence = key->tx_next_sequence;
    ++key->tx_next_sequence;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_security_protect_group_hello(
    ucn_v6_security_manager_t *manager,
    size_t group_slot,
    size_t key_slot,
    ucn_v6_frame_t *frame,
    uint8_t *frame_work,
    size_t frame_work_capacity,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    ucn_v6_frame_t protected_frame;
    const ucn_v6_group_policy_slot_t *group;
    const ucn_v6_group_key_slot_t *key;
    ucn_v6_key_selector_t selector;
    size_t encoded_length = 0U;
    size_t link_tag_offset;
    uint32_t sequence;
    ucn_v6_result_t rc;

    if (!manager_storage_is_valid(manager) || manager->faulted ||
        group_slot >= UCN_V6_CONFIG_STATIC_GROUP_SLOTS ||
        key_slot >= UCN_V6_CONFIG_GROUP_KEY_SLOTS || frame == NULL ||
        frame_work == NULL || output == NULL || output_length == NULL ||
        frame_work == output || !manager->committed.local_binding_valid) {
        return UCN_V6_ERR_ARGUMENT;
    }
    protected_frame = *frame;
    group = &manager->committed.groups[group_slot];
    key = &manager->committed.group_keys[group_slot][key_slot];
    if (group->state != UCN_V6_GROUP_SLOT_ACTIVE ||
        key->state != UCN_V6_GROUP_KEY_ACTIVE ||
        protected_frame.frame_type != UCN_V6_FRAME_CONTROL ||
        protected_frame.protocol_opcode !=
            UCN_V6_PROTOCOL_OPCODE_GROUP_HELLO ||
        protected_frame.flags != (UCN_V6_FLAG_GROUP_CONTEXT |
                                  UCN_V6_FLAG_PROTOCOL_CONTEXT) ||
        protected_frame.realm_id != manager->committed.local_binding.realm_id ||
        protected_frame.source_address !=
            manager->committed.local_binding.node_address ||
        protected_frame.source_binding_generation !=
            manager->committed.local_binding.binding_generation) {
        return UCN_V6_ERR_SECURITY;
    }
    protected_frame.packet_sequence = 1U;
    protected_frame.group.group_id = group->group_id;
    protected_frame.group.group_generation = group->group_generation;
    protected_frame.group.suite_id = key->suite_id;
    protected_frame.group.key_id = key->key_id;
    protected_frame.group.key_generation = key->current_generation;
    memset(protected_frame.link_tag, 0, sizeof(protected_frame.link_tag));
    rc = ucn_v6_wire_encoded_size(&protected_frame, &encoded_length);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    if (frame_work_capacity < encoded_length ||
        output_capacity < encoded_length) {
        return UCN_V6_ERR_NO_SPACE;
    }
    rc = reserve_group_tx_sequence(manager, group_slot, key_slot, &sequence);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    key = &manager->committed.group_keys[group_slot][key_slot];
    protected_frame.packet_sequence = sequence;
    rc = ucn_v6_wire_encode(&protected_frame, frame_work,
                            frame_work_capacity, &encoded_length);
    if (rc != UCN_V6_OK || encoded_length < 20U) {
        return rc != UCN_V6_OK ? rc : UCN_V6_ERR_STATE;
    }
    selector.suite_id = key->suite_id;
    selector.key_id = key->key_id;
    selector.key_generation = key->current_generation;
    link_tag_offset = encoded_length - UCN_V6_SECURITY_TAG_BYTES - 4U;
    rc = crypto_compute_tag(manager, &selector, frame_work,
                            link_tag_offset, NULL, 0U,
                            protected_frame.link_tag);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    rc = ucn_v6_wire_encode(&protected_frame, frame_work,
                            frame_work_capacity, &encoded_length);
    if (rc != UCN_V6_OK) {
        return rc;
    }
    memcpy(output, frame_work, encoded_length);
    *frame = protected_frame;
    *output_length = encoded_length;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_security_copy_view(
    const ucn_v6_security_manager_t *manager,
    ucn_v6_security_view_t *view)
{
    ucn_v6_security_view_t next;
    size_t index;
    size_t key_index;

    if (!manager_storage_is_valid(manager) || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&next, 0, sizeof(next));
    next.snapshot_generation = manager->committed.snapshot_generation;
    next.faulted = manager->faulted;
    for (index = 0U; index < UCN_V6_CONFIG_SECURITY_SESSIONS; ++index) {
        if (manager->committed.sessions[index].occupied &&
            manager->committed.sessions[index].admitted &&
            !manager->committed.sessions[index].revoked) {
            ++next.admitted_sessions;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_ACL_ENTRIES; ++index) {
        if (manager->committed.acl_entries[index].occupied &&
            !manager->committed.acl_entries[index].revoked) {
            ++next.acl_entries;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_STATIC_GROUP_SLOTS; ++index) {
        if (manager->committed.groups[index].state ==
            UCN_V6_GROUP_SLOT_ACTIVE) {
            ++next.active_groups;
        }
        for (key_index = 0U;
             key_index < UCN_V6_CONFIG_GROUP_KEY_SLOTS; ++key_index) {
            if (manager->committed.group_keys[index][key_index].state ==
                UCN_V6_GROUP_KEY_ACTIVE) {
                ++next.active_group_keys;
            }
        }
    }
    *view = next;
    return UCN_V6_OK;
}
