#include "../internal/ucn_v6_qos_private.h"

#include <limits.h>
#include <string.h>

#define UCN_V6_QOS_SCHEMA UINT16_C(1)

typedef char ucn_v6_qos_owner_storage_must_fit[
    sizeof(struct ucn_v6_qos_owner) <= UCN_V6_QOS_OWNER_STORAGE_BYTES ?
        1 : -1];

static const uint8_t class_schedule[12] = {
    0U, 1U, 2U, 3U, 0U, 1U, 2U, 0U, 1U, 0U, 0U, 0U
};

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

static bool owner_is_valid(const ucn_v6_qos_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_QOS_OWNER_MAGIC &&
           owner->schema == UCN_V6_QOS_SCHEMA && owner->initialized &&
           !owner->faulted && owner->canary == UCN_V6_QOS_OWNER_CANARY &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH;
}

static bool policy_is_valid(const ucn_v6_qos_policy_t *policy)
{
    size_t index;
    if (policy == NULL || policy->refill_period_us == 0U ||
        policy->max_flows_per_session == 0U ||
        policy->max_flows_per_session > UCN_V6_CONFIG_QOS_FLOW_SLOTS ||
        policy->q2_quantum_bytes == 0U || policy->q3_quantum_bytes == 0U) {
        return false;
    }
    for (index = 0U; index < 4U; ++index) {
        if (policy->source_flow_burst[index] == 0U ||
            policy->source_flow_refill[index] == 0U ||
            policy->max_hop_budget_us[index] == 0U) {
            return false;
        }
    }
    return true;
}

static size_t class_capacity(ucn_v6_traffic_class_t traffic_class)
{
    static const size_t capacities[4] = {
        UCN_V6_CONFIG_QOS_Q0_DEPTH,
        UCN_V6_CONFIG_QOS_Q1_DEPTH,
        UCN_V6_CONFIG_QOS_Q2_DEPTH,
        UCN_V6_CONFIG_QOS_Q3_DEPTH
    };
    return (uint32_t)traffic_class < 4U ?
               capacities[(size_t)traffic_class] : 0U;
}

static void hash_bytes(uint64_t *hash, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;
    for (index = 0U; index < length; ++index) {
        *hash ^= bytes[index];
        *hash *= UINT64_C(1099511628211);
    }
}

static void hash_u16(uint64_t *hash, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
    hash_bytes(hash, bytes, sizeof(bytes));
}

static void hash_u32(uint64_t *hash, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
    hash_bytes(hash, bytes, sizeof(bytes));
}

static bool opened_is_schedulable(const ucn_v6_security_open_result_t *opened)
{
    const ucn_v6_frame_t *frame;
    if (opened == NULL || !opened->hop_authenticated ||
        opened->group_discovery_only ||
        !ucn_v6_principal_is_valid(&opened->authenticated_principal) ||
        !session_is_valid(&opened->ingress_peer_session)) {
        return false;
    }
    frame = &opened->frame;
    return (uint32_t)frame->traffic_class < 4U &&
           (uint32_t)frame->delivery_guarantee <=
               (uint32_t)UCN_V6_DELIVERY_RELIABLE &&
           frame->realm_id != 0U && frame->source_address != 0U &&
           frame->destination_address != 0U &&
           frame->source_binding_generation != 0U &&
           frame->destination_binding_generation != 0U &&
           frame->session_generation != 0U &&
           frame->session_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           (frame->flags & UCN_V6_FLAG_MESSAGE_CONTEXT) != 0U &&
           frame->message.source_endpoint != 0U &&
           frame->message.destination_endpoint != 0U;
}

static ucn_v6_session_key_t source_session(
    const ucn_v6_security_open_result_t *opened)
{
    return opened->ingress_peer_session;
}

void ucn_v6_qos_default_policy(ucn_v6_qos_policy_t *policy)
{
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
        policy->source_flow_burst[0] = 8U;
        policy->source_flow_burst[1] = 6U;
        policy->source_flow_burst[2] = 4U;
        policy->source_flow_burst[3] = 3U;
        policy->source_flow_refill[0] = 4U;
        policy->source_flow_refill[1] = 3U;
        policy->source_flow_refill[2] = 2U;
        policy->source_flow_refill[3] = 1U;
        policy->refill_period_us = UINT64_C(1000);
        policy->max_hop_budget_us[0] = UINT64_C(100000);
        policy->max_hop_budget_us[1] = UINT64_C(250000);
        policy->max_hop_budget_us[2] = UINT64_C(1000000);
        policy->max_hop_budget_us[3] = UINT64_C(5000000);
        policy->max_flows_per_session =
            UCN_V6_CONFIG_QOS_FLOW_SLOTS < 8U ?
                UCN_V6_CONFIG_QOS_FLOW_SLOTS : 8U;
        policy->q2_quantum_bytes = 512U;
        policy->q3_quantum_bytes = 256U;
    }
}

ucn_v6_result_t ucn_v6_qos_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_qos_policy_t *policy,
    ucn_v6_qos_owner_t **owner_out)
{
    ucn_v6_qos_owner_t *owner;
    if (owner_out == NULL || !policy_is_valid(policy) ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        ucn_v6_storage_validate(storage, storage_bytes,
                                UCN_V6_QOS_OWNER_STORAGE_BYTES,
                                UCN_V6_STORAGE_ALIGNMENT) != UCN_V6_OK) {
        return UCN_V6_ERR_CONFIG;
    }
    owner = (ucn_v6_qos_owner_t *)storage;
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_QOS_OWNER_MAGIC;
    owner->schema = UCN_V6_QOS_SCHEMA;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->policy = *policy;
    owner->initialized = true;
    owner->canary = UCN_V6_QOS_OWNER_CANARY;
    *owner_out = owner;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_qos_flow_id(
    const ucn_v6_security_open_result_t *opened,
    uint64_t *flow_id)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const ucn_v6_frame_t *frame;
    if (!opened_is_schedulable(opened) || flow_id == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    frame = &opened->frame;
    hash_bytes(&hash, opened->authenticated_principal.bytes,
               sizeof(opened->authenticated_principal.bytes));
    hash_bytes(&hash, opened->ingress_peer_session.principal.bytes,
               sizeof(opened->ingress_peer_session.principal.bytes));
    hash_u32(&hash, opened->ingress_peer_session.binding.realm_id);
    hash_u32(&hash, opened->ingress_peer_session.binding.node_address);
    hash_u32(&hash,
             opened->ingress_peer_session.binding.binding_generation);
    hash_u32(&hash,
             opened->ingress_peer_session.session_generation);
    hash_u32(&hash, frame->realm_id);
    hash_u32(&hash, frame->source_address);
    hash_u32(&hash, frame->destination_address);
    hash_u32(&hash, frame->source_binding_generation);
    hash_u32(&hash, frame->destination_binding_generation);
    hash_u32(&hash, frame->session_generation);
    hash_u16(&hash, frame->message.source_endpoint);
    hash_u16(&hash, frame->message.destination_endpoint);
    hash_u16(&hash, frame->protocol_opcode);
    {
        uint8_t traffic = (uint8_t)frame->traffic_class;
        uint8_t frame_type = (uint8_t)frame->frame_type;
        hash_bytes(&hash, &frame_type, sizeof(frame_type));
        hash_bytes(&hash, &traffic, sizeof(traffic));
    }
    *flow_id = hash != 0U ? hash : UINT64_C(1);
    return UCN_V6_OK;
}

static ucn_v6_qos_flow_state_t *find_flow(ucn_v6_qos_owner_t *owner,
                                          const ucn_v6_session_key_t *source,
                                          uint64_t flow_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_QOS_FLOW_SLOTS; ++index) {
        if (owner->flows[index].occupied &&
            owner->flows[index].flow_id == flow_id &&
            session_equal(&owner->flows[index].source, source)) {
            return &owner->flows[index];
        }
    }
    return NULL;
}

static size_t count_session_flows(const ucn_v6_qos_owner_t *owner,
                                  const ucn_v6_session_key_t *source)
{
    size_t index;
    size_t count = 0U;
    for (index = 0U; index < UCN_V6_CONFIG_QOS_FLOW_SLOTS; ++index) {
        if (owner->flows[index].occupied &&
            session_equal(&owner->flows[index].source, source)) {
            ++count;
        }
    }
    return count;
}

static ucn_v6_qos_flow_state_t *find_free_flow(ucn_v6_qos_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_QOS_FLOW_SLOTS; ++index) {
        if (!owner->flows[index].occupied) {
            return &owner->flows[index];
        }
    }
    return NULL;
}

static bool token_exists(const ucn_v6_qos_owner_t *owner, uint64_t token)
{
    size_t index;
    for (index = 0U; index < UCN_V6_QOS_QUEUE_CAPACITY; ++index) {
        if (owner->queue[index].occupied &&
            owner->queue[index].buffer_token == token) {
            return true;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (owner->inflight[index].occupied &&
            owner->inflight[index].buffer_token == token) {
            return true;
        }
    }
    return false;
}

static ucn_v6_qos_queue_item_t *find_latest(
    ucn_v6_qos_owner_t *owner,
    const ucn_v6_session_key_t *source,
    uint64_t flow_id)
{
    size_t index;
    for (index = 0U; index < UCN_V6_QOS_QUEUE_CAPACITY; ++index) {
        if (owner->queue[index].occupied &&
            owner->queue[index].delivery_guarantee ==
                UCN_V6_DELIVERY_LATEST &&
            owner->queue[index].flow_id == flow_id &&
            session_equal(&owner->queue[index].source, source)) {
            return &owner->queue[index];
        }
    }
    return NULL;
}

static ucn_v6_qos_queue_item_t *find_free_queue(ucn_v6_qos_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_QOS_QUEUE_CAPACITY; ++index) {
        if (!owner->queue[index].occupied) {
            return &owner->queue[index];
        }
    }
    return NULL;
}

static bool refill_preview(const ucn_v6_qos_owner_t *owner,
                           const ucn_v6_qos_flow_state_t *flow,
                           uint64_t now_us,
                           uint16_t tokens[4],
                           uint64_t *last_refill_us)
{
    size_t index;
    uint64_t periods;
    if (now_us < flow->last_refill_us) {
        return false;
    }
    memcpy(tokens, flow->tokens, sizeof(flow->tokens));
    *last_refill_us = flow->last_refill_us;
    periods = (now_us - flow->last_refill_us) /
              owner->policy.refill_period_us;
    if (periods == 0U) {
        return true;
    }
    for (index = 0U; index < 4U; ++index) {
        uint64_t added;
        uint64_t available;
        if (periods > UINT64_MAX / owner->policy.source_flow_refill[index]) {
            added = UINT64_MAX;
        } else {
            added = periods * owner->policy.source_flow_refill[index];
        }
        available = (uint64_t)tokens[index] + added;
        tokens[index] = (uint16_t)(
            available > owner->policy.source_flow_burst[index] ?
                owner->policy.source_flow_burst[index] : available);
    }
    if (periods > (UINT64_MAX - flow->last_refill_us) /
                      owner->policy.refill_period_us) {
        *last_refill_us = now_us;
    } else {
        *last_refill_us = flow->last_refill_us +
            periods * owner->policy.refill_period_us;
    }
    return true;
}

ucn_v6_result_t ucn_v6_qos_enqueue(
    ucn_v6_qos_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_security_open_result_t *opened,
    uint64_t buffer_token,
    uint16_t payload_bytes,
    uint8_t local_priority,
    ucn_v6_qos_enqueue_result_t *result_out)
{
    ucn_v6_qos_enqueue_result_t result;
    ucn_v6_session_key_t source;
    ucn_v6_qos_flow_state_t *flow;
    ucn_v6_qos_flow_state_t *free_flow = NULL;
    ucn_v6_qos_queue_item_t *queue;
    uint64_t flow_id;
    uint16_t tokens[4];
    uint64_t last_refill;
    size_t class_index;
    if (!owner_is_valid(owner) || !opened_is_schedulable(opened) ||
        buffer_token == 0U || payload_bytes == 0U || local_priority > 7U ||
        result_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (token_exists(owner, buffer_token) ||
        ucn_v6_qos_flow_id(opened, &flow_id) != UCN_V6_OK) {
        return UCN_V6_ERR_REPLAY;
    }
    class_index = (size_t)opened->frame.traffic_class;
    if ((opened->frame.flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U &&
        (opened->frame.hop_budget.initial_budget_us == 0U ||
         opened->frame.hop_budget.remaining_budget_us == 0U ||
         opened->frame.hop_budget.remaining_budget_us >
             opened->frame.hop_budget.initial_budget_us ||
         opened->frame.hop_budget.initial_budget_us >
             owner->policy.max_hop_budget_us[class_index])) {
        return UCN_V6_ERR_ACCESS;
    }
    source = source_session(opened);
    if (!session_is_valid(&source)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    flow = find_flow(owner, &source, flow_id);
    if (flow == NULL) {
        if (count_session_flows(owner, &source) >=
                owner->policy.max_flows_per_session ||
            (free_flow = find_free_flow(owner)) == NULL) {
            saturating_increment(&owner->stats.rejected_quota[class_index]);
            return UCN_V6_ERR_NO_SPACE;
        }
        memcpy(tokens, owner->policy.source_flow_burst, sizeof(tokens));
        last_refill = now_us;
    } else if (!refill_preview(owner, flow, now_us, tokens,
                               &last_refill)) {
        return UCN_V6_ERR_STATE;
    }
    if (tokens[class_index] == 0U) {
        saturating_increment(&owner->stats.rejected_quota[class_index]);
        return UCN_V6_ERR_NO_SPACE;
    }
    queue = opened->frame.delivery_guarantee == UCN_V6_DELIVERY_LATEST ?
                find_latest(owner, &source, flow_id) : NULL;
    if (queue != NULL && owner->selected &&
        (size_t)(queue - owner->queue) == owner->selected_queue_index) {
        return UCN_V6_ERR_STATE;
    }
    if (queue == NULL) {
        if (owner->stats.queued[class_index] >=
                class_capacity(opened->frame.traffic_class) ||
            (queue = find_free_queue(owner)) == NULL ||
            owner->next_arrival_order == UINT64_MAX) {
            return UCN_V6_ERR_NO_SPACE;
        }
    }
    memset(&result, 0, sizeof(result));
    result.accepted = true;
    result.flow_id = flow_id;
    if (queue->occupied) {
        result.replaced_latest = true;
        result.replaced_buffer_token = queue->buffer_token;
    }
    if (flow == NULL) {
        flow = free_flow;
        memset(flow, 0, sizeof(*flow));
        flow->occupied = true;
        flow->source = source;
        flow->flow_id = flow_id;
        ++owner->stats.flow_slots;
    }
    memcpy(flow->tokens, tokens, sizeof(tokens));
    --flow->tokens[class_index];
    flow->last_refill_us = last_refill;
    if (!queue->occupied) {
        ++owner->stats.queued[class_index];
    } else {
        saturating_increment(&owner->stats.latest_replaced[class_index]);
    }
    memset(queue, 0, sizeof(*queue));
    queue->occupied = true;
    queue->buffer_token = buffer_token;
    queue->flow_id = flow_id;
    queue->source = source;
    queue->traffic_class = opened->frame.traffic_class;
    queue->delivery_guarantee = opened->frame.delivery_guarantee;
    queue->payload_bytes = payload_bytes;
    queue->local_priority = local_priority;
    queue->has_hop_budget =
        (opened->frame.flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) != 0U;
    queue->initial_budget_us = opened->frame.hop_budget.initial_budget_us;
    queue->remaining_budget_us = opened->frame.hop_budget.remaining_budget_us;
    queue->enqueued_at_us = now_us;
    queue->arrival_order = ++owner->next_arrival_order;
    saturating_increment(&owner->stats.enqueued[class_index]);
    *result_out = result;
    return UCN_V6_OK;
}

static bool flow_has_class_item(const ucn_v6_qos_owner_t *owner,
                                const ucn_v6_qos_flow_state_t *flow,
                                ucn_v6_traffic_class_t traffic_class)
{
    size_t index;
    for (index = 0U; index < UCN_V6_QOS_QUEUE_CAPACITY; ++index) {
        if (owner->queue[index].occupied &&
            owner->queue[index].traffic_class == traffic_class &&
            owner->queue[index].flow_id == flow->flow_id &&
            session_equal(&owner->queue[index].source, &flow->source)) {
            return true;
        }
    }
    return false;
}

static size_t choose_flow_item(const ucn_v6_qos_owner_t *owner,
                               const ucn_v6_qos_flow_state_t *flow,
                               ucn_v6_traffic_class_t traffic_class)
{
    size_t index;
    size_t best = UCN_V6_QOS_QUEUE_CAPACITY;
    for (index = 0U; index < UCN_V6_QOS_QUEUE_CAPACITY; ++index) {
        const ucn_v6_qos_queue_item_t *item = &owner->queue[index];
        const ucn_v6_qos_queue_item_t *chosen;
        if (!item->occupied || item->traffic_class != traffic_class ||
            item->flow_id != flow->flow_id ||
            !session_equal(&item->source, &flow->source)) {
            continue;
        }
        if (best == UCN_V6_QOS_QUEUE_CAPACITY) {
            best = index;
            continue;
        }
        chosen = &owner->queue[best];
        if (traffic_class == UCN_V6_TRAFFIC_Q0 &&
            (item->local_priority < chosen->local_priority ||
             (item->local_priority == chosen->local_priority &&
              item->has_hop_budget &&
              (!chosen->has_hop_budget ||
               (UINT64_MAX - item->enqueued_at_us <
                    item->remaining_budget_us ? UINT64_MAX :
                    item->enqueued_at_us + item->remaining_budget_us) <
               (UINT64_MAX - chosen->enqueued_at_us <
                    chosen->remaining_budget_us ? UINT64_MAX :
                    chosen->enqueued_at_us + chosen->remaining_budget_us))))) {
            best = index;
        } else if (traffic_class != UCN_V6_TRAFFIC_Q0 &&
                   item->arrival_order < chosen->arrival_order) {
            best = index;
        }
    }
    return best;
}

static size_t choose_class_item(ucn_v6_qos_owner_t *owner,
                                ucn_v6_traffic_class_t traffic_class)
{
    size_t attempt;
    size_t class_index = (size_t)traffic_class;
    for (attempt = 0U; attempt < UCN_V6_CONFIG_QOS_FLOW_SLOTS; ++attempt) {
        size_t flow_index =
            ((size_t)owner->flow_cursor[class_index] + attempt) %
            UCN_V6_CONFIG_QOS_FLOW_SLOTS;
        ucn_v6_qos_flow_state_t *flow = &owner->flows[flow_index];
        size_t item_index;
        uint32_t quantum;
        if (!flow->occupied ||
            !flow_has_class_item(owner, flow, traffic_class)) {
            continue;
        }
        item_index = choose_flow_item(owner, flow, traffic_class);
        if (item_index == UCN_V6_QOS_QUEUE_CAPACITY) {
            continue;
        }
        if (traffic_class == UCN_V6_TRAFFIC_Q2 ||
            traffic_class == UCN_V6_TRAFFIC_Q3) {
            quantum = traffic_class == UCN_V6_TRAFFIC_Q2 ?
                          owner->policy.q2_quantum_bytes :
                          owner->policy.q3_quantum_bytes;
            if (UINT32_MAX - flow->deficit[class_index] < quantum) {
                flow->deficit[class_index] = UINT32_MAX;
            } else {
                flow->deficit[class_index] += quantum;
            }
            if (flow->deficit[class_index] <
                    owner->queue[item_index].payload_bytes) {
                continue;
            }
            flow->deficit[class_index] -=
                owner->queue[item_index].payload_bytes;
        }
        owner->flow_cursor[class_index] = (uint8_t)(
            (flow_index + 1U) % UCN_V6_CONFIG_QOS_FLOW_SLOTS);
        return item_index;
    }
    return UCN_V6_QOS_QUEUE_CAPACITY;
}

static ucn_v6_qos_inflight_t *find_free_inflight(ucn_v6_qos_owner_t *owner)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (!owner->inflight[index].occupied) {
            return &owner->inflight[index];
        }
    }
    return NULL;
}

ucn_v6_result_t ucn_v6_qos_select_next(
    ucn_v6_qos_owner_t *owner,
    uint64_t now_us,
    ucn_v6_qos_selection_t *selection_out)
{
    size_t attempt;
    size_t selected = UCN_V6_QOS_QUEUE_CAPACITY;
    ucn_v6_qos_selection_t selection;
    ucn_v6_qos_queue_item_t *item;
    if (!owner_is_valid(owner) || selection_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (owner->selected) {
        return UCN_V6_ERR_STATE;
    }
    for (attempt = 0U; attempt < sizeof(class_schedule); ++attempt) {
        uint8_t schedule_index = (uint8_t)(
            (owner->schedule_cursor + attempt) % sizeof(class_schedule));
        ucn_v6_traffic_class_t traffic_class =
            (ucn_v6_traffic_class_t)class_schedule[schedule_index];
        selected = choose_class_item(owner, traffic_class);
        if (selected != UCN_V6_QOS_QUEUE_CAPACITY) {
            owner->schedule_cursor = (uint8_t)(
                (schedule_index + 1U) % sizeof(class_schedule));
            break;
        }
    }
    if (selected == UCN_V6_QOS_QUEUE_CAPACITY) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    item = &owner->queue[selected];
    if (now_us < item->enqueued_at_us) {
        return UCN_V6_ERR_STATE;
    }
    memset(&selection, 0, sizeof(selection));
    selection.action = UCN_V6_QOS_ACTION_SEND;
    selection.buffer_token = item->buffer_token;
    selection.flow_id = item->flow_id;
    selection.source = item->source;
    selection.traffic_class = item->traffic_class;
    selection.delivery_guarantee = item->delivery_guarantee;
    selection.payload_bytes = item->payload_bytes;
    selection.local_priority = item->local_priority;
    selection.has_hop_budget = item->has_hop_budget;
    selection.initial_budget_us = item->initial_budget_us;
    selection.remaining_budget_us = item->remaining_budget_us;
    if (item->has_hop_budget) {
        uint64_t residence = now_us - item->enqueued_at_us;
        if (residence >= item->remaining_budget_us) {
            selection.action = UCN_V6_QOS_ACTION_DROP_EXPIRED;
            selection.remaining_budget_us = 0U;
        } else {
            selection.remaining_budget_us -= residence;
        }
    }
    if (selection.action == UCN_V6_QOS_ACTION_SEND &&
        find_free_inflight(owner) == NULL) {
        return UCN_V6_ERR_NO_SPACE;
    }
    owner->selected = true;
    owner->selected_queue_index = (uint16_t)selected;
    owner->selected_action = selection.action;
    owner->stats.selection_pending = true;
    saturating_increment(
        &owner->stats.scheduler_selected[(size_t)item->traffic_class]);
    *selection_out = selection;
    return UCN_V6_OK;
}

static void remove_queue_item(ucn_v6_qos_owner_t *owner, size_t index)
{
    size_t class_index = (size_t)owner->queue[index].traffic_class;
    memset(&owner->queue[index], 0, sizeof(owner->queue[index]));
    if (owner->stats.queued[class_index] != 0U) {
        --owner->stats.queued[class_index];
    }
}

ucn_v6_result_t ucn_v6_qos_complete_selection(
    ucn_v6_qos_owner_t *owner,
    uint64_t buffer_token,
    ucn_v6_qos_selection_result_t result)
{
    ucn_v6_qos_queue_item_t *item;
    size_t class_index;
    if (!owner_is_valid(owner) || !owner->selected || buffer_token == 0U ||
        (uint32_t)result < (uint32_t)UCN_V6_QOS_SELECTION_RETRY ||
        (uint32_t)result > (uint32_t)UCN_V6_QOS_SELECTION_DROP_RETIRED ||
        owner->selected_queue_index >= UCN_V6_QOS_QUEUE_CAPACITY) {
        return UCN_V6_ERR_ARGUMENT;
    }
    item = &owner->queue[owner->selected_queue_index];
    if (!item->occupied || item->buffer_token != buffer_token) {
        return UCN_V6_ERR_REPLAY;
    }
    class_index = (size_t)item->traffic_class;
    if ((owner->selected_action == UCN_V6_QOS_ACTION_DROP_EXPIRED &&
         result != UCN_V6_QOS_SELECTION_DROP_RETIRED) ||
        (owner->selected_action == UCN_V6_QOS_ACTION_SEND &&
         result == UCN_V6_QOS_SELECTION_DROP_RETIRED)) {
        return UCN_V6_ERR_STATE;
    }
    if (result == UCN_V6_QOS_SELECTION_LINK_SUBMITTED) {
        ucn_v6_qos_inflight_t *inflight;
        if (owner->selected_action != UCN_V6_QOS_ACTION_SEND ||
            (inflight = find_free_inflight(owner)) == NULL) {
            return UCN_V6_ERR_STATE;
        }
        memset(inflight, 0, sizeof(*inflight));
        inflight->occupied = true;
        inflight->buffer_token = item->buffer_token;
        inflight->flow_id = item->flow_id;
        inflight->source = item->source;
        inflight->traffic_class = item->traffic_class;
        inflight->stage = UCN_V6_QOS_COMPLETION_LINK_SUBMITTED;
        ++owner->stats.inflight;
        saturating_increment(&owner->stats.link_submitted[class_index]);
        remove_queue_item(owner, owner->selected_queue_index);
    } else if (result == UCN_V6_QOS_SELECTION_DROP_RETIRED) {
        if (owner->selected_action != UCN_V6_QOS_ACTION_DROP_EXPIRED) {
            return UCN_V6_ERR_STATE;
        }
        saturating_increment(&owner->stats.dropped_expired[class_index]);
        remove_queue_item(owner, owner->selected_queue_index);
    }
    owner->selected = false;
    owner->selected_queue_index = 0U;
    owner->selected_action = 0;
    owner->stats.selection_pending = false;
    return UCN_V6_OK;
}

static ucn_v6_qos_inflight_t *find_inflight(ucn_v6_qos_owner_t *owner,
                                            uint64_t token)
{
    size_t index;
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (owner->inflight[index].occupied &&
            owner->inflight[index].buffer_token == token) {
            return &owner->inflight[index];
        }
    }
    return NULL;
}

ucn_v6_result_t ucn_v6_qos_record_completion(
    ucn_v6_qos_owner_t *owner,
    uint64_t buffer_token,
    ucn_v6_qos_completion_stage_t stage)
{
    ucn_v6_qos_inflight_t *inflight;
    size_t class_index;
    uint32_t *counter;
    if (!owner_is_valid(owner) || buffer_token == 0U ||
        (uint32_t)stage < UCN_V6_QOS_COMPLETION_LINK_SUBMITTED ||
        (uint32_t)stage > UCN_V6_QOS_COMPLETION_APPLICATION_RESULT) {
        return UCN_V6_ERR_ARGUMENT;
    }
    inflight = find_inflight(owner, buffer_token);
    if (inflight == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    if (stage == inflight->stage) {
        return UCN_V6_OK;
    }
    if ((uint32_t)stage != (uint32_t)inflight->stage + 1U) {
        return UCN_V6_ERR_STATE;
    }
    class_index = (size_t)inflight->traffic_class;
    counter = stage == UCN_V6_QOS_COMPLETION_PHYSICAL_COMPLETED ?
                  &owner->stats.physical_completed[class_index] :
              stage == UCN_V6_QOS_COMPLETION_REMOTE_ACKED ?
                  &owner->stats.remote_acked[class_index] :
                  &owner->stats.application_result[class_index];
    saturating_increment(counter);
    inflight->stage = stage;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_qos_retire_completion(
    ucn_v6_qos_owner_t *owner,
    uint64_t buffer_token)
{
    ucn_v6_qos_inflight_t *inflight;
    if (!owner_is_valid(owner) || buffer_token == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    inflight = find_inflight(owner, buffer_token);
    if (inflight == NULL) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    memset(inflight, 0, sizeof(*inflight));
    if (owner->stats.inflight != 0U) {
        --owner->stats.inflight;
    }
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_qos_forward_budget(
    const ucn_v6_security_open_result_t *opened,
    uint64_t policy_max_budget_us,
    uint64_t residence_bound_us,
    uint64_t transmit_bound_us,
    ucn_v6_hop_budget_context_t *next_budget)
{
    const ucn_v6_hop_budget_context_t *current;
    ucn_v6_hop_budget_context_t next;
    uint64_t debit;
    if (!opened_is_schedulable(opened) || next_budget == NULL ||
        policy_max_budget_us == 0U ||
        (opened->frame.flags & UCN_V6_FLAG_HOP_BUDGET_CONTEXT) == 0U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    current = &opened->frame.hop_budget;
    if (current->initial_budget_us == 0U ||
        current->remaining_budget_us == 0U ||
        current->remaining_budget_us > current->initial_budget_us ||
        current->initial_budget_us > policy_max_budget_us ||
        UINT64_MAX - residence_bound_us < transmit_bound_us) {
        return UCN_V6_ERR_ACCESS;
    }
    debit = residence_bound_us + transmit_bound_us;
    if (debit >= current->remaining_budget_us) {
        return UCN_V6_ERR_TIMEOUT;
    }
    next.initial_budget_us = current->initial_budget_us;
    next.remaining_budget_us = current->remaining_budget_us - debit;
    *next_budget = next;
    return UCN_V6_OK;
}

uint8_t ucn_v6_qos_hardware_priority(
    ucn_v6_traffic_class_t traffic_class,
    uint8_t hardware_priority_count)
{
    uint32_t traffic = (uint32_t)traffic_class;
    uint32_t rank;
    if (traffic >= 4U || hardware_priority_count == 0U) {
        return 0U;
    }
    rank = 3U - traffic;
    if (hardware_priority_count == 1U) {
        return 0U;
    }
    return (uint8_t)((rank * (hardware_priority_count - 1U)) / 3U);
}

ucn_v6_result_t ucn_v6_qos_invalidate_session(
    ucn_v6_qos_owner_t *owner,
    const ucn_v6_session_key_t *session,
    uint64_t *retired_tokens,
    size_t retired_capacity,
    size_t *retired_count)
{
    size_t index;
    size_t count = 0U;
    if (!owner_is_valid(owner) || !session_is_valid(session) ||
        retired_count == NULL ||
        (retired_capacity != 0U && retired_tokens == NULL)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_V6_QOS_QUEUE_CAPACITY; ++index) {
        if (owner->queue[index].occupied &&
            session_equal(&owner->queue[index].source, session)) {
            ++count;
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (owner->inflight[index].occupied &&
            session_equal(&owner->inflight[index].source, session)) {
            ++count;
        }
    }
    if (count > retired_capacity) {
        return UCN_V6_ERR_NO_SPACE;
    }
    count = 0U;
    for (index = 0U; index < UCN_V6_QOS_QUEUE_CAPACITY; ++index) {
        if (owner->queue[index].occupied &&
            session_equal(&owner->queue[index].source, session)) {
            retired_tokens[count++] = owner->queue[index].buffer_token;
            if (owner->selected && owner->selected_queue_index == index) {
                owner->selected = false;
                owner->stats.selection_pending = false;
            }
            remove_queue_item(owner, index);
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_QOS_FLOW_SLOTS; ++index) {
        if (owner->flows[index].occupied &&
            session_equal(&owner->flows[index].source, session)) {
            memset(&owner->flows[index], 0, sizeof(owner->flows[index]));
            if (owner->stats.flow_slots != 0U) {
                --owner->stats.flow_slots;
            }
        }
    }
    for (index = 0U; index < UCN_V6_CONFIG_QOS_INFLIGHT; ++index) {
        if (owner->inflight[index].occupied &&
            session_equal(&owner->inflight[index].source, session)) {
            retired_tokens[count++] = owner->inflight[index].buffer_token;
            memset(&owner->inflight[index], 0,
                   sizeof(owner->inflight[index]));
            if (owner->stats.inflight != 0U) {
                --owner->stats.inflight;
            }
        }
    }
    *retired_count = count;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_qos_copy_stats(
    const ucn_v6_qos_owner_t *owner,
    ucn_v6_qos_stats_t *stats)
{
    if (!owner_is_valid(owner) || stats == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    *stats = owner->stats;
    stats->faulted = owner->faulted;
    return UCN_V6_OK;
}
