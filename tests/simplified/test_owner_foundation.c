#include "internal/ucn_owner.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition_) do { \
    if (!(condition_)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition_); \
        return 1; \
    } \
} while (0)

typedef struct fake_lock {
    bool locked;
    bool fail_next;
    uint32_t enter_count;
    uint32_t leave_count;
} fake_lock_t;

static ucn_result_t fake_lock_enter(void *context)
{
    fake_lock_t *lock = (fake_lock_t *)context;

    if (lock == NULL || lock->locked || lock->fail_next) {
        if (lock != NULL) {
            lock->fail_next = false;
        }
        return UCN_ERR_STATE;
    }
    lock->locked = true;
    ++lock->enter_count;
    return UCN_OK;
}

static void fake_lock_leave(void *context)
{
    fake_lock_t *lock = (fake_lock_t *)context;

    if (lock != NULL && lock->locked) {
        lock->locked = false;
        ++lock->leave_count;
    }
}

static ucn_i_lock_ops_t make_lock_ops(fake_lock_t *lock)
{
    ucn_i_lock_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.struct_size = (uint16_t)sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = fake_lock_enter;
    ops.leave = fake_lock_leave;
    return ops;
}

static int test_callback_gate(void)
{
    fake_lock_t lock = {0};
    ucn_i_lock_ops_t ops = make_lock_ops(&lock);
    ucn_i_callback_gate_t gate = {0};
    ucn_i_callback_gate_t before;
    ucn_i_callback_claim_t first = {11U, 21U, 1U, 7U};
    ucn_i_callback_claim_t second = {12U, 22U, 1U, 8U};
    ucn_i_callback_claim_t observed = {0};
    bool active = false;

    ops.context = &gate;
    CHECK(ucn_i_callback_gate_init(&gate, 5U, &ops) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&gate, &(ucn_i_callback_gate_t){0}, sizeof(gate)) == 0);
    ops.context = &lock;
    CHECK(ucn_i_callback_gate_init(&gate, 5U, &ops) == UCN_OK);
    before = gate;
    CHECK(ucn_i_callback_gate_init(&gate, 5U, &ops) == UCN_ERR_STATE);
    CHECK(memcmp(&gate, &before, sizeof(gate)) == 0);
    CHECK(ucn_i_callback_gate_enter(&gate, &first) == UCN_OK);
    before = gate;
    CHECK(ucn_i_callback_gate_enter(&gate, &second) == UCN_ERR_STATE);
    CHECK(memcmp(&gate, &before, sizeof(gate)) == 0);
    CHECK(ucn_i_callback_gate_leave(&gate, &second) == UCN_ERR_STATE);
    CHECK(memcmp(&gate, &before, sizeof(gate)) == 0);
    CHECK(ucn_i_callback_gate_destroy(&gate) == UCN_ERR_STATE);
    CHECK(memcmp(&gate, &before, sizeof(gate)) == 0);
    CHECK(ucn_i_callback_gate_view(&gate, &active, &observed) == UCN_OK);
    CHECK(active && memcmp(&observed, &first, sizeof(first)) == 0);
    CHECK(ucn_i_callback_gate_leave(&gate, &first) == UCN_OK);
    CHECK(ucn_i_callback_gate_enter(&gate, &second) == UCN_OK);
    CHECK(ucn_i_callback_gate_leave(&gate, &second) == UCN_OK);
    lock.fail_next = true;
    before = gate;
    CHECK(ucn_i_callback_gate_enter(&gate, &first) == UCN_ERR_STATE);
    CHECK(memcmp(&gate, &before, sizeof(gate)) == 0);
    CHECK(ucn_i_callback_gate_destroy(&gate) == UCN_OK);
    CHECK(memcmp(&gate, &(ucn_i_callback_gate_t){0}, sizeof(gate)) == 0);
    CHECK(!lock.locked && lock.enter_count == lock.leave_count);
    return 0;
}

static int test_owner_mailbox(void)
{
    fake_lock_t lock = {0};
    ucn_i_lock_ops_t ops = make_lock_ops(&lock);
    ucn_i_owner_mailbox_t mailbox = {0};
    ucn_i_owner_mailbox_t before;
    ucn_i_owner_work_hint_t hint;
    const ucn_i_owner_work_hint_t sentinel = {
        UINT32_C(0xA5A5A5A5), UINT8_C(0xA5), {0xA5U, 0xA5U, 0xA5U}
    };

    ops.context = &mailbox;
    CHECK(ucn_i_owner_mailbox_init(&mailbox,
          UCN_I_OWNER_WORK_CLASS_LIMIT, &ops) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&mailbox, &(ucn_i_owner_mailbox_t){0}, sizeof(mailbox)) == 0);
    ops.context = &lock;
    CHECK(ucn_i_owner_mailbox_init(&mailbox,
          UCN_I_OWNER_WORK_CLASS_LIMIT, &ops) == UCN_OK);
    hint = sentinel;
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &hint) == UCN_ERR_NOT_FOUND);
    CHECK(memcmp(&hint, &sentinel, sizeof(hint)) == 0);

    CHECK(ucn_i_owner_mailbox_publish(
          &mailbox, UCN_I_OWNER_WORK_COMPLETION) == UCN_OK);
    CHECK(ucn_i_owner_mailbox_publish(
          &mailbox, UCN_I_OWNER_WORK_COMPLETION) == UCN_OK);
    CHECK(ucn_i_owner_mailbox_publish(
          &mailbox, UCN_I_OWNER_WORK_INVALIDATION) == UCN_OK);
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &hint) == UCN_OK);
    CHECK(hint.work_class == UCN_I_OWNER_WORK_COMPLETION &&
          hint.occurrences == 2U);

    /* A continuously refreshed class zero cannot starve the older class one. */
    CHECK(ucn_i_owner_mailbox_publish(
          &mailbox, UCN_I_OWNER_WORK_COMPLETION) == UCN_OK);
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &hint) == UCN_OK);
    CHECK(hint.work_class == UCN_I_OWNER_WORK_INVALIDATION &&
          hint.occurrences == 1U);
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &hint) == UCN_OK);
    CHECK(hint.work_class == UCN_I_OWNER_WORK_COMPLETION &&
          hint.occurrences == 1U);

    mailbox.pending_mask = UINT32_C(1) << UCN_I_OWNER_WORK_REQUEST;
    mailbox.occurrences[UCN_I_OWNER_WORK_REQUEST] = UINT32_MAX;
    CHECK(ucn_i_owner_mailbox_publish(
          &mailbox, UCN_I_OWNER_WORK_REQUEST) == UCN_OK);
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &hint) == UCN_OK);
    CHECK(hint.work_class == UCN_I_OWNER_WORK_REQUEST &&
          hint.occurrences == UINT32_MAX);

    before = mailbox;
    hint = sentinel;
    lock.fail_next = true;
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &hint) == UCN_ERR_STATE);
    CHECK(memcmp(&mailbox, &before, sizeof(mailbox)) == 0);
    CHECK(memcmp(&hint, &sentinel, sizeof(hint)) == 0);
    CHECK(ucn_i_owner_mailbox_publish(&mailbox,
          UCN_I_OWNER_WORK_CLASS_LIMIT) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&mailbox, &before, sizeof(mailbox)) == 0);

    CHECK(ucn_i_owner_mailbox_publish(
          &mailbox, UCN_I_OWNER_WORK_TIMER) == UCN_OK);
    before = mailbox;
    CHECK(ucn_i_owner_mailbox_destroy(&mailbox) == UCN_ERR_STATE);
    CHECK(memcmp(&mailbox, &before, sizeof(mailbox)) == 0);
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &hint) == UCN_OK);
    CHECK(hint.work_class == UCN_I_OWNER_WORK_TIMER);
    CHECK(ucn_i_owner_mailbox_destroy(&mailbox) == UCN_OK);
    CHECK(memcmp(&mailbox, &(ucn_i_owner_mailbox_t){0}, sizeof(mailbox)) == 0);
    CHECK(!lock.locked && lock.enter_count == lock.leave_count);
    return 0;
}

int main(void)
{
    CHECK(test_callback_gate() == 0);
    CHECK(test_owner_mailbox() == 0);
    puts("UCN simplified Owner foundation tests passed");
    return 0;
}
