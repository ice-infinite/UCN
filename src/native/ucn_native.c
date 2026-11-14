#include "ucn/ucn_native_backend.h"
#include "internal/ucn_stack.h"
#include <string.h>

#define MAGIC UINT32_C(0x4e415432)
#define PUB 1U
#define SUB 2U
typedef struct entity {
    uint32_t generation;
    uint16_t channel;
    uint8_t open, active;
    ucn_native_receive_fn receive;
    void *context;
} entity_t;
typedef struct command {
    uint64_t deadline;
    ucn_native_target_t target;
    uint16_t channel, next_target;
    uint32_t order;
    uint8_t valid, unicast;
    uint8_t payload[UCN_NATIVE_PAYLOAD_BYTES];
} command_t;
typedef struct event {
    uint64_t received;
    uint64_t age_anchor;
    uint32_t sequence, recipients;
    uint32_t order;
    uint16_t channel, source;
    uint8_t local, valid, protection;
    ucn_native_scope_t scope;
    uint8_t payload[UCN_NATIVE_PAYLOAD_BYTES];
} event_t;
typedef struct lifecycle {
    uint32_t owner_instance, layout;
    ucn_native_scope_t source[UCN_NATIVE_CAPACITY][UCN_NATIVE_SOURCES];
    ucn_native_scope_t retired_target[UCN_NATIVE_CAPACITY][UCN_NATIVE_TARGETS];
    uint32_t cancelled[UCN_NATIVE_CAPACITY];
    uint8_t retired_source[UCN_NATIVE_CAPACITY][UCN_NATIVE_SOURCES];
} lifecycle_t;
UCN_STATIC_ASSERT(sizeof(lifecycle_t) <= UCN_NATIVE_LIFECYCLE_STORAGE_BYTES, native_lifecycle_storage);
struct ucn_native {
    uint32_t magic, instance;
    ucn_node_t *core;
    const ucn_native_backend_t *backend;
    lifecycle_t *lifecycle;
    const void *backend_context;
    size_t backend_context_bytes;
    ucn_lock_ops_t lock;
    ucn_node_view_t core_view;
    ucn_native_channel_t channels[UCN_NATIVE_CAPACITY];
    ucn_endpoint_handle_t endpoints[UCN_NATIVE_CAPACITY];
    uint32_t source_high[UCN_NATIVE_CAPACITY][UCN_NATIVE_SOURCES];
    uint32_t local_sequence;
    uint32_t command_order, event_order;
    uint8_t fenced[UCN_NATIVE_CAPACITY][UCN_NATIVE_SOURCES];
    entity_t pubs[UCN_NATIVE_CAPACITY], subs[UCN_NATIVE_CAPACITY];
    command_t commands[UCN_NATIVE_CAPACITY];
    event_t events[UCN_NATIVE_CAPACITY];
    ucn_native_status_t stats;
    uint64_t now;
    uint16_t channel_count, command_cursor, event_cursor, sub_cursor;
    uint8_t running, stepping, dispatching, stopping, stop_applied, dead, in_callback;
    /* EN: Mutually exclusive actor phases share one bounded workspace.
     * ZH: 互斥的Actor阶段共享单份有界工作区，不把副本压入任务栈。 */
    union {
        struct { ucn_native_sample_t sample; uint8_t payload[UCN_NATIVE_PAYLOAD_BYTES]; } app;
        struct { ucn_native_backend_message_t message; uint8_t payload[UCN_NATIVE_PAYLOAD_BYTES]; } rx;
        struct { ucn_target_t target; ucn_send_options_t options; ucn_native_target_t native_target; } send;
        struct { ucn_step_budget_t budget; ucn_step_result_t result; } core;
    } work;
};
UCN_STATIC_ASSERT(sizeof(ucn_native_t) <= UCN_NATIVE_STORAGE_BYTES, native_storage);
UCN_STATIC_ASSERT(UCN_NATIVE_CAPACITY <= 32U, native_recipient_mask);

/* EN: Reject partial aliases before writes; uintptr arithmetic cannot wrap.
 * 中文：写入前拒绝部分别名，使用不会回绕的区间比较。 */
static bool overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an != 0U && bn != 0U && (x <= y ? y - x < an : x - y < bn);
}
static ucn_result_t enter(ucn_native_t *n)
{
    ucn_result_t r;
    if (n == NULL || n->magic != MAGIC) { return UCN_ERR_ARGUMENT; }
    r = n->lock.enter(n->lock.context);
    if (r != UCN_OK) { return r; }
    if (n->dead) { n->lock.leave(n->lock.context); return UCN_ERR_STATE; }
    return UCN_OK;
}
static ucn_result_t leave(ucn_native_t *n, ucn_result_t r)
{ n->lock.leave(n->lock.context); return r; }
static bool external_output(const ucn_native_t *n, const void *out, size_t bytes)
{
    return n != NULL && n->magic == MAGIC && out != NULL && !overlap(n, UCN_NATIVE_STORAGE_BYTES, out, bytes) &&
        !overlap(n->lock.context, n->lock.context != NULL ? 1U : 0U, out, bytes) &&
        (n->backend == NULL || (!overlap(n->backend, sizeof(*n->backend), out, bytes) &&
        !overlap(n->backend_context, n->backend_context_bytes, out, bytes))) &&
        !overlap(n->lifecycle, n->lifecycle != NULL ? UCN_NATIVE_LIFECYCLE_STORAGE_BYTES : 0U, out, bytes) &&
        !overlap(n->core, ucn_storage_required(), out, bytes);
}
static uint16_t find_channel(const ucn_native_t *n, uint16_t id)
{
    uint16_t i;
    for (i = 0U; i < n->channel_count; ++i) { if (n->channels[i].id == id) { return i; } }
    return UINT16_MAX;
}
static bool source_equal(ucn_native_source_t a, ucn_native_source_t b)
{ return a.address == b.address && a.binding_generation == b.binding_generation && a.principal_digest == b.principal_digest; }
static bool scope_equal(ucn_native_scope_t a, ucn_native_scope_t b)
{ return a.instance == b.instance && a.generation == b.generation && a.slot == b.slot; }
static bool scope_namespace(ucn_native_scope_t a, ucn_native_scope_t b)
{ return a.instance == b.instance && a.slot == b.slot; }
ucn_result_t ucn_native_backend_enable_lifecycle(ucn_native_t *n,
    ucn_native_lifecycle_storage_t *storage, size_t bytes)
{
    size_t i; ucn_result_t r;
    if (bytes != sizeof(*storage) || !external_output(n, storage, bytes) ||
        (uintptr_t)storage % sizeof(uint64_t) != 0U) { return UCN_ERR_ARGUMENT; }
    r = enter(n); if (r != UCN_OK) { return r; }
    if (n->backend == NULL || n->running || n->stopping || n->stepping || n->dispatching || n->lifecycle != NULL) {
        return leave(n, UCN_ERR_STATE);
    }
    for (i = 0U; i < bytes; ++i) { if (storage->bytes[i] != 0U) { return leave(n, UCN_ERR_ARGUMENT); } }
    n->lifecycle = (lifecycle_t *)storage;
    n->lifecycle->owner_instance = n->instance; n->lifecycle->layout = UCN_NATIVE_LAYOUT;
    return leave(n, UCN_OK);
}
ucn_result_t ucn_native_backend_lifecycle_range(ucn_native_t *n,
    ucn_native_borrowed_range_t *range_out)
{
    ucn_result_t r;
    if (!external_output(n, range_out, sizeof(*range_out))) { return UCN_ERR_ARGUMENT; }
    r = enter(n); if (r != UCN_OK) { return r; }
    if (n->backend == NULL || n->lifecycle == NULL || n->running || n->stopping ||
        n->stepping || n->dispatching || n->in_callback) { return leave(n, UCN_ERR_STATE); }
    range_out->address = n->lifecycle;
    range_out->bytes = UCN_NATIVE_LIFECYCLE_STORAGE_BYTES;
    return leave(n, UCN_OK);
}
static bool target_peer(ucn_native_target_t target, ucn_native_source_t source)
{ return target.address == source.address && target.binding_generation == source.binding_generation; }
ucn_result_t ucn_native_backend_check_binding(ucn_native_t *n,
    const ucn_native_backend_t *backend)
{
    ucn_result_t r;
    if (backend == NULL) { return UCN_ERR_ARGUMENT; }
    r = enter(n);
    if (r != UCN_OK) { return r; }
    if (n->backend != backend || n->backend_context != backend->context ||
        n->backend_context_bytes != backend->context_bytes ||
        n->stepping || n->dispatching || n->in_callback) { return leave(n, UCN_ERR_STATE); }
    return leave(n, UCN_OK);
}
/* Validate the complete operation before touching any source, event or command. */
static ucn_result_t peer_check(const ucn_native_t *n, const ucn_native_source_t *source, const ucn_native_scope_t *scope)
{
    uint16_t ch, i; bool source_address = false, exact_source = false, target = false;
    if (!scope->instance || !scope->generation || scope->reserved_zero || !source->address || !source->binding_generation) { return UCN_ERR_ARGUMENT; }
    if (n->backend == NULL || n->lifecycle == NULL || n->stepping || n->dispatching || n->in_callback) { return UCN_ERR_STATE; }
    for (ch = 0U; ch < n->channel_count; ++ch) {
        const ucn_native_channel_t *c = &n->channels[ch];
        for (i = 0U; i < c->source_count; ++i) {
            if (c->sources[i].address == source->address && c->sources[i].binding_generation == source->binding_generation) { source_address = true; }
            if (source_equal(c->sources[i], *source)) {
                const ucn_native_scope_t prior = n->lifecycle->source[ch][i];
                exact_source = true;
                if (prior.instance != 0U && !scope_namespace(prior, *scope)) { return UCN_ERR_ACCESS; }
            }
        }
        for (i = 0U; i < c->target_count; ++i) {
            if (target_peer(c->targets[i], *source)) {
                const ucn_native_scope_t prior = n->lifecycle->retired_target[ch][i];
                target = true;
                if (prior.instance != 0U && !scope_namespace(prior, *scope)) { return UCN_ERR_ACCESS; }
            }
        }
    }
    return (exact_source || (!source_address && target)) ? UCN_OK : UCN_ERR_NOT_FOUND;
}
static bool peer_newer(const ucn_native_t *n, const ucn_native_source_t *source, uint32_t generation)
{
    uint16_t ch, i;
    for (ch = 0U; ch < n->channel_count; ++ch) {
        for (i = 0U; i < n->channels[ch].source_count; ++i) {
            if (source_equal(n->channels[ch].sources[i], *source) && n->lifecycle->source[ch][i].generation > generation) { return true; }
        }
    }
    return false;
}
ucn_result_t ucn_native_backend_retire_peer(ucn_native_t *n, ucn_native_source_t source, ucn_native_scope_t scope)
{
    uint16_t ch, i, j; bool newer; ucn_result_t r = enter(n); if (r != UCN_OK) { return r; }
    r = peer_check(n, &source, &scope); if (r != UCN_OK) { return leave(n, r); }
    newer = peer_newer(n, &source, scope.generation);
    for (ch = 0U; ch < n->channel_count; ++ch) {
        const ucn_native_channel_t *c = &n->channels[ch];
        for (i = 0U; i < c->source_count; ++i) {
            if (source_equal(c->sources[i], source)) {
                if (n->lifecycle->source[ch][i].generation <= scope.generation) {
                    n->lifecycle->source[ch][i] = scope; n->lifecycle->retired_source[ch][i] = 1U;
                }
                for (j = 0U; j < UCN_NATIVE_CAPACITY; ++j) {
                    event_t *e = &n->events[j];
                    if (e->valid && e->channel == ch && e->source == i && scope_equal(e->scope, scope)) { e->valid = 0U; }
                }
            }
        }
        for (i = 0U; i < c->target_count; ++i) {
            ucn_native_scope_t *prior = &n->lifecycle->retired_target[ch][i];
            if (!target_peer(c->targets[i], source) || newer || prior->generation >= scope.generation) { continue; }
            *prior = scope;
            for (j = 0U; j < UCN_NATIVE_CAPACITY; ++j) {
                const command_t *cmd = &n->commands[j];
                uint16_t bit = cmd->unicast ? 0U : i;
                if (cmd->valid && cmd->channel == ch && cmd->next_target <= bit &&
                    (!cmd->unicast || target_peer(cmd->target, source)) && !(n->lifecycle->cancelled[j] & (UINT32_C(1) << bit))) {
                    n->lifecycle->cancelled[j] |= UINT32_C(1) << bit; ++n->stats.failed;
                }
            }
        }
    }
    return leave(n, UCN_OK);
}
ucn_result_t ucn_native_backend_peer_quiescent(ucn_native_t *n, ucn_native_source_t source, ucn_native_scope_t scope, bool *out)
{
    uint16_t i, j; bool quiet = true, newer; ucn_result_t r;
    if (!external_output(n, out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    r = enter(n); if (r != UCN_OK) { return r; }
    r = peer_check(n, &source, &scope); if (r != UCN_OK) { return leave(n, r); }
    newer = peer_newer(n, &source, scope.generation);
    for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) {
        const event_t *e = &n->events[i]; const command_t *cmd = &n->commands[i];
        if (e->valid && source_equal(n->channels[e->channel].sources[e->source], source) && scope_equal(e->scope, scope)) { quiet = false; }
        if (!cmd->valid || newer) { continue; }
        for (j = 0U; j < n->channels[cmd->channel].target_count; ++j) {
            uint16_t bit = cmd->unicast ? 0U : j;
            if (target_peer(n->channels[cmd->channel].targets[j], source) &&
                n->lifecycle->retired_target[cmd->channel][j].generation < scope.generation &&
                cmd->next_target <= bit && (!cmd->unicast || target_peer(cmd->target, source)) &&
                !(n->lifecycle->cancelled[i] & (UINT32_C(1) << bit))) { quiet = false; }
        }
    }
    *out = quiet; return leave(n, UCN_OK);
}
static uint32_t recipients(const ucn_native_t *n, uint16_t ch)
{
    uint16_t i; uint32_t mask = 0U;
    for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) {
        if (n->subs[i].open && n->subs[i].channel == ch) { mask |= UINT32_C(1) << i; }
    }
    return mask;
}
static UCN_I_STACK_PHASE uint16_t event_slot(const ucn_native_t *n, uint16_t ch, uint16_t src, bool local)
{
    uint16_t i, free_slot = UINT16_MAX;
    for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) {
        if (!n->events[i].valid && free_slot == UINT16_MAX) { free_slot = i; }
        if (n->channels[ch].queue_policy == UCN_NATIVE_KEEP_LATEST && n->events[i].valid &&
            n->events[i].channel == ch && n->events[i].source == src && n->events[i].local == local) { return i; }
    }
    return free_slot;
}
static UCN_I_STACK_PHASE void queue_event(ucn_native_t *n, uint16_t at, const void *bytes)
{
    event_t *e = &n->events[at];
    /* The locked caller stages metadata directly in the selected fixed slot.
     * The completed slot becomes observable only after the caller unlocks. */
    e->age_anchor = e->received < n->now ? e->received : n->now;
    e->recipients = recipients(n, e->channel);
    e->protection = e->local ? UCN_NATIVE_LOCAL : UCN_NATIVE_STATIC_TRUSTED;
    memset(&e->scope, 0, sizeof(e->scope));
    ++n->event_order; e->order = n->event_order;
    e->valid = e->recipients != 0U ? 1U : 0U;
    memcpy(e->payload, bytes, n->channels[e->channel].payload_bytes);
}
static UCN_I_STACK_PHASE ucn_endpoint_disposition_t receive_frame(void *ctx, const ucn_endpoint_message_t *m)
{
    ucn_native_t *n = (ucn_native_t *)ctx;
    uint16_t ch, src, at;
    if (enter(n) != UCN_OK) { return UCN_ENDPOINT_DROP; }
    if (!n->running || n->stopping || m->origin_sequence == 0U) { n->lock.leave(n->lock.context); return UCN_ENDPOINT_DROP; }
    for (ch = 0U; ch < n->channel_count; ++ch) {
        ucn_native_channel_t *c = &n->channels[ch];
        if (c->security_minimum != UCN_NATIVE_STATIC_TRUSTED || c->service_id != m->service_id || c->payload_bytes != m->payload_bytes || c->traffic_class != m->traffic_class) { continue; }
        for (src = 0U; src < c->source_count; ++src) {
            const ucn_native_source_t *actual = &c->sources[src];
            if (actual->address != m->source_address ||
                actual->binding_generation != m->source_binding_generation ||
                actual->principal_digest != m->configured_principal_digest || n->fenced[ch][src] ||
                (c->queue_policy == UCN_NATIVE_KEEP_LATEST && m->origin_sequence <= n->source_high[ch][src])) { continue; }
            if (n->event_order == UINT32_MAX) { n->lock.leave(n->lock.context); return UCN_ENDPOINT_DROP; }
            at = event_slot(n, ch, src, false);
            if (at == UINT16_MAX) { ++n->stats.dropped; n->lock.leave(n->lock.context); return UCN_ENDPOINT_DROP; }
            if (m->origin_sequence > n->source_high[ch][src]) { n->source_high[ch][src] = m->origin_sequence; }
            n->events[at].received = m->receive_timestamp_us; n->events[at].sequence = m->origin_sequence;
            n->events[at].channel = ch; n->events[at].source = src; n->events[at].local = 0U;
            queue_event(n, at, m->payload);
            n->lock.leave(n->lock.context); return UCN_ENDPOINT_ACCEPT;
        }
    }
    ++n->stats.dropped; n->lock.leave(n->lock.context); return UCN_ENDPOINT_DROP;
}

static ucn_result_t validate_channels(ucn_node_t *core, const ucn_native_backend_t *backend, const ucn_native_config_t *cfg, uint32_t local_address)
{
    ucn_static_binding_t binding;
    uint16_t i, j, k;
    ucn_result_t r;
    for (i = 0U; i < cfg->channel_count; ++i) {
        const ucn_native_channel_t *c = &cfg->channels[i];
        if ((backend == NULL && c->security_minimum != UCN_NATIVE_STATIC_TRUSTED) ||
            (backend != NULL && c->security_minimum != UCN_NATIVE_ORIGIN_AUTH && c->security_minimum != UCN_NATIVE_ENCRYPTED)) { return UCN_ERR_UNSUPPORTED; }
        if (!c->id || !c->service_id || c->service_id == UINT16_MAX || !c->schema_id || !c->schema_revision ||
            !c->payload_bytes || c->payload_bytes > UCN_NATIVE_PAYLOAD_BYTES || c->reserved_zero ||
            c->traffic_class < UCN_TRAFFIC_Q1 || c->traffic_class > UCN_TRAFFIC_Q3 ||
            (c->queue_policy != UCN_NATIVE_FIFO && c->queue_policy != UCN_NATIVE_KEEP_LATEST) ||
            !c->max_local_age_us || c->source_count == 0U || c->source_count > UCN_NATIVE_SOURCES || c->target_count > UCN_NATIVE_TARGETS) { return UCN_ERR_CONFIG; }
        if (backend != NULL) {
            r = backend->preflight(backend->context, c);
            if (r != UCN_OK) { return r; }
        }
        for (j = 0U; j < i; ++j) {
            /* Unique service avoids ambiguous schema aliases in this first subset. */
            if (cfg->channels[j].id == c->id || cfg->channels[j].service_id == c->service_id) { return UCN_ERR_CONFIG; }
        }
        for (j = 0U; j < c->source_count; ++j) {
            if (backend != NULL) {
                ucn_native_source_t source = {0U, 0U, 0U};
                r = backend->binding(backend->context, c->sources[j].address, &source);
                binding.binding_generation = source.binding_generation; binding.principal_digest = source.principal_digest;
            } else { r = ucn_get_static_binding(core, c->sources[j].address, &binding); }
            if (r != UCN_OK || binding.binding_generation != c->sources[j].binding_generation || binding.principal_digest != c->sources[j].principal_digest) { return UCN_ERR_CONFIG; }
            for (k = 0U; k < j; ++k) { if (source_equal(c->sources[j], c->sources[k])) { return UCN_ERR_CONFIG; } }
        }
        for (j = 0U; j < c->target_count; ++j) {
            if (c->targets[j].address == local_address) { return UCN_ERR_CONFIG; }
            if (backend != NULL) {
                ucn_native_source_t source = {0U, 0U, 0U};
                r = backend->binding(backend->context, c->targets[j].address, &source);
                binding.binding_generation = source.binding_generation;
            } else { r = ucn_get_static_binding(core, c->targets[j].address, &binding); }
            if (r != UCN_OK || binding.binding_generation != c->targets[j].binding_generation) { return UCN_ERR_CONFIG; }
            for (k = 0U; k < j; ++k) { if (c->targets[k].address == c->targets[j].address) { return UCN_ERR_CONFIG; } }
        }
    }
    return UCN_OK;
}

static ucn_result_t init(void *storage, size_t bytes, ucn_node_t *core,
    const ucn_native_backend_t *backend, const ucn_native_config_t *cfg, ucn_native_t **out)
{
    ucn_node_view_t view;
    ucn_native_t *n = (ucn_native_t *)storage;
    const uint8_t *raw = (const uint8_t *)storage;
    uint16_t i;
    uint16_t services[UCN_NATIVE_CAPACITY];
    size_t b;
    ucn_result_t r;
    if (storage == NULL || cfg == NULL || out == NULL || (core == NULL && backend == NULL) || bytes < UCN_NATIVE_STORAGE_BYTES ||
        (uintptr_t)storage % sizeof(uint64_t) != 0U ||
        overlap(storage, bytes, cfg, sizeof(*cfg)) || overlap(storage, bytes, out, sizeof(*out)) ||
        overlap(storage, bytes, core, ucn_storage_required()) || overlap(cfg, sizeof(*cfg), out, sizeof(*out)) ||
        overlap(core, ucn_storage_required(), out, sizeof(*out)) ||
        overlap(core, ucn_storage_required(), cfg, sizeof(*cfg)) ||
        cfg->channels == NULL || cfg->channel_count == 0U || cfg->channel_count > UCN_NATIVE_CAPACITY ||
        overlap(storage, bytes, cfg->channels, cfg->channel_count * sizeof(*cfg->channels)) ||
        overlap(core, ucn_storage_required(), cfg->channels, cfg->channel_count * sizeof(*cfg->channels)) ||
        overlap(cfg->channels, cfg->channel_count * sizeof(*cfg->channels), out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    if (cfg->api_version != UCN_NATIVE_API_VERSION || cfg->storage_layout != UCN_NATIVE_LAYOUT ||
        cfg->manifest_hash != UCN_NATIVE_MANIFEST_HASH || cfg->instance == 0U || cfg->reserved_zero != 0U ||
        cfg->lock.enter == NULL || cfg->lock.leave == NULL || cfg->lock.struct_size != sizeof(cfg->lock) ||
        cfg->lock.api_version != UCN_API_VERSION) { return UCN_ERR_CONFIG; }
    if (cfg->lock.context != NULL && (overlap(storage, bytes, cfg->lock.context, 1U) ||
        overlap(core, ucn_storage_required(), cfg->lock.context, 1U) || overlap(out, sizeof(*out), cfg->lock.context, 1U))) { return UCN_ERR_ARGUMENT; }
    for (b = 0U; b < UCN_NATIVE_STORAGE_BYTES; ++b) { if (raw[b] != 0U) { return UCN_ERR_STATE; } }
    r = backend != NULL ? backend->local_view(backend->context, &view) : ucn_get_node_view(core, &view);
    if (r != UCN_OK) { return r; }
    if (view.lifecycle != UCN_LIFECYCLE_INITIALIZED || view.endpoint_generation_high_water != 0U || view.endpoint_count != 0U ||
        cfg->channel_count > UCN_ENDPOINT_COUNT || (backend == NULL && !view.trusted_o0_network)) { return UCN_ERR_STATE; }
    r = validate_channels(core, backend, cfg, view.local_address);
    if (r != UCN_OK) { return r; }
    memset(n, 0, sizeof(*n)); n->magic = MAGIC; n->core = core; n->backend = backend; n->lock = cfg->lock;
    if (backend != NULL) { n->backend_context = backend->context; n->backend_context_bytes = backend->context_bytes; }
    n->instance = cfg->instance; n->core_view = view; n->channel_count = cfg->channel_count;
    memcpy(n->channels, cfg->channels, cfg->channel_count * sizeof(*cfg->channels));
    for (i = 0U; i < n->channel_count; ++i) {
        services[i] = n->channels[i].service_id;
    }
    r = backend != NULL ? UCN_OK : ucn_endpoint_install(core, services, n->channel_count, receive_frame, n, n->endpoints);
    if (r != UCN_OK) { memset(n, 0, sizeof(*n)); return r; }
    *out = n; return UCN_OK;
}
ucn_result_t ucn_native_init(void *storage, size_t bytes, ucn_node_t *core,
    const ucn_native_config_t *cfg, ucn_native_t **out)
{ return init(storage, bytes, core, NULL, cfg, out); }
ucn_result_t ucn_native_init_backend(void *storage, size_t bytes,
    const ucn_native_backend_t *b, const ucn_native_config_t *cfg, ucn_native_t **out)
{
    if (b == NULL || b->struct_size != sizeof(*b) || b->api_version != UCN_NATIVE_API_VERSION ||
        b->local_view == NULL || b->binding == NULL || b->start == NULL || b->stop == NULL || b->preflight == NULL ||
        b->step == NULL || b->send == NULL || b->poll == NULL || b->validate == NULL ||
        ((b->context == NULL) != (b->context_bytes == 0U)) ||
        overlap(storage, bytes, b, sizeof(*b)) || overlap(out, sizeof(*out), b, sizeof(*b)) ||
        overlap(storage, bytes, b->context, b->context_bytes) || overlap(out, sizeof(*out), b->context, b->context_bytes) ||
        overlap(cfg, sizeof(*cfg), b->context, b->context_bytes) ||
        (cfg != NULL && overlap(cfg->channels, cfg->channel_count * sizeof(*cfg->channels), b->context, b->context_bytes))) { return UCN_ERR_ARGUMENT; }
    return init(storage, bytes, NULL, b, cfg, out);
}

static ucn_result_t create(ucn_native_t *n, uint16_t id, uint8_t kind,
                            ucn_native_receive_fn fn, void *ctx, ucn_native_handle_t *out)
{
    uint16_t ch, i;
    entity_t *table;
    ucn_native_handle_t h;
    ucn_result_t r;
    if (!external_output(n, out, sizeof(*out)) || (ctx != NULL && overlap(ctx, 1U, out, sizeof(*out)))) { return UCN_ERR_ARGUMENT; }
    r = enter(n); if (r != UCN_OK) { return r; }
    ch = find_channel(n, id);
    if (n->stopping || n->stepping || ch == UINT16_MAX) { return leave(n, UCN_ERR_STATE); }
    table = kind == PUB ? n->pubs : n->subs;
    for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) {
        if (!table[i].open && !table[i].active && table[i].generation != UINT32_MAX) {
            ++table[i].generation; table[i].channel = ch; table[i].open = 1U;
            table[i].receive = fn; table[i].context = ctx;
            memset(&h, 0, sizeof(h)); h.instance = n->instance; h.slot = i;
            h.generation = table[i].generation; h.kind = kind;
            *out = h; return leave(n, UCN_OK);
        }
    }
    return leave(n, UCN_ERR_NO_SPACE);
}
ucn_result_t ucn_publisher_create(ucn_native_t *n, uint16_t id, ucn_native_handle_t *h)
{ return create(n, id, PUB, NULL, NULL, h); }
ucn_result_t ucn_native_check_schema(ucn_native_t *n, uint16_t id, uint32_t schema, uint16_t revision, uint16_t bytes)
{
    uint16_t ch; ucn_result_t r = enter(n); if (r != UCN_OK) { return r; }
    ch = find_channel(n, id);
    if (ch == UINT16_MAX || n->channels[ch].schema_id != schema || n->channels[ch].schema_revision != revision || n->channels[ch].payload_bytes != bytes) { return leave(n, UCN_ERR_CONFIG); }
    return leave(n, UCN_OK);
}
ucn_result_t ucn_subscription_create(ucn_native_t *n, uint16_t id, ucn_native_receive_fn fn, void *ctx, ucn_native_handle_t *h)
{ if (fn == NULL) { return UCN_ERR_ARGUMENT; } return create(n, id, SUB, fn, ctx, h); }
static bool handle_valid(ucn_native_t *n, ucn_native_handle_t h, uint8_t kind)
{
    entity_t *e;
    if (h.instance != n->instance || h.kind != kind || h.reserved_zero || h.slot >= UCN_NATIVE_CAPACITY || !h.generation) { return false; }
    e = kind == PUB ? &n->pubs[h.slot] : &n->subs[h.slot];
    return e->generation == h.generation;
}
static ucn_result_t enqueue(ucn_native_t *n, uint16_t ch, const void *p, size_t bytes, const ucn_native_target_t *target)
{
    uint16_t i, src = UINT16_MAX, at = UINT16_MAX;
    bool borrowed_sample;
    ucn_native_channel_t *c = &n->channels[ch];
    if (!n->running || n->stopping || n->stepping) { return UCN_ERR_STATE; }
    borrowed_sample = n->in_callback && (uintptr_t)p >= (uintptr_t)n->work.app.payload &&
        bytes <= n->work.app.sample.payload_bytes &&
        (uintptr_t)p - (uintptr_t)n->work.app.payload <= n->work.app.sample.payload_bytes - bytes;
    if (p == NULL || bytes != c->payload_bytes || (!borrowed_sample && !external_output(n, p, bytes))) { return UCN_ERR_ARGUMENT; }
    if (UINT64_MAX - n->now < c->max_local_age_us) { return UCN_ERR_EXHAUSTED; }
    if (n->command_order == UINT32_MAX) { return UCN_ERR_EXHAUSTED; }
    for (i = 0U; i < c->source_count; ++i) {
        if (c->sources[i].address == n->core_view.local_address && c->sources[i].binding_generation == n->core_view.local_binding_generation && !n->fenced[ch][i]) { src = i; }
    }
    if (src == UINT16_MAX) { return UCN_ERR_ACCESS; }
    for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) { if (!n->commands[i].valid) { break; } }
    if (i == UCN_NATIVE_CAPACITY) { return UCN_ERR_NO_SPACE; }
    if (target == NULL && recipients(n, ch) != 0U) {
        at = event_slot(n, ch, src, true);
        if (at == UINT16_MAX) { return UCN_ERR_NO_SPACE; }
        if (n->local_sequence == UINT32_MAX || n->event_order == UINT32_MAX) { return UCN_ERR_EXHAUSTED; }
    }
    memset(&n->commands[i], 0, sizeof(n->commands[i]));
    if (n->lifecycle != NULL) { n->lifecycle->cancelled[i] = 0U; }
    n->commands[i].valid = 1U; n->commands[i].channel = ch;
    ++n->command_order; n->commands[i].order = n->command_order;
    n->commands[i].deadline = n->now + c->max_local_age_us;
    n->commands[i].unicast = target != NULL ? 1U : 0U;
    if (target != NULL) { n->commands[i].target = *target; }
    memcpy(n->commands[i].payload, p, bytes);
    if (at != UINT16_MAX) {
        ++n->local_sequence;
        n->events[at].received = n->now; n->events[at].sequence = n->local_sequence;
        n->events[at].channel = ch; n->events[at].source = src; n->events[at].local = 1U;
        queue_event(n, at, p);
    }
    ++n->stats.accepted; return UCN_OK;
}
ucn_result_t ucn_publish(ucn_native_t *n, ucn_native_handle_t h, const void *p, size_t bytes)
{
    ucn_result_t r = enter(n); if (r != UCN_OK) { return r; }
    if (!handle_valid(n, h, PUB) || !n->pubs[h.slot].open) { return leave(n, UCN_ERR_STATE); }
    return leave(n, enqueue(n, n->pubs[h.slot].channel, p, bytes, NULL));
}
ucn_result_t ucn_native_send(ucn_native_t *n, uint16_t id, ucn_native_target_t t, const void *p, size_t bytes)
{
    uint16_t ch, i; ucn_result_t r = enter(n); if (r != UCN_OK) { return r; }
    ch = find_channel(n, id); if (ch == UINT16_MAX) { return leave(n, UCN_ERR_ARGUMENT); }
    for (i = 0U; i < n->channels[ch].target_count; ++i) {
        if (n->channels[ch].targets[i].address == t.address && n->channels[ch].targets[i].binding_generation == t.binding_generation) { return leave(n, enqueue(n, ch, p, bytes, &t)); }
    }
    return leave(n, UCN_ERR_ACCESS);
}
static ucn_result_t close_entity(ucn_native_t *n, ucn_native_handle_t h, uint8_t kind)
{
    uint16_t i; entity_t *e; ucn_result_t r = enter(n); if (r != UCN_OK) { return r; }
    if (!handle_valid(n, h, kind)) { return leave(n, UCN_ERR_STATE); }
    e = kind == PUB ? &n->pubs[h.slot] : &n->subs[h.slot];
    if (!e->open) { return leave(n, UCN_ERR_STATE); }
    e->open = 0U;
    if (kind == SUB) {
        for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) {
            n->events[i].recipients &= ~(UINT32_C(1) << h.slot);
            if (!n->events[i].recipients) { n->events[i].valid = 0U; }
        }
    }
    return leave(n, UCN_OK);
}
ucn_result_t ucn_publisher_close(ucn_native_t *n, ucn_native_handle_t h) { return close_entity(n, h, PUB); }
ucn_result_t ucn_subscription_close(ucn_native_t *n, ucn_native_handle_t h) { return close_entity(n, h, SUB); }
ucn_result_t ucn_subscription_quiescent(ucn_native_t *n, ucn_native_handle_t h, bool *out)
{
    ucn_result_t r;
    if (!external_output(n, out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    r = enter(n); if (r != UCN_OK) { return r; }
    if (!handle_valid(n, h, SUB)) { return leave(n, UCN_ERR_STATE); }
    *out = !n->subs[h.slot].open && !n->subs[h.slot].active;
    return leave(n, UCN_OK);
}
ucn_result_t ucn_native_source_fence(ucn_native_t *n, uint16_t id, ucn_native_source_t source)
{
    uint16_t ch, i, j; ucn_result_t r = enter(n); if (r != UCN_OK) { return r; }
    if (n->stepping) { return leave(n, UCN_ERR_STATE); }
    ch = find_channel(n, id); if (ch == UINT16_MAX) { return leave(n, UCN_ERR_ARGUMENT); }
    for (i = 0U; i < n->channels[ch].source_count; ++i) {
        if (source_equal(source, n->channels[ch].sources[i])) {
            n->fenced[ch][i] = 1U;
            for (j = 0U; j < UCN_NATIVE_CAPACITY; ++j) {
                if (n->events[j].channel == ch && n->events[j].source == i) { n->events[j].valid = 0U; }
            }
            return leave(n, UCN_OK);
        }
    }
    return leave(n, UCN_ERR_NOT_FOUND);
}

ucn_result_t ucn_native_start(ucn_native_t *n, uint64_t now)
{
    ucn_result_t r = enter(n); if (r != UCN_OK) { return r; }
    if (n->running || n->stepping || n->stopping) { return leave(n, UCN_ERR_STATE); }
    n->stepping = 1U; leave(n, UCN_OK);
    r = n->backend != NULL ? n->backend->start(n->backend->context, now) : ucn_start(n->core, now);
    if (enter(n) != UCN_OK) { return UCN_ERR_STATE; }
    n->stepping = 0U;
    if (r == UCN_OK) { n->running = 1U; n->now = now; }
    return leave(n, r);
}
/* Only step owns commands. It drops the Native lock before every Core call.
 * 仅step改动已接纳命令；每次Core调用前释放Native短锁。 */
static UCN_I_STACK_PHASE bool prepare_command(ucn_native_t *n, uint16_t i)
{
    command_t *cmd = &n->commands[i];
    ucn_native_channel_t *c = &n->channels[cmd->channel];
    uint16_t older;
    bool source_blocked = true;
    for (older = 0U; older < c->source_count; ++older) {
        if (c->sources[older].address == n->core_view.local_address && !n->fenced[cmd->channel][older]) { source_blocked = false; }
    }
    for (older = 0U; older < UCN_NATIVE_CAPACITY; ++older) {
        if (n->commands[older].valid && n->commands[older].channel == cmd->channel && n->commands[older].order < cmd->order) { return false; }
    }
    while (cmd->next_target < (cmd->unicast ? 1U : c->target_count) && n->lifecycle != NULL &&
        (n->lifecycle->cancelled[i] & (UINT32_C(1) << cmd->next_target))) { ++cmd->next_target; }
    if (cmd->next_target >= (cmd->unicast ? 1U : c->target_count)) { cmd->valid = 0U; return false; }
    if (source_blocked || n->stopping || n->now >= cmd->deadline) {
        for (older = cmd->next_target; older < (cmd->unicast ? 1U : c->target_count); ++older) {
            if (n->lifecycle == NULL || !(n->lifecycle->cancelled[i] & (UINT32_C(1) << older))) { ++n->stats.failed; }
        }
        cmd->valid = 0U; return false;
    }
    {
        ucn_target_t *target = &n->work.send.target;
        ucn_send_options_t *opt = &n->work.send.options;
        n->work.send.native_target = cmd->unicast ? cmd->target : c->targets[cmd->next_target];
        memset(target, 0, sizeof(*target)); target->struct_size = sizeof(*target); target->api_version = UCN_API_VERSION;
        target->address = n->work.send.native_target.address;
        target->binding_generation = n->work.send.native_target.binding_generation; target->service_id = c->service_id;
        memset(opt, 0, sizeof(*opt)); opt->struct_size = sizeof(*opt); opt->api_version = UCN_API_VERSION;
        opt->copy_payload = 1U; opt->hop_limit = 4U; opt->traffic_class = c->traffic_class; opt->absolute_deadline_us = cmd->deadline;
        return true;
    }
}
/* Preparation has returned before this synchronous submission. The actor
 * never carries table-scan temporaries across Core/Backend/Driver calls.
 * 准备阶段已返回，不将扫描临时量压在Core/Backend/Driver调用链之上。 */
static UCN_I_STACK_PHASE void send_prepared(ucn_native_t *n, uint16_t i)
{
    command_t *cmd = &n->commands[i];
    const ucn_native_channel_t *channel = &n->channels[cmd->channel];
    ucn_result_t r = n->backend != NULL ? n->backend->send(n->backend->context, channel,
        n->work.send.native_target, cmd->payload, n->now, cmd->deadline) :
        ucn_send(n->core, &n->work.send.target, cmd->payload, channel->payload_bytes,
                 &n->work.send.options, NULL);
    if (r == UCN_OK) { ++cmd->next_target; ++n->stats.core_admitted; }
    else if (r != UCN_ERR_NO_SPACE && r != UCN_ERR_STATE) { ++n->stats.failed; ++cmd->next_target; }
}

/* EN: Backend already verified bytes/replay. Native additionally binds channel
 * and exact live scope; it never accepts an application 'authenticated' flag.
 * ZH: 后端已验证字节和Replay；Facade另核验通道和当前Scope。 */
static UCN_I_STACK_PHASE void poll_backend(ucn_native_t *n)
{
    ucn_native_backend_message_t *m = &n->work.rx.message;
    uint16_t ch, src, at;
    uint8_t principal[16];
    if (enter(n) != UCN_OK) { return; }
    for (ch = 0U; ch < n->channel_count; ++ch) {
        const ucn_native_channel_t *c = &n->channels[ch];
        if (c->service_id != m->service_id || c->payload_bytes != m->payload_bytes || c->traffic_class != m->traffic_class ||
            m->protection < c->security_minimum || m->protection > UCN_NATIVE_ENCRYPTED || !m->origin_sequence ||
            !m->scope.instance || !m->scope.generation || m->scope.reserved_zero) { continue; }
        for (src = 0U; src < c->source_count; ++src) {
            if (!source_equal(c->sources[src], m->source) || n->fenced[ch][src] || n->stopping) { continue; }
            leave(n, UCN_OK);
            if (n->backend->validate(n->backend->context, &m->scope, &m->source, c->service_id,
                c->security_minimum, n->now, principal) != UCN_OK) { return; }
            if (enter(n) != UCN_OK) { return; }
            if (n->lifecycle != NULL) {
                ucn_native_scope_t *prior = &n->lifecycle->source[ch][src];
                if (prior->instance != 0U && (!scope_namespace(*prior, m->scope) ||
                    m->scope.generation < prior->generation ||
                    (scope_equal(*prior, m->scope) && n->lifecycle->retired_source[ch][src]))) {
                    leave(n, UCN_OK); return;
                }
                if (!scope_equal(*prior, m->scope)) {
                    *prior = m->scope; n->source_high[ch][src] = 0U;
                    n->lifecycle->retired_source[ch][src] = 0U;
                }
            }
            if (c->queue_policy == UCN_NATIVE_KEEP_LATEST && m->origin_sequence <= n->source_high[ch][src]) {
                leave(n, UCN_OK); return;
            }
            at = event_slot(n, ch, src, false);
            if (at == UINT16_MAX || n->event_order == UINT32_MAX) { ++n->stats.dropped; leave(n, UCN_OK); return; }
            if (m->origin_sequence > n->source_high[ch][src]) { n->source_high[ch][src] = m->origin_sequence; }
            n->events[at].received = m->receive_time_us; n->events[at].sequence = m->origin_sequence;
            n->events[at].channel = ch; n->events[at].source = src; n->events[at].local = 0U;
            queue_event(n, at, n->work.rx.payload);
            n->events[at].scope = m->scope;
            n->events[at].protection = m->protection;
            leave(n, UCN_OK); return;
        }
    }
    ++n->stats.dropped; leave(n, UCN_OK);
}
ucn_result_t ucn_native_step(ucn_native_t *n, uint64_t now, uint16_t budget)
{
    uint16_t k, i; ucn_result_t r;
    r = enter(n); if (r != UCN_OK) { return r; }
    if (!budget || n->stepping || n->dispatching || (!n->running && !n->stopping) || now < n->now) { return leave(n, UCN_ERR_STATE); }
    n->stepping = 1U; n->now = now; leave(n, UCN_OK);
    if (n->stopping && !n->stop_applied) {
        r = n->backend != NULL ? n->backend->stop(n->backend->context) : ucn_stop(n->core); if (r == UCN_OK) { n->stop_applied = 1U; }
    }
    for (k = 0U; k < budget; ++k) {
        i = n->command_cursor; n->command_cursor = (uint16_t)((i + 1U) % UCN_NATIVE_CAPACITY);
        if (n->commands[i].valid && prepare_command(n, i)) {
            send_prepared(n, i);
        }
    }
    memset(&n->work.core.budget, 0, sizeof(n->work.core.budget));
    n->work.core.budget.struct_size = sizeof(n->work.core.budget);
    n->work.core.budget.api_version = UCN_API_VERSION; n->work.core.budget.max_work = budget;
    r = n->backend != NULL ? n->backend->local_view(n->backend->context, &n->core_view) : ucn_get_node_view(n->core, &n->core_view);
    if (r == UCN_OK && n->core_view.lifecycle != UCN_LIFECYCLE_QUIESCENT) {
        r = n->backend != NULL ? n->backend->step(n->backend->context, n->now, budget) :
            ucn_step(n->core, n->now, &n->work.core.budget, &n->work.core.result);
        if (r == UCN_OK && n->backend != NULL && !n->stopping) {
            for (k = 0U; k < budget; ++k) {
                /* Authenticate/pull and application admission are disjoint
                 * phases: no admission-frame spills above the crypto stack.
                 * 认证拉取与应用接纳分阶段，不让接纳帧叠在密码调用链上。 */
                if (n->backend->poll(n->backend->context, &n->work.rx.message,
                    n->work.rx.payload, sizeof(n->work.rx.payload)) == UCN_OK) { poll_backend(n); }
            }
        }
    }
    if (enter(n) != UCN_OK) { return UCN_ERR_STATE; }
    n->stepping = 0U;
    if (r != UCN_OK) {
        n->stopping = 1U;
        for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) { n->events[i].valid = 0U; }
    }
    return leave(n, r);
}
ucn_result_t ucn_native_dispatch(ucn_native_t *n, uint64_t now, uint16_t budget, uint16_t *out)
{
    uint16_t count = 0U, work;
    ucn_result_t r;
    if (!external_output(n, out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    r = enter(n); if (r != UCN_OK) { return r; }
    if (!budget || n->stepping || n->dispatching || now < n->now) { return leave(n, UCN_ERR_STATE); }
    n->dispatching = 1U; n->now = now;
    for (work = 0U; work < budget; ++work) {
        uint16_t scan, at = UINT16_MAX, sub = UINT16_MAX, j;
        ucn_native_sample_t *sample = &n->work.app.sample;
        uint32_t subscription_generation;
        ucn_native_receive_fn fn; void *ctx;
        for (scan = 0U; scan < UCN_NATIVE_CAPACITY; ++scan) {
            uint16_t e = (uint16_t)((n->event_cursor + scan) % UCN_NATIVE_CAPACITY);
            event_t *ev = &n->events[e];
            if (!ev->valid) { continue; }
            if (n->channels[ev->channel].queue_policy == UCN_NATIVE_FIFO) {
                bool blocked = false;
                uint16_t previous;
                for (previous = 0U; previous < UCN_NATIVE_CAPACITY; ++previous) {
                    const event_t *p = &n->events[previous];
                    if (p->valid && p->channel == ev->channel && p->source == ev->source && p->local == ev->local && p->order < ev->order) { blocked = true; break; }
                }
                if (blocked) { continue; }
            }
            if ((!ev->local && n->lifecycle != NULL &&
                (n->lifecycle->retired_source[ev->channel][ev->source] ||
                !scope_equal(n->lifecycle->source[ev->channel][ev->source], ev->scope))) ||
                n->stopping || n->fenced[ev->channel][ev->source] || now < ev->age_anchor ||
                now - ev->age_anchor >= n->channels[ev->channel].max_local_age_us) { ev->valid = 0U; ++n->stats.dropped; continue; }
            /* IRQ capture may occur after step's clock snapshot. Do not deliver
             * before capture, and never extend lifetime past local admission. */
            if (now < ev->received) { continue; }
            for (j = 0U; j < UCN_NATIVE_CAPACITY; ++j) {
                uint16_t s = (uint16_t)((n->sub_cursor + j) % UCN_NATIVE_CAPACITY);
                if (ev->recipients & (UINT32_C(1) << s)) { sub = s; break; }
            }
            if (sub != UINT16_MAX) { at = e; break; }
        }
        if (at == UINT16_MAX) { break; }
        {
            event_t *e = &n->events[at];
            subscription_generation = n->subs[sub].generation;
            sample->source = n->channels[e->channel].sources[e->source]; sample->receive_time_us = e->received;
            sample->origin_sequence = e->sequence; sample->channel_id = n->channels[e->channel].id;
            sample->payload_bytes = n->channels[e->channel].payload_bytes;
            sample->provenance = e->protection;
            memset(sample->principal, 0, sizeof(sample->principal));
            if (n->backend != NULL) {
                leave(n, UCN_OK);
                r = n->backend->validate(n->backend->context, &e->scope, &sample->source,
                    n->channels[e->channel].service_id, n->channels[e->channel].security_minimum,
                    now, sample->principal);
                if (enter(n) != UCN_OK) { return UCN_ERR_STATE; }
                if (r != UCN_OK || !e->valid || n->stopping || n->fenced[e->channel][e->source]) {
                    e->valid = 0U; ++n->stats.dropped; continue;
                }
                if (!n->subs[sub].open || n->subs[sub].generation != subscription_generation ||
                    !(e->recipients & (UINT32_C(1) << sub))) { continue; }
            }
            memcpy(n->work.app.payload, e->payload, sample->payload_bytes); sample->payload = n->work.app.payload;
            e->recipients &= ~(UINT32_C(1) << sub); if (!e->recipients) { e->valid = 0U; }
        }
        n->event_cursor = (uint16_t)((at + 1U) % UCN_NATIVE_CAPACITY); n->sub_cursor = (uint16_t)((sub + 1U) % UCN_NATIVE_CAPACITY);
        fn = n->subs[sub].receive; ctx = n->subs[sub].context; n->subs[sub].active = 1U;
        n->in_callback = 1U;
        leave(n, UCN_OK); fn(ctx, sample);
        if (enter(n) != UCN_OK) { return UCN_ERR_STATE; }
        n->in_callback = 0U;
        n->subs[sub].active = 0U; ++n->stats.delivered; ++count;
    }
    n->dispatching = 0U; *out = count; return leave(n, UCN_OK);
}
ucn_result_t ucn_native_stop(ucn_native_t *n)
{
    uint16_t i;
    ucn_result_t r = enter(n); if (r != UCN_OK) { return r; }
    if (n->stepping) { return leave(n, UCN_ERR_STATE); }
    n->stopping = 1U;
    for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) { n->events[i].valid = 0U; }
    return leave(n, UCN_OK);
}
ucn_result_t ucn_native_status(ucn_native_t *n, ucn_native_status_t *out)
{
    uint16_t i; ucn_result_t r;
    if (!external_output(n, out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    r = enter(n); if (r != UCN_OK) { return r; }
    if (n->stepping) { return leave(n, UCN_ERR_STATE); }
    *out = n->stats; out->pending_commands = 0U; out->pending_events = 0U;
    for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) { out->pending_commands += n->commands[i].valid ? 1U : 0U; out->pending_events += n->events[i].valid ? 1U : 0U; }
    out->stopping = n->stopping; out->core_quiescent = n->core_view.lifecycle == UCN_LIFECYCLE_QUIESCENT ? 1U : 0U;
    return leave(n, UCN_OK);
}
ucn_result_t ucn_native_deinit(ucn_native_t *n)
{
    uint16_t i; ucn_result_t r = enter(n);
    if (r != UCN_OK) { return r; }
    if (n->stepping || n->dispatching || !n->stop_applied || n->core_view.lifecycle != UCN_LIFECYCLE_QUIESCENT) { return leave(n, UCN_ERR_STATE); }
    for (i = 0U; i < UCN_NATIVE_CAPACITY; ++i) { if (n->commands[i].valid || n->subs[i].active) { return leave(n, UCN_ERR_STATE); } }
    n->stepping = 1U; leave(n, UCN_OK);
    if (n->backend != NULL) {
        if (enter(n) != UCN_OK) { return UCN_ERR_STATE; }
        n->running = 0U; n->stepping = 0U; n->dead = 1U; return leave(n, UCN_OK);
    }
    for (i = 0U; i < n->channel_count; ++i) {
        r = ucn_endpoint_remove(n->core, n->endpoints[i]); if (r != UCN_OK) {
            if (enter(n) != UCN_OK) { return UCN_ERR_STATE; }
            n->stepping = 0U; return leave(n, r);
        }
    }
    r = ucn_deinit(n->core); if (r != UCN_OK) {
        if (enter(n) != UCN_OK) { return UCN_ERR_STATE; }
        n->stepping = 0U; return leave(n, r);
    }
    /* Retain fence/magic: this Root cannot be reset to resurrect old handles. */
    if (enter(n) != UCN_OK) { return UCN_ERR_STATE; }
    n->running = 0U; n->stepping = 0U; n->dead = 1U;
    return leave(n, UCN_OK);
}
