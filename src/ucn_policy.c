#include <string.h>

#include "ucn/ucn_node_storage.h"
#include "ucn/ucn_time.h"

static bool policy_node_id_is_valid(ucn_node_id_t node_id)
{
    return node_id != 0U && node_id != UCN_NODE_BROADCAST;
}

static bool policy_traffic_class_is_valid(uint8_t traffic_class)
{
    return traffic_class <= (uint8_t)UCN_TRAFFIC_Q3_BULK;
}

static bool policy_key_is_valid(const ucn_route_policy_key_t *key)
{
    return key != NULL && policy_node_id_is_valid(key->destination) &&
           ucn_endpoint_is_static(key->endpoint) &&
           (key->traffic_class == UCN_POLICY_ANY_TRAFFIC_CLASS ||
            policy_traffic_class_is_valid(key->traffic_class));
}

static bool policy_key_equal(const ucn_route_policy_key_t *left,
                             const ucn_route_policy_key_t *right)
{
    return left->destination == right->destination &&
           left->endpoint == right->endpoint &&
           left->traffic_class == right->traffic_class;
}

static bool policy_flow_key_equal(const ucn_policy_flow_key_t *left,
                                  const ucn_policy_flow_key_t *right)
{
    return left->destination == right->destination &&
           left->endpoint == right->endpoint &&
           left->traffic_class == right->traffic_class;
}

static bool policy_link_is_registered(const ucn_node_t *node,
                                      const ucn_link_t *link)
{
    size_t index;

    if (node == NULL || link == NULL) {
        return false;
    }
    for (index = 0U; index < node->link_count; ++index) {
        if (node->links[index] == link) {
            return true;
        }
    }
    return false;
}

static bool policy_link_is_present(ucn_link_t *const *links,
                                   size_t link_count,
                                   const ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < link_count; ++index) {
        if (links[index] == link) {
            return true;
        }
    }
    return false;
}

static ucn_route_policy_entry_t *find_route_policy_entry(
    ucn_policy_state_t *state,
    const ucn_route_policy_key_t *key)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_ROUTE_POLICIES; ++index) {
        if (state->policies[index].occupied &&
            policy_key_equal(&state->policies[index].config.key, key)) {
            return &state->policies[index];
        }
    }
    return NULL;
}

static const ucn_route_policy_entry_t *find_route_policy_match(
    const ucn_policy_state_t *state,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class)
{
    size_t index;
    const ucn_route_policy_entry_t *wildcard = NULL;

    if (state == NULL || !policy_node_id_is_valid(destination) ||
        !ucn_endpoint_is_static(endpoint) ||
        !policy_traffic_class_is_valid((uint8_t)traffic_class)) {
        return NULL;
    }
    for (index = 0U; index < UCN_MAX_ROUTE_POLICIES; ++index) {
        const ucn_route_policy_entry_t *entry = &state->policies[index];

        if (!entry->occupied || entry->config.key.destination != destination ||
            entry->config.key.endpoint != endpoint) {
            continue;
        }
        if (entry->config.key.traffic_class == (uint8_t)traffic_class) {
            return entry;
        }
        if (entry->config.key.traffic_class == UCN_POLICY_ANY_TRAFFIC_CLASS) {
            wildcard = entry;
        }
    }
    return wildcard;
}

static ucn_policy_path_entry_t *find_policy_path_entry(ucn_policy_state_t *state,
                                                        uint16_t local_path_id)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_POLICY_PATHS; ++index) {
        if (state->paths[index].occupied &&
            state->paths[index].local_path_id == local_path_id) {
            return &state->paths[index];
        }
    }
    return NULL;
}

static const ucn_policy_path_entry_t *find_policy_path_entry_const(
    const ucn_policy_state_t *state,
    uint16_t local_path_id)
{
    return find_policy_path_entry((ucn_policy_state_t *)state, local_path_id);
}

static ucn_policy_flow_binding_t *find_flow_binding(ucn_policy_state_t *state,
                                                     const ucn_policy_flow_key_t *key)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
        if (state->flows[index].occupied &&
            policy_flow_key_equal(&state->flows[index].key, key)) {
            return &state->flows[index];
        }
    }
    return NULL;
}

static ucn_policy_link_quality_snapshot_t *find_quality_snapshot(
    ucn_policy_state_t *state,
    const ucn_link_t *link)
{
    size_t index;

    for (index = 0U; index < UCN_MAX_LINKS; ++index) {
        if (state->quality[index].occupied && state->quality[index].link == link) {
            return &state->quality[index];
        }
    }
    return NULL;
}

static ucn_policy_link_quality_snapshot_t *find_or_allocate_quality_snapshot(
    ucn_policy_state_t *state,
    ucn_link_t *link)
{
    ucn_policy_link_quality_snapshot_t *entry = find_quality_snapshot(state, link);
    size_t index;

    if (entry != NULL) {
        return entry;
    }
    for (index = 0U; index < UCN_MAX_LINKS; ++index) {
        if (!state->quality[index].occupied) {
            entry = &state->quality[index];
            (void)memset(entry, 0, sizeof(*entry));
            entry->occupied = true;
            entry->link = link;
            return entry;
        }
    }
    return NULL;
}

static uint16_t policy_ewma(uint16_t previous, uint16_t sample)
{
    const uint32_t alpha = UCN_POLICY_QUALITY_EWMA_ALPHA_PERCENT;
    const uint32_t value = (uint32_t)previous * (100U - alpha) +
                           (uint32_t)sample * alpha;

    return (uint16_t)((value + 50U) / 100U);
}

static void update_optional_ewma(bool *valid,
                                 uint16_t *value,
                                 bool sample_valid,
                                 uint16_t sample)
{
    if (!sample_valid) {
        *valid = false;
        *value = 0U;
        return;
    }
    if (!*valid) {
        *valid = true;
        *value = sample;
        return;
    }
    *value = policy_ewma(*value, sample);
}

void ucn_policy_refresh_link_quality(ucn_policy_state_t *state,
                                     ucn_link_t *const *links,
                                     size_t link_count,
                                     uint32_t now_ms)
{
    size_t index;

    if (state == NULL || links == NULL || link_count > UCN_MAX_LINKS) {
        return;
    }
    if (state->quality_sampled &&
        (uint32_t)(now_ms - state->last_quality_sample_ms) <
            UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS) {
        return;
    }

    state->quality_sampled = true;
    state->last_quality_sample_ms = now_ms;

    for (index = 0U; index < UCN_MAX_LINKS; ++index) {
        ucn_policy_link_quality_snapshot_t *snapshot = &state->quality[index];

        if (snapshot->occupied &&
            !policy_link_is_present(links, link_count, snapshot->link)) {
            (void)memset(snapshot, 0, sizeof(*snapshot));
        }
    }

    for (index = 0U; index < UCN_MAX_POLICY_PATHS; ++index) {
        ucn_policy_path_entry_t *path = &state->paths[index];

        if (path->occupied &&
            !policy_link_is_present(links, link_count, path->egress_link)) {
            path->state = UCN_POLICY_PATH_DOWN;
        }
    }

    for (index = 0U; index < link_count; ++index) {
        ucn_link_t *link = links[index];
        ucn_policy_link_quality_snapshot_t *snapshot;
        ucn_link_status_t status;
        ucn_link_metrics_t metrics;
        ucn_result_t result;

        if (link == NULL || link->ops == NULL || link->ops->get_status == NULL) {
            state->stats.quality_metrics_unavailable++;
            continue;
        }
        snapshot = find_or_allocate_quality_snapshot(state, link);
        if (snapshot == NULL) {
            state->stats.quality_metrics_unavailable++;
            continue;
        }

        (void)memset(&status, 0, sizeof(status));
        result = link->ops->get_status(link, &status);
        snapshot->is_up = result == UCN_OK && status.is_up;
        snapshot->sampled_at_ms = now_ms;
        if (!snapshot->is_up) {
            state->stats.quality_link_down++;
        }

        (void)memset(&metrics, 0, sizeof(metrics));
        if (link->ops->get_metrics == NULL ||
            link->ops->get_metrics(link, &metrics) != UCN_OK) {
            snapshot->route_cost_valid = false;
            snapshot->route_cost = 0U;
            update_optional_ewma(&snapshot->rtt_valid, &snapshot->rtt_ewma_ms,
                                 false, 0U);
            update_optional_ewma(&snapshot->tx_failure_rate_valid,
                                 &snapshot->tx_failure_ewma_per_mille,
                                 false, 0U);
            update_optional_ewma(&snapshot->queue_pressure_valid,
                                 &snapshot->queue_pressure_ewma_per_mille,
                                 false, 0U);
            state->stats.quality_metrics_unavailable++;
            continue;
        }

        update_optional_ewma(&snapshot->route_cost_valid, &snapshot->route_cost,
                             metrics.route_cost_valid && metrics.route_cost != 0U &&
                                 metrics.route_cost != UCN_LINK_ROUTE_COST_UNKNOWN,
                             metrics.route_cost);
        update_optional_ewma(&snapshot->rtt_valid, &snapshot->rtt_ewma_ms,
                             metrics.rtt_valid, metrics.rtt_ms);
        update_optional_ewma(&snapshot->tx_failure_rate_valid,
                             &snapshot->tx_failure_ewma_per_mille,
                             metrics.tx_failure_rate_valid &&
                                 metrics.tx_failure_per_mille <=
                                     UCN_LINK_METRIC_PER_MILLE_MAX,
                             metrics.tx_failure_per_mille);
        update_optional_ewma(&snapshot->queue_pressure_valid,
                             &snapshot->queue_pressure_ewma_per_mille,
                             metrics.queue_pressure_valid &&
                                 metrics.queue_pressure_per_mille <=
                                     UCN_LINK_METRIC_PER_MILLE_MAX,
                             metrics.queue_pressure_per_mille);
        state->stats.quality_samples++;
    }

    for (index = 0U; index < UCN_MAX_POLICY_PATHS; ++index) {
        ucn_policy_path_entry_t *path = &state->paths[index];
        ucn_policy_link_quality_snapshot_t *snapshot;

        if (!path->occupied) {
            continue;
        }
        snapshot = find_quality_snapshot(state, path->egress_link);
        if (snapshot == NULL || !snapshot->is_up ||
            !snapshot->queue_pressure_valid ||
            snapshot->queue_pressure_ewma_per_mille <
                UCN_POLICY_BALANCE_QUEUE_PRESSURE_THRESHOLD_PER_MILLE) {
            path->congestion_samples = 0U;
        } else if (path->congestion_samples <
                   UCN_POLICY_BALANCE_CONGESTED_SAMPLE_LIMIT) {
            path->congestion_samples++;
        }
        if (snapshot != NULL && !snapshot->is_up) {
            path->state = UCN_POLICY_PATH_DOWN;
        }
    }
}

void ucn_policy_expire_flows(ucn_policy_state_t *state, uint32_t now_ms)
{
    size_t index;

    if (state == NULL) {
        return;
    }
    for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
        if (state->flows[index].occupied &&
            ucn_deadline_expired(now_ms, state->flows[index].expires_at_ms)) {
            (void)memset(&state->flows[index], 0, sizeof(state->flows[index]));
            state->stats.flow_bindings_expired++;
        }
    }
}

ucn_result_t ucn_node_set_route_policy(ucn_node_t *node,
                                       const ucn_route_policy_config_t *config)
{
    ucn_route_policy_entry_t *entry;
    size_t index;

    if (node == NULL || config == NULL || !policy_key_is_valid(&config->key) ||
        config->mode > UCN_ROUTE_POLICY_AUTO_BALANCE) {
        return UCN_ERR_ARGUMENT;
    }
    if (config->primary_local_path_id != 0U &&
        config->primary_local_path_id == config->backup_local_path_id) {
        return UCN_ERR_ARGUMENT;
    }
    if (config->mode == UCN_ROUTE_POLICY_PINNED_STRICT &&
        config->allow_discovery_on_hard_failure) {
        return UCN_ERR_ARGUMENT;
    }
    if (config->mode == UCN_ROUTE_POLICY_AUTO_BALANCE &&
        config->key.traffic_class != (uint8_t)UCN_TRAFFIC_Q1_REALTIME) {
        return UCN_ERR_UNSUPPORTED;
    }
    if (config->mode == UCN_ROUTE_POLICY_AUTO_BALANCE) {
        if (config->primary_local_path_id == 0U ||
            config->allow_discovery_on_hard_failure ||
            (config->balance_flow_lease_ms != 0U &&
             !ucn_duration_is_valid(config->balance_flow_lease_ms))) {
            return UCN_ERR_ARGUMENT;
        }
    } else if (config->balance_flow_lease_ms != 0U) {
        return UCN_ERR_ARGUMENT;
    }

    entry = find_route_policy_entry(&node->policy_state, &config->key);
    if (entry == NULL) {
        for (index = 0U; index < UCN_MAX_ROUTE_POLICIES; ++index) {
            if (!node->policy_state.policies[index].occupied) {
                entry = &node->policy_state.policies[index];
                break;
            }
        }
    }
    if (entry == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    /* A set operation establishes a new diagnostic baseline for this rule.
     * Otherwise an old mode/Path pair could leak its hit count into a changed
     * Policy record with the same lookup key. */
    entry->match_hits = 0U;
    entry->occupied = true;
    entry->config = *config;
    node->policy_state.stats.policy_config_updates++;
    return UCN_OK;
}

ucn_result_t ucn_node_clear_route_policy(ucn_node_t *node,
                                         const ucn_route_policy_key_t *key)
{
    ucn_route_policy_entry_t *entry;

    if (node == NULL || !policy_key_is_valid(key)) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_route_policy_entry(&node->policy_state, key);
    if (entry == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    (void)memset(entry, 0, sizeof(*entry));
    return UCN_OK;
}

const ucn_route_policy_entry_t *ucn_node_find_route_policy(
    const ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class)
{
    if (node == NULL) {
        return NULL;
    }
    return find_route_policy_match(&node->policy_state, destination, endpoint,
                                   traffic_class);
}

ucn_result_t ucn_node_set_policy_path(ucn_node_t *node,
                                      const ucn_policy_path_config_t *config)
{
    ucn_policy_path_entry_t *entry;
    size_t index;

    if (node == NULL || config == NULL || config->local_path_id == 0U ||
        !policy_node_id_is_valid(config->destination) || config->egress_link == NULL ||
        !policy_link_is_registered(node, config->egress_link)) {
        return UCN_ERR_ARGUMENT;
    }

    entry = find_policy_path_entry(&node->policy_state, config->local_path_id);
    if (entry == NULL) {
        for (index = 0U; index < UCN_MAX_POLICY_PATHS; ++index) {
            if (!node->policy_state.paths[index].occupied) {
                entry = &node->policy_state.paths[index];
                break;
            }
        }
    }
    if (entry == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    if (config->verified && config->wire_path_id != 0U) {
        const ucn_path_forward_entry_t *wire_path =
            ucn_path_find(&node->path_state, node->config.node_id,
                          node->session_id, config->wire_path_id,
                          config->destination);

        if (wire_path == NULL || ucn_path_is_expired(wire_path, node->now_ms) ||
            wire_path->terminal || wire_path->egress_link != config->egress_link) {
            return UCN_ERR_CONFIG;
        }
    }

    entry->occupied = true;
    entry->local_path_id = config->local_path_id;
    entry->wire_path_id = config->wire_path_id;
    entry->destination = config->destination;
    entry->egress_link = config->egress_link;
    entry->state = config->verified ? UCN_POLICY_PATH_VERIFIED :
                                      UCN_POLICY_PATH_CANDIDATE;
    entry->congestion_samples = 0U;
    entry->configured_at_ms = node->now_ms;
    node->policy_state.stats.path_config_updates++;
    return UCN_OK;
}

ucn_result_t ucn_node_clear_policy_path(ucn_node_t *node,
                                        uint16_t local_path_id)
{
    ucn_policy_path_entry_t *entry;
    size_t index;

    if (node == NULL || local_path_id == 0U) {
        return UCN_ERR_ARGUMENT;
    }
    entry = find_policy_path_entry(&node->policy_state, local_path_id);
    if (entry == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    (void)memset(entry, 0, sizeof(*entry));
    for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
        if (node->policy_state.flows[index].occupied &&
            node->policy_state.flows[index].local_path_id == local_path_id) {
            (void)memset(&node->policy_state.flows[index], 0,
                         sizeof(node->policy_state.flows[index]));
        }
    }
    return UCN_OK;
}

const ucn_policy_path_entry_t *ucn_node_find_policy_path(
    const ucn_node_t *node,
    uint16_t local_path_id)
{
    if (node == NULL || local_path_id == 0U) {
        return NULL;
    }
    return find_policy_path_entry_const(&node->policy_state, local_path_id);
}

ucn_result_t ucn_node_bind_q1_flow(ucn_node_t *node,
                                   ucn_node_id_t destination,
                                   ucn_endpoint_t endpoint,
                                   uint16_t local_path_id,
                                   uint32_t lease_ms)
{
    const ucn_policy_path_entry_t *path;
    ucn_policy_flow_key_t key;
    ucn_policy_flow_binding_t *binding;
    size_t index;

    if (node == NULL || !policy_node_id_is_valid(destination) ||
        !ucn_endpoint_is_static(endpoint) || local_path_id == 0U ||
        !ucn_duration_is_valid(lease_ms)) {
        return UCN_ERR_ARGUMENT;
    }
    ucn_policy_expire_flows(&node->policy_state, node->now_ms);
    path = ucn_node_find_policy_path(node, local_path_id);
    if (path == NULL || path->destination != destination ||
        path->state != UCN_POLICY_PATH_VERIFIED) {
        return UCN_ERR_CONFIG;
    }

    key.destination = destination;
    key.endpoint = endpoint;
    key.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    binding = find_flow_binding(&node->policy_state, &key);
    if (binding == NULL) {
        for (index = 0U; index < UCN_MAX_POLICY_FLOWS; ++index) {
            if (!node->policy_state.flows[index].occupied) {
                binding = &node->policy_state.flows[index];
                node->policy_state.stats.flow_bindings_created++;
                break;
            }
        }
    }
    if (binding == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    binding->occupied = true;
    binding->key = key;
    binding->local_path_id = local_path_id;
    binding->expires_at_ms = ucn_deadline_from_now(node->now_ms, lease_ms);
    binding->last_used_at_ms = node->now_ms;
    return UCN_OK;
}

const ucn_policy_flow_binding_t *ucn_node_find_q1_flow(
    const ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint)
{
    ucn_policy_flow_key_t key;
    const ucn_policy_flow_binding_t *binding;

    if (node == NULL || !policy_node_id_is_valid(destination) ||
        !ucn_endpoint_is_static(endpoint)) {
        return NULL;
    }
    key.destination = destination;
    key.endpoint = endpoint;
    key.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    binding = find_flow_binding((ucn_policy_state_t *)&node->policy_state, &key);
    if (binding != NULL &&
        ucn_deadline_expired(node->now_ms, binding->expires_at_ms)) {
        return NULL;
    }
    return binding;
}

const ucn_policy_link_quality_snapshot_t *ucn_node_get_link_quality(
    const ucn_node_t *node,
    const ucn_link_t *link)
{
    if (node == NULL || link == NULL) {
        return NULL;
    }
    return find_quality_snapshot((ucn_policy_state_t *)&node->policy_state, link);
}

void ucn_policy_mark_path_down(ucn_policy_state_t *state,
                               uint16_t local_path_id)
{
    ucn_policy_path_entry_t *entry;

    if (state == NULL || local_path_id == 0U) {
        return;
    }
    entry = find_policy_path_entry(state, local_path_id);
    if (entry != NULL) {
        entry->state = UCN_POLICY_PATH_DOWN;
    }
}

void ucn_policy_touch_q1_flow(ucn_policy_state_t *state,
                              ucn_node_id_t destination,
                              ucn_endpoint_t endpoint,
                              uint32_t now_ms)
{
    ucn_policy_flow_key_t key;
    ucn_policy_flow_binding_t *binding;

    if (state == NULL || !policy_node_id_is_valid(destination) ||
        !ucn_endpoint_is_static(endpoint)) {
        return;
    }
    key.destination = destination;
    key.endpoint = endpoint;
    key.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    binding = find_flow_binding(state, &key);
    if (binding != NULL) {
        binding->last_used_at_ms = now_ms;
    }
}

const ucn_policy_stats_t *ucn_node_get_policy_stats(const ucn_node_t *node)
{
    return node == NULL ? NULL : &node->policy_state.stats;
}
