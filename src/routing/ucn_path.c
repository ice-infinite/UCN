#include <string.h>

#include "ucn/ucn_frame.h"
#include "ucn/ucn_path.h"
#include "ucn/ucn_time.h"

/*
 * EN: Checks the `identity_matches` condition against current Path forwarding state.
 * 中文：根据当前 Path 转发 状态检查 `identity_matches` 条件。
 */
static bool path_identity_matches(const ucn_path_forward_entry_t *entry,
                                  ucn_node_id_t owner,
                                  ucn_session_id_t owner_session_id,
                                  ucn_path_id_t path_id)
{
    return entry->occupied && entry->owner == owner &&
           entry->owner_session_id == owner_session_id &&
           entry->path_id == path_id;
}

/*
 * EN: Checks the `is_expired` predicate against current Path forwarding state.
 * 中文：根据当前 Path 转发 状态检查 `is_expired` 条件。
 */
bool ucn_path_is_expired(const ucn_path_forward_entry_t *entry,
                         uint32_t now_ms)
{
    return entry == NULL || !entry->occupied ||
           ucn_deadline_expired(now_ms, entry->expires_at_ms);
}

/*
 * EN: Looks up `find` in bounded Path forwarding state without allocation.
 * 中文：在固定容量的 Path 转发 状态中查找 `find`，且不进行动态分配。
 */
const ucn_path_forward_entry_t *ucn_path_find(
    const ucn_path_state_t *state,
    ucn_node_id_t owner,
    ucn_session_id_t owner_session_id,
    ucn_path_id_t path_id,
    ucn_node_id_t destination)
{
    size_t index;

    if (state == NULL || owner == 0U || owner == UCN_NODE_BROADCAST ||
        owner_session_id == 0U || path_id == 0U || destination == 0U ||
        destination == UCN_NODE_BROADCAST) {
        return NULL;
    }
    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        const ucn_path_forward_entry_t *entry = &state->entries[index];

        if (path_identity_matches(entry, owner, owner_session_id, path_id) &&
            entry->destination == destination) {
            return entry;
        }
    }
    return NULL;
}

/*
 * EN: Validates and installs `install_capable` into bounded Path forwarding state.
 * 中文：验证 `install_capable` 并将其安装到固定容量的 Path 转发 状态中。
 */
ucn_result_t ucn_path_install_capable(
    ucn_path_state_t *state,
    const ucn_path_forward_config_t *config,
    const ucn_path_capability_t *capability)
{
    ucn_path_forward_entry_t *free_entry = NULL;
    size_t index;

    if (state == NULL || config == NULL || config->owner == 0U ||
        config->owner == UCN_NODE_BROADCAST || config->owner_session_id == 0U ||
        config->path_id == 0U || config->destination == 0U ||
        config->destination == UCN_NODE_BROADCAST || config->expires_at_ms == 0U ||
        (config->next_hop == 0U && config->remaining_hops != 0U) ||
        (config->next_hop != 0U && config->remaining_hops == 0U) ||
        (config->next_hop == 0U && config->egress_link != NULL) ||
        (config->next_hop != 0U && config->egress_link == NULL) ||
        (capability != NULL &&
         (config->next_hop == 0U || capability->minimum_mtu == 0U ||
         ucn_wire_profile_get_descriptor(
             capability->maximum_wire_profile) == NULL))) {
        return UCN_ERR_ARGUMENT;
    }

    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        ucn_path_forward_entry_t *entry = &state->entries[index];

        if (path_identity_matches(entry, config->owner, config->owner_session_id,
                                  config->path_id)) {
            if (entry->destination != config->destination) {
                return UCN_ERR_ACCESS;
            }
            entry->next_hop = config->next_hop;
            entry->remaining_hops = config->remaining_hops;
            entry->maximum_wire_profile = capability == NULL ?
                (uint8_t)UCN_WIRE_PROFILE_UNSPECIFIED :
                (uint8_t)capability->maximum_wire_profile;
            entry->minimum_mtu = capability == NULL ? 0U :
                capability->minimum_mtu;
            entry->egress_link = config->egress_link;
            entry->terminal = config->next_hop == 0U;
            entry->expires_at_ms = config->expires_at_ms;
            state->stats.installs++;
            return UCN_OK;
        }
        if (!entry->occupied && free_entry == NULL) {
            free_entry = entry;
        }
    }
    if (free_entry == NULL) {
        return UCN_ERR_NO_SPACE;
    }

    (void)memset(free_entry, 0, sizeof(*free_entry));
    free_entry->occupied = true;
    free_entry->owner = config->owner;
    free_entry->owner_session_id = config->owner_session_id;
    free_entry->path_id = config->path_id;
    free_entry->destination = config->destination;
    free_entry->next_hop = config->next_hop;
    free_entry->remaining_hops = config->remaining_hops;
    free_entry->maximum_wire_profile = capability == NULL ?
        (uint8_t)UCN_WIRE_PROFILE_UNSPECIFIED :
        (uint8_t)capability->maximum_wire_profile;
    free_entry->minimum_mtu = capability == NULL ? 0U :
        capability->minimum_mtu;
    free_entry->egress_link = config->egress_link;
    free_entry->terminal = config->next_hop == 0U;
    free_entry->expires_at_ms = config->expires_at_ms;
    state->stats.installs++;
    return UCN_OK;
}

/*
 * EN: Validates and installs `install` in bounded Path forwarding state.
 * 中文：验证 `install` 并将其安装到固定容量的 Path 转发 状态中。
 */
ucn_result_t ucn_path_install(ucn_path_state_t *state,
                              const ucn_path_forward_config_t *config)
{
    return ucn_path_install_capable(state, config, NULL);
}

/*
 * EN: Clears or releases `revoke` from bounded Path forwarding state.
 * 中文：从固定容量的 Path 转发 状态中清除或释放 `revoke`。
 */
ucn_result_t ucn_path_revoke(ucn_path_state_t *state,
                             ucn_node_id_t owner,
                             ucn_session_id_t owner_session_id,
                             ucn_path_id_t path_id,
                             ucn_node_id_t destination)
{
    size_t index;

    if (state == NULL || owner == 0U || owner == UCN_NODE_BROADCAST ||
        owner_session_id == 0U || path_id == 0U || destination == 0U ||
        destination == UCN_NODE_BROADCAST) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        ucn_path_forward_entry_t *entry = &state->entries[index];

        if (path_identity_matches(entry, owner, owner_session_id, path_id) &&
            entry->destination == destination) {
            (void)memset(entry, 0, sizeof(*entry));
            state->stats.revokes++;
            return UCN_OK;
        }
    }
    return UCN_ERR_NOT_FOUND;
}

/*
 * EN: Checks or removes expired `expire` state in Path forwarding.
 * 中文：检查或移除 Path 转发 中已过期的 `expire` 状态。
 */
void ucn_path_expire(ucn_path_state_t *state, uint32_t now_ms)
{
    size_t index;

    if (state == NULL) {
        return;
    }
    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        ucn_path_forward_entry_t *entry = &state->entries[index];

        if (ucn_path_is_expired(entry, now_ms)) {
            if (entry->occupied) {
                (void)memset(entry, 0, sizeof(*entry));
                state->stats.expired++;
            }
        }
    }
}
