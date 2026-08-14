#include <string.h>

#include "ucn/ucn_frame.h"
#include "ucn/ucn_time.h"
#include "ucn/ucn_transfer.h"

static uint16_t read_u16_be(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8U) | (uint16_t)input[1]);
}

static uint32_t read_u32_be(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

static void write_u16_be(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static bool transfer_class_is_valid(ucn_transfer_class_t transfer_class)
{
    return transfer_class >= UCN_TRANSFER_CLASS_T32 &&
           transfer_class < UCN_TRANSFER_CLASS_COUNT;
}

size_t ucn_transfer_class_max_bytes(ucn_transfer_class_t transfer_class)
{
    static const uint16_t limits[UCN_TRANSFER_CLASS_COUNT] = {
        32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U, 8192U
    };

    return transfer_class_is_valid(transfer_class) ?
               (size_t)limits[(size_t)transfer_class] : 0U;
}

ucn_transfer_class_t ucn_transfer_smallest_class_for_length(size_t length)
{
    ucn_transfer_class_t transfer_class;

    if (length == 0U || length > 8192U) {
        return UCN_TRANSFER_CLASS_COUNT;
    }
    for (transfer_class = UCN_TRANSFER_CLASS_T32;
         transfer_class < UCN_TRANSFER_CLASS_COUNT;
         transfer_class = (ucn_transfer_class_t)(transfer_class + 1)) {
        if (length <= ucn_transfer_class_max_bytes(transfer_class)) {
            return transfer_class;
        }
    }
    return UCN_TRANSFER_CLASS_COUNT;
}

uint32_t ucn_transfer_crc32(const uint8_t *data, size_t length)
{
    size_t index;
    uint32_t crc = UINT32_C(0xFFFFFFFF);

    if (data == NULL && length != 0U) {
        return 0U;
    }
    for (index = 0U; index < length; ++index) {
        uint8_t bit;

        crc ^= (uint32_t)data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static bool fragment_shape_is_valid(const ucn_transfer_fragment_t *fragment)
{
    size_t class_limit;
    size_t end_offset;
    bool starts;
    bool ends;

    if (fragment == NULL || !ucn_endpoint_is_static(fragment->target_endpoint) ||
        !transfer_class_is_valid(fragment->transfer_class) ||
        fragment->transfer_class < UCN_TRANSFER_CLASS_T128 ||
        fragment->transfer_id == 0U || fragment->total_length == 0U ||
        fragment->data == NULL || fragment->data_length == 0U ||
        (fragment->flags & (uint8_t)~UCN_TRANSFER_KNOWN_FLAGS) != 0U) {
        return false;
    }
    class_limit = ucn_transfer_class_max_bytes(fragment->transfer_class);
    end_offset = (size_t)fragment->fragment_offset + fragment->data_length;
    starts = (fragment->flags & UCN_TRANSFER_FLAG_START) != 0U;
    ends = (fragment->flags & UCN_TRANSFER_FLAG_END) != 0U;
    return fragment->total_length <= class_limit &&
           end_offset <= fragment->total_length &&
           starts == (fragment->fragment_offset == 0U) &&
           ends == (end_offset == fragment->total_length) &&
           (ends || fragment->data_length >=
                        UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES);
}

ucn_result_t ucn_transfer_encode_fragment(
    const ucn_transfer_fragment_t *fragment,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_length)
{
    size_t required;

    if (fragment == NULL || output == NULL || output_length == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!fragment_shape_is_valid(fragment)) {
        return UCN_ERR_MALFORMED;
    }
    required = UCN_TRANSFER_FRAGMENT_HEADER_BYTES + fragment->data_length;
    if (required > output_capacity || required > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    output[0] = UCN_TRANSFER_FORMAT_VERSION;
    output[1] = fragment->target_endpoint;
    output[2] = (uint8_t)fragment->transfer_class;
    output[3] = fragment->flags;
    write_u16_be(&output[4], fragment->transfer_id);
    write_u16_be(&output[6], fragment->total_length);
    write_u16_be(&output[8], fragment->fragment_offset);
    write_u32_be(&output[10], fragment->message_crc32);
    (void)memcpy(&output[UCN_TRANSFER_FRAGMENT_HEADER_BYTES], fragment->data,
                 fragment->data_length);
    *output_length = required;
    return UCN_OK;
}

ucn_result_t ucn_transfer_decode_fragment(
    const uint8_t *input,
    size_t input_length,
    ucn_transfer_fragment_t *fragment)
{
    if (input == NULL || fragment == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length <= UCN_TRANSFER_FRAGMENT_HEADER_BYTES ||
        input_length > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    if (input[0] != UCN_TRANSFER_FORMAT_VERSION) {
        return UCN_ERR_VERSION;
    }
    (void)memset(fragment, 0, sizeof(*fragment));
    fragment->target_endpoint = input[1];
    fragment->transfer_class = (ucn_transfer_class_t)input[2];
    fragment->flags = input[3];
    fragment->transfer_id = read_u16_be(&input[4]);
    fragment->total_length = read_u16_be(&input[6]);
    fragment->fragment_offset = read_u16_be(&input[8]);
    fragment->message_crc32 = read_u32_be(&input[10]);
    fragment->data = &input[UCN_TRANSFER_FRAGMENT_HEADER_BYTES];
    fragment->data_length =
        (uint16_t)(input_length - UCN_TRANSFER_FRAGMENT_HEADER_BYTES);
    return fragment_shape_is_valid(fragment) ? UCN_OK : UCN_ERR_MALFORMED;
}

static bool ack_shape_is_valid(const ucn_transfer_ack_t *ack)
{
    return ack != NULL && ucn_endpoint_is_static(ack->target_endpoint) &&
           ack->transfer_id != 0U &&
           ack->status >= UCN_TRANSFER_ACK_OK &&
           ack->status <= UCN_TRANSFER_ACK_REJECTED;
}

ucn_result_t ucn_transfer_encode_ack(const ucn_transfer_ack_t *ack,
                                     uint8_t output[UCN_TRANSFER_ACK_BYTES])
{
    if (ack == NULL || output == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (!ack_shape_is_valid(ack)) {
        return UCN_ERR_MALFORMED;
    }
    output[0] = UCN_TRANSFER_FORMAT_VERSION;
    output[1] = ack->target_endpoint;
    write_u16_be(&output[2], ack->transfer_id);
    write_u16_be(&output[4], ack->next_expected_offset);
    output[6] = (uint8_t)ack->status;
    output[7] = 0U;
    return UCN_OK;
}

ucn_result_t ucn_transfer_decode_ack(const uint8_t *input,
                                     size_t input_length,
                                     ucn_transfer_ack_t *ack)
{
    if (input == NULL || ack == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (input_length != UCN_TRANSFER_ACK_BYTES) {
        return UCN_ERR_MALFORMED;
    }
    if (input[0] != UCN_TRANSFER_FORMAT_VERSION) {
        return UCN_ERR_VERSION;
    }
    if (input[7] != 0U) {
        return UCN_ERR_MALFORMED;
    }
    ack->target_endpoint = input[1];
    ack->transfer_id = read_u16_be(&input[2]);
    ack->next_expected_offset = read_u16_be(&input[4]);
    ack->status = (ucn_transfer_ack_status_t)input[6];
    return ack_shape_is_valid(ack) ? UCN_OK : UCN_ERR_MALFORMED;
}

static uint32_t class_deadline_ms(ucn_transfer_class_t transfer_class)
{
    static const uint32_t deadlines[UCN_TRANSFER_CLASS_COUNT] = {
        1000U, 1000U, 1000U, 2000U, 4000U, 8000U, 15000U, 30000U, 45000U
    };

    return transfer_class_is_valid(transfer_class) ?
               deadlines[(size_t)transfer_class] : 0U;
}

static ucn_transfer_endpoint_binding_t *find_endpoint(
    ucn_transfer_t *transfer,
    ucn_endpoint_t endpoint)
{
    size_t index;

    for (index = 0U; index < UCN_TRANSFER_MAX_ENDPOINTS; ++index) {
        if (transfer->endpoints[index].occupied &&
            transfer->endpoints[index].endpoint == endpoint) {
            return &transfer->endpoints[index];
        }
    }
    return NULL;
}

static ucn_transfer_peer_capability_t *find_peer(
    ucn_transfer_t *transfer,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < UCN_TRANSFER_MAX_PEERS; ++index) {
        if (transfer->peers[index].occupied &&
            transfer->peers[index].node_id == node_id) {
            return &transfer->peers[index];
        }
    }
    return NULL;
}

static void direct_endpoint_handler(void *context, const ucn_frame_t *frame)
{
    ucn_transfer_endpoint_binding_t *binding =
        (ucn_transfer_endpoint_binding_t *)context;
    ucn_transfer_t *transfer;
    ucn_transfer_class_t inferred;

    if (binding == NULL || !binding->occupied || binding->owner == NULL ||
        frame == NULL) {
        return;
    }
    transfer = binding->owner;
    inferred = ucn_transfer_smallest_class_for_length(frame->payload_length);
    if (inferred > UCN_TRANSFER_CLASS_T64 ||
        inferred > binding->maximum_class ||
        (binding->require_e2e &&
         (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) == 0U)) {
        transfer->stats.rx_rejected++;
        return;
    }
    binding->handler(binding->context, frame->source, frame->session_id,
                     binding->endpoint, inferred, frame->payload,
                     frame->payload_length, UCN_TRANSFER_RX_HANDLE_DIRECT);
}

static uint16_t allocate_transfer_id(ucn_transfer_t *transfer)
{
    uint16_t id = transfer->next_transfer_id;

    if (id == 0U) {
        id = 1U;
    }
    transfer->next_transfer_id = (uint16_t)(id + 1U);
    if (transfer->next_transfer_id == 0U) {
        transfer->next_transfer_id = 1U;
    }
    return id;
}

static void complete_tx_slot(ucn_transfer_t *transfer,
                             ucn_transfer_tx_slot_t *slot,
                             ucn_transfer_completion_status_t status)
{
    ucn_transfer_completion_fn completion = slot->completion;
    void *context = slot->completion_context;
    ucn_node_id_t destination = slot->destination;
    ucn_endpoint_t endpoint = slot->endpoint;
    uint16_t transfer_id = slot->transfer_id;

    if (status == UCN_TRANSFER_COMPLETION_DELIVERED) {
        transfer->stats.messages_delivered++;
    } else if (status != UCN_TRANSFER_COMPLETION_SENT) {
        transfer->stats.tx_failed++;
    }
    (void)memset(slot, 0, sizeof(*slot));
    if (completion != NULL) {
        completion(context, destination, endpoint, transfer_id, status);
    }
}

static void clear_rx_slot(ucn_transfer_rx_slot_t *slot)
{
    uint16_t generation;

    if (slot == NULL) {
        return;
    }
    generation = slot->generation;
    (void)memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
}

static ucn_transfer_rx_slot_t *find_rx_slot(
    ucn_transfer_t *transfer,
    const ucn_frame_t *frame,
    const ucn_transfer_fragment_t *fragment)
{
    size_t index;

    for (index = 0U; index < UCN_TRANSFER_RX_SLOTS; ++index) {
        ucn_transfer_rx_slot_t *slot = &transfer->rx_slots[index];

        if (slot->occupied && slot->source == frame->source &&
            slot->source_session_id == frame->session_id &&
            slot->endpoint == fragment->target_endpoint &&
            slot->transfer_id == fragment->transfer_id) {
            return slot;
        }
    }
    return NULL;
}

static ucn_transfer_recent_completion_t *find_recent(
    ucn_transfer_t *transfer,
    const ucn_frame_t *frame,
    const ucn_transfer_fragment_t *fragment)
{
    size_t index;

    for (index = 0U; index < UCN_TRANSFER_RECENT_COMPLETIONS; ++index) {
        ucn_transfer_recent_completion_t *recent = &transfer->recent[index];

        if (recent->occupied && recent->source == frame->source &&
            recent->source_session_id == frame->session_id &&
            recent->endpoint == fragment->target_endpoint &&
            recent->transfer_id == fragment->transfer_id &&
            recent->total_length == fragment->total_length &&
            recent->message_crc32 == fragment->message_crc32) {
            return recent;
        }
    }
    return NULL;
}

static void remember_completion(ucn_transfer_t *transfer,
                                const ucn_transfer_rx_slot_t *slot)
{
    size_t index;
    size_t selected = UCN_TRANSFER_RECENT_COMPLETIONS;

    for (index = 0U; index < UCN_TRANSFER_RECENT_COMPLETIONS; ++index) {
        if (!transfer->recent[index].occupied ||
            ucn_deadline_expired(transfer->now_ms,
                                 transfer->recent[index].expires_at_ms)) {
            selected = index;
            break;
        }
    }
    if (selected == UCN_TRANSFER_RECENT_COMPLETIONS) {
        selected = 0U;
    }
    transfer->recent[selected].occupied = true;
    transfer->recent[selected].source = slot->source;
    transfer->recent[selected].source_session_id = slot->source_session_id;
    transfer->recent[selected].endpoint = slot->endpoint;
    transfer->recent[selected].transfer_id = slot->transfer_id;
    transfer->recent[selected].total_length = slot->total_length;
    transfer->recent[selected].message_crc32 = slot->message_crc32;
    transfer->recent[selected].expires_at_ms = ucn_deadline_from_now(
        transfer->now_ms, transfer->config.recent_completion_ms);
}

static ucn_result_t send_ack(ucn_transfer_t *transfer,
                             ucn_node_id_t destination,
                             ucn_endpoint_t endpoint,
                             uint16_t transfer_id,
                             uint16_t next_expected_offset,
                             ucn_transfer_ack_status_t status)
{
    ucn_transfer_ack_t ack;
    uint8_t payload[UCN_TRANSFER_ACK_BYTES];
    ucn_result_t result;

    ack.target_endpoint = endpoint;
    ack.transfer_id = transfer_id;
    ack.next_expected_offset = next_expected_offset;
    ack.status = status;
    result = ucn_transfer_encode_ack(&ack, payload);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_node_send(transfer->config.node, destination,
                           UCN_MSG_TRANSFER_ACK, UCN_TRAFFIC_Q1_REALTIME,
                           payload, (uint16_t)sizeof(payload));
    if (result == UCN_OK) {
        transfer->stats.acknowledgements_sent++;
    } else if (result == UCN_ERR_NOT_FOUND) {
        (void)ucn_node_discover_route(transfer->config.node, destination,
                                      transfer->now_ms);
    }
    return result;
}

static ucn_transfer_rx_slot_t *allocate_rx_slot(
    ucn_transfer_t *transfer,
    const ucn_frame_t *frame,
    const ucn_transfer_fragment_t *fragment)
{
    size_t index;

    for (index = 0U; index < UCN_TRANSFER_RX_SLOTS; ++index) {
        ucn_transfer_rx_slot_t *slot = &transfer->rx_slots[index];

        if (!slot->occupied) {
            uint16_t generation = (uint16_t)(slot->generation + 1U);

            if (generation == 0U) {
                generation = 1U;
            }
            (void)memset(slot, 0, sizeof(*slot));
            slot->generation = generation;
            slot->occupied = true;
            slot->source = frame->source;
            slot->source_session_id = frame->session_id;
            slot->endpoint = fragment->target_endpoint;
            slot->transfer_class = fragment->transfer_class;
            slot->transfer_id = fragment->transfer_id;
            slot->total_length = fragment->total_length;
            slot->message_crc32 = fragment->message_crc32;
            slot->deadline_ms = ucn_deadline_from_now(
                transfer->now_ms, transfer->config.rx_timeout_ms);
            return slot;
        }
    }
    return NULL;
}

static ucn_transfer_rx_handle_t make_rx_handle(
    const ucn_transfer_t *transfer,
    const ucn_transfer_rx_slot_t *slot)
{
    size_t index = (size_t)(slot - transfer->rx_slots);

    return ((uint32_t)slot->generation << 8U) | (uint32_t)(index + 1U);
}

static void handle_fragment(ucn_transfer_t *transfer,
                            const ucn_frame_t *frame)
{
    ucn_transfer_fragment_t fragment;
    ucn_transfer_endpoint_binding_t *binding;
    ucn_transfer_rx_slot_t *slot;
    ucn_transfer_recent_completion_t *recent;
    ucn_result_t result;

    result = ucn_transfer_decode_fragment(frame->payload, frame->payload_length,
                                          &fragment);
    if (result != UCN_OK) {
        transfer->stats.rx_rejected++;
        return;
    }
    binding = find_endpoint(transfer, fragment.target_endpoint);
    if (binding == NULL || fragment.transfer_class > binding->maximum_class ||
        fragment.total_length > UCN_TRANSFER_MAX_MESSAGE_BYTES ||
        (binding->require_e2e &&
         (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) == 0U)) {
        transfer->stats.rx_rejected++;
        (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                       fragment.transfer_id, 0U, UCN_TRANSFER_ACK_REJECTED);
        return;
    }

    recent = find_recent(transfer, frame, &fragment);
    if (recent != NULL) {
        transfer->stats.fragments_duplicate_or_out_of_order++;
        (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                       fragment.transfer_id, recent->total_length,
                       UCN_TRANSFER_ACK_OK);
        return;
    }

    slot = find_rx_slot(transfer, frame, &fragment);
    if (slot == NULL) {
        if ((fragment.flags & UCN_TRANSFER_FLAG_START) == 0U ||
            fragment.fragment_offset != 0U) {
            transfer->stats.fragments_duplicate_or_out_of_order++;
            /* A pipelined sender may have lost its START fragment while a
             * later fragment still arrived.  ACK the cumulative gap instead
             * of rejecting the whole message so bounded Go-Back-N can repair
             * it.  Legacy Stop-and-Wait senders never enter this branch. */
            (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                           fragment.transfer_id, 0U,
                           UCN_TRANSFER_ACK_OK);
            return;
        }
        slot = allocate_rx_slot(transfer, frame, &fragment);
        if (slot == NULL) {
            transfer->stats.rx_slot_full++;
            (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                           fragment.transfer_id, 0U,
                           UCN_TRANSFER_ACK_NO_SLOT);
            return;
        }
    }
    if (slot->completed || slot->transfer_class != fragment.transfer_class ||
        slot->total_length != fragment.total_length ||
        slot->message_crc32 != fragment.message_crc32) {
        transfer->stats.rx_rejected++;
        (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                       fragment.transfer_id, slot->received_length,
                       UCN_TRANSFER_ACK_BAD_FORMAT);
        return;
    }
    if (fragment.fragment_offset != slot->received_length) {
        transfer->stats.fragments_duplicate_or_out_of_order++;
        (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                       fragment.transfer_id, slot->received_length,
                       UCN_TRANSFER_ACK_OK);
        return;
    }
    if ((size_t)slot->received_length + fragment.data_length >
            UCN_TRANSFER_MAX_MESSAGE_BYTES ||
        slot->fragment_count >= 512U) {
        transfer->stats.rx_rejected++;
        (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                       fragment.transfer_id, slot->received_length,
                       UCN_TRANSFER_ACK_BAD_FORMAT);
        return;
    }

    (void)memcpy(&slot->data[slot->received_length], fragment.data,
                 fragment.data_length);
    slot->received_length =
        (uint16_t)(slot->received_length + fragment.data_length);
    slot->fragment_count++;
    slot->deadline_ms = ucn_deadline_from_now(transfer->now_ms,
                                               transfer->config.rx_timeout_ms);
    transfer->stats.fragments_received++;

    if (slot->received_length != slot->total_length) {
        (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                       fragment.transfer_id, slot->received_length,
                       UCN_TRANSFER_ACK_OK);
        return;
    }
    if ((fragment.flags & UCN_TRANSFER_FLAG_END) == 0U ||
        ucn_transfer_crc32(slot->data, slot->total_length) !=
            slot->message_crc32) {
        transfer->stats.integrity_failed++;
        (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                       fragment.transfer_id, 0U,
                       UCN_TRANSFER_ACK_INTEGRITY_FAIL);
        clear_rx_slot(slot);
        return;
    }

    slot->completed = true;
    slot->deadline_ms = ucn_deadline_from_now(
        transfer->now_ms, transfer->config.completed_hold_ms);
    remember_completion(transfer, slot);
    transfer->stats.messages_reassembled++;
    (void)send_ack(transfer, frame->source, fragment.target_endpoint,
                   fragment.transfer_id, slot->total_length,
                   UCN_TRANSFER_ACK_OK);
    binding->handler(binding->context, slot->source, slot->source_session_id,
                     slot->endpoint, slot->transfer_class, slot->data,
                     slot->total_length, make_rx_handle(transfer, slot));
}

static bool begin_window_recovery(ucn_transfer_t *transfer,
                                  ucn_transfer_tx_slot_t *slot)
{
    if (slot->resend_active ||
        slot->acknowledged_offset >= slot->inflight_end_offset) {
        return true;
    }
    if (slot->retry_count >= transfer->config.max_retries) {
        complete_tx_slot(transfer, slot,
                         UCN_TRANSFER_COMPLETION_RETRY_EXHAUSTED);
        return false;
    }
    slot->retry_count++;
    slot->resend_offset = slot->acknowledged_offset;
    slot->resend_end_offset = slot->inflight_end_offset;
    slot->resend_active = true;
    slot->ack_deadline_ms = ucn_deadline_from_now(
        transfer->now_ms, transfer->config.ack_timeout_ms);
    transfer->stats.window_recovery_rounds++;
    return true;
}

static void handle_ack(ucn_transfer_t *transfer, const ucn_frame_t *frame)
{
    ucn_transfer_ack_t ack;
    size_t index;

    if (ucn_transfer_decode_ack(frame->payload, frame->payload_length, &ack) !=
        UCN_OK) {
        transfer->stats.rx_rejected++;
        return;
    }
    transfer->stats.acknowledgements_received++;
    for (index = 0U; index < UCN_TRANSFER_TX_SLOTS; ++index) {
        ucn_transfer_tx_slot_t *slot = &transfer->tx_slots[index];

        if (!slot->occupied || slot->destination != frame->source ||
            slot->endpoint != ack.target_endpoint ||
            slot->transfer_id != ack.transfer_id) {
            continue;
        }
        if (ack.status != UCN_TRANSFER_ACK_OK) {
            complete_tx_slot(transfer, slot,
                             UCN_TRANSFER_COMPLETION_REMOTE_REJECTED);
            return;
        }
        if (ack.next_expected_offset > slot->inflight_end_offset ||
            ack.next_expected_offset < slot->acknowledged_offset) {
            return;
        }
        if (ack.next_expected_offset > slot->acknowledged_offset) {
            slot->acknowledged_offset = ack.next_expected_offset;
            slot->retry_count = 0U;
            if (slot->resend_active &&
                slot->resend_offset < slot->acknowledged_offset) {
                slot->resend_offset = slot->acknowledged_offset;
            }
            if (slot->resend_active &&
                slot->acknowledged_offset >= slot->resend_end_offset) {
                slot->resend_active = false;
            }
            if (slot->acknowledged_offset == slot->total_length) {
                complete_tx_slot(transfer, slot,
                                 UCN_TRANSFER_COMPLETION_DELIVERED);
                return;
            }
            slot->awaiting_ack =
                slot->acknowledged_offset < slot->inflight_end_offset;
            if (slot->awaiting_ack) {
                slot->ack_deadline_ms = ucn_deadline_from_now(
                    transfer->now_ms, transfer->config.ack_timeout_ms);
            }
        } else if (slot->awaiting_ack && !slot->resend_active) {
            /* A duplicate cumulative ACK means the receiver still has a gap.
             * Start one bounded Go-Back-N round immediately instead of
             * waiting for the full ACK timeout. */
            (void)begin_window_recovery(transfer, slot);
        }
        return;
    }
}

static void transfer_node_rx_handler(void *context, const ucn_frame_t *frame)
{
    ucn_transfer_t *transfer = (ucn_transfer_t *)context;

    if (transfer == NULL || !transfer->initialized || frame == NULL) {
        return;
    }
    if (frame->message_type == UCN_MSG_TRANSFER_FRAGMENT) {
        handle_fragment(transfer, frame);
    } else if (frame->message_type == UCN_MSG_TRANSFER_ACK) {
        handle_ack(transfer, frame);
    } else if (transfer->config.fallback_rx_handler != NULL) {
        transfer->config.fallback_rx_handler(
            transfer->config.fallback_rx_context, frame);
    }
}

ucn_result_t ucn_transfer_init(ucn_transfer_t *transfer,
                               const ucn_transfer_config_t *config)
{
    size_t available_fragment_data;

    if (transfer == NULL || config == NULL || config->node == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    if (UCN_MAX_PAYLOAD_BYTES <= UCN_TRANSFER_FRAGMENT_HEADER_BYTES) {
        return UCN_ERR_CONFIG;
    }
    (void)memset(transfer, 0, sizeof(*transfer));
    transfer->config = *config;
    available_fragment_data =
        UCN_MAX_PAYLOAD_BYTES - UCN_TRANSFER_FRAGMENT_HEADER_BYTES;
    if (transfer->config.fragment_data_limit == 0U) {
        transfer->config.fragment_data_limit = (uint16_t)available_fragment_data;
    }
    if (transfer->config.fragment_data_limit > available_fragment_data ||
        transfer->config.fragment_data_limit <
            UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES) {
        return UCN_ERR_CONFIG;
    }
    if (transfer->config.max_retries == 0U) {
        transfer->config.max_retries = UCN_TRANSFER_MAX_RETRIES;
    }
    if (transfer->config.ack_timeout_ms == 0U) {
        transfer->config.ack_timeout_ms = UCN_TRANSFER_ACK_TIMEOUT_MS;
    }
    if (transfer->config.rx_timeout_ms == 0U) {
        transfer->config.rx_timeout_ms = UCN_TRANSFER_RX_TIMEOUT_MS;
    }
    if (transfer->config.completed_hold_ms == 0U) {
        transfer->config.completed_hold_ms = UCN_TRANSFER_COMPLETED_HOLD_MS;
    }
    if (transfer->config.recent_completion_ms == 0U) {
        transfer->config.recent_completion_ms =
            UCN_TRANSFER_RECENT_COMPLETION_MS;
    }
    if (!ucn_duration_is_valid(transfer->config.ack_timeout_ms) ||
        !ucn_duration_is_valid(transfer->config.rx_timeout_ms) ||
        !ucn_duration_is_valid(transfer->config.completed_hold_ms) ||
        !ucn_duration_is_valid(transfer->config.recent_completion_ms)) {
        return UCN_ERR_CONFIG;
    }
    transfer->next_transfer_id = 1U;
    transfer->tx_window_size = 1U;
    transfer->initialized = true;
    ucn_node_set_rx_handler(config->node, transfer_node_rx_handler, transfer);
    return UCN_OK;
}

ucn_result_t ucn_transfer_set_tx_window_size(ucn_transfer_t *transfer,
                                              uint8_t tx_window_size)
{
    size_t index;

    if (transfer == NULL || !transfer->initialized || tx_window_size == 0U ||
        tx_window_size > UCN_TRANSFER_MAX_WINDOW) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_TRANSFER_TX_SLOTS; ++index) {
        if (transfer->tx_slots[index].occupied) {
            return UCN_ERR_ACCESS;
        }
    }
    transfer->tx_window_size = tx_window_size;
    return UCN_OK;
}

ucn_result_t ucn_transfer_bind_endpoint(
    ucn_transfer_t *transfer,
    ucn_endpoint_t endpoint,
    ucn_transfer_class_t maximum_class,
    bool require_e2e,
    ucn_transfer_receive_fn handler,
    void *context)
{
    ucn_transfer_endpoint_binding_t *binding;
    size_t index;

    if (transfer == NULL || !transfer->initialized ||
        !ucn_endpoint_is_static(endpoint) || handler == NULL ||
        !transfer_class_is_valid(maximum_class) ||
        ucn_transfer_class_max_bytes(maximum_class) >
            UCN_TRANSFER_MAX_MESSAGE_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    binding = find_endpoint(transfer, endpoint);
    if (binding == NULL) {
        for (index = 0U; index < UCN_TRANSFER_MAX_ENDPOINTS; ++index) {
            if (!transfer->endpoints[index].occupied) {
                binding = &transfer->endpoints[index];
                break;
            }
        }
    }
    if (binding == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    {
        ucn_transfer_endpoint_binding_t previous = *binding;
        ucn_result_t result;

        binding->occupied = true;
        binding->owner = transfer;
        binding->endpoint = endpoint;
        binding->maximum_class = maximum_class;
        binding->require_e2e = require_e2e;
        binding->handler = handler;
        binding->context = context;
        result = ucn_node_set_endpoint_handler(transfer->config.node, endpoint,
                                               direct_endpoint_handler,
                                               binding);
        if (result != UCN_OK) {
            *binding = previous;
        }
        return result;
    }
}

ucn_result_t ucn_transfer_set_peer_capability(
    ucn_transfer_t *transfer,
    ucn_node_id_t node_id,
    ucn_transfer_class_t maximum_class)
{
    ucn_transfer_peer_capability_t *peer;
    size_t index;

    if (transfer == NULL || !transfer->initialized || node_id == 0U ||
        node_id == UCN_NODE_BROADCAST ||
        !transfer_class_is_valid(maximum_class)) {
        return UCN_ERR_ARGUMENT;
    }
    peer = find_peer(transfer, node_id);
    if (peer == NULL) {
        for (index = 0U; index < UCN_TRANSFER_MAX_PEERS; ++index) {
            if (!transfer->peers[index].occupied) {
                peer = &transfer->peers[index];
                break;
            }
        }
    }
    if (peer == NULL) {
        return UCN_ERR_NO_SPACE;
    }
    peer->occupied = true;
    peer->node_id = node_id;
    peer->maximum_class = maximum_class;
    if (peer->maximum_window_size == 0U) {
        peer->maximum_window_size = 1U;
    }
    return UCN_OK;
}

ucn_result_t ucn_transfer_set_peer_window_capability(
    ucn_transfer_t *transfer,
    ucn_node_id_t node_id,
    uint8_t maximum_window_size)
{
    ucn_transfer_peer_capability_t *peer;

    if (transfer == NULL || !transfer->initialized || node_id == 0U ||
        node_id == UCN_NODE_BROADCAST || maximum_window_size == 0U ||
        maximum_window_size > UCN_TRANSFER_MAX_WINDOW) {
        return UCN_ERR_ARGUMENT;
    }
    peer = find_peer(transfer, node_id);
    if (peer == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    peer->maximum_window_size = maximum_window_size;
    return UCN_OK;
}

ucn_result_t ucn_transfer_send(
    ucn_transfer_t *transfer,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_transfer_class_t transfer_class,
    const uint8_t *data,
    uint16_t length,
    ucn_transfer_completion_fn completion,
    void *completion_context)
{
    ucn_transfer_peer_capability_t *peer;
    size_t class_limit;
    size_t index;

    if (transfer == NULL || !transfer->initialized || destination == 0U ||
        destination == UCN_NODE_BROADCAST || !ucn_endpoint_is_static(endpoint) ||
        !transfer_class_is_valid(transfer_class) || data == NULL || length == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    class_limit = ucn_transfer_class_max_bytes(transfer_class);
    if (length > class_limit || length > UCN_TRANSFER_MAX_MESSAGE_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }
    peer = find_peer(transfer, destination);
    if (peer == NULL || transfer_class > peer->maximum_class) {
        return UCN_ERR_ACCESS;
    }
    if (transfer_class <= UCN_TRANSFER_CLASS_T64) {
        ucn_result_t result = ucn_node_send_endpoint(
            transfer->config.node, destination, endpoint,
            UCN_TRAFFIC_Q1_REALTIME, data, length);

        if (result == UCN_OK) {
            transfer->stats.direct_sent++;
            if (completion != NULL) {
                completion(completion_context, destination, endpoint, 0U,
                           UCN_TRANSFER_COMPLETION_SENT);
            }
        }
        return result;
    }
    for (index = 0U; index < UCN_TRANSFER_TX_SLOTS; ++index) {
        ucn_transfer_tx_slot_t *slot = &transfer->tx_slots[index];

        if (!slot->occupied) {
            (void)memset(slot, 0, sizeof(*slot));
            slot->occupied = true;
            slot->destination = destination;
            slot->endpoint = endpoint;
            slot->transfer_class = transfer_class;
            slot->transfer_id = allocate_transfer_id(transfer);
            slot->data = data;
            slot->total_length = length;
            slot->fragment_data_limit = transfer->config.fragment_data_limit;
            slot->window_size = transfer->tx_window_size <
                                        peer->maximum_window_size ?
                                    transfer->tx_window_size :
                                    peer->maximum_window_size;
            if (slot->window_size == 0U) {
                slot->window_size = 1U;
            }
            slot->message_crc32 = ucn_transfer_crc32(data, length);
            slot->deadline_ms = ucn_deadline_from_now(
                transfer->now_ms, class_deadline_ms(transfer_class));
            slot->completion = completion;
            slot->completion_context = completion_context;
            transfer->stats.tx_accepted++;
            return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

static uint8_t outstanding_fragment_count(
    const ucn_transfer_tx_slot_t *slot)
{
    uint32_t outstanding_bytes;
    uint32_t fragments;

    if (slot->inflight_end_offset <= slot->acknowledged_offset ||
        slot->fragment_data_limit == 0U) {
        return 0U;
    }
    outstanding_bytes = (uint32_t)slot->inflight_end_offset -
                        (uint32_t)slot->acknowledged_offset;
    fragments = (outstanding_bytes + slot->fragment_data_limit - 1U) /
                slot->fragment_data_limit;
    return fragments > UINT8_MAX ? UINT8_MAX : (uint8_t)fragments;
}

static ucn_result_t send_tx_fragment(ucn_transfer_t *transfer,
                                     ucn_transfer_tx_slot_t *slot,
                                     uint16_t fragment_offset,
                                     bool retry)
{
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
    ucn_transfer_fragment_t fragment;
    uint16_t send_limit;
    uint16_t remaining;
    uint16_t data_length;
    uint16_t fragment_end_offset;
    uint16_t previous_inflight_end;
    uint16_t previous_resend_offset;
    uint8_t prospective_in_flight;
    size_t payload_length;
    ucn_result_t result;
    uint16_t transfer_id = slot->transfer_id;

    send_limit = retry ? slot->resend_end_offset : slot->total_length;
    if (fragment_offset >= send_limit || send_limit > slot->total_length) {
        return UCN_ERR_NOT_FOUND;
    }
    remaining = (uint16_t)(send_limit - fragment_offset);
    data_length = remaining < slot->fragment_data_limit ?
                      remaining : slot->fragment_data_limit;
    if (remaining > data_length &&
        data_length < UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES) {
        return UCN_ERR_TOO_LARGE;
    }

    (void)memset(&fragment, 0, sizeof(fragment));
    fragment.target_endpoint = slot->endpoint;
    fragment.transfer_class = slot->transfer_class;
    fragment.flags = fragment_offset == 0U ?
                         UCN_TRANSFER_FLAG_START : 0U;
    fragment_end_offset = (uint16_t)(fragment_offset + data_length);
    if (fragment_end_offset ==
        slot->total_length) {
        fragment.flags |= UCN_TRANSFER_FLAG_END;
    }
    fragment.transfer_id = slot->transfer_id;
    fragment.total_length = slot->total_length;
    fragment.fragment_offset = fragment_offset;
    fragment.message_crc32 = slot->message_crc32;
    fragment.data = &slot->data[fragment_offset];
    fragment.data_length = data_length;
    result = ucn_transfer_encode_fragment(&fragment, payload, sizeof(payload),
                                          &payload_length);
    if (result != UCN_OK) {
        return result;
    }

    previous_inflight_end = slot->inflight_end_offset;
    previous_resend_offset = slot->resend_offset;
    if (retry) {
        slot->resend_offset = fragment_end_offset;
    } else {
        slot->inflight_end_offset = fragment_end_offset;
    }
    slot->awaiting_ack = true;
    slot->ack_deadline_ms = ucn_deadline_from_now(
        transfer->now_ms, transfer->config.ack_timeout_ms);
    prospective_in_flight = outstanding_fragment_count(slot);
    result = ucn_node_send(transfer->config.node, slot->destination,
                           UCN_MSG_TRANSFER_FRAGMENT,
                           UCN_TRAFFIC_Q1_REALTIME, payload,
                           (uint16_t)payload_length);
    if (result == UCN_OK) {
        transfer->stats.fragments_sent++;
        if (retry) {
            transfer->stats.fragments_retried++;
        }
        if (prospective_in_flight >
            transfer->stats.fragments_in_flight_peak) {
            transfer->stats.fragments_in_flight_peak = prospective_in_flight;
        }
    }
    /* A synchronous virtual Link may have delivered the ACK recursively and
     * completed/reused the slot before ucn_node_send() returns. */
    if (!slot->occupied || slot->transfer_id != transfer_id) {
        return result;
    }
    if (result == UCN_OK) {
        if (retry && slot->resend_active &&
            slot->resend_offset >= slot->resend_end_offset) {
            slot->resend_active = false;
        }
        return UCN_OK;
    }
    if (retry) {
        slot->resend_offset = previous_resend_offset;
    } else {
        slot->inflight_end_offset = previous_inflight_end;
    }
    slot->awaiting_ack =
        slot->acknowledged_offset < slot->inflight_end_offset;
    if (result == UCN_ERR_TOO_LARGE &&
        slot->fragment_data_limit > UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES) {
        uint16_t reduced = (uint16_t)(slot->fragment_data_limit / 2U);

        if (reduced < UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES) {
            reduced = UCN_TRANSFER_MIN_FRAGMENT_DATA_BYTES;
        }
        slot->fragment_data_limit = reduced;
    } else if (result == UCN_ERR_TOO_LARGE) {
        /* The minimum legal fragment still cannot cross this egress.  Repeating
         * the same impossible frame until the class deadline would only waste
         * CPU and airtime, so fail the bounded Transfer immediately. */
        complete_tx_slot(transfer, slot,
                         UCN_TRANSFER_COMPLETION_SEND_FAILED);
    } else if (result == UCN_ERR_NOT_FOUND) {
        (void)ucn_node_discover_route(transfer->config.node,
                                      slot->destination, transfer->now_ms);
    }
    return result;
}

static void expire_transfer_state(ucn_transfer_t *transfer)
{
    size_t index;

    for (index = 0U; index < UCN_TRANSFER_RECENT_COMPLETIONS; ++index) {
        if (transfer->recent[index].occupied &&
            ucn_deadline_expired(transfer->now_ms,
                                 transfer->recent[index].expires_at_ms)) {
            (void)memset(&transfer->recent[index], 0,
                         sizeof(transfer->recent[index]));
        }
    }
    for (index = 0U; index < UCN_TRANSFER_RX_SLOTS; ++index) {
        ucn_transfer_rx_slot_t *slot = &transfer->rx_slots[index];

        if (slot->occupied &&
            ucn_deadline_expired(transfer->now_ms, slot->deadline_ms)) {
            if (slot->completed) {
                transfer->stats.completed_hold_expired++;
            } else {
                transfer->stats.rx_expired++;
                (void)send_ack(transfer, slot->source, slot->endpoint,
                               slot->transfer_id, slot->received_length,
                               UCN_TRANSFER_ACK_EXPIRED);
            }
            clear_rx_slot(slot);
        }
    }
    for (index = 0U; index < UCN_TRANSFER_TX_SLOTS; ++index) {
        ucn_transfer_tx_slot_t *slot = &transfer->tx_slots[index];

        if (slot->occupied &&
            ucn_deadline_expired(transfer->now_ms, slot->deadline_ms)) {
            complete_tx_slot(transfer, slot, UCN_TRANSFER_COMPLETION_TIMEOUT);
        }
    }
}

ucn_result_t ucn_transfer_step(ucn_transfer_t *transfer, uint32_t now_ms)
{
    size_t examined;

    if (transfer == NULL || !transfer->initialized) {
        return UCN_ERR_ARGUMENT;
    }
    transfer->now_ms = now_ms;
    expire_transfer_state(transfer);

    for (examined = 0U; examined < UCN_TRANSFER_TX_SLOTS; ++examined) {
        size_t index = (transfer->next_tx_slot + examined) %
                       UCN_TRANSFER_TX_SLOTS;
        ucn_transfer_tx_slot_t *slot = &transfer->tx_slots[index];
        ucn_result_t result;

        if (!slot->occupied) {
            continue;
        }
        transfer->next_tx_slot = (index + 1U) % UCN_TRANSFER_TX_SLOTS;
        if (slot->resend_active) {
            if (slot->resend_offset >= slot->resend_end_offset) {
                slot->resend_active = false;
            } else {
                result = send_tx_fragment(transfer, slot,
                                          slot->resend_offset, true);
                goto handle_send_result;
            }
        }
        if (slot->awaiting_ack &&
            ucn_deadline_expired(now_ms, slot->ack_deadline_ms)) {
            if (!begin_window_recovery(transfer, slot)) {
                return UCN_ERR_NOT_FOUND;
            }
            result = send_tx_fragment(transfer, slot,
                                      slot->resend_offset, true);
        } else if (slot->inflight_end_offset < slot->total_length &&
                   outstanding_fragment_count(slot) < slot->window_size) {
            result = send_tx_fragment(transfer, slot,
                                      slot->inflight_end_offset, false);
        } else {
            return UCN_ERR_NOT_FOUND;
        }
handle_send_result:
        if (result == UCN_OK || result == UCN_ERR_NO_SPACE ||
            result == UCN_ERR_LINK_DOWN || result == UCN_ERR_NOT_FOUND ||
            result == UCN_ERR_TOO_LARGE) {
            return result;
        }
        if (slot->occupied) {
            complete_tx_slot(transfer, slot,
                             UCN_TRANSFER_COMPLETION_SEND_FAILED);
        }
        return result;
    }
    return UCN_ERR_NOT_FOUND;
}

ucn_result_t ucn_transfer_release_received(ucn_transfer_t *transfer,
                                           ucn_transfer_rx_handle_t handle)
{
    size_t encoded_index;
    size_t index;
    uint16_t generation;
    ucn_transfer_rx_slot_t *slot;

    if (transfer == NULL || !transfer->initialized ||
        handle == UCN_TRANSFER_RX_HANDLE_DIRECT) {
        return UCN_ERR_ARGUMENT;
    }
    encoded_index = (size_t)(handle & UINT32_C(0xFF));
    if (encoded_index == 0U || encoded_index > UCN_TRANSFER_RX_SLOTS) {
        return UCN_ERR_ARGUMENT;
    }
    index = encoded_index - 1U;
    generation = (uint16_t)(handle >> 8U);
    slot = &transfer->rx_slots[index];
    if (!slot->occupied || !slot->completed ||
        slot->generation != generation) {
        return UCN_ERR_NOT_FOUND;
    }
    clear_rx_slot(slot);
    return UCN_OK;
}

const ucn_transfer_stats_t *ucn_transfer_get_stats(
    const ucn_transfer_t *transfer)
{
    return transfer == NULL || !transfer->initialized ? NULL : &transfer->stats;
}
