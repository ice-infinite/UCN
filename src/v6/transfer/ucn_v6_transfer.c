#include "../internal/ucn_v6_transfer_private.h"

#include <limits.h>
#include <string.h>

#define UCN_V6_TRANSFER_SCHEMA UINT16_C(1)

typedef char ucn_v6_transfer_owner_storage_must_fit[
    sizeof(struct ucn_v6_transfer_owner) <= UCN_V6_TRANSFER_OWNER_STORAGE_BYTES ?
        1 : -1];

static void saturating_increment(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++(*value);
    }
}

static bool principal_equal(const ucn_v6_principal_t *left,
                            const ucn_v6_principal_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->bytes, right->bytes, sizeof(left->bytes)) == 0;
}

static bool session_equal(const ucn_v6_session_key_t *left,
                          const ucn_v6_session_key_t *right)
{
    return left != NULL && right != NULL &&
           principal_equal(&left->principal, &right->principal) &&
           ucn_v6_binding_key_equal(&left->binding, &right->binding) &&
           left->session_generation == right->session_generation;
}

static bool session_is_valid(const ucn_v6_session_key_t *session)
{
    return session != NULL &&
           ucn_v6_principal_is_valid(&session->principal) &&
           ucn_v6_binding_key_is_valid(&session->binding) &&
           session->session_generation != 0U &&
           session->session_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool owner_is_valid(const ucn_v6_transfer_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_TRANSFER_OWNER_MAGIC &&
           owner->schema == UCN_V6_TRANSFER_SCHEMA && owner->initialized &&
           !owner->faulted && owner->canary == UCN_V6_TRANSFER_OWNER_CANARY &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH;
}

static bool deadline_build(uint64_t now_us,
                           uint64_t duration_us,
                           uint64_t *deadline_us)
{
    if (duration_us == 0U || UINT64_MAX - now_us < duration_us) {
        return false;
    }
    *deadline_us = now_us + duration_us;
    return true;
}

static bool domain_is_valid(const ucn_v6_route_domain_t *domain)
{
    return domain != NULL &&
           ucn_v6_principal_is_valid(&domain->origin_principal) &&
           ucn_v6_principal_is_valid(&domain->destination_principal) &&
           ucn_v6_binding_key_is_valid(&domain->origin_binding) &&
           ucn_v6_binding_key_is_valid(&domain->destination_binding) &&
           domain->origin_binding.realm_id ==
               domain->destination_binding.realm_id &&
           domain->origin_session_generation != 0U &&
           domain->origin_session_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool send_request_is_valid(
    const ucn_v6_transfer_send_request_t *request)
{
    uint32_t count;
    size_t class_bytes;
    if (request == NULL || !domain_is_valid(&request->route_domain) ||
        !request->path.valid || request->message_id == 0U ||
        request->message_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        request->buffer_token == 0U || request->payload == NULL ||
        request->payload_length == 0U ||
        request->payload_length > UCN_V6_TRANSFER_MAX_MESSAGE_BYTES ||
        request->message.payload_length != request->payload_length ||
        request->message.operation_id == 0U ||
        request->message.operation_id > UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        request->message_id != request->message.operation_id ||
        request->message.delivery_guarantee != UCN_V6_DELIVERY_RELIABLE ||
        (request->message.traffic_class != UCN_V6_TRAFFIC_Q2 &&
         request->message.traffic_class != UCN_V6_TRAFFIC_Q3) ||
        request->message.source_endpoint == 0U ||
        request->message.destination_endpoint == 0U ||
        request->fragment_data_budget == 0U ||
        request->fragment_data_budget > request->path.fragment_data_budget ||
        request->window_size == 0U ||
        request->window_size > UCN_V6_CONFIG_TRANSFER_WINDOW ||
        request->window_size > request->path.max_window ||
        (request->path.feature_bits & UCN_V6_FEATURE_TRANSFER) == 0U ||
        request->path.route_generation == 0U ||
        request->path.route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        request->path.path_id == 0U || request->path.path_generation == 0U ||
        request->path.path_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return false;
    }
    if (!principal_equal(&request->path.destination_principal,
                         &request->route_domain.destination_principal) ||
        !ucn_v6_binding_key_equal(&request->path.destination_binding,
                                  &request->route_domain.destination_binding) ||
        request->path.session_generation == 0U ||
        request->path.max_message_class < request->message_class ||
        request->path.deadline_us == 0U) {
        return false;
    }
    class_bytes = ucn_v6_message_class_bytes(request->message_class);
    if (class_bytes == 0U ||
        (uint32_t)request->message_class >
            UCN_V6_CONFIG_TRANSFER_MAX_CLASS ||
        request->payload_length > class_bytes) {
        return false;
    }
    count = ((uint32_t)request->payload_length +
             request->fragment_data_budget - 1U) /
            request->fragment_data_budget;
    return count != 0U && count <= UINT16_MAX;
}

static ucn_v6_transfer_tx_slot_t *find_tx(ucn_v6_transfer_owner_t *owner,
                                          uint64_t message_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            owner->tx[index].request.message_id == message_id) {
            return &owner->tx[index];
        }
    }
    return NULL;
}

static const ucn_v6_transfer_tx_slot_t *find_tx_const(
    const ucn_v6_transfer_owner_t *owner,
    uint64_t message_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            owner->tx[index].request.message_id == message_id) {
            return &owner->tx[index];
        }
    }
    return NULL;
}

static ucn_v6_transfer_tx_slot_t *find_free_tx(ucn_v6_transfer_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        if (!owner->tx[index].occupied) {
            return &owner->tx[index];
        }
    }
    return NULL;
}

static void initialize_tx_window(ucn_v6_transfer_tx_slot_t *tx)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_WINDOW; ++index) {
        memset(&tx->window[index], 0, sizeof(tx->window[index]));
        if ((uint32_t)tx->cumulative_base + index < tx->fragment_count &&
            index < tx->request.window_size) {
            tx->window[index].fragment_index =
                (uint16_t)(tx->cumulative_base + index);
        } else {
            tx->window[index].fragment_index = UINT16_MAX;
        }
    }
}

ucn_v6_result_t ucn_v6_transfer_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    uint64_t retry_interval_us,
    uint8_t fragment_max_attempts,
    uint64_t receive_timeout_us,
    uint64_t recent_completion_us,
    ucn_v6_transfer_owner_t **owner_out)
{
    ucn_v6_transfer_owner_t *owner;
    if (owner_out == NULL || retry_interval_us == 0U ||
        fragment_max_attempts == 0U || receive_timeout_us == 0U ||
        recent_completion_us == 0U ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_TRANSFER_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK) {
        return UCN_V6_ERR_CONFIG;
    }
    owner = (ucn_v6_transfer_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_TRANSFER_OWNER_MAGIC;
    owner->schema = UCN_V6_TRANSFER_SCHEMA;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->retry_interval_us = retry_interval_us;
    owner->fragment_max_attempts = fragment_max_attempts;
    owner->receive_timeout_us = receive_timeout_us;
    owner->recent_completion_us = recent_completion_us;
    owner->initialized = true;
    owner->canary = UCN_V6_TRANSFER_OWNER_CANARY;
    *owner_out = owner;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_send_begin(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_transfer_send_request_t *request)
{
    ucn_v6_transfer_tx_slot_t *tx;
    uint16_t fragment_count;
    uint16_t path_concurrency = 0U;
    size_t index;
    if (!owner_is_valid(owner) || !send_request_is_valid(request) ||
        request->path.deadline_us <= now_us) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (find_tx(owner, request->message_id) != NULL ||
        request->message_id <= owner->tx_message_high_water) {
        return UCN_V6_ERR_REPLAY;
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            principal_equal(
                &owner->tx[index].request.path.destination_principal,
                &request->path.destination_principal) &&
            ucn_v6_binding_key_equal(
                &owner->tx[index].request.path.destination_binding,
                &request->path.destination_binding) &&
            owner->tx[index].request.path.session_generation ==
                request->path.session_generation &&
            owner->tx[index].request.path.route_generation ==
                request->path.route_generation &&
            owner->tx[index].request.path.path_id == request->path.path_id &&
            owner->tx[index].request.path.path_generation ==
                request->path.path_generation) {
            ++path_concurrency;
        }
    }
    if (path_concurrency >= request->path.max_concurrency) {
        return UCN_V6_ERR_NO_SPACE;
    }
    if (owner->tx_message_high_water ==
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        request->message_id != owner->tx_message_high_water + 1U) {
        return UCN_V6_ERR_REPLAY;
    }
    if ((tx = find_free_tx(owner)) == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    fragment_count = (uint16_t)(
        ((uint32_t)request->payload_length + request->fragment_data_budget -
         1U) /
        request->fragment_data_budget);
    memset(tx, 0, sizeof(*tx));
    tx->occupied = true;
    tx->phase = UCN_V6_TRANSFER_TX_SENDING;
    tx->request = *request;
    tx->fragment_count = fragment_count;
    tx->message_crc32c =
        ucn_v6_crc32c(request->payload, request->payload_length);
    initialize_tx_window(tx);
    owner->tx_message_high_water = request->message_id;
    ++owner->stats.tx_active;
    saturating_increment(&owner->stats.transfers_started);
    return UCN_V6_OK;
}

static ucn_v6_transfer_tx_fragment_state_t *find_tx_fragment(
    ucn_v6_transfer_tx_slot_t *tx,
    uint16_t fragment_index)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_WINDOW; ++index) {
        if (tx->window[index].fragment_index == fragment_index) {
            return &tx->window[index];
        }
    }
    return NULL;
}

ucn_v6_result_t ucn_v6_transfer_next_fragment(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    uint64_t message_id,
    ucn_v6_transfer_fragment_t *fragment_out)
{
    ucn_v6_transfer_tx_slot_t *tx;
    ucn_v6_transfer_fragment_t fragment;
    size_t index;
    if (!owner_is_valid(owner) || fragment_out == NULL || message_id == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (owner->selected) {
        return UCN_V6_ERR_STATE;
    }
    tx = find_tx(owner, message_id);
    if (tx == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (tx->phase != UCN_V6_TRANSFER_TX_SENDING) {
        return tx->phase == UCN_V6_TRANSFER_TX_REASSEMBLED ?
                   UCN_V6_ERR_STATE : UCN_V6_ERR_TIMEOUT;
    }
    if (now_us >= tx->request.path.deadline_us) {
        tx->phase = UCN_V6_TRANSFER_TX_FAILED;
        return UCN_V6_ERR_TIMEOUT;
    }
    for (index = 0U; index < tx->request.window_size; ++index) {
        ucn_v6_transfer_tx_fragment_state_t *state = &tx->window[index];
        uint32_t offset;
        uint16_t length;
        bool retry_due;
        if (state->fragment_index == UINT16_MAX || state->acknowledged) {
            continue;
        }
        retry_due = state->sent && now_us >= state->last_submit_us &&
                    now_us - state->last_submit_us >=
                        owner->retry_interval_us;
        if (state->sent && !retry_due) {
            continue;
        }
        if (state->sent && state->attempts >= owner->fragment_max_attempts) {
            tx->phase = UCN_V6_TRANSFER_TX_FAILED;
            return UCN_V6_ERR_EXHAUSTED;
        }
        offset = (uint32_t)state->fragment_index *
                 tx->request.fragment_data_budget;
        length = (uint16_t)(tx->request.payload_length - offset);
        if (length > tx->request.fragment_data_budget) {
            length = tx->request.fragment_data_budget;
        }
        memset(&fragment, 0, sizeof(fragment));
        fragment.message_class = tx->request.message_class;
        fragment.message_id = tx->request.message_id;
        fragment.total_length = tx->request.payload_length;
        fragment.fragment_index = state->fragment_index;
        fragment.fragment_count = tx->fragment_count;
        fragment.fragment_data_budget = tx->request.fragment_data_budget;
        fragment.data_length = length;
        fragment.message_crc32c = tx->message_crc32c;
        fragment.data = &tx->request.payload[offset];
        owner->selected = true;
        owner->selected_tx_index = (uint16_t)(tx - owner->tx);
        owner->selected_fragment_index = state->fragment_index;
        owner->stats.selection_pending = true;
        *fragment_out = fragment;
        return UCN_V6_OK;
    }
    return UCN_V6_ERR_NOT_FOUND;
}

ucn_v6_result_t ucn_v6_transfer_record_fragment_submit(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    uint64_t message_id,
    uint16_t fragment_index,
    bool submitted)
{
    ucn_v6_transfer_tx_slot_t *tx;
    ucn_v6_transfer_tx_fragment_state_t *state;
    bool retransmission;
    if (!owner_is_valid(owner) || !owner->selected || message_id == 0U ||
        owner->selected_tx_index >= UCN_V6_CONFIG_TRANSFER_TX_SLOTS ||
        owner->selected_fragment_index != fragment_index) {
        return UCN_V6_ERR_ARGUMENT;
    }
    tx = &owner->tx[owner->selected_tx_index];
    if (!tx->occupied || tx->request.message_id != message_id ||
        tx->phase != UCN_V6_TRANSFER_TX_SENDING ||
        (state = find_tx_fragment(tx, fragment_index)) == NULL) {
        return UCN_V6_ERR_REPLAY;
    }
    if (submitted && state->attempts == UINT8_MAX) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    if (submitted && state->sent && now_us < state->last_submit_us) {
        return UCN_V6_ERR_STATE;
    }
    retransmission = state->sent;
    if (submitted) {
        state->sent = true;
        ++state->attempts;
        state->last_submit_us = now_us;
        saturating_increment(&owner->stats.fragments_submitted);
        if (retransmission) {
            saturating_increment(&owner->stats.fragments_retransmitted);
        }
    }
    owner->selected = false;
    owner->selected_tx_index = 0U;
    owner->selected_fragment_index = 0U;
    owner->stats.selection_pending = false;
    return UCN_V6_OK;
}

static bool sack_context_matches(const ucn_v6_transfer_tx_slot_t *tx,
                                 const ucn_v6_security_open_result_t *opened)
{
    const ucn_v6_frame_t *frame;
    if (opened == NULL || !opened->hop_authenticated ||
        !opened->endpoint_authorized || opened->group_discovery_only) {
        return false;
    }
    frame = &opened->frame;
    return frame->frame_type == UCN_V6_FRAME_TRANSFER &&
           frame->protocol_opcode == UCN_V6_PROTOCOL_OPCODE_TRANSFER_SACK &&
           (frame->flags & (UCN_V6_FLAG_E2E_CONTEXT |
                            UCN_V6_FLAG_PROTOCOL_CONTEXT |
                            UCN_V6_FLAG_MESSAGE_CONTEXT |
                            UCN_V6_FLAG_ROUTE_CONTEXT |
                            UCN_V6_FLAG_PATH_CONTEXT)) ==
               (UCN_V6_FLAG_E2E_CONTEXT |
                UCN_V6_FLAG_PROTOCOL_CONTEXT |
                UCN_V6_FLAG_MESSAGE_CONTEXT |
                UCN_V6_FLAG_ROUTE_CONTEXT |
                UCN_V6_FLAG_PATH_CONTEXT) &&
           principal_equal(&opened->authenticated_principal,
                           &tx->request.route_domain.destination_principal) &&
           frame->realm_id ==
               tx->request.route_domain.destination_binding.realm_id &&
           frame->source_address ==
               tx->request.route_domain.destination_binding.node_address &&
           frame->source_binding_generation ==
               tx->request.route_domain.destination_binding.binding_generation &&
           frame->session_generation == tx->request.path.session_generation &&
           frame->message.operation_id == tx->request.message.operation_id &&
           frame->route_generation == tx->request.path.route_generation &&
           frame->path.path_id == tx->request.path.path_id &&
           frame->path.path_generation == tx->request.path.path_generation;
}

static bool tx_fragment_was_sent(const ucn_v6_transfer_tx_slot_t *tx,
                                 uint16_t fragment_index)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_WINDOW; ++index) {
        if (tx->window[index].fragment_index == fragment_index) {
            return tx->window[index].sent;
        }
    }
    return fragment_index < tx->cumulative_base;
}

static void rebuild_window(ucn_v6_transfer_tx_slot_t *tx,
                           const ucn_v6_transfer_tx_fragment_state_t *old)
{
    size_t index;
    size_t old_index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_WINDOW; ++index) {
        uint16_t wanted = UINT16_MAX;
        memset(&tx->window[index], 0, sizeof(tx->window[index]));
        if ((uint32_t)tx->cumulative_base + index < tx->fragment_count &&
            index < tx->request.window_size) {
            wanted = (uint16_t)(tx->cumulative_base + index);
        }
        tx->window[index].fragment_index = wanted;
        for (old_index = 0U; old_index < UCN_V6_CONFIG_TRANSFER_WINDOW;
             ++old_index) {
            if (old[old_index].fragment_index == wanted) {
                tx->window[index] = old[old_index];
                break;
            }
        }
    }
}

ucn_v6_result_t ucn_v6_transfer_apply_sack(
    ucn_v6_transfer_owner_t *owner,
    const ucn_v6_security_open_result_t *opened)
{
    ucn_v6_transfer_sack_t sack;
    ucn_v6_transfer_tx_slot_t *tx;
    ucn_v6_transfer_tx_fragment_state_t candidate[UCN_V6_CONFIG_TRANSFER_WINDOW];
    uint16_t new_base;
    uint16_t index;
    if (!owner_is_valid(owner) || opened == NULL ||
        ucn_v6_transfer_sack_decode(opened->frame.payload,
                                    opened->frame.payload_length,
                                    &sack) != UCN_V6_OK) {
        return UCN_V6_ERR_MALFORMED;
    }
    tx = find_tx(owner, sack.message_id);
    if (tx == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (!sack_context_matches(tx, opened) ||
        sack.fragment_count != tx->fragment_count ||
        sack.cumulative_base < tx->cumulative_base ||
        sack.cumulative_base > tx->fragment_count ||
        (uint32_t)sack.cumulative_base >
            (uint32_t)tx->cumulative_base + tx->request.window_size) {
        return UCN_V6_ERR_REPLAY;
    }
    memcpy(candidate, tx->window, sizeof(candidate));
    for (index = tx->cumulative_base; index < sack.cumulative_base; ++index) {
        ucn_v6_transfer_tx_fragment_state_t *state;
        if (!tx_fragment_was_sent(tx, index) ||
            (state = find_tx_fragment(tx, index)) == NULL) {
            return UCN_V6_ERR_REPLAY;
        }
        candidate[(size_t)(state - tx->window)].acknowledged = true;
    }
    for (index = 0U; index < 32U; ++index) {
        uint32_t absolute = (uint32_t)sack.cumulative_base + index;
        if ((sack.received_bitmap & (UINT32_C(1) << index)) != 0U) {
            ucn_v6_transfer_tx_fragment_state_t *state;
            if (absolute >= tx->fragment_count ||
                !tx_fragment_was_sent(tx, (uint16_t)absolute) ||
                (state = find_tx_fragment(tx, (uint16_t)absolute)) == NULL) {
                return UCN_V6_ERR_REPLAY;
            }
            candidate[(size_t)(state - tx->window)].acknowledged = true;
        }
    }
    new_base = tx->cumulative_base;
    while (new_base < tx->fragment_count) {
        size_t state_index;
        bool acknowledged = false;
        for (state_index = 0U;
             state_index < UCN_V6_CONFIG_TRANSFER_WINDOW;
             ++state_index) {
            if (candidate[state_index].fragment_index == new_base &&
                candidate[state_index].acknowledged) {
                acknowledged = true;
                break;
            }
        }
        if (!acknowledged) {
            break;
        }
        ++new_base;
    }
    memcpy(tx->window, candidate, sizeof(candidate));
    tx->cumulative_base = new_base;
    rebuild_window(tx, candidate);
    if (tx->cumulative_base == tx->fragment_count) {
        tx->phase = UCN_V6_TRANSFER_TX_REASSEMBLED;
    }
    saturating_increment(&owner->stats.sacks_applied);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_copy_tx(
    const ucn_v6_transfer_owner_t *owner,
    uint64_t message_id,
    ucn_v6_transfer_tx_view_t *view_out)
{
    const ucn_v6_transfer_tx_slot_t *tx;
    ucn_v6_transfer_tx_view_t view;
    if (!owner_is_valid(owner) || view_out == NULL ||
        (tx = find_tx_const(owner, message_id)) == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    memset(&view, 0, sizeof(view));
    view.message_id = tx->request.message_id;
    view.buffer_token = tx->request.buffer_token;
    view.phase = tx->phase;
    view.fragment_count = tx->fragment_count;
    view.cumulative_base = tx->cumulative_base;
    view.fragment_data_budget = tx->request.fragment_data_budget;
    view.window_size = tx->request.window_size;
    *view_out = view;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_retire_tx(
    ucn_v6_transfer_owner_t *owner,
    uint64_t message_id,
    uint64_t *buffer_token)
{
    ucn_v6_transfer_tx_slot_t *tx;
    uint64_t token;
    if (!owner_is_valid(owner) || buffer_token == NULL ||
        (tx = find_tx(owner, message_id)) == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (tx->phase == UCN_V6_TRANSFER_TX_SENDING ||
        (owner->selected &&
         owner->selected_tx_index == (uint16_t)(tx - owner->tx))) {
        return UCN_V6_ERR_STATE;
    }
    token = tx->request.buffer_token;
    memset(tx, 0, sizeof(*tx));
    if (owner->stats.tx_active != 0U) {
        --owner->stats.tx_active;
    }
    *buffer_token = token;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_rebind_path(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    uint64_t message_id,
    const ucn_v6_path_capability_t *path)
{
    ucn_v6_transfer_tx_slot_t *tx;
    if (!owner_is_valid(owner) || path == NULL || !path->valid ||
        (tx = find_tx(owner, message_id)) == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (tx->phase != UCN_V6_TRANSFER_TX_SENDING || owner->selected ||
        !principal_equal(&path->destination_principal,
                         &tx->request.route_domain.destination_principal) ||
        !ucn_v6_binding_key_equal(
            &path->destination_binding,
            &tx->request.route_domain.destination_binding) ||
        path->session_generation != tx->request.path.session_generation ||
        path->fragment_data_budget < tx->request.fragment_data_budget ||
        path->max_window < tx->request.window_size ||
        path->max_message_class < tx->request.message_class ||
        (path->feature_bits & UCN_V6_FEATURE_TRANSFER) == 0U ||
        path->route_generation == 0U ||
        path->route_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        path->path_id == 0U || path->path_generation == 0U ||
        path->path_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        path->deadline_us <= now_us) {
        return UCN_V6_ERR_STATE;
    }
    tx->request.path = *path;
    return UCN_V6_OK;
}

static ucn_v6_session_key_t origin_from_opened(
    const ucn_v6_security_open_result_t *opened)
{
    ucn_v6_session_key_t origin;
    memset(&origin, 0, sizeof(origin));
    origin.binding.realm_id = opened->frame.realm_id;
    origin.binding.node_address = opened->frame.source_address;
    origin.binding.binding_generation =
        opened->frame.source_binding_generation;
    origin.principal = opened->authenticated_principal;
    origin.session_generation = opened->frame.session_generation;
    return origin;
}

static bool fragment_context_is_valid(
    const ucn_v6_security_open_result_t *opened)
{
    return opened != NULL && opened->hop_authenticated &&
           opened->endpoint_authorized && !opened->group_discovery_only &&
           opened->frame.frame_type == UCN_V6_FRAME_TRANSFER &&
           opened->frame.protocol_opcode ==
               UCN_V6_PROTOCOL_OPCODE_TRANSFER_FRAGMENT &&
           (opened->frame.flags & (UCN_V6_FLAG_E2E_CONTEXT |
                                   UCN_V6_FLAG_PROTOCOL_CONTEXT |
                                   UCN_V6_FLAG_MESSAGE_CONTEXT |
                                   UCN_V6_FLAG_ROUTE_CONTEXT |
                                   UCN_V6_FLAG_PATH_CONTEXT)) ==
               (UCN_V6_FLAG_E2E_CONTEXT |
                UCN_V6_FLAG_PROTOCOL_CONTEXT |
                UCN_V6_FLAG_MESSAGE_CONTEXT |
                UCN_V6_FLAG_ROUTE_CONTEXT |
                UCN_V6_FLAG_PATH_CONTEXT) &&
           opened->frame.delivery_guarantee == UCN_V6_DELIVERY_RELIABLE &&
           (opened->frame.traffic_class == UCN_V6_TRAFFIC_Q2 ||
            opened->frame.traffic_class == UCN_V6_TRAFFIC_Q3) &&
           opened->frame.message.operation_id != 0U;
}

static ucn_v6_transfer_rx_slot_t *find_rx(
    ucn_v6_transfer_owner_t *owner,
    const ucn_v6_session_key_t *origin,
    uint64_t operation_id,
    uint64_t message_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RX_SLOTS; ++index) {
        if (owner->rx[index].occupied && owner->rx[index].message_id == message_id &&
            owner->rx[index].operation_id == operation_id &&
            session_equal(&owner->rx[index].origin, origin)) {
            return &owner->rx[index];
        }
    }
    return NULL;
}

static const ucn_v6_transfer_rx_slot_t *find_rx_const(
    const ucn_v6_transfer_owner_t *owner,
    const ucn_v6_session_key_t *origin,
    uint64_t operation_id,
    uint64_t message_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RX_SLOTS; ++index) {
        if (owner->rx[index].occupied && owner->rx[index].message_id == message_id &&
            owner->rx[index].operation_id == operation_id &&
            session_equal(&owner->rx[index].origin, origin)) {
            return &owner->rx[index];
        }
    }
    return NULL;
}

static ucn_v6_transfer_rx_slot_t *find_free_rx(ucn_v6_transfer_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RX_SLOTS; ++index) {
        if (!owner->rx[index].occupied) {
            return &owner->rx[index];
        }
    }
    return NULL;
}

static bool bit_is_set(const uint8_t *bits, uint16_t index)
{
    return (bits[index / 8U] & (uint8_t)(1U << (index % 8U))) != 0U;
}

static void bit_set(uint8_t *bits, uint16_t index)
{
    bits[index / 8U] |= (uint8_t)(1U << (index % 8U));
}

static void build_sack(const ucn_v6_transfer_rx_slot_t *rx,
                       ucn_v6_transfer_sack_t *sack)
{
    uint16_t base = 0U;
    uint16_t index;
    memset(sack, 0, sizeof(*sack));
    while (base < rx->fragment_count && bit_is_set(rx->received, base)) {
        ++base;
    }
    sack->message_id = rx->message_id;
    sack->cumulative_base = base;
    sack->fragment_count = rx->fragment_count;
    for (index = 0U; index < 32U &&
                     (uint32_t)base + index < rx->fragment_count;
         ++index) {
        if (bit_is_set(rx->received, (uint16_t)(base + index))) {
            sack->received_bitmap |= UINT32_C(1) << index;
        }
    }
}

static const ucn_v6_transfer_recent_slot_t *find_recent(
    const ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_session_key_t *origin,
    uint64_t operation_id,
    uint64_t message_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RECENT; ++index) {
        const ucn_v6_transfer_recent_slot_t *recent = &owner->recent[index];
        if (recent->occupied && now_us < recent->deadline_us &&
            recent->operation_id == operation_id &&
            recent->message_id == message_id &&
            session_equal(&recent->origin, origin)) {
            return recent;
        }
    }
    return NULL;
}

ucn_v6_result_t ucn_v6_transfer_receive_fragment(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    ucn_v6_transfer_rx_result_t *result_out)
{
    ucn_v6_transfer_fragment_t fragment;
    ucn_v6_transfer_rx_result_t result;
    ucn_v6_transfer_rx_slot_t *rx;
    const ucn_v6_transfer_recent_slot_t *recent;
    ucn_v6_session_key_t origin;
    uint64_t deadline;
    uint32_t offset;
    if (!owner_is_valid(owner) || result_out == NULL ||
        !fragment_context_is_valid(opened) ||
        ucn_v6_transfer_fragment_decode(opened->frame.payload,
                                        opened->frame.payload_length,
                                        &fragment) != UCN_V6_OK) {
        return UCN_V6_ERR_MALFORMED;
    }
    origin = origin_from_opened(opened);
    if (!session_is_valid(&origin) ||
        fragment.message_id != opened->frame.message.operation_id) {
        return UCN_V6_ERR_SECURITY;
    }
    memset(&result, 0, sizeof(result));
    recent = find_recent(owner, now_us, &origin,
                         opened->frame.message.operation_id,
                         fragment.message_id);
    if (recent != NULL) {
        if (recent->message_class != fragment.message_class ||
            recent->total_length != fragment.total_length ||
            recent->fragment_count != fragment.fragment_count ||
            recent->fragment_data_budget != fragment.fragment_data_budget ||
            recent->message_crc32c != fragment.message_crc32c) {
            return UCN_V6_ERR_REPLAY;
        }
        result.accepted = true;
        result.complete = true;
        result.recent_replay = true;
        result.sack.message_id = recent->message_id;
        result.sack.cumulative_base = recent->fragment_count;
        result.sack.fragment_count = recent->fragment_count;
        *result_out = result;
        saturating_increment(&owner->stats.receive_duplicates);
        return UCN_V6_OK;
    }
    rx = find_rx(owner, &origin, opened->frame.message.operation_id,
                 fragment.message_id);
    if (rx == NULL) {
        if (!deadline_build(now_us, owner->receive_timeout_us, &deadline) ||
            (rx = find_free_rx(owner)) == NULL) {
            return UCN_V6_ERR_NO_SPACE;
        }
        memset(rx, 0, sizeof(*rx));
        rx->occupied = true;
        rx->origin = origin;
        rx->operation_id = opened->frame.message.operation_id;
        rx->message_id = fragment.message_id;
        rx->message_class = fragment.message_class;
        rx->total_length = fragment.total_length;
        rx->fragment_count = fragment.fragment_count;
        rx->fragment_data_budget = fragment.fragment_data_budget;
        rx->message_crc32c = fragment.message_crc32c;
        rx->deadline_us = deadline;
        ++owner->stats.rx_active;
    } else if (rx->message_class != fragment.message_class ||
               rx->total_length != fragment.total_length ||
               rx->fragment_count != fragment.fragment_count ||
               rx->fragment_data_budget != fragment.fragment_data_budget ||
               rx->message_crc32c != fragment.message_crc32c ||
               now_us >= rx->deadline_us) {
        return UCN_V6_ERR_REPLAY;
    }
    offset = (uint32_t)fragment.fragment_index *
             fragment.fragment_data_budget;
    if (rx->complete) {
        if (!bit_is_set(rx->received, fragment.fragment_index) ||
            memcmp(&rx->data[offset], fragment.data,
                   fragment.data_length) != 0) {
            return UCN_V6_ERR_SECURITY;
        }
        memset(&result, 0, sizeof(result));
        result.accepted = true;
        result.complete = true;
        result.recent_replay = true;
        build_sack(rx, &result.sack);
        *result_out = result;
        saturating_increment(&owner->stats.receive_duplicates);
        return UCN_V6_OK;
    }
    if (bit_is_set(rx->received, fragment.fragment_index)) {
        if (memcmp(&rx->data[offset], fragment.data,
                   fragment.data_length) != 0) {
            return UCN_V6_ERR_SECURITY;
        }
        saturating_increment(&owner->stats.receive_duplicates);
    } else {
        memcpy(&rx->data[offset], fragment.data, fragment.data_length);
        bit_set(rx->received, fragment.fragment_index);
        ++rx->received_count;
        saturating_increment(&owner->stats.receive_fragments);
    }
    if (rx->received_count == rx->fragment_count) {
        if (ucn_v6_crc32c(rx->data, rx->total_length) != rx->message_crc32c) {
            memset(rx, 0, sizeof(*rx));
            if (owner->stats.rx_active != 0U) {
                --owner->stats.rx_active;
            }
            return UCN_V6_ERR_SECURITY;
        }
        if (!deadline_build(now_us, owner->receive_timeout_us, &deadline)) {
            memset(rx, 0, sizeof(*rx));
            if (owner->stats.rx_active != 0U) {
                --owner->stats.rx_active;
            }
            return UCN_V6_ERR_EXHAUSTED;
        }
        rx->complete = true;
        rx->deadline_us = deadline;
        saturating_increment(&owner->stats.completed_messages);
    }
    result.accepted = true;
    result.complete = rx->complete;
    build_sack(rx, &result.sack);
    *result_out = result;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_copy_completed(
    const ucn_v6_transfer_owner_t *owner,
    const ucn_v6_session_key_t *origin,
    uint64_t operation_id,
    uint64_t message_id,
    uint8_t *output,
    size_t output_capacity,
    ucn_v6_transfer_completed_t *completed_out)
{
    const ucn_v6_transfer_rx_slot_t *rx;
    ucn_v6_transfer_completed_t completed;
    if (!owner_is_valid(owner) || !session_is_valid(origin) ||
        output == NULL || completed_out == NULL ||
        (rx = find_rx_const(owner, origin, operation_id, message_id)) == NULL ||
        !rx->complete) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (output_capacity < rx->total_length) {
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(&completed, 0, sizeof(completed));
    completed.origin = rx->origin;
    completed.operation_id = rx->operation_id;
    completed.message_id = rx->message_id;
    completed.message_class = rx->message_class;
    completed.payload_length = rx->total_length;
    completed.message_crc32c = rx->message_crc32c;
    memcpy(output, rx->data, rx->total_length);
    *completed_out = completed;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_retire_completed(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_session_key_t *origin,
    uint64_t operation_id,
    uint64_t message_id)
{
    ucn_v6_transfer_rx_slot_t *rx;
    ucn_v6_transfer_recent_slot_t *recent = NULL;
    uint64_t deadline;
    size_t index;
    if (!owner_is_valid(owner) || !session_is_valid(origin) ||
        (rx = find_rx(owner, origin, operation_id, message_id)) == NULL ||
        !rx->complete) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (!deadline_build(now_us, owner->recent_completion_us, &deadline)) {
        return UCN_V6_ERR_EXHAUSTED;
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RECENT; ++index) {
        if (!owner->recent[index].occupied) {
            recent = &owner->recent[index];
            break;
        }
    }
    if (recent == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(recent, 0, sizeof(*recent));
    recent->occupied = true;
    recent->origin = rx->origin;
    recent->operation_id = rx->operation_id;
    recent->message_id = rx->message_id;
    recent->message_class = rx->message_class;
    recent->total_length = rx->total_length;
    recent->fragment_count = rx->fragment_count;
    recent->fragment_data_budget = rx->fragment_data_budget;
    recent->message_crc32c = rx->message_crc32c;
    recent->deadline_us = deadline;
    memset(rx, 0, sizeof(*rx));
    if (owner->stats.rx_active != 0U) {
        --owner->stats.rx_active;
    }
    ++owner->stats.recent_completions;
    return UCN_V6_OK;
}

static ucn_v6_transfer_credit_slot_t *find_credit(
    ucn_v6_transfer_owner_t *owner,
    const ucn_v6_session_key_t *peer,
    uint16_t link_id,
    ucn_v6_traffic_class_t traffic_class)
{
    size_t index;
    for (index = 0U;
         index < UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS * 2U; ++index) {
        if (owner->credits[index].occupied &&
            owner->credits[index].update.link_id == link_id &&
            owner->credits[index].update.traffic_class == traffic_class &&
            session_equal(&owner->credits[index].peer, peer)) {
            return &owner->credits[index];
        }
    }
    return NULL;
}

static ucn_v6_transfer_credit_slot_t *find_free_credit(
    ucn_v6_transfer_owner_t *owner)
{
    size_t index;
    for (index = 0U;
         index < UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS * 2U; ++index) {
        if (!owner->credits[index].occupied) {
            return &owner->credits[index];
        }
    }
    return NULL;
}

static bool credit_update_equal(
    const ucn_v6_transfer_credit_update_t *left,
    const ucn_v6_transfer_credit_update_t *right)
{
    return left->link_id == right->link_id &&
           left->link_generation == right->link_generation &&
           left->traffic_class == right->traffic_class &&
           left->credit_generation == right->credit_generation &&
           left->update_sequence == right->update_sequence &&
           left->available_credit == right->available_credit &&
           left->maximum_credit == right->maximum_credit &&
           left->lease_duration_us == right->lease_duration_us;
}

static bool credit_has_reservation(
    const ucn_v6_transfer_owner_t *owner,
    const ucn_v6_session_key_t *peer,
    const ucn_v6_transfer_credit_update_t *credit)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS;
         ++index) {
        const ucn_v6_transfer_credit_reservation_slot_t *slot =
            &owner->reservations[index];
        if (slot->occupied && session_equal(&slot->value.peer, peer) &&
            slot->value.link_id == credit->link_id &&
            slot->value.link_generation == credit->link_generation &&
            slot->value.traffic_class == credit->traffic_class &&
            slot->value.credit_generation == credit->credit_generation) {
            return true;
        }
    }
    return false;
}

ucn_v6_result_t ucn_v6_transfer_ingest_credit(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    uint16_t policy_maximum_credit)
{
    ucn_v6_transfer_credit_update_t credit;
    ucn_v6_transfer_credit_slot_t *slot;
    uint64_t deadline;
    if (!owner_is_valid(owner) || opened == NULL ||
        !opened->hop_authenticated || opened->group_discovery_only ||
        !session_is_valid(&opened->ingress_peer_session) ||
        opened->frame.frame_type != UCN_V6_FRAME_TRANSFER ||
        opened->frame.protocol_opcode !=
            UCN_V6_PROTOCOL_OPCODE_TRANSFER_CREDIT ||
        policy_maximum_credit == 0U ||
        ucn_v6_transfer_credit_decode(opened->frame.payload,
                                      opened->frame.payload_length,
                                      &credit) != UCN_V6_OK ||
        credit.maximum_credit > policy_maximum_credit ||
        !deadline_build(now_us, credit.lease_duration_us, &deadline)) {
        return UCN_V6_ERR_ACCESS;
    }
    slot = find_credit(owner, &opened->ingress_peer_session,
                       credit.link_id, credit.traffic_class);
    if (slot == NULL) {
        if (credit.credit_generation != 1U || credit.update_sequence != 1U ||
            (slot = find_free_credit(owner)) == NULL) {
            return UCN_V6_ERR_NO_SPACE;
        }
    } else if (credit.link_generation != slot->update.link_generation) {
        if (credit_has_reservation(owner, &opened->ingress_peer_session,
                                   &slot->update)) {
            return UCN_V6_ERR_STATE;
        }
        if (slot->update.link_generation ==
                UCN_V6_SERIAL_ROTATION_THRESHOLD ||
            credit.link_generation != slot->update.link_generation + 1U ||
            credit.credit_generation != 1U || credit.update_sequence != 1U) {
            return UCN_V6_ERR_REPLAY;
        }
    } else if (credit.credit_generation == slot->update.credit_generation) {
        if (credit.update_sequence == slot->update.update_sequence) {
            return credit_update_equal(&credit, &slot->update) ?
                       UCN_V6_OK : UCN_V6_ERR_REPLAY;
        }
        if (credit_has_reservation(owner, &opened->ingress_peer_session,
                                   &slot->update)) {
            return UCN_V6_ERR_STATE;
        }
        if (slot->update.update_sequence ==
                UCN_V6_SERIAL_ROTATION_THRESHOLD ||
            credit.update_sequence != slot->update.update_sequence + 1U) {
            return UCN_V6_ERR_REPLAY;
        }
    } else if (credit_has_reservation(owner,
                                      &opened->ingress_peer_session,
                                      &slot->update)) {
        return UCN_V6_ERR_STATE;
    } else if (slot->update.credit_generation ==
                   UCN_V6_SERIAL_ROTATION_THRESHOLD ||
               credit.credit_generation !=
                   slot->update.credit_generation + 1U ||
               credit.update_sequence != 1U) {
        return UCN_V6_ERR_REPLAY;
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->peer = opened->ingress_peer_session;
    slot->update = credit;
    slot->deadline_us = deadline;
    return UCN_V6_OK;
}

static ucn_v6_transfer_credit_reservation_slot_t *find_free_reservation(
    ucn_v6_transfer_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS;
         ++index) {
        if (!owner->reservations[index].occupied) {
            return &owner->reservations[index];
        }
    }
    return NULL;
}

ucn_v6_result_t ucn_v6_transfer_reserve_credit(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_session_key_t *peer,
    uint16_t link_id,
    uint32_t link_generation,
    ucn_v6_traffic_class_t traffic_class,
    ucn_v6_transfer_credit_reservation_t *reservation_out)
{
    ucn_v6_transfer_credit_slot_t *credit = NULL;
    ucn_v6_transfer_credit_reservation_slot_t *slot;
    ucn_v6_transfer_credit_reservation_t reservation;
    size_t index;
    if (!owner_is_valid(owner) || !session_is_valid(peer) ||
        reservation_out == NULL || link_id == 0U ||
        link_generation == 0U ||
        link_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD ||
        (traffic_class != UCN_V6_TRAFFIC_Q2 &&
         traffic_class != UCN_V6_TRAFFIC_Q3)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U;
         index < UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS * 2U; ++index) {
        if (owner->credits[index].occupied &&
            owner->credits[index].update.link_id == link_id &&
            owner->credits[index].update.link_generation == link_generation &&
            owner->credits[index].update.traffic_class == traffic_class &&
            session_equal(&owner->credits[index].peer, peer)) {
            credit = &owner->credits[index];
            break;
        }
    }
    if (credit == NULL || now_us >= credit->deadline_us ||
        credit->update.available_credit == 0U) {
        saturating_increment(&owner->stats.credit_rejections);
        return UCN_V6_ERR_NO_SPACE;
    }
    if (owner->next_credit_reservation_id ==
            UCN_V6_SERIAL64_ROTATION_THRESHOLD ||
        (slot = find_free_reservation(owner)) == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(&reservation, 0, sizeof(reservation));
    reservation.reservation_id = ++owner->next_credit_reservation_id;
    reservation.peer = *peer;
    reservation.link_id = link_id;
    reservation.link_generation = link_generation;
    reservation.traffic_class = traffic_class;
    reservation.credit_generation = credit->update.credit_generation;
    --credit->update.available_credit;
    memset(slot, 0, sizeof(*slot));
    slot->occupied = true;
    slot->value = reservation;
    ++owner->stats.credit_reservations;
    *reservation_out = reservation;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_invalidate_session(
    ucn_v6_transfer_owner_t *owner,
    const ucn_v6_session_key_t *session,
    uint64_t *retired_tx_buffer_tokens,
    size_t retired_capacity,
    size_t *retired_count,
    ucn_v6_transfer_invalidation_result_t *result_out)
{
    ucn_v6_transfer_invalidation_result_t result;
    size_t needed = 0U;
    size_t index;
    if (!owner_is_valid(owner) || !session_is_valid(session) ||
        retired_count == NULL || result_out == NULL ||
        (retired_capacity != 0U && retired_tx_buffer_tokens == NULL)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        const ucn_v6_transfer_tx_slot_t *tx = &owner->tx[index];
        if (tx->occupied &&
            principal_equal(&tx->request.path.destination_principal,
                            &session->principal) &&
            ucn_v6_binding_key_equal(&tx->request.path.destination_binding,
                                     &session->binding) &&
            tx->request.path.session_generation ==
                session->session_generation) {
            ++needed;
        }
    }
    if (needed > retired_capacity) {
        return UCN_V6_ERR_NO_SPACE;
    }
    memset(&result, 0, sizeof(result));
    needed = 0U;
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        ucn_v6_transfer_tx_slot_t *tx = &owner->tx[index];
        if (tx->occupied &&
            principal_equal(&tx->request.path.destination_principal,
                            &session->principal) &&
            ucn_v6_binding_key_equal(&tx->request.path.destination_binding,
                                     &session->binding) &&
            tx->request.path.session_generation ==
                session->session_generation) {
            retired_tx_buffer_tokens[needed++] = tx->request.buffer_token;
            if (owner->selected && owner->selected_tx_index == index) {
                owner->selected = false;
                owner->stats.selection_pending = false;
            }
            memset(tx, 0, sizeof(*tx));
            ++result.tx_retired;
            if (owner->stats.tx_active != 0U) {
                --owner->stats.tx_active;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RX_SLOTS; ++index) {
        if (owner->rx[index].occupied &&
            session_equal(&owner->rx[index].origin, session)) {
            memset(&owner->rx[index], 0, sizeof(owner->rx[index]));
            ++result.rx_retired;
            if (owner->stats.rx_active != 0U) {
                --owner->stats.rx_active;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RECENT; ++index) {
        if (owner->recent[index].occupied &&
            session_equal(&owner->recent[index].origin, session)) {
            memset(&owner->recent[index], 0, sizeof(owner->recent[index]));
            ++result.recent_retired;
            if (owner->stats.recent_completions != 0U) {
                --owner->stats.recent_completions;
            }
        }
    }
    for (index = 0U;
         index < UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS * 2U; ++index) {
        if (owner->credits[index].occupied &&
            session_equal(&owner->credits[index].peer, session)) {
            memset(&owner->credits[index], 0, sizeof(owner->credits[index]));
            ++result.credit_slots_retired;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS;
         ++index) {
        if (owner->reservations[index].occupied &&
            session_equal(&owner->reservations[index].value.peer, session)) {
            memset(&owner->reservations[index], 0,
                   sizeof(owner->reservations[index]));
            ++result.credit_reservations_retired;
            if (owner->stats.credit_reservations != 0U) {
                --owner->stats.credit_reservations;
            }
        }
    }
    *retired_count = needed;
    *result_out = result;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_finish_credit(
    ucn_v6_transfer_owner_t *owner,
    uint64_t reservation_id,
    bool submitted)
{
    ucn_v6_transfer_credit_reservation_slot_t *reservation = NULL;
    size_t index;
    if (!owner_is_valid(owner) || reservation_id == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_CREDIT_RESERVATIONS;
         ++index) {
        if (owner->reservations[index].occupied &&
            owner->reservations[index].value.reservation_id == reservation_id) {
            reservation = &owner->reservations[index];
            break;
        }
    }
    if (reservation == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (!submitted) {
        for (index = 0U;
             index < UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS * 2U; ++index) {
            ucn_v6_transfer_credit_slot_t *credit = &owner->credits[index];
            if (credit->occupied &&
                credit->update.link_id == reservation->value.link_id &&
                credit->update.link_generation ==
                    reservation->value.link_generation &&
                credit->update.traffic_class ==
                    reservation->value.traffic_class &&
                credit->update.credit_generation ==
                    reservation->value.credit_generation &&
                session_equal(&credit->peer, &reservation->value.peer) &&
                credit->update.available_credit <
                    credit->update.maximum_credit) {
                ++credit->update.available_credit;
                break;
            }
        }
    }
    memset(reservation, 0, sizeof(*reservation));
    if (owner->stats.credit_reservations != 0U) {
        --owner->stats.credit_reservations;
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_expire(
    ucn_v6_transfer_owner_t *owner,
    uint64_t now_us)
{
    size_t index;
    if (!owner_is_valid(owner)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_TX_SLOTS; ++index) {
        if (owner->tx[index].occupied &&
            owner->tx[index].phase == UCN_V6_TRANSFER_TX_SENDING &&
            now_us >= owner->tx[index].request.path.deadline_us) {
            owner->tx[index].phase = UCN_V6_TRANSFER_TX_FAILED;
            if (owner->selected && owner->selected_tx_index == index) {
                owner->selected = false;
                owner->selected_tx_index = 0U;
                owner->selected_fragment_index = 0U;
                owner->stats.selection_pending = false;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RX_SLOTS; ++index) {
        if (owner->rx[index].occupied &&
            now_us >= owner->rx[index].deadline_us) {
            memset(&owner->rx[index], 0, sizeof(owner->rx[index]));
            if (owner->stats.rx_active != 0U) {
                --owner->stats.rx_active;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_TRANSFER_RECENT; ++index) {
        if (owner->recent[index].occupied &&
            now_us >= owner->recent[index].deadline_us) {
            memset(&owner->recent[index], 0, sizeof(owner->recent[index]));
            if (owner->stats.recent_completions != 0U) {
                --owner->stats.recent_completions;
            }
        }
    }
    for (index = 0U;
         index < UCN_V6_CONFIG_TRANSFER_CREDIT_LINKS * 2U; ++index) {
        if (owner->credits[index].occupied &&
            now_us >= owner->credits[index].deadline_us) {
            memset(&owner->credits[index], 0, sizeof(owner->credits[index]));
        }
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_transfer_copy_stats(
    const ucn_v6_transfer_owner_t *owner,
    ucn_v6_transfer_stats_t *stats)
{
    if (!owner_is_valid(owner) || stats == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *stats = owner->stats;
    stats->faulted = owner->faulted;
    return UCN_V6_OK;
}
