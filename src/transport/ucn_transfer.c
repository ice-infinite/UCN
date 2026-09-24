#include "transport/ucn_transport_private.h"

#include "internal/ucn_checked.h"

#include <string.h>

static bool setup_valid(const ucn_i_transport_transfer_setup_t *setup)
{
    uint8_t encoded[UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES];
    size_t encoded_bytes = 0U;

    return setup != NULL &&
           ucn_i_transport_transfer_setup_encode(
               setup, encoded, sizeof(encoded), &encoded_bytes) == UCN_OK &&
           encoded_bytes != 0U &&
           setup->total_length <= UCN_I_TRANSPORT_TRANSFER_BYTES &&
           setup->fragment_count <= UCN_I_TRANSPORT_TRANSFER_FRAGMENTS;
}

static bool binding_valid(const ucn_i_transport_binding_t *binding)
{
    return binding != NULL && binding->address != 0U &&
           binding->generation != 0U &&
           ucn_i_transport_p_bytes_nonzero(binding->principal,
                                           sizeof(binding->principal));
}

static bool binding_equal(const ucn_i_transport_binding_t *left,
                          const ucn_i_transport_binding_t *right)
{
    return left->address == right->address &&
           left->generation == right->generation &&
           memcmp(left->principal, right->principal,
                  sizeof(left->principal)) == 0;
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

static bool security_domain_equal(
    const ucn_i_transport_security_facts_t *left,
    const ucn_i_transport_security_facts_t *right)
{
    return left->session_generation == right->session_generation &&
           left->key_generation == right->key_generation &&
           left->link_generation == right->link_generation &&
           left->policy_generation == right->policy_generation &&
           left->link_id == right->link_id &&
           left->origin_security == right->origin_security &&
           left->endpoint_public_unauthenticated ==
               right->endpoint_public_unauthenticated &&
           left->trusted_link_policy == right->trusted_link_policy;
}

static bool transfer_facts_valid(
    const ucn_i_transport_transfer_facts_t *facts,
    uint8_t parent_kind)
{
    if (facts == NULL || !binding_valid(&facts->source) ||
        !binding_valid(&facts->destination) || facts->realm == 0U ||
        facts->transport_policy_generation == 0U ||
        facts->parent_kind != parent_kind ||
        facts->reserved_zero[0] != 0U || facts->reserved_zero[1] != 0U ||
        facts->reserved_zero[2] != 0U ||
        !ucn_i_transport_p_security_valid(&facts->security)) {
        return false;
    }
    if (parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1) {
        return bytes_zero(facts->parent_fingerprint,
                          sizeof(facts->parent_fingerprint));
    }
    return parent_kind == UCN_I_TRANSPORT_PARENT_KIND_FLOW &&
           ucn_i_transport_p_bytes_nonzero(
               facts->parent_fingerprint,
               sizeof(facts->parent_fingerprint));
}

static bool transfer_facts_equal(
    const ucn_i_transport_transfer_facts_t *left,
    const ucn_i_transport_transfer_facts_t *right)
{
    return binding_equal(&left->source, &right->source) &&
           binding_equal(&left->destination, &right->destination) &&
           security_domain_equal(&left->security, &right->security) &&
           memcmp(left->parent_fingerprint, right->parent_fingerprint,
                  sizeof(left->parent_fingerprint)) == 0 &&
           left->realm == right->realm &&
           left->transport_policy_generation ==
               right->transport_policy_generation &&
           left->parent_kind == right->parent_kind;
}

static bool response_facts_match(
    const ucn_i_transport_transfer_facts_t *forward,
    const ucn_i_transport_transfer_facts_t *response)
{
    return binding_equal(&forward->source, &response->destination) &&
           binding_equal(&forward->destination, &response->source) &&
           security_domain_equal(&forward->security, &response->security) &&
           memcmp(forward->parent_fingerprint,
                  response->parent_fingerprint,
                  sizeof(forward->parent_fingerprint)) == 0 &&
           forward->realm == response->realm &&
           forward->transport_policy_generation ==
               response->transport_policy_generation &&
           forward->parent_kind == response->parent_kind;
}

static bool c1_parent_matches_facts(
    const ucn_i_transport_parent_record_t *parent,
    const ucn_i_transport_transfer_facts_t *facts)
{
    return binding_equal(&parent->context.source, &facts->source) &&
           binding_equal(&parent->context.destination,
                         &facts->destination) &&
           parent->context.realm == facts->realm &&
           parent->context.transport_policy_generation ==
               facts->transport_policy_generation &&
           parent->context.security_session_generation ==
               facts->security.session_generation &&
           parent->context.security_key_generation ==
               facts->security.key_generation &&
           parent->context.origin_security ==
               facts->security.origin_security;
}

static ucn_result_t setup_deadline(
    const ucn_i_transport_transfer_setup_t *setup,
    uint64_t now_us,
    uint64_t *deadline_out)
{
    uint64_t duration_us;

    if (setup == NULL || deadline_out == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    duration_us = (uint64_t)setup->lifetime_ms * UINT64_C(1000);
    return ucn_i_deadline_from_duration_us(now_us, duration_us, deadline_out);
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

static bool bit_is_set(const uint8_t *bitmap, uint16_t index)
{
    return (bitmap[index / 8U] & (uint8_t)(1U << (index % 8U))) != 0U;
}

static void bit_set(uint8_t *bitmap, uint16_t index)
{
    bitmap[index / 8U] |= (uint8_t)(1U << (index % 8U));
}

static bool sack_bitmap_is_valid(const ucn_i_transport_sack_t *sack,
                                 uint16_t fragment_count)
{
    uint16_t remaining;
    uint32_t allowed;

    if (sack->window_base >= fragment_count ||
        sack->receive_credit > fragment_count ||
        sack->reserved_zero[0] != 0U || sack->reserved_zero[1] != 0U ||
        sack->reserved_zero[2] != 0U) {
        return false;
    }
    remaining = (uint16_t)(fragment_count - sack->window_base);
    allowed = remaining >= 32U ? UINT32_MAX :
                  (UINT32_C(1) << remaining) - 1U;
    return (sack->bitmap & ~allowed) == 0U;
}

static size_t fragment_offset(
    const ucn_i_transport_transfer_setup_t *setup,
    uint16_t fragment_index)
{
    return (size_t)fragment_index * setup->fragment_budget;
}

static size_t fragment_bytes(
    const ucn_i_transport_transfer_setup_t *setup,
    uint16_t fragment_index)
{
    size_t offset = fragment_offset(setup, fragment_index);
    size_t remaining = setup->total_length - offset;

    return remaining < setup->fragment_budget ? remaining :
                                                  setup->fragment_budget;
}

static ucn_result_t transfer_tx_begin(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    bool require_c1_parent,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    const uint8_t *message,
    uint32_t message_bytes,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    uint64_t deadline;
    uint8_t digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_TRANSPORT_TRANSFER_TX_COUNT;
    bool exhausted_slot = false;

    if (!setup_valid(setup) ||
        !transfer_facts_valid(facts, setup->parent_kind) ||
        facts->security.authenticated_replay_candidate != 0U ||
        message == NULL || handle_out == NULL ||
        message_bytes != setup->total_length ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), setup, sizeof(*setup)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), message, message_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(setup, sizeof(*setup), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(message, message_bytes, handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if ((require_c1_parent &&
         setup->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_C1) ||
        (!require_c1_parent &&
         setup->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_FLOW)) {
        return UCN_ERR_STATE;
    }
    result = setup_deadline(setup, now_us, &deadline);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_i_sha256_128(message, message_bytes, digest,
                              &owner->hash_workspace);
    if (result != UCN_OK || memcmp(digest, setup->message_digest, 16U) != 0) {
        ucn_i_transport_p_unlock(owner);
        return result == UCN_OK ? UCN_ERR_MALFORMED : result;
    }
    if (require_c1_parent) {
        ucn_i_transport_parent_record_t *parent_record =
            ucn_i_transport_p_parent(owner, parent);
        if (parent_record == NULL || parent_record->active == 0U ||
            parent_record->prepared != 0U ||
            parent_record->grant_available == 0U ||
            parent_record->context.parent_generation !=
                setup->parent_generation ||
            !c1_parent_matches_facts(parent_record, facts) ||
            parent_record->granted_transfer_id != setup->transfer_id ||
            parent_record->transfer_high_water != setup->transfer_id) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    for (index = 0U; index < UCN_I_TRANSPORT_TRANSFER_TX_COUNT; ++index) {
        ucn_i_transport_transfer_tx_record_t *record =
            &owner->transfer_tx[index];

        if (record->occupied == 0U) {
            if (record->generation == UINT16_MAX) {
                exhausted_slot = true;
            } else if (free_index == UCN_I_TRANSPORT_TRANSFER_TX_COUNT) {
                free_index = index;
            }
        } else if (record->setup.parent_kind == setup->parent_kind &&
                   record->setup.parent_id == setup->parent_id &&
                   record->setup.parent_generation == setup->parent_generation &&
                   record->setup.transfer_id == setup->transfer_id) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    if (free_index == UCN_I_TRANSPORT_TRANSFER_TX_COUNT) {
        ucn_i_transport_p_unlock(owner);
        return exhausted_slot ? UCN_ERR_EXHAUSTED : UCN_ERR_NO_SPACE;
    }
    {
        ucn_i_transport_transfer_tx_record_t *record =
            &owner->transfer_tx[free_index];
        uint16_t generation;

        if (next_generation(record->generation, &generation) != UCN_OK) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_EXHAUSTED;
        }

        memset(record, 0, sizeof(*record));
        record->setup = *setup;
        record->facts = *facts;
        record->facts.security.authenticated_replay_candidate = 0U;
        memcpy(record->message, message, message_bytes);
        record->deadline_us = deadline;
        record->message_bytes = message_bytes;
        record->generation = generation;
        record->phase = UCN_I_TRANSPORT_TRANSFER_SETUP_PENDING;
        record->occupied = 1U;
        *handle_out = ucn_i_transport_p_handle(
            owner, (uint16_t)free_index, generation,
            UCN_I_TRANSPORT_TRANSFER_TX_KIND);
    }
    if (require_c1_parent) {
        ucn_i_transport_parent_record_t *parent_record =
            ucn_i_transport_p_parent(owner, parent);
        parent_record->granted_transfer_id = 0U;
        parent_record->grant_available = 0U;
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_tx_begin(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    const uint8_t *message,
    uint32_t message_bytes,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    ucn_handle_t no_parent;

    memset(&no_parent, 0, sizeof(no_parent));
    return transfer_tx_begin(owner, no_parent, false, setup, facts, message,
                             message_bytes, now_us, handle_out);
}

ucn_result_t ucn_i_transport_c1_transfer_tx_begin(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    const uint8_t *message,
    uint32_t message_bytes,
    uint64_t now_us,
    ucn_handle_t *handle_out)
{
    return transfer_tx_begin(owner, parent, true, setup, facts, message,
                             message_bytes, now_us, handle_out);
}

ucn_result_t ucn_i_transport_transfer_tx_accept_setup(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *response_facts,
    uint64_t now_us)
{
    ucn_i_transport_transfer_tx_record_t *record;
    ucn_result_t result;

    if (!setup_valid(setup) ||
        !transfer_facts_valid(response_facts, setup->parent_kind) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), setup, sizeof(*setup)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), response_facts,
                             sizeof(*response_facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_transfer_tx(owner, handle);
    if (record == NULL ||
        !ucn_i_transport_p_setup_equal(&record->setup, setup) ||
        !response_facts_match(&record->facts, response_facts)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (record->phase == UCN_I_TRANSPORT_TRANSFER_COMPLETE) {
        ucn_i_transport_p_unlock(owner);
        return UCN_OK;
    }
    if (record->phase == UCN_I_TRANSPORT_TRANSFER_SENDING) {
        if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
            record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_TIMEOUT;
        }
        ucn_i_transport_p_unlock(owner);
        return UCN_OK;
    }
    if (record->phase != UCN_I_TRANSPORT_TRANSFER_SETUP_PENDING) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (response_facts->security.authenticated_replay_candidate != 0U) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_REPLAY;
    }
    if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
        record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    record->setup_accepted = 1U;
    record->phase = UCN_I_TRANSPORT_TRANSFER_SENDING;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_tx_fragment_copy(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint16_t fragment_index,
    uint64_t now_us,
    ucn_i_transport_fragment_prefix_t *prefix_out,
    uint8_t *payload_out,
    size_t payload_capacity,
    size_t *payload_bytes)
{
    ucn_i_transport_transfer_tx_record_t *record;
    ucn_i_transport_fragment_prefix_t prefix;
    size_t offset;
    size_t bytes;
    ucn_result_t result;

    if (prefix_out == NULL || payload_out == NULL || payload_bytes == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), prefix_out,
                             sizeof(*prefix_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload_out,
                             payload_capacity) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload_bytes,
                             sizeof(*payload_bytes)) ||
        ucn_i_ranges_overlap(prefix_out, sizeof(*prefix_out), payload_out,
                             payload_capacity) ||
        ucn_i_ranges_overlap(prefix_out, sizeof(*prefix_out), payload_bytes,
                             sizeof(*payload_bytes)) ||
        ucn_i_ranges_overlap(payload_bytes, sizeof(*payload_bytes), payload_out,
                             payload_capacity)) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_transfer_tx(owner, handle);
    if (record == NULL ||
        record->phase != UCN_I_TRANSPORT_TRANSFER_SENDING ||
        fragment_index >= record->setup.fragment_count) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
        record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    offset = fragment_offset(&record->setup, fragment_index);
    bytes = fragment_bytes(&record->setup, fragment_index);
    if (payload_capacity < bytes) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    memset(&prefix, 0, sizeof(prefix));
    prefix.transfer_id = record->setup.transfer_id;
    prefix.parent_generation =
        record->setup.parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
            record->setup.parent_generation : 0U;
    prefix.fragment_index = fragment_index;
    prefix.fragment_count = record->setup.fragment_count;
    prefix.parent_kind = record->setup.parent_kind;
    memcpy(payload_out, &record->message[offset], bytes);
    *prefix_out = prefix;
    *payload_bytes = bytes;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_tx_accept_sack(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_transport_sack_t *sack,
    const ucn_i_transport_transfer_facts_t *response_facts,
    uint64_t now_us)
{
    ucn_i_transport_transfer_tx_record_t *record;
    uint16_t offset;
    ucn_result_t result;

    if (sack == NULL ||
        !transfer_facts_valid(response_facts, sack->parent_kind) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), sack, sizeof(*sack)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), response_facts,
                             sizeof(*response_facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_transfer_tx(owner, handle);
    if (record == NULL ||
        (record->phase != UCN_I_TRANSPORT_TRANSFER_SENDING &&
         record->phase != UCN_I_TRANSPORT_TRANSFER_COMPLETE) ||
        sack->parent_kind != record->setup.parent_kind ||
        sack->transfer_id != record->setup.transfer_id ||
        sack->parent_generation !=
            (record->setup.parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
                 record->setup.parent_generation : 0U) ||
        sack->original_service_id !=
            (record->setup.parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
                 record->setup.service_id : 0U) ||
        !response_facts_match(&record->facts, response_facts) ||
        !sack_bitmap_is_valid(sack, record->setup.fragment_count)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (record->phase == UCN_I_TRANSPORT_TRANSFER_COMPLETE) {
        ucn_i_transport_p_unlock(owner);
        return UCN_OK;
    }
    if (response_facts->security.authenticated_replay_candidate != 0U) {
        for (offset = 0U; offset < 32U; ++offset) {
            uint16_t fragment = (uint16_t)(sack->window_base + offset);
            if (fragment >= record->setup.fragment_count) {
                break;
            }
            if ((sack->bitmap & (UINT32_C(1) << offset)) != 0U &&
                !bit_is_set(record->acknowledged, fragment)) {
                ucn_i_transport_p_unlock(owner);
                return UCN_ERR_REPLAY;
            }
        }
        ucn_i_transport_p_unlock(owner);
        return UCN_OK;
    }
    if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
        record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    for (offset = 0U; offset < 32U; ++offset) {
        uint16_t fragment = (uint16_t)(sack->window_base + offset);
        if (fragment >= record->setup.fragment_count) {
            break;
        }
        if ((sack->bitmap & (UINT32_C(1) << offset)) != 0U &&
            !bit_is_set(record->acknowledged, fragment)) {
            bit_set(record->acknowledged, fragment);
            ++record->acknowledged_count;
        }
    }
    if (record->acknowledged_count == record->setup.fragment_count) {
        record->phase = UCN_I_TRANSPORT_TRANSFER_COMPLETE;
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

static bool transfer_key_equal(
    const ucn_i_transport_transfer_setup_t *left,
    const ucn_i_transport_transfer_setup_t *right)
{
    return left->parent_kind == right->parent_kind &&
           left->parent_id == right->parent_id &&
           left->parent_generation == right->parent_generation &&
           left->transfer_id == right->transfer_id;
}

static ucn_result_t transfer_rx_setup(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    bool require_c1_parent,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    uint64_t now_us,
    ucn_handle_t *handle_out,
    bool *exact_duplicate_out)
{
    uint64_t deadline;
    ucn_result_t result;
    size_t index;
    size_t free_index = UCN_I_TRANSPORT_TRANSFER_RX_COUNT;
    size_t free_receipt = UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT;
    bool exhausted_slot = false;
    bool exhausted_receipt = false;

    if (!setup_valid(setup) ||
        !transfer_facts_valid(facts, setup->parent_kind) ||
        handle_out == NULL ||
        exact_duplicate_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), setup, sizeof(*setup)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), exact_duplicate_out,
                             sizeof(*exact_duplicate_out)) ||
        ucn_i_ranges_overlap(setup, sizeof(*setup), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(setup, sizeof(*setup), exact_duplicate_out,
                             sizeof(*exact_duplicate_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), exact_duplicate_out,
                             sizeof(*exact_duplicate_out)) ||
        ucn_i_ranges_overlap(handle_out, sizeof(*handle_out),
                             exact_duplicate_out,
                             sizeof(*exact_duplicate_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if ((require_c1_parent &&
         setup->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_C1) ||
        (!require_c1_parent &&
         setup->parent_kind != UCN_I_TRANSPORT_PARENT_KIND_FLOW)) {
        return UCN_ERR_STATE;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    for (index = 0U; index < UCN_I_TRANSPORT_TRANSFER_RX_COUNT; ++index) {
        ucn_i_transport_transfer_rx_record_t *record =
            &owner->transfer_rx[index];

        if (record->occupied == 0U) {
            if (record->generation == UINT16_MAX) {
                exhausted_slot = true;
            } else if (free_index == UCN_I_TRANSPORT_TRANSFER_RX_COUNT) {
                free_index = index;
            }
            continue;
        }
        if (transfer_key_equal(&record->setup, setup)) {
            if (!ucn_i_transport_p_setup_equal(&record->setup, setup) ||
                !transfer_facts_equal(&record->facts, facts)) {
                ucn_i_transport_p_unlock(owner);
                return UCN_ERR_REPLAY;
            }
            if (facts->security.origin_security != 0U &&
                facts->security.authenticated_replay_candidate == 0U) {
                ucn_i_transport_p_unlock(owner);
                return UCN_ERR_REPLAY;
            }
            if (record->phase == UCN_I_TRANSPORT_TRANSFER_DELIVERED) {
                ucn_i_transport_terminal_receipt_record_t *receipt;
                if (record->receipt_slot >=
                    UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT) {
                    ucn_i_transport_p_unlock(owner);
                    return UCN_ERR_STATE;
                }
                receipt = &owner->transfer_receipts[record->receipt_slot];
                if (receipt->occupied == 0U || receipt->terminal == 0U ||
                    ucn_i_deadline_expired_us(now_us,
                                              receipt->expires_at_us)) {
                    ucn_i_transport_p_unlock(owner);
                    return UCN_ERR_TIMEOUT;
                }
                *handle_out = ucn_i_transport_p_handle(
                    owner, record->receipt_slot, receipt->generation,
                    UCN_I_TRANSPORT_TRANSFER_RECEIPT_KIND);
                *exact_duplicate_out = true;
                ucn_i_transport_p_unlock(owner);
                return UCN_OK;
            }
            if (record->phase == UCN_I_TRANSPORT_TRANSFER_RECEIVING &&
                ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
                record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
                ucn_i_transport_p_unlock(owner);
                return UCN_ERR_TIMEOUT;
            }
            if (record->phase == UCN_I_TRANSPORT_TRANSFER_ABORTED ||
                record->phase == UCN_I_TRANSPORT_TRANSFER_FAILED) {
                ucn_i_transport_p_unlock(owner);
                return UCN_ERR_STATE;
            }
            *handle_out = ucn_i_transport_p_handle(
                owner, (uint16_t)index, record->generation,
                UCN_I_TRANSPORT_TRANSFER_RX_KIND);
            *exact_duplicate_out = true;
            ucn_i_transport_p_unlock(owner);
            return UCN_OK;
        }
    }
    for (index = 0U; index < UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT;
         ++index) {
        ucn_i_transport_terminal_receipt_record_t *receipt =
            &owner->transfer_receipts[index];

        if (receipt->occupied == 0U) {
            if (receipt->generation == UINT16_MAX) {
                exhausted_receipt = true;
            } else if (free_receipt ==
                       UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT) {
                free_receipt = index;
            }
            continue;
        }
        if (!transfer_key_equal(&receipt->setup, setup)) {
            continue;
        }
        if (!ucn_i_transport_p_setup_equal(&receipt->setup, setup) ||
            !transfer_facts_equal(&receipt->facts, facts)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_REPLAY;
        }
        if (facts->security.origin_security != 0U &&
            facts->security.authenticated_replay_candidate == 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_REPLAY;
        }
        if (receipt->terminal == 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        if (ucn_i_deadline_expired_us(now_us, receipt->expires_at_us)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_TIMEOUT;
        }
        *handle_out = ucn_i_transport_p_handle(
            owner, (uint16_t)index, receipt->generation,
            UCN_I_TRANSPORT_TRANSFER_RECEIPT_KIND);
        *exact_duplicate_out = true;
        ucn_i_transport_p_unlock(owner);
        return UCN_OK;
    }
    if (require_c1_parent) {
        ucn_i_transport_parent_record_t *parent_record =
            ucn_i_transport_p_parent(owner, parent);
        if (parent_record == NULL || parent_record->active == 0U ||
            parent_record->prepared != 0U ||
            parent_record->context.parent_generation !=
                setup->parent_generation ||
            !c1_parent_matches_facts(parent_record, facts) ||
            parent_record->transfer_high_water != setup->transfer_id) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    if (facts->security.authenticated_replay_candidate != 0U) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_REPLAY;
    }
    result = setup_deadline(setup, now_us, &deadline);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    if (free_index == UCN_I_TRANSPORT_TRANSFER_RX_COUNT) {
        ucn_i_transport_p_unlock(owner);
        return exhausted_slot ? UCN_ERR_EXHAUSTED : UCN_ERR_NO_SPACE;
    }
    if (free_receipt == UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT) {
        ucn_i_transport_p_unlock(owner);
        return exhausted_receipt ? UCN_ERR_EXHAUSTED : UCN_ERR_NO_SPACE;
    }
    {
        ucn_i_transport_transfer_rx_record_t *record =
            &owner->transfer_rx[free_index];
        ucn_i_transport_terminal_receipt_record_t *receipt =
            &owner->transfer_receipts[free_receipt];
        uint16_t generation;
        uint16_t receipt_generation;

        if (next_generation(record->generation, &generation) != UCN_OK ||
            next_generation(receipt->generation, &receipt_generation) !=
                UCN_OK) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_EXHAUSTED;
        }

        memset(record, 0, sizeof(*record));
        memset(receipt, 0, sizeof(*receipt));
        record->setup = *setup;
        record->facts = *facts;
        record->facts.security.authenticated_replay_candidate = 0U;
        record->deadline_us = deadline;
        record->generation = generation;
        record->receipt_slot = (uint16_t)free_receipt;
        record->phase = UCN_I_TRANSPORT_TRANSFER_RECEIVING;
        record->occupied = 1U;
        receipt->setup = *setup;
        receipt->facts = record->facts;
        receipt->generation = receipt_generation;
        receipt->occupied = 1U;
        *handle_out = ucn_i_transport_p_handle(
            owner, (uint16_t)free_index, generation,
            UCN_I_TRANSPORT_TRANSFER_RX_KIND);
        *exact_duplicate_out = false;
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_rx_setup(
    ucn_i_transport_owner_t *owner,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    uint64_t now_us,
    ucn_handle_t *handle_out,
    bool *exact_duplicate_out)
{
    ucn_handle_t no_parent;

    memset(&no_parent, 0, sizeof(no_parent));
    return transfer_rx_setup(owner, no_parent, false, setup, facts, now_us,
                             handle_out, exact_duplicate_out);
}

ucn_result_t ucn_i_transport_c1_transfer_rx_setup(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t parent,
    const ucn_i_transport_transfer_setup_t *setup,
    const ucn_i_transport_transfer_facts_t *facts,
    uint64_t now_us,
    ucn_handle_t *handle_out,
    bool *exact_duplicate_out)
{
    return transfer_rx_setup(owner, parent, true, setup, facts, now_us,
                             handle_out, exact_duplicate_out);
}

static ucn_result_t fragment_fingerprint(
    ucn_i_transport_owner_t *owner,
    const uint8_t *payload,
    uint16_t payload_bytes,
    const uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    uint8_t digest_out[UCN_I_TRANSPORT_DIGEST_BYTES])
{
    uint8_t canonical[UCN_I_TRANSPORT_DIGEST_BYTES * 2U];
    uint8_t payload_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    ucn_result_t result = ucn_i_sha256_128(
        payload, payload_bytes, payload_digest, &owner->hash_workspace);

    if (result != UCN_OK) {
        return result;
    }
    memcpy(canonical, aad_digest, 16U);
    memcpy(&canonical[16], payload_digest, 16U);
    return ucn_i_sha256_128(canonical, sizeof(canonical), digest_out,
                            &owner->hash_workspace);
}

ucn_result_t ucn_i_transport_transfer_rx_fragment(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    const ucn_i_transport_fragment_prefix_t *prefix,
    const ucn_i_transport_transfer_facts_t *facts,
    const uint8_t *payload,
    uint16_t payload_bytes,
    const uint8_t aad_digest[UCN_I_TRANSPORT_DIGEST_BYTES],
    uint64_t now_us,
    bool *exact_duplicate_out)
{
    ucn_i_transport_transfer_rx_record_t *record;
    uint8_t fingerprint[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t message_digest[UCN_I_TRANSPORT_DIGEST_BYTES];
    size_t offset;
    size_t expected;
    ucn_result_t result;

    if (prefix == NULL ||
        !transfer_facts_valid(facts, prefix->parent_kind) ||
        payload == NULL || payload_bytes == 0U ||
        aad_digest == NULL || exact_duplicate_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), prefix,
                             sizeof(*prefix)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload, payload_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), aad_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), exact_duplicate_out,
                             sizeof(*exact_duplicate_out)) ||
        ucn_i_ranges_overlap(prefix, sizeof(*prefix), exact_duplicate_out,
                             sizeof(*exact_duplicate_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), exact_duplicate_out,
                             sizeof(*exact_duplicate_out)) ||
        ucn_i_ranges_overlap(payload, payload_bytes, exact_duplicate_out,
                             sizeof(*exact_duplicate_out)) ||
        ucn_i_ranges_overlap(aad_digest, 16U, exact_duplicate_out,
                             sizeof(*exact_duplicate_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_transfer_rx(owner, handle);
    if (record == NULL ||
        (record->phase != UCN_I_TRANSPORT_TRANSFER_RECEIVING &&
         record->phase != UCN_I_TRANSPORT_TRANSFER_COMPLETE &&
         record->phase != UCN_I_TRANSPORT_TRANSFER_DELIVERED) ||
        prefix->parent_kind != record->setup.parent_kind ||
        prefix->transfer_id != record->setup.transfer_id ||
        prefix->parent_generation !=
            (record->setup.parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
                 record->setup.parent_generation : 0U) ||
        prefix->fragment_count != record->setup.fragment_count ||
        prefix->fragment_index >= record->setup.fragment_count ||
        !transfer_facts_equal(&record->facts, facts)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (record->phase == UCN_I_TRANSPORT_TRANSFER_DELIVERED) {
        ucn_i_transport_terminal_receipt_record_t *receipt;
        if (record->receipt_slot >=
            UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        receipt = &owner->transfer_receipts[record->receipt_slot];
        if (receipt->occupied == 0U || receipt->terminal == 0U ||
            ucn_i_deadline_expired_us(now_us, receipt->expires_at_us)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_TIMEOUT;
        }
    }
    expected = fragment_bytes(&record->setup, prefix->fragment_index);
    if (payload_bytes != expected) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_MALFORMED;
    }
    result = fragment_fingerprint(owner, payload, payload_bytes, aad_digest,
                                  fingerprint);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    if (bit_is_set(record->received, prefix->fragment_index)) {
        if (memcmp(record->fragment_digest[prefix->fragment_index], fingerprint,
                   16U) != 0) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_REPLAY;
        }
        if (facts->security.origin_security != 0U &&
            facts->security.authenticated_replay_candidate == 0U) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_REPLAY;
        }
        *exact_duplicate_out = true;
        ucn_i_transport_p_unlock(owner);
        return UCN_OK;
    }
    if (record->phase != UCN_I_TRANSPORT_TRANSFER_RECEIVING) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (facts->security.authenticated_replay_candidate != 0U) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_REPLAY;
    }
    if (ucn_i_deadline_expired_us(now_us, record->deadline_us)) {
        record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    offset = fragment_offset(&record->setup, prefix->fragment_index);
    memcpy(&record->message[offset], payload, payload_bytes);
    memcpy(record->fragment_digest[prefix->fragment_index], fingerprint, 16U);
    bit_set(record->received, prefix->fragment_index);
    ++record->received_count;
    record->received_bytes += payload_bytes;
    *exact_duplicate_out = false;
    if (record->received_count == record->setup.fragment_count) {
        result = ucn_i_sha256_128(record->message, record->setup.total_length,
                                  message_digest, &owner->hash_workspace);
        if (result != UCN_OK ||
            memcmp(message_digest, record->setup.message_digest, 16U) != 0) {
            record->phase = UCN_I_TRANSPORT_TRANSFER_FAILED;
            ucn_i_transport_p_unlock(owner);
            return result == UCN_OK ? UCN_ERR_SECURITY : result;
        }
        record->phase = UCN_I_TRANSPORT_TRANSFER_COMPLETE;
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_rx_sack(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint16_t window_base,
    uint16_t receive_credit,
    ucn_i_transport_sack_t *sack_out)
{
    ucn_i_transport_transfer_rx_record_t *record;
    ucn_i_transport_sack_t sack;
    uint16_t offset;
    ucn_result_t result;

    if (sack_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), sack_out,
                             sizeof(*sack_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_transfer_rx(owner, handle);
    if (record == NULL ||
        (record->phase != UCN_I_TRANSPORT_TRANSFER_RECEIVING &&
         record->phase != UCN_I_TRANSPORT_TRANSFER_COMPLETE) ||
        window_base >= record->setup.fragment_count) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (receive_credit > record->setup.fragment_count) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_ARGUMENT;
    }
    memset(&sack, 0, sizeof(sack));
    sack.parent_kind = record->setup.parent_kind;
    sack.transfer_id = record->setup.transfer_id;
    sack.parent_generation =
        record->setup.parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
            record->setup.parent_generation : 0U;
    sack.original_service_id =
        record->setup.parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ?
            record->setup.service_id : 0U;
    sack.window_base = window_base;
    sack.receive_credit = receive_credit;
    for (offset = 0U; offset < 32U; ++offset) {
        uint16_t fragment = (uint16_t)(window_base + offset);
        if (fragment >= record->setup.fragment_count) {
            break;
        }
        if (bit_is_set(record->received, fragment)) {
            sack.bitmap |= UINT32_C(1) << offset;
        }
    }
    *sack_out = sack;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_rx_copy_complete(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    ucn_i_transport_transfer_rx_record_t *record;
    ucn_result_t result;

    if (output == NULL || output_bytes == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                             output_capacity) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                             sizeof(*output_bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_transfer_rx(owner, handle);
    if (record == NULL ||
        record->phase != UCN_I_TRANSPORT_TRANSFER_COMPLETE) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (output_capacity < record->setup.total_length) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    memcpy(output, record->message, record->setup.total_length);
    *output_bytes = record->setup.total_length;
    record->copy_completed = 1U;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_rx_mark_delivered(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint64_t now_us)
{
    ucn_i_transport_transfer_rx_record_t *record;
    ucn_i_transport_terminal_receipt_record_t *receipt;
    uint64_t expires_at;
    ucn_result_t result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_transfer_rx(owner, handle);
    if (record == NULL ||
        record->phase != UCN_I_TRANSPORT_TRANSFER_COMPLETE ||
        record->copy_completed == 0U ||
        record->receipt_slot >= UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    receipt = &owner->transfer_receipts[record->receipt_slot];
    if (receipt->occupied == 0U || receipt->terminal != 0U ||
        !ucn_i_transport_p_setup_equal(&receipt->setup, &record->setup) ||
        !transfer_facts_equal(&receipt->facts, &record->facts)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = ucn_i_deadline_from_duration_us(
        now_us, owner->receipt_lifetime_us, &expires_at);
    if (result != UCN_OK) {
        ucn_i_transport_p_unlock(owner);
        return result;
    }
    receipt->expires_at_us = expires_at;
    receipt->terminal_status = 0U;
    receipt->terminal = 1U;
    record->phase = UCN_I_TRANSPORT_TRANSFER_DELIVERED;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_view(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    ucn_i_transport_transfer_view_t *view_out)
{
    ucn_i_transport_transfer_view_t view;
    ucn_result_t result;

    if (view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    memset(&view, 0, sizeof(view));
    if (handle.object_kind == UCN_I_TRANSPORT_TRANSFER_TX_KIND) {
        ucn_i_transport_transfer_tx_record_t *record =
            ucn_i_transport_p_transfer_tx(owner, handle);
        if (record == NULL) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_NOT_FOUND;
        }
        view.setup = record->setup;
        view.deadline_us = record->deadline_us;
        view.completed_fragments = record->acknowledged_count;
        view.phase = record->phase;
    } else if (handle.object_kind == UCN_I_TRANSPORT_TRANSFER_RX_KIND) {
        ucn_i_transport_transfer_rx_record_t *record =
            ucn_i_transport_p_transfer_rx(owner, handle);
        if (record == NULL) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_NOT_FOUND;
        }
        view.setup = record->setup;
        view.deadline_us = record->deadline_us;
        view.completed_bytes = record->received_bytes;
        view.completed_fragments = record->received_count;
        view.phase = record->phase;
    } else {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_ARGUMENT;
    }
    *view_out = view;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_retire(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_result_t result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (handle.object_kind == UCN_I_TRANSPORT_TRANSFER_TX_KIND) {
        ucn_i_transport_transfer_tx_record_t *record =
            ucn_i_transport_p_transfer_tx(owner, handle);
        uint16_t generation;
        if (record == NULL ||
            (record->phase != UCN_I_TRANSPORT_TRANSFER_COMPLETE &&
             record->phase != UCN_I_TRANSPORT_TRANSFER_ABORTED &&
             record->phase != UCN_I_TRANSPORT_TRANSFER_FAILED)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        generation = record->generation;
        memset(record, 0, sizeof(*record));
        record->generation = generation;
    } else if (handle.object_kind == UCN_I_TRANSPORT_TRANSFER_RX_KIND) {
        ucn_i_transport_transfer_rx_record_t *record =
            ucn_i_transport_p_transfer_rx(owner, handle);
        ucn_i_transport_terminal_receipt_record_t *receipt;
        uint16_t generation;
        if (record == NULL ||
            (record->phase != UCN_I_TRANSPORT_TRANSFER_DELIVERED &&
             record->phase != UCN_I_TRANSPORT_TRANSFER_ABORTED &&
             record->phase != UCN_I_TRANSPORT_TRANSFER_FAILED)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        if (record->receipt_slot >=
            UCN_I_TRANSPORT_TRANSFER_RECEIPT_COUNT) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        receipt = &owner->transfer_receipts[record->receipt_slot];
        if (receipt->occupied == 0U ||
            !ucn_i_transport_p_setup_equal(&receipt->setup,
                                           &record->setup) ||
            !transfer_facts_equal(&receipt->facts, &record->facts) ||
            (record->phase == UCN_I_TRANSPORT_TRANSFER_DELIVERED &&
             receipt->terminal == 0U)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        if (record->phase != UCN_I_TRANSPORT_TRANSFER_DELIVERED) {
            uint16_t receipt_generation = receipt->generation;
            memset(receipt, 0, sizeof(*receipt));
            receipt->generation = receipt_generation;
        }
        generation = record->generation;
        memset(record, 0, sizeof(*record));
        record->generation = generation;
    } else {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_ARGUMENT;
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_transfer_abort(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle)
{
    ucn_result_t result = ucn_i_transport_p_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (handle.object_kind == UCN_I_TRANSPORT_TRANSFER_TX_KIND) {
        ucn_i_transport_transfer_tx_record_t *record =
            ucn_i_transport_p_transfer_tx(owner, handle);
        if (record == NULL ||
            (record->phase != UCN_I_TRANSPORT_TRANSFER_SETUP_PENDING &&
             record->phase != UCN_I_TRANSPORT_TRANSFER_SENDING)) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        record->phase = UCN_I_TRANSPORT_TRANSFER_ABORTED;
    } else if (handle.object_kind == UCN_I_TRANSPORT_TRANSFER_RX_KIND) {
        ucn_i_transport_transfer_rx_record_t *record =
            ucn_i_transport_p_transfer_rx(owner, handle);
        if (record == NULL ||
            record->phase != UCN_I_TRANSPORT_TRANSFER_RECEIVING) {
            ucn_i_transport_p_unlock(owner);
            return UCN_ERR_STATE;
        }
        record->phase = UCN_I_TRANSPORT_TRANSFER_ABORTED;
    } else {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_ARGUMENT;
    }
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_transport_terminal_receipt_view(
    ucn_i_transport_owner_t *owner,
    ucn_handle_t handle,
    uint64_t now_us,
    ucn_i_transport_terminal_receipt_view_t *view_out)
{
    ucn_i_transport_terminal_receipt_record_t *record;
    ucn_i_transport_terminal_receipt_view_t view;
    ucn_result_t result;

    if (view_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_transport_p_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    record = ucn_i_transport_p_transfer_receipt(owner, handle);
    if (record == NULL || record->terminal == 0U) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    if (ucn_i_deadline_expired_us(now_us, record->expires_at_us)) {
        ucn_i_transport_p_unlock(owner);
        return UCN_ERR_TIMEOUT;
    }
    memset(&view, 0, sizeof(view));
    view.setup = record->setup;
    view.expires_at_us = record->expires_at_us;
    view.terminal_status = record->terminal_status;
    *view_out = view;
    ucn_i_transport_p_unlock(owner);
    return UCN_OK;
}
