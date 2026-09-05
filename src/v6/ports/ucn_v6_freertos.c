#include "ucn/v6/ports/ucn_v6_freertos.h"

#include <string.h>

#define UCN_V6_FREERTOS_MAGIC UINT32_C(0x56364652)
#define UCN_V6_FREERTOS_CANARY UINT64_C(0x5636465245455254)

struct ucn_v6_freertos_port {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint64_t layout_hash;
    ucn_v6_freertos_port_ops_t ops;
    ucn_v6_stack_budget_t stack_budget;
    ucn_v6_stack_owner_storage_t owner_storage;
    ucn_v6_stack_owner_t *owner;
    ucn_v6_adapter_owner_t *adapter;
    bool initialized;
    uint64_t canary;
};

typedef char ucn_v6_freertos_storage_size_check[
    sizeof(ucn_v6_freertos_port_t) <= UCN_V6_FREERTOS_PORT_STORAGE_BYTES ?
        1 : -1];

static bool port_valid(const ucn_v6_freertos_port_t *port)
{
    return port != NULL && port->magic == UCN_V6_FREERTOS_MAGIC &&
           port->schema == UCN_V6_STORAGE_LAYOUT &&
           port->layout_hash == UCN_V6_COMPILED_LAYOUT_HASH &&
           port->owner != NULL && port->initialized &&
           port->canary == UCN_V6_FREERTOS_CANARY;
}

static bool ops_valid(const ucn_v6_freertos_port_ops_t *ops)
{
    return ops != NULL &&
           ops->struct_size == sizeof(ucn_v6_freertos_port_ops_t) &&
           ops->api_version == UCN_V6_FREERTOS_PORT_API_VERSION &&
           ops->lock_task != NULL && ops->try_lock_from_isr != NULL &&
           ops->unlock_task != NULL && ops->unlock_from_isr != NULL &&
           ops->notify_owner_task != NULL &&
           ops->wait_for_notification != NULL &&
           ops->read_monotonic_time_us != NULL;
}

static void owner_lock_task(void *context)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    port->ops.lock_task(port->ops.context);
}

static bool owner_try_lock_from_isr(void *context)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    return port->ops.try_lock_from_isr(port->ops.context);
}

static void owner_unlock_task(void *context)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    port->ops.unlock_task(port->ops.context);
}

static void owner_unlock_from_isr(void *context)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    port->ops.unlock_from_isr(port->ops.context);
}

static void owner_notify(void *context, bool from_isr)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    port->ops.notify_owner_task(port->ops.context, from_isr);
}

static ucn_v6_result_t adapter_lock_task(void *context)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    port->ops.lock_task(port->ops.context);
    return UCN_V6_OK;
}

static bool adapter_try_lock_from_isr(void *context)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    return port->ops.try_lock_from_isr(port->ops.context);
}

static void adapter_unlock_task(void *context)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    port->ops.unlock_task(port->ops.context);
}

static void adapter_unlock_from_isr(void *context)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    port->ops.unlock_from_isr(port->ops.context);
}

static ucn_v6_result_t adapter_post(
    void *context, ucn_v6_owner_event_t event, bool from_isr)
{
    ucn_v6_freertos_port_t *port = (ucn_v6_freertos_port_t *)context;
    if (!port_valid(port)) return UCN_V6_ERR_STATE;
    return ucn_v6_stack_owner_post(port->owner, event, from_isr);
}

ucn_v6_result_t ucn_v6_freertos_port_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_freertos_port_ops_t *ops,
    const ucn_v6_stack_hooks_t *stack_hooks,
    const ucn_v6_stack_budget_t *stack_budget,
    ucn_v6_freertos_port_t **port_out)
{
    ucn_v6_freertos_port_t *port;
    ucn_v6_owner_lock_ops_t owner_ops;
    ucn_v6_result_t result;
    if (port_out != NULL) *port_out = NULL;
    if (port_out == NULL || !ops_valid(ops) || stack_hooks == NULL ||
        !ucn_v6_stack_budget_is_valid(manifest, stack_budget)) {
        return UCN_V6_ERR_CONFIG;
    }
    result = ucn_v6_storage_validate(storage, storage_bytes,
                                     UCN_V6_FREERTOS_PORT_STORAGE_BYTES,
                                     UCN_V6_STORAGE_ALIGNMENT);
    if (result != UCN_V6_OK) return result;
    ops->lock_task(ops->context);
    memset(storage, 0, storage_bytes);
    port = (ucn_v6_freertos_port_t *)storage;
    port->magic = UCN_V6_FREERTOS_MAGIC;
    port->schema = UCN_V6_STORAGE_LAYOUT;
    port->layout_hash = UCN_V6_COMPILED_LAYOUT_HASH;
    port->ops = *ops;
    port->stack_budget = *stack_budget;
    port->canary = UCN_V6_FREERTOS_CANARY;
    memset(&owner_ops, 0, sizeof(owner_ops));
    owner_ops.context = port;
    owner_ops.lock_task = owner_lock_task;
    owner_ops.try_lock_from_isr = owner_try_lock_from_isr;
    owner_ops.unlock_task = owner_unlock_task;
    owner_ops.unlock_from_isr = owner_unlock_from_isr;
    owner_ops.notify = owner_notify;
    ops->unlock_task(ops->context);
    result = ucn_v6_stack_owner_init_in_place(
        &port->owner_storage, sizeof(port->owner_storage), manifest,
        &owner_ops, stack_hooks, &port->owner);
    if (result != UCN_V6_OK) {
        memset(storage, 0, storage_bytes);
        return result;
    }
    port->initialized = true;
    *port_out = port;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_freertos_port_make_adapter_runtime(
    ucn_v6_freertos_port_t *port,
    ucn_v6_driver_runtime_ops_t *runtime_ops)
{
    ucn_v6_driver_runtime_ops_t next;
    if (!port_valid(port) || runtime_ops == NULL) return UCN_V6_ERR_ARGUMENT;
    memset(&next, 0, sizeof(next));
    next.context = port;
    next.lock_task = adapter_lock_task;
    next.try_lock_from_isr = adapter_try_lock_from_isr;
    next.unlock_task = adapter_unlock_task;
    next.unlock_from_isr = adapter_unlock_from_isr;
    next.post_owner_event = adapter_post;
    *runtime_ops = next;
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_freertos_port_bind_adapter(
    ucn_v6_freertos_port_t *port,
    ucn_v6_adapter_owner_t *adapter)
{
    if (!port_valid(port) || adapter == NULL) return UCN_V6_ERR_ARGUMENT;
    port->ops.lock_task(port->ops.context);
    if (port->adapter != NULL) {
        port->ops.unlock_task(port->ops.context);
        return UCN_V6_ERR_REPLAY;
    }
    port->adapter = adapter;
    port->ops.unlock_task(port->ops.context);
    return UCN_V6_OK;
}

ucn_v6_result_t ucn_v6_freertos_port_run(
    ucn_v6_freertos_port_t *port,
    ucn_v6_stack_run_result_t *run_result)
{
    uint64_t now_us;
    ucn_v6_result_t result;
    if (!port_valid(port) || run_result == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (port->adapter == NULL) return UCN_V6_ERR_STATE;
    result = port->ops.read_monotonic_time_us(port->ops.context, &now_us);
    if (result != UCN_V6_OK) return result;
    return ucn_v6_stack_owner_run(port->owner, now_us,
                                  &port->stack_budget, run_result);
}

ucn_v6_result_t ucn_v6_freertos_port_wait_and_run(
    ucn_v6_freertos_port_t *port,
    uint64_t max_wait_us,
    ucn_v6_stack_run_result_t *run_result)
{
    ucn_v6_stack_owner_view_t view;
    bool notified = false;
    uint64_t now_us;
    uint64_t wait_us;
    ucn_v6_result_t result;
    if (!port_valid(port) || run_result == NULL ||
        max_wait_us == 0U) return UCN_V6_ERR_ARGUMENT;
    result = ucn_v6_stack_owner_copy_view(port->owner, &view);
    if (result != UCN_V6_OK) return result;
    result = port->ops.read_monotonic_time_us(port->ops.context, &now_us);
    if (result != UCN_V6_OK) return result;
    wait_us = max_wait_us;
    if (view.has_next_deadline && view.next_deadline_us > now_us &&
        view.next_deadline_us - now_us < wait_us) {
        wait_us = view.next_deadline_us - now_us;
    }
    if (view.pending_event_mask == 0U && !view.rerun_pending &&
        (!view.has_next_deadline || now_us < view.next_deadline_us)) {
        result = port->ops.wait_for_notification(
            port->ops.context, wait_us, &notified);
        if (result != UCN_V6_OK) return result;
        if (!notified) {
            result = ucn_v6_stack_owner_post(
                port->owner, UCN_V6_OWNER_EVENT_TIMER, false);
            if (result != UCN_V6_OK) return result;
        }
    }
    return ucn_v6_freertos_port_run(port, run_result);
}

ucn_v6_result_t ucn_v6_freertos_port_post_timer(
    ucn_v6_freertos_port_t *port,
    bool from_isr)
{
    if (!port_valid(port)) return UCN_V6_ERR_ARGUMENT;
    return ucn_v6_stack_owner_post(
        port->owner, UCN_V6_OWNER_EVENT_TIMER, from_isr);
}
