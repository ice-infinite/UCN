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
    return ucn_link_cost_ewma_update(previous, sample);
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

static void clear_quality_metrics(ucn_policy_link_quality_snapshot_t *snapshot)
{
    snapshot->route_cost_valid = false;
    snapshot->route_cost = 0U;
    update_optional_ewma(&snapshot->rtt_valid, &snapshot->rtt_ewma_ms,
                         false, 0U);
    update_optional_ewma(&snapshot->tx_failure_rate_valid,
                         &snapshot->tx_failure_ewma_per_mille, false, 0U);
    update_optional_ewma(&snapshot->queue_pressure_valid,
                         &snapshot->queue_pressure_ewma_per_mille, false, 0U);
    update_optional_ewma(&snapshot->rx_failure_rate_valid,
                         &snapshot->rx_failure_ewma_per_mille, false, 0U);
    update_optional_ewma(&snapshot->medium_busy_valid,
                         &snapshot->medium_busy_ewma_per_mille, false, 0U);
    update_optional_ewma(&snapshot->medium_quality_valid,
                         &snapshot->medium_quality_ewma_per_mille, false, 0U);
    snapshot->metrics_timestamp_valid = false;
    snapshot->metrics_timestamp_ms = 0U;
    snapshot->rtt_reference_valid = false;
    snapshot->rtt_reference_ms = 0U;
    snapshot->administrative_bias = 0;
    snapshot->adapter_bad_metric_count = 0U;
}

static void resolve_quality_cost(ucn_policy_state_t *state,
                                 ucn_policy_link_quality_snapshot_t *snapshot,
                                 const ucn_link_status_t *status,
                                 bool medium_metrics_share_source,
                                 uint32_t now_ms)
{
    ucn_link_cost_input_t input;
    ucn_result_t result;

    (void)memset(&input, 0, sizeof(input));
    input.link_up = snapshot->is_up;
    input.mtu_sufficient = status != NULL &&
                           ucn_link_effective_mtu(snapshot->link, status) != 0U;
    input.capability_allowed = true;
    input.base_cost_valid = snapshot->route_cost_valid;
    input.base_cost = snapshot->route_cost;
    input.rtt_reference_valid = snapshot->rtt_reference_valid;
    input.rtt_reference_ms = snapshot->rtt_reference_ms;
    input.administrative_bias = snapshot->administrative_bias;
    input.queue_pressure_valid = snapshot->queue_pressure_valid;
    input.queue_pressure_per_mille = snapshot->queue_pressure_ewma_per_mille;
    input.tx_failure_rate_valid = snapshot->tx_failure_rate_valid;
    input.tx_failure_per_mille = snapshot->tx_failure_ewma_per_mille;
    input.rx_failure_rate_valid = snapshot->rx_failure_rate_valid;
    input.rx_failure_per_mille = snapshot->rx_failure_ewma_per_mille;
    input.rtt_valid = snapshot->rtt_valid;
    input.rtt_ms = snapshot->rtt_ewma_ms;
    input.medium_busy_valid = snapshot->medium_busy_valid;
    input.medium_busy_per_mille = snapshot->medium_busy_ewma_per_mille;
    input.medium_quality_valid = snapshot->medium_quality_valid;
    input.medium_quality_per_mille = snapshot->medium_quality_ewma_per_mille;
    input.medium_metrics_share_source = medium_metrics_share_source;
    input.metrics_timestamp_valid = snapshot->metrics_timestamp_valid;
    input.metrics_timestamp_ms = snapshot->metrics_timestamp_ms;
    input.now_ms = now_ms;
    result = ucn_link_cost_resolve(&input, &snapshot->cost);
    if (result != UCN_OK) {
        snapshot->cost.selectable = false;
        snapshot->rejected_metric_count++;
        state->stats.quality_bad_metrics++;
    }
    if (snapshot->cost.exclusion == UCN_LINK_COST_EXCLUSION_METRICS_STALE) {
        state->stats.quality_metrics_stale++;
    }
}

bool ucn_policy_refresh_link_quality(ucn_policy_state_t *state,
                                     ucn_link_t *const *links,
                                     size_t link_count,
                                     uint32_t now_ms)
{
    size_t index;

    if (state == NULL || links == NULL || link_count > UCN_MAX_LINKS) {
        return false;
    }
    if (state->quality_sampled &&
        (uint32_t)(now_ms - state->last_quality_sample_ms) <
            UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS) {
        return false;
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

    for (index = 0U; index < link_count; ++index) {
        ucn_link_t *link = links[index];
        ucn_policy_link_quality_snapshot_t *snapshot;
        ucn_link_status_t status;
        ucn_link_metrics_t metrics;
        ucn_result_t result;
        bool metrics_available;
        bool medium_metrics_share_source = false;
        uint32_t rejected_this_sample = 0U;

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
        metrics_available = link->ops->get_metrics != NULL &&
            link->ops->get_metrics(link, &metrics) == UCN_OK;
        if (!metrics_available) {
            clear_quality_metrics(snapshot);
            state->stats.quality_metrics_unavailable++;
            resolve_quality_cost(state, snapshot, &status, false, now_ms);
            continue;
        }

        snapshot->route_cost_valid = metrics.route_cost_valid &&
                                     metrics.route_cost != 0U &&
                                     metrics.route_cost !=
                                         UCN_LINK_ROUTE_COST_UNKNOWN;
        snapshot->route_cost = snapshot->route_cost_valid ?
                                   metrics.route_cost : 0U;
        if (metrics.route_cost_valid && !snapshot->route_cost_valid) {
            rejected_this_sample++;
        }
        update_optional_ewma(&snapshot->rtt_valid, &snapshot->rtt_ewma_ms,
                             metrics.rtt_valid, metrics.rtt_ms);
        update_optional_ewma(&snapshot->tx_failure_rate_valid,
                             &snapshot->tx_failure_ewma_per_mille,
                             metrics.tx_failure_rate_valid &&
                                 metrics.tx_failure_per_mille <=
                                     UCN_LINK_METRIC_PER_MILLE_MAX,
                             metrics.tx_failure_per_mille);
        if (metrics.tx_failure_rate_valid &&
            metrics.tx_failure_per_mille > UCN_LINK_METRIC_PER_MILLE_MAX) {
            rejected_this_sample++;
        }
        update_optional_ewma(&snapshot->queue_pressure_valid,
                             &snapshot->queue_pressure_ewma_per_mille,
                             metrics.queue_pressure_valid &&
                                 metrics.queue_pressure_per_mille <=
                                     UCN_LINK_METRIC_PER_MILLE_MAX,
                             metrics.queue_pressure_per_mille);
        if (metrics.queue_pressure_valid &&
            metrics.queue_pressure_per_mille > UCN_LINK_METRIC_PER_MILLE_MAX) {
            rejected_this_sample++;
        }
        update_optional_ewma(&snapshot->rx_failure_rate_valid,
                             &snapshot->rx_failure_ewma_per_mille,
                             metrics.rx_failure_rate_valid &&
                                 metrics.rx_failure_per_mille <=
                                     UCN_LINK_METRIC_PER_MILLE_MAX,
                             metrics.rx_failure_per_mille);
        if (metrics.rx_failure_rate_valid &&
            metrics.rx_failure_per_mille > UCN_LINK_METRIC_PER_MILLE_MAX) {
            rejected_this_sample++;
        }
        update_optional_ewma(&snapshot->medium_busy_valid,
                             &snapshot->medium_busy_ewma_per_mille,
                             metrics.medium_busy_valid &&
                                 metrics.medium_busy_per_mille <=
                                     UCN_LINK_METRIC_PER_MILLE_MAX,
                             metrics.medium_busy_per_mille);
        if (metrics.medium_busy_valid &&
            metrics.medium_busy_per_mille > UCN_LINK_METRIC_PER_MILLE_MAX) {
            rejected_this_sample++;
        }
        update_optional_ewma(&snapshot->medium_quality_valid,
                             &snapshot->medium_quality_ewma_per_mille,
                             metrics.medium_quality_valid &&
                                 metrics.medium_quality_per_mille <=
                                     UCN_LINK_METRIC_PER_MILLE_MAX,
                             metrics.medium_quality_per_mille);
        if (metrics.medium_quality_valid &&
            metrics.medium_quality_per_mille > UCN_LINK_METRIC_PER_MILLE_MAX) {
            rejected_this_sample++;
        }
        medium_metrics_share_source = metrics.medium_metrics_share_source;
        if (metrics.medium_busy_valid && metrics.medium_quality_valid &&
            medium_metrics_share_source) {
            rejected_this_sample++;
        }
        snapshot->metrics_timestamp_valid = metrics.metrics_timestamp_valid;
        snapshot->metrics_timestamp_ms = metrics.metrics_timestamp_valid ?
                                             metrics.metrics_timestamp_ms : now_ms;
        snapshot->rtt_reference_valid = metrics.rtt_reference_valid &&
                                        metrics.rtt_reference_ms != 0U;
        snapshot->rtt_reference_ms = snapshot->rtt_reference_valid ?
                                         metrics.rtt_reference_ms : 0U;
        snapshot->administrative_bias = metrics.administrative_bias_valid ?
                                            metrics.administrative_bias : 0;
        snapshot->adapter_bad_metric_count = metrics.bad_metric_count;
        snapshot->rejected_metric_count += rejected_this_sample;
        state->stats.quality_bad_metrics += rejected_this_sample;
        resolve_quality_cost(state, snapshot, &status,
                             medium_metrics_share_source, now_ms);
        state->stats.quality_samples++;
    }

    return true;
}

/* Policy Paths are logical forwarding objects.  Their configured Link is a
 * stable binding key, while liveness and congestion must follow the physical
 * Bearer selected for that logical next hop at this sample. */
void ucn_policy_refresh_path_egress(ucn_policy_state_t *state,
                                    uint16_t local_path_id,
                                    ucn_link_t *active_egress_link,
                                    bool path_available)
{
    ucn_policy_path_entry_t *path;
    ucn_policy_link_quality_snapshot_t *snapshot;

    if (state == NULL || local_path_id == 0U) {
        return;
    }
    path = find_policy_path_entry(state, local_path_id);
    if (path == NULL) {
        return;
    }
    snapshot = path_available && active_egress_link != NULL ?
        find_quality_snapshot(state, active_egress_link) : NULL;
    if (snapshot == NULL || !snapshot->is_up) {
        path->congestion_samples = 0U;
        path->state = UCN_POLICY_PATH_DOWN;
        return;
    }
    if (path->state == UCN_POLICY_PATH_DOWN) {
        path->state = UCN_POLICY_PATH_VERIFIED;
    }
    if (!snapshot->queue_pressure_valid ||
        snapshot->queue_pressure_ewma_per_mille <
            UCN_POLICY_BALANCE_QUEUE_PRESSURE_THRESHOLD_PER_MILLE) {
        path->congestion_samples = 0U;
    } else if (path->congestion_samples <
               UCN_POLICY_BALANCE_CONGESTED_SAMPLE_LIMIT) {
        path->congestion_samples++;
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
    if (config->constraints.max_hops > node->config.default_hop_limit ||
        config->constraints.max_route_cost == UCN_ROUTE_COST_UNKNOWN) {
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
        !policy_link_is_registered(node, config->egress_link) ||
        (config->route_cost_valid &&
         (config->route_cost == 0U ||
          config->route_cost == UCN_ROUTE_COST_UNKNOWN))) {
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
    entry->route_cost_valid = config->route_cost_valid;
    entry->route_cost = config->route_cost;
    entry->verified_rtt_valid = config->verified_rtt_valid;
    entry->verified_rtt_ms = config->verified_rtt_ms;
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
