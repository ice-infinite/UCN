#include "internal/ucn_owner.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition_) do { \
    if (!(condition_)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition_); \
        return 1; \
    } \
} while (0)

typedef struct pthread_lock {
    pthread_mutex_t mutex;
} pthread_lock_t;

typedef struct gate_thread_input {
    ucn_i_callback_gate_t *gate;
    ucn_i_callback_claim_t claim;
    ucn_result_t result;
} gate_thread_input_t;

typedef struct mailbox_thread_input {
    ucn_i_owner_mailbox_t *mailbox;
    ucn_i_owner_work_class_t work_class;
    uint32_t iterations;
    ucn_result_t result;
} mailbox_thread_input_t;

static ucn_result_t pthread_lock_enter(void *context)
{
    pthread_lock_t *lock = (pthread_lock_t *)context;

    return lock != NULL && pthread_mutex_lock(&lock->mutex) == 0 ?
           UCN_OK : UCN_ERR_STATE;
}

static void pthread_lock_leave(void *context)
{
    pthread_lock_t *lock = (pthread_lock_t *)context;

    if (lock != NULL) {
        (void)pthread_mutex_unlock(&lock->mutex);
    }
}

static ucn_i_lock_ops_t make_lock_ops(pthread_lock_t *lock)
{
    ucn_i_lock_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.struct_size = (uint16_t)sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = pthread_lock_enter;
    ops.leave = pthread_lock_leave;
    return ops;
}

static void *gate_thread(void *argument)
{
    gate_thread_input_t *input = (gate_thread_input_t *)argument;

    input->result = ucn_i_callback_gate_enter(input->gate, &input->claim);
    return NULL;
}

static void *mailbox_thread(void *argument)
{
    mailbox_thread_input_t *input = (mailbox_thread_input_t *)argument;
    uint32_t index;

    input->result = UCN_OK;
    for (index = 0U; index < input->iterations; ++index) {
        ucn_result_t result = ucn_i_owner_mailbox_publish(
            input->mailbox, input->work_class);
        if (result != UCN_OK) {
            input->result = result;
            break;
        }
    }
    return NULL;
}

int main(void)
{
    pthread_lock_t gate_lock;
    pthread_lock_t mailbox_lock;
    ucn_i_lock_ops_t gate_ops;
    ucn_i_lock_ops_t mailbox_ops;
    ucn_i_callback_gate_t gate = {0};
    ucn_i_owner_mailbox_t mailbox = {0};
    gate_thread_input_t gate_a = {&gate, {1U, 1U, 1U, 1U}, UCN_ERR_STATE};
    gate_thread_input_t gate_b = {&gate, {2U, 2U, 1U, 1U}, UCN_ERR_STATE};
    mailbox_thread_input_t mailbox_a = {
        &mailbox, UCN_I_OWNER_WORK_COMPLETION, 10000U, UCN_ERR_STATE};
    mailbox_thread_input_t mailbox_b = {
        &mailbox, UCN_I_OWNER_WORK_TIMER, 10000U, UCN_ERR_STATE};
    ucn_i_owner_work_hint_t first;
    ucn_i_owner_work_hint_t second;
    pthread_t thread_a;
    pthread_t thread_b;

    CHECK(pthread_mutex_init(&gate_lock.mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&mailbox_lock.mutex, NULL) == 0);
    gate_ops = make_lock_ops(&gate_lock);
    mailbox_ops = make_lock_ops(&mailbox_lock);
    CHECK(ucn_i_callback_gate_init(&gate, 1U, &gate_ops) == UCN_OK);
    CHECK(pthread_create(&thread_a, NULL, gate_thread, &gate_a) == 0);
    CHECK(pthread_create(&thread_b, NULL, gate_thread, &gate_b) == 0);
    CHECK(pthread_join(thread_a, NULL) == 0);
    CHECK(pthread_join(thread_b, NULL) == 0);
    CHECK((gate_a.result == UCN_OK && gate_b.result == UCN_ERR_STATE) ||
          (gate_b.result == UCN_OK && gate_a.result == UCN_ERR_STATE));
    CHECK(ucn_i_callback_gate_leave(
          &gate, gate_a.result == UCN_OK ? &gate_a.claim : &gate_b.claim) ==
          UCN_OK);
    CHECK(ucn_i_callback_gate_destroy(&gate) == UCN_OK);

    CHECK(ucn_i_owner_mailbox_init(&mailbox,
          UCN_I_OWNER_WORK_CLASS_LIMIT, &mailbox_ops) == UCN_OK);
    CHECK(pthread_create(&thread_a, NULL, mailbox_thread, &mailbox_a) == 0);
    CHECK(pthread_create(&thread_b, NULL, mailbox_thread, &mailbox_b) == 0);
    CHECK(pthread_join(thread_a, NULL) == 0);
    CHECK(pthread_join(thread_b, NULL) == 0);
    CHECK(mailbox_a.result == UCN_OK && mailbox_b.result == UCN_OK);
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &first) == UCN_OK);
    CHECK(ucn_i_owner_mailbox_take(&mailbox, &second) == UCN_OK);
    CHECK(first.work_class != second.work_class);
    CHECK(first.occurrences == 10000U && second.occurrences == 10000U);
    CHECK(ucn_i_owner_mailbox_destroy(&mailbox) == UCN_OK);
    CHECK(pthread_mutex_destroy(&gate_lock.mutex) == 0);
    CHECK(pthread_mutex_destroy(&mailbox_lock.mutex) == 0);
    puts("UCN simplified Owner concurrency tests passed");
    return 0;
}
