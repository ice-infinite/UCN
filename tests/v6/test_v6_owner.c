#include "ucn/v6/ucn_v6_owner.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

typedef struct fake_lock {
    bool locked;
    unsigned task_notifications;
    unsigned isr_notifications;
} fake_lock_t;

typedef struct fake_handler {
    ucn_v6_protocol_owner_t *owner;
    ucn_v6_owner_event_t seen[16];
    size_t seen_count;
    size_t fail_at;
    ucn_v6_result_t nested_result;
} fake_handler_t;

static bool fake_try_lock(void *context, bool from_isr)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    (void)from_isr;
    if (lock->locked) {
        return false;
    }
    lock->locked = true;
    return true;
}

static void fake_unlock(void *context, bool from_isr)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    (void)from_isr;
    lock->locked = false;
}

static void fake_notify(void *context, bool from_isr)
{
    fake_lock_t *lock = (fake_lock_t *)context;
    if (from_isr) {
        ++lock->isr_notifications;
    } else {
        ++lock->task_notifications;
    }
}

static ucn_v6_result_t record_event(void *context, ucn_v6_owner_event_t event)
{
    fake_handler_t *handler = (fake_handler_t *)context;
    uint16_t nested_processed = UINT16_MAX;

    if (handler->seen_count < sizeof(handler->seen) / sizeof(handler->seen[0])) {
        handler->seen[handler->seen_count] = event;
    }
    ++handler->seen_count;
    if (handler->seen_count == 1U) {
        handler->nested_result = ucn_v6_protocol_owner_run(
            handler->owner, 1U, record_event, handler, &nested_processed);
        if (handler->nested_result != UCN_V6_ERR_STATE ||
            nested_processed != UINT16_MAX) {
            return UCN_V6_ERR_STATE;
        }
    }
    if (handler->fail_at != 0U && handler->seen_count == handler->fail_at) {
        return UCN_V6_ERR_TIMEOUT;
    }
    return UCN_V6_OK;
}

static ucn_v6_owner_lock_ops_t make_lock_ops(fake_lock_t *lock)
{
    ucn_v6_owner_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = lock;
    ops.try_lock = fake_try_lock;
    ops.unlock = fake_unlock;
    ops.notify = fake_notify;
    return ops;
}

static int test_init_preflight_is_non_destructive(void)
{
    ucn_v6_protocol_owner_storage_t storage;
    ucn_v6_protocol_owner_storage_t before;
    ucn_v6_feature_manifest_t bad_manifest = *ucn_v6_compiled_manifest();
    fake_lock_t lock;
    ucn_v6_owner_lock_ops_t ops;
    ucn_v6_protocol_owner_t *owner = NULL;

    memset(&lock, 0, sizeof(lock));
    ops = make_lock_ops(&lock);
    memset(&storage, 0xA5, sizeof(storage));
    before = storage;
    bad_manifest.layout_hash ^= UINT64_C(1);
    CHECK(ucn_v6_protocol_owner_init_in_place(
              storage.bytes, sizeof(storage), &bad_manifest, &ops,
              &owner) == UCN_V6_ERR_CONFIG);
    CHECK(owner == NULL);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);
    CHECK(ucn_v6_protocol_owner_init_in_place(
              storage.bytes + 1U, sizeof(storage) - 1U,
              ucn_v6_compiled_manifest(), &ops,
              &owner) == UCN_V6_ERR_CONFIG);
    CHECK(memcmp(&storage, &before, sizeof(storage)) == 0);
    return 0;
}

static int test_owner_event_lifecycle(void)
{
    ucn_v6_protocol_owner_storage_t storage;
    fake_lock_t lock;
    ucn_v6_owner_lock_ops_t ops;
    ucn_v6_protocol_owner_t *owner = NULL;
    ucn_v6_protocol_owner_view_t view;
    fake_handler_t handler;
    uint16_t processed = UINT16_MAX;

    memset(&lock, 0, sizeof(lock));
    memset(&handler, 0, sizeof(handler));
    ops = make_lock_ops(&lock);
    CHECK(ucn_v6_protocol_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &owner) == UCN_V6_OK);
    handler.owner = owner;
    CHECK(ucn_v6_protocol_owner_post(
              owner, UCN_V6_OWNER_EVENT_RX, true) == UCN_V6_OK);
    CHECK(ucn_v6_protocol_owner_post(
              owner, UCN_V6_OWNER_EVENT_RX, false) == UCN_V6_OK);
    CHECK(ucn_v6_protocol_owner_post(
              owner, UCN_V6_OWNER_EVENT_TIMER, false) == UCN_V6_OK);
    CHECK(lock.isr_notifications == 1U && lock.task_notifications == 2U);

    handler.fail_at = 2U;
    CHECK(ucn_v6_protocol_owner_run(
              owner, 3U, record_event, &handler,
              &processed) == UCN_V6_ERR_TIMEOUT);
    CHECK(processed == 1U);
    CHECK(handler.seen[0] == UCN_V6_OWNER_EVENT_RX);
    CHECK(handler.seen[1] == UCN_V6_OWNER_EVENT_TIMER);
    CHECK(ucn_v6_protocol_owner_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(!view.running && view.pending_total == 2U);
    CHECK(view.pending_by_event[UCN_V6_OWNER_EVENT_TIMER - 1U] == 1U);

    memset(&handler, 0, sizeof(handler));
    handler.owner = owner;
    CHECK(ucn_v6_protocol_owner_run(
              owner, 8U, record_event, &handler,
              &processed) == UCN_V6_OK);
    CHECK(processed == 2U);
    CHECK(handler.seen[0] == UCN_V6_OWNER_EVENT_TIMER);
    CHECK(handler.seen[1] == UCN_V6_OWNER_EVENT_RX);
    CHECK(ucn_v6_protocol_owner_copy_view(owner, &view) == UCN_V6_OK);
    CHECK(view.pending_total == 0U && !view.faulted);
    return 0;
}

static int test_fixed_event_capacity_and_corruption(void)
{
    ucn_v6_protocol_owner_storage_t storage;
    fake_lock_t lock;
    ucn_v6_owner_lock_ops_t ops;
    ucn_v6_protocol_owner_t *owner = NULL;
    size_t index;

    memset(&lock, 0, sizeof(lock));
    ops = make_lock_ops(&lock);
    CHECK(ucn_v6_protocol_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &owner) == UCN_V6_OK);
    for (index = 0U; index < UCN_V6_CONFIG_OWNER_EVENT_DEPTH; ++index) {
        CHECK(ucn_v6_protocol_owner_post(
                  owner, UCN_V6_OWNER_EVENT_PROVIDER, false) == UCN_V6_OK);
    }
    CHECK(ucn_v6_protocol_owner_post(
              owner, UCN_V6_OWNER_EVENT_PROVIDER,
              false) == UCN_V6_ERR_NO_SPACE);
    storage.bytes[0] ^= 1U;
    CHECK(ucn_v6_protocol_owner_post(
              owner, UCN_V6_OWNER_EVENT_RX, false) == UCN_V6_ERR_STATE);
    return 0;
}

int main(void)
{
    CHECK(test_init_preflight_is_non_destructive() == 0);
    CHECK(test_owner_event_lifecycle() == 0);
    CHECK(test_fixed_event_capacity_and_corruption() == 0);
    puts("ucn v6 protocol owner tests passed");
    return 0;
}
