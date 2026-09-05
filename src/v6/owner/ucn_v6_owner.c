#include "../internal/ucn_v6_owner_private.h"

#include <string.h>

typedef char ucn_v6_stack_owner_storage_size_check[
    sizeof(ucn_v6_stack_owner_t) <= UCN_V6_STACK_OWNER_STORAGE_BYTES ? 1 : -1];

static bool event_is_valid(ucn_v6_owner_event_t event)
{
    return event >= UCN_V6_OWNER_EVENT_RX &&
           event <= UCN_V6_OWNER_EVENT_PROVIDER;
}

static bool lock_ops_are_valid(const ucn_v6_owner_lock_ops_t *ops)
{
    return ops != NULL && ops->lock_task != NULL &&
           ops->try_lock_from_isr != NULL && ops->unlock_task != NULL &&
           ops->unlock_from_isr != NULL && ops->notify != NULL;
}

static bool mailbox_is_valid(const ucn_v6_owner_mailbox_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_OWNER_MAILBOX_MAGIC &&
           owner->schema == UCN_V6_STORAGE_LAYOUT &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->canary == UCN_V6_OWNER_MAILBOX_CANARY &&
           owner->event_depth != 0U;
}

/* The caller holds the mailbox lock.  A coalesced event is a latch, so every
 * per-event value is canonical 0/1 and pending_total counts set latches. */
static bool mailbox_state_is_valid_locked(
    const ucn_v6_owner_mailbox_t *owner)
{
    uint16_t pending_total = 0U;
    size_t index;

    if (!mailbox_is_valid(owner) ||
        owner->next_event_index >= UCN_V6_OWNER_EVENT_COUNT) {
        return false;
    }
    for (index = 0U; index < UCN_V6_OWNER_EVENT_COUNT; ++index) {
        if (owner->pending_by_event[index] > 1U) {
            return false;
        }
        pending_total = (uint16_t)(pending_total +
                                   owner->pending_by_event[index]);
    }
    return pending_total == owner->pending_total &&
           pending_total <= owner->event_depth;
}

static ucn_v6_result_t mailbox_init(
    ucn_v6_owner_mailbox_t *owner,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_owner_lock_ops_t *lock_ops)
{
    ucn_v6_result_t result;

    if (owner == NULL || !lock_ops_are_valid(lock_ops)) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_manifest_validate_exact(manifest);
    if (result != UCN_V6_OK) {
        return result;
    }
    lock_ops->lock_task(lock_ops->context);
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_V6_OWNER_MAILBOX_MAGIC;
    owner->schema = UCN_V6_STORAGE_LAYOUT;
    owner->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    owner->lock_ops = *lock_ops;
    owner->event_depth = manifest->owner_event_depth;
    owner->canary = UCN_V6_OWNER_MAILBOX_CANARY;
    lock_ops->unlock_task(lock_ops->context);
    return UCN_V6_OK;
}

static ucn_v6_result_t mailbox_post(
    ucn_v6_owner_mailbox_t *owner,
    ucn_v6_owner_event_t event,
    bool from_isr)
{
    size_t index;
    ucn_v6_owner_lock_ops_t ops;

    if (owner == NULL || !event_is_valid(event)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!mailbox_is_valid(owner)) {
        return UCN_V6_ERR_STATE;
    }
    ops = owner->lock_ops;
    if (!lock_ops_are_valid(&ops)) {
        return UCN_V6_ERR_STATE;
    }
    if (from_isr) {
        if (!ops.try_lock_from_isr(ops.context)) {
            return UCN_V6_ERR_STATE;
        }
    } else {
        ops.lock_task(ops.context);
    }
    if (!mailbox_state_is_valid_locked(owner) || owner->faulted) {
        if (from_isr) ops.unlock_from_isr(ops.context);
        else ops.unlock_task(ops.context);
        return UCN_V6_ERR_STATE;
    }
    index = (size_t)event - 1U;
    if (owner->pending_by_event[index] != 0U) {
        if (from_isr) ops.unlock_from_isr(ops.context);
        else ops.unlock_task(ops.context);
        return UCN_V6_OK;
    }
    if (owner->pending_total >= owner->event_depth) {
        if (from_isr) ops.unlock_from_isr(ops.context);
        else ops.unlock_task(ops.context);
        return UCN_V6_ERR_NO_SPACE;
    }
    owner->pending_by_event[index] = 1U;
    ++owner->pending_total;
    if (from_isr) ops.unlock_from_isr(ops.context);
    else ops.unlock_task(ops.context);
    ops.notify(ops.context, from_isr);
    return UCN_V6_OK;
}

typedef ucn_v6_result_t (*mailbox_event_handler_t)(
    void *context, ucn_v6_owner_event_t event);

static ucn_v6_result_t mailbox_run(
    ucn_v6_owner_mailbox_t *owner,
    uint16_t budget,
    mailbox_event_handler_t handler,
    void *handler_context,
    uint16_t *processed)
{
    uint16_t count = 0U;
    ucn_v6_result_t result = UCN_V6_OK;
    ucn_v6_owner_lock_ops_t ops;

    if (owner == NULL || budget == 0U || handler == NULL ||
        processed == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!mailbox_is_valid(owner)) {
        return UCN_V6_ERR_STATE;
    }
    ops = owner->lock_ops;
    if (!lock_ops_are_valid(&ops)) {
        return UCN_V6_ERR_STATE;
    }
    ops.lock_task(ops.context);
    if (!mailbox_state_is_valid_locked(owner) || owner->faulted ||
        owner->running) {
        ops.unlock_task(ops.context);
        return UCN_V6_ERR_STATE;
    }
    owner->running = true;
    ops.unlock_task(ops.context);

    while (count < budget) {
        size_t offset;
        size_t selected = UCN_V6_OWNER_EVENT_COUNT;

        ops.lock_task(ops.context);
        if (!mailbox_state_is_valid_locked(owner) || owner->faulted) {
            ops.unlock_task(ops.context);
            result = UCN_V6_ERR_STATE;
            break;
        }
        for (offset = 0U; offset < UCN_V6_OWNER_EVENT_COUNT; ++offset) {
            size_t candidate =
                ((size_t)owner->next_event_index + offset) %
                UCN_V6_OWNER_EVENT_COUNT;
            if (owner->pending_by_event[candidate] != 0U) {
                selected = candidate;
                break;
            }
        }
        if (selected == UCN_V6_OWNER_EVENT_COUNT) {
            ops.unlock_task(ops.context);
            break;
        }
        /* Consume before the callback.  A concurrent post while the callback
         * runs then creates the next latch instead of being erased here. */
        owner->pending_by_event[selected] = 0U;
        --owner->pending_total;
        owner->next_event_index =
            (uint8_t)((selected + 1U) % UCN_V6_OWNER_EVENT_COUNT);
        ops.unlock_task(ops.context);

        result = handler(handler_context,
                         (ucn_v6_owner_event_t)(selected + 1U));
        ++count;
        if (result != UCN_V6_OK) {
            break;
        }
    }

    ops.lock_task(ops.context);
    if (mailbox_is_valid(owner)) {
        owner->running = false;
    }
    ops.unlock_task(ops.context);
    *processed = count;
    return result;
}

static ucn_v6_result_t mailbox_copy_view(
    ucn_v6_owner_mailbox_t *owner,
    ucn_v6_owner_mailbox_view_t *view)
{
    ucn_v6_owner_mailbox_view_t next;
    ucn_v6_owner_lock_ops_t ops;
    size_t index;

    if (owner == NULL || view == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (!mailbox_is_valid(owner)) {
        return UCN_V6_ERR_STATE;
    }
    ops = owner->lock_ops;
    if (!lock_ops_are_valid(&ops)) {
        return UCN_V6_ERR_STATE;
    }
    ops.lock_task(ops.context);
    if (!mailbox_state_is_valid_locked(owner)) {
        ops.unlock_task(ops.context);
        return UCN_V6_ERR_STATE;
    }
    memset(&next, 0, sizeof(next));
    next.pending_total = owner->pending_total;
    next.next_event_index = owner->next_event_index;
    next.running = owner->running;
    next.faulted = owner->faulted;
    for (index = 0U; index < UCN_V6_OWNER_EVENT_COUNT; ++index) {
        next.pending_by_event[index] = owner->pending_by_event[index];
    }
    ops.unlock_task(ops.context);
    *view = next;
    return UCN_V6_OK;
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

static bool principal_is_valid_local(const ucn_v6_principal_t *principal)
{
    size_t index;
    bool nonzero = false;
    bool non_ff = false;
    for (index = 0U; index < sizeof(principal->bytes); ++index) {
        nonzero = nonzero || principal->bytes[index] != 0U;
        non_ff = non_ff || principal->bytes[index] != UINT8_MAX;
    }
    return nonzero && non_ff;
}

static bool session_is_zero(const ucn_v6_session_key_t *session)
{
    return session->binding.realm_id == 0U &&
           session->binding.node_address == 0U &&
           session->binding.binding_generation == 0U &&
           session->session_generation == 0U &&
           bytes_are_zero(session->principal.bytes,
                          sizeof(session->principal.bytes));
}

static bool session_is_valid_local(const ucn_v6_session_key_t *session)
{
    return principal_is_valid_local(&session->principal) &&
           session->binding.realm_id != 0U &&
           session->binding.realm_id != UINT32_MAX &&
           session->binding.node_address != 0U &&
           session->binding.node_address != UINT32_MAX &&
           session->binding.binding_generation != 0U &&
           session->binding.binding_generation <=
               UCN_V6_SERIAL_ROTATION_THRESHOLD &&
           session->session_generation != 0U &&
           session->session_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

bool ucn_v6_stack_invalidation_is_valid(
    const ucn_v6_stack_invalidation_t *value)
{
    bool link_valid;
    bool session_valid;
    if (value == NULL ||
        value->type < UCN_V6_STACK_INVALIDATE_LINK ||
        value->type > UCN_V6_STACK_INVALIDATE_PATH) {
        return false;
    }
    link_valid = value->link_id != 0U && value->link_id != UINT16_MAX &&
                 value->link_generation != 0U &&
                 value->link_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
    session_valid = session_is_valid_local(&value->session);
    if (!link_valid) {
        return false;
    }
    if (value->type == UCN_V6_STACK_INVALIDATE_LINK) {
        return session_is_zero(&value->session) &&
               value->capability_generation == 0U &&
               value->path_id == 0U && value->path_generation == 0U;
    }
    if (!session_valid) {
        return false;
    }
    if (value->type == UCN_V6_STACK_INVALIDATE_SESSION) {
        return value->capability_generation == 0U &&
               value->path_id == 0U && value->path_generation == 0U;
    }
    if (value->capability_generation == 0U ||
        value->capability_generation > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return false;
    }
    if (value->type == UCN_V6_STACK_INVALIDATE_CAPABILITY) {
        return value->path_id == 0U && value->path_generation == 0U;
    }
    return value->path_id != 0U && value->path_id != UINT16_MAX &&
           value->path_generation != 0U &&
           value->path_generation <= UCN_V6_SERIAL_ROTATION_THRESHOLD;
}

static bool stack_hooks_are_valid(const ucn_v6_feature_manifest_t *manifest,
                                  const ucn_v6_stack_hooks_t *hooks)
{
    bool realtime_enabled;
    bool cluster_enabled;
    bool adapter_enabled;
    if (manifest == NULL || hooks == NULL || hooks->rx_ingress == NULL ||
        hooks->tx_completion == NULL || hooks->timer_expiry == NULL ||
        hooks->persistence == NULL || hooks->hop_security == NULL ||
        hooks->e2e_security == NULL || hooks->capability == NULL ||
        hooks->route_authority == NULL || hooks->operation == NULL ||
        hooks->endpoint == NULL || hooks->qos_tx == NULL ||
        hooks->invalidate_security == NULL ||
        hooks->invalidate_capability == NULL ||
        hooks->invalidate_transfer == NULL ||
        hooks->invalidate_route == NULL || hooks->invalidate_qos == NULL ||
        hooks->invalidate_endpoint == NULL) {
        return false;
    }
    realtime_enabled =
        (manifest->feature_bits & UCN_V6_FEATURE_REALTIME) != 0U;
    cluster_enabled =
        (manifest->feature_bits & UCN_V6_FEATURE_CLUSTER) != 0U;
    adapter_enabled =
        (manifest->feature_bits & UCN_V6_FEATURE_ADAPTER) != 0U;
    return (realtime_enabled ?
                hooks->realtime != NULL &&
                    hooks->invalidate_realtime != NULL :
                hooks->realtime == NULL &&
                    hooks->invalidate_realtime == NULL) &&
           (cluster_enabled ?
                hooks->cluster != NULL && hooks->invalidate_cluster != NULL :
                hooks->cluster == NULL &&
                    hooks->invalidate_cluster == NULL) &&
           (adapter_enabled ? hooks->invalidate_adapter != NULL :
                              hooks->invalidate_adapter == NULL);
}

static bool stack_owner_is_valid(const ucn_v6_stack_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_V6_STACK_OWNER_MAGIC &&
           owner->schema == UCN_V6_STORAGE_LAYOUT && owner->initialized &&
           owner->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           owner->canary == UCN_V6_STACK_OWNER_CANARY &&
           mailbox_is_valid(&owner->mailbox);
}

static bool stack_phase_is_enabled(uint32_t feature_bits, size_t phase)
{
    if (phase == UCN_V6_STACK_PHASE_REALTIME) {
        return (feature_bits & UCN_V6_FEATURE_REALTIME) != 0U;
    }
    if (phase == UCN_V6_STACK_PHASE_CLUSTER) {
        return (feature_bits & UCN_V6_FEATURE_CLUSTER) != 0U;
    }
    return true;
}

static bool phase_is_enabled(const ucn_v6_stack_owner_t *owner,
                             size_t phase)
{
    return stack_phase_is_enabled(owner->feature_bits, phase);
}

bool ucn_v6_stack_budget_is_valid(
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_stack_budget_t *budget)
{
    uint32_t total = 0U;
    size_t phase;
    if (ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        budget == NULL || budget->max_total_work == 0U) {
        return false;
    }
    for (phase = 0U; phase < UCN_V6_STACK_PHASE_COUNT; ++phase) {
        bool enabled = stack_phase_is_enabled(manifest->feature_bits, phase);
        if ((enabled && budget->phase_work[phase] == 0U) ||
            (!enabled && budget->phase_work[phase] != 0U)) {
            return false;
        }
        total += budget->phase_work[phase];
    }
    return total <= budget->max_total_work;
}

typedef struct ucn_v6_stack_event_collector {
    uint32_t event_mask;
} ucn_v6_stack_event_collector_t;

static ucn_v6_result_t collect_stack_event(void *context,
                                           ucn_v6_owner_event_t event)
{
    ucn_v6_stack_event_collector_t *collector =
        (ucn_v6_stack_event_collector_t *)context;
    if (collector == NULL || !event_is_valid(event)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    collector->event_mask |= UINT32_C(1) << ((uint32_t)event - 1U);
    return UCN_V6_OK;
}

static void record_first_error(ucn_v6_result_t candidate,
                               ucn_v6_result_t *first_error)
{
    if (*first_error == UCN_V6_OK && candidate != UCN_V6_OK) {
        *first_error = candidate;
    }
}

static ucn_v6_result_t apply_stack_invalidation(
    ucn_v6_stack_owner_t *owner,
    const ucn_v6_stack_invalidation_t *invalidation)
{
    ucn_v6_result_t first_error = UCN_V6_OK;
    ucn_v6_stack_hooks_t *hooks = &owner->hooks;
    ucn_v6_owner_lock_ops_t ops = owner->mailbox.lock_ops;
    bool from_link = invalidation->type == UCN_V6_STACK_INVALIDATE_LINK;
    bool from_session = invalidation->type <=
                         UCN_V6_STACK_INVALIDATE_SESSION;

    /* No later protocol phase is allowed to run while this fan-out is in
     * progress.  All applicable dependents are called even after one reports
     * an error; any error permanently faults this runtime, preserving the
     * fail-closed boundary without pretending partial rollback is possible. */
    if (from_link && hooks->invalidate_adapter != NULL) {
        record_first_error(hooks->invalidate_adapter(hooks->context,
                                                     invalidation),
                           &first_error);
    }
    if (from_session) {
        record_first_error(hooks->invalidate_security(hooks->context,
                                                      invalidation),
                           &first_error);
    }
    /* Capability owns the complete Link -> Session -> Capability -> Path
     * parent chain, so it must consume every invalidation type.  In
     * particular, a PATH event may originate in Route/Transfer rather than in
     * Capability itself; skipping this callback would leave a live parent
     * record behind while every dependent had already fenced it. */
    record_first_error(hooks->invalidate_capability(hooks->context,
                                                    invalidation),
                       &first_error);
    if (hooks->invalidate_realtime != NULL) {
        record_first_error(hooks->invalidate_realtime(hooks->context,
                                                      invalidation),
                           &first_error);
    }
    if (hooks->invalidate_cluster != NULL) {
        record_first_error(hooks->invalidate_cluster(hooks->context,
                                                     invalidation),
                           &first_error);
    }
    record_first_error(hooks->invalidate_transfer(hooks->context,
                                                  invalidation),
                       &first_error);
    record_first_error(hooks->invalidate_route(hooks->context, invalidation),
                       &first_error);
    record_first_error(hooks->invalidate_qos(hooks->context, invalidation),
                       &first_error);
    record_first_error(hooks->invalidate_endpoint(hooks->context,
                                                  invalidation),
                       &first_error);
    if (first_error == UCN_V6_OK) {
        ops.lock_task(ops.context);
        if (!stack_owner_is_valid(owner)) {
            first_error = UCN_V6_ERR_STATE;
        } else if (owner->invalidations_applied != UINT32_MAX) {
            ++owner->invalidations_applied;
        }
        ops.unlock_task(ops.context);
    }
    return first_error;
}

static ucn_v6_stack_phase_hook_t stack_phase_hook(
    const ucn_v6_stack_hooks_t *hooks,
    size_t phase)
{
    switch ((ucn_v6_stack_phase_t)phase) {
    case UCN_V6_STACK_PHASE_RX_INGRESS: return hooks->rx_ingress;
    case UCN_V6_STACK_PHASE_TX_COMPLETION: return hooks->tx_completion;
    case UCN_V6_STACK_PHASE_TIMER_EXPIRY: return hooks->timer_expiry;
    case UCN_V6_STACK_PHASE_PERSISTENCE: return hooks->persistence;
    case UCN_V6_STACK_PHASE_HOP_SECURITY: return hooks->hop_security;
    case UCN_V6_STACK_PHASE_E2E_SECURITY: return hooks->e2e_security;
    case UCN_V6_STACK_PHASE_CAPABILITY: return hooks->capability;
    case UCN_V6_STACK_PHASE_ROUTE_AUTHORITY: return hooks->route_authority;
    case UCN_V6_STACK_PHASE_REALTIME: return hooks->realtime;
    case UCN_V6_STACK_PHASE_OPERATION: return hooks->operation;
    case UCN_V6_STACK_PHASE_ENDPOINT: return hooks->endpoint;
    case UCN_V6_STACK_PHASE_CLUSTER: return hooks->cluster;
    case UCN_V6_STACK_PHASE_QOS_TX: return hooks->qos_tx;
    default: return NULL;
    }
}

ucn_v6_result_t ucn_v6_stack_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_owner_lock_ops_t *lock_ops,
    const ucn_v6_stack_hooks_t *hooks,
    ucn_v6_stack_owner_t **owner_out)
{
    ucn_v6_stack_owner_t initialized;
    ucn_v6_stack_owner_t *owner;
    ucn_v6_result_t result;

    if (owner_out == NULL || !lock_ops_are_valid(lock_ops) ||
        ucn_v6_manifest_validate_exact(manifest) != UCN_V6_OK ||
        !stack_hooks_are_valid(manifest, hooks)) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     sizeof(ucn_v6_stack_owner_t),
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) {
        return result;
    }
    memset(&initialized, 0, sizeof(initialized));
    result = mailbox_init(&initialized.mailbox, manifest, lock_ops);
    if (result != UCN_V6_OK) {
        return result;
    }
    initialized.magic = UCN_V6_STACK_OWNER_MAGIC;
    initialized.schema = UCN_V6_STORAGE_LAYOUT;
    initialized.layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    initialized.feature_bits = manifest->feature_bits;
    initialized.hooks = *hooks;
    initialized.rerun_pending = true;
    initialized.initialized = true;
    initialized.canary = UCN_V6_STACK_OWNER_CANARY;
    owner = (ucn_v6_stack_owner_t *)storage;
    *owner = initialized;
    *owner_out = owner;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_stack_owner_post(
    ucn_v6_stack_owner_t *owner,
    ucn_v6_owner_event_t event,
    bool from_isr)
{
    if (!stack_owner_is_valid(owner)) {
        return UCN_V6_ERR_STATE;
    }
    return mailbox_post(&owner->mailbox, event, from_isr);
}

ucn_v6_result_t ucn_v6_stack_owner_run(
    ucn_v6_stack_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_stack_budget_t *budget,
    ucn_v6_stack_run_result_t *result_out)
{
    ucn_v6_stack_run_result_t run_result;
    ucn_v6_stack_event_collector_t collector;
    ucn_v6_owner_mailbox_view_t mailbox_view;
    ucn_v6_owner_lock_ops_t ops;
    uint16_t drained = 0U;
    uint16_t work_done = 0U;
    uint64_t next_deadline = 0U;
    bool has_next_deadline = false;
    bool triggered;
    size_t phase;
    ucn_v6_result_t result;

    if (!stack_owner_is_valid(owner) || result_out == NULL ||
        !ucn_v6_stack_budget_is_valid(ucn_v6_compiled_manifest(), budget)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ops = owner->mailbox.lock_ops;
    ops.lock_task(ops.context);
    if (!stack_owner_is_valid(owner) || owner->faulted || owner->running ||
        (owner->has_time && now_us < owner->last_now_us)) {
        ops.unlock_task(ops.context);
        return UCN_V6_ERR_STATE;
    }
    owner->running = true;
    triggered = owner->rerun_pending ||
                (owner->has_next_deadline &&
                 now_us >= owner->next_deadline_us);
    owner->rerun_pending = false;
    ops.unlock_task(ops.context);

    memset(&collector, 0, sizeof(collector));
    result = mailbox_run(
        &owner->mailbox,
        owner->mailbox.event_depth < UCN_V6_OWNER_EVENT_COUNT ?
            owner->mailbox.event_depth :
            (uint16_t)UCN_V6_OWNER_EVENT_COUNT,
        collect_stack_event, &collector, &drained);
    if (result != UCN_V6_OK) {
        goto fail;
    }
    triggered = triggered || collector.event_mask != 0U;
    memset(&run_result, 0, sizeof(run_result));
    run_result.last_error = UCN_V6_OK;

    if (triggered) {
        for (phase = 0U; phase < UCN_V6_STACK_PHASE_COUNT; ++phase) {
            ucn_v6_stack_phase_hook_t hook;
            ucn_v6_stack_phase_result_t phase_result;
            uint16_t phase_budget;
            if (!phase_is_enabled(owner, phase)) {
                continue;
            }
            hook = stack_phase_hook(&owner->hooks, phase);
            phase_budget = budget->phase_work[phase];
            memset(&phase_result, 0, sizeof(phase_result));
            result = hook(owner->hooks.context, now_us, phase_budget,
                          &phase_result);
            run_result.phases_run_mask |= UINT32_C(1) << phase;
            if (result != UCN_V6_OK ||
                phase_result.work_done > phase_budget ||
                (phase_result.has_more && phase_result.work_done == 0U) ||
                (phase_result.has_deadline &&
                 phase_result.next_deadline_us == 0U) ||
                (phase_result.has_invalidation &&
                 (phase_result.work_done == 0U ||
                   !ucn_v6_stack_invalidation_is_valid(
                      &phase_result.invalidation)))) {
                if (result == UCN_V6_OK) {
                    result = UCN_V6_ERR_STATE;
                }
                goto fail_with_result;
            }
            work_done = (uint16_t)(work_done + phase_result.work_done);
            if (phase_result.has_more) {
                run_result.phases_backlogged_mask |= UINT32_C(1) << phase;
            }
            if (phase_result.has_deadline &&
                (!has_next_deadline ||
                 phase_result.next_deadline_us < next_deadline)) {
                has_next_deadline = true;
                next_deadline = phase_result.next_deadline_us;
            }
            if (phase_result.has_invalidation) {
                result = apply_stack_invalidation(
                    owner, &phase_result.invalidation);
                if (result != UCN_V6_OK) {
                    goto fail_with_result;
                }
            }
        }
    } else if (owner->has_next_deadline) {
        has_next_deadline = true;
        next_deadline = owner->next_deadline_us;
    }

    result = mailbox_copy_view(&owner->mailbox, &mailbox_view);
    if (result != UCN_V6_OK) {
        goto fail_with_result;
    }
    for (phase = 0U; phase < UCN_V6_OWNER_EVENT_COUNT; ++phase) {
        if (mailbox_view.pending_by_event[phase] != 0U) {
            run_result.pending_event_mask |= UINT32_C(1) << phase;
        }
    }
    run_result.work_done = work_done;
    run_result.has_next_deadline = has_next_deadline;
    run_result.next_deadline_us = has_next_deadline ? next_deadline : 0U;
    run_result.more_work = run_result.phases_backlogged_mask != 0U ||
                           run_result.pending_event_mask != 0U ||
                           (has_next_deadline && next_deadline <= now_us);

    ops.lock_task(ops.context);
    owner->last_now_us = now_us;
    owner->has_time = true;
    owner->has_next_deadline = has_next_deadline;
    owner->next_deadline_us = has_next_deadline ? next_deadline : 0U;
    owner->rerun_pending = run_result.more_work;
    owner->last_error = UCN_V6_OK;
    owner->running = false;
    ops.unlock_task(ops.context);
    *result_out = run_result;
    return UCN_V6_OK;

fail_with_result:
    run_result.work_done = work_done;
    run_result.last_error = result;
    *result_out = run_result;
fail:
    ops.lock_task(ops.context);
    if (stack_owner_is_valid(owner)) {
        owner->faulted = true;
        owner->mailbox.faulted = true;
        owner->last_error = result;
        owner->running = false;
    }
    ops.unlock_task(ops.context);
    return result;
}

ucn_v6_result_t ucn_v6_stack_owner_copy_view(
    ucn_v6_stack_owner_t *owner,
    ucn_v6_stack_owner_view_t *view_out)
{
    ucn_v6_stack_owner_view_t view;
    ucn_v6_owner_lock_ops_t ops;
    size_t index;

    if (!stack_owner_is_valid(owner) || view_out == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ops = owner->mailbox.lock_ops;
    ops.lock_task(ops.context);
    if (!stack_owner_is_valid(owner) ||
        !mailbox_state_is_valid_locked(&owner->mailbox)) {
        ops.unlock_task(ops.context);
        return UCN_V6_ERR_STATE;
    }
    memset(&view, 0, sizeof(view));
    for (index = 0U; index < UCN_V6_OWNER_EVENT_COUNT; ++index) {
        if (owner->mailbox.pending_by_event[index] != 0U) {
            view.pending_event_mask |= UINT32_C(1) << index;
        }
    }
    view.last_now_us = owner->last_now_us;
    view.next_deadline_us = owner->next_deadline_us;
    view.invalidations_applied = owner->invalidations_applied;
    view.has_time = owner->has_time;
    view.has_next_deadline = owner->has_next_deadline;
    view.rerun_pending = owner->rerun_pending;
    view.running = owner->running;
    view.faulted = owner->faulted;
    view.last_error = owner->last_error;
    ops.unlock_task(ops.context);
    *view_out = view;
    return UCN_V6_OK;
}
