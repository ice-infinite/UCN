#include "internal/ucn_adapter.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition_)                                                     \
    do {                                                                      \
        if (!(condition_)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                   \
            return 1;                                                         \
        }                                                                     \
    } while (0)

typedef struct shared_gate {
    pthread_mutex_t callback_mutex;
    pthread_mutex_t event_mutex;
    pthread_cond_t event_condition;
    uint8_t callback_entered;
    uint8_t callback_release;
} shared_gate_t;

typedef struct adapter_fixture {
    pthread_mutex_t state_mutex;
    shared_gate_t *shared;
    uint32_t submit_calls;
    uint8_t wait_in_submit;
} adapter_fixture_t;

typedef struct submit_thread_context {
    ucn_i_adapter_t *adapter;
    ucn_driver_token_t token;
    ucn_result_t result;
    ucn_i_adapter_tx_view_t view;
    uint8_t frame;
} submit_thread_context_t;

static ucn_result_t state_enter(void *context)
{
    adapter_fixture_t *fixture = (adapter_fixture_t *)context;

    return fixture != NULL && pthread_mutex_lock(&fixture->state_mutex) == 0
               ? UCN_OK
               : UCN_ERR_STATE;
}

static void state_leave(void *context)
{
    adapter_fixture_t *fixture = (adapter_fixture_t *)context;

    if (fixture != NULL) {
        (void)pthread_mutex_unlock(&fixture->state_mutex);
    }
}

static ucn_result_t callback_gate_enter(void *context)
{
    shared_gate_t *gate = (shared_gate_t *)context;

    return gate != NULL && pthread_mutex_trylock(&gate->callback_mutex) == 0
               ? UCN_OK
               : UCN_ERR_STATE;
}

static void callback_gate_leave(void *context)
{
    shared_gate_t *gate = (shared_gate_t *)context;

    if (gate != NULL) {
        (void)pthread_mutex_unlock(&gate->callback_mutex);
    }
}

static ucn_driver_submit_result_t submit(void *context,
                                         ucn_link_handle_t link,
                                         const uint8_t *bytes,
                                         size_t length,
                                         ucn_driver_token_t token)
{
    adapter_fixture_t *fixture = (adapter_fixture_t *)context;

    (void)token;
    if (fixture == NULL || fixture->shared == NULL || bytes == NULL ||
        length != 1U || link.object_kind != UCN_OBJECT_KIND_LINK) {
        return UCN_DRIVER_SUBMIT_UNKNOWN;
    }
    ++fixture->submit_calls;
    if (fixture->wait_in_submit) {
        shared_gate_t *gate = fixture->shared;
        if (pthread_mutex_lock(&gate->event_mutex) != 0) {
            return UCN_DRIVER_SUBMIT_UNKNOWN;
        }
        gate->callback_entered = 1U;
        (void)pthread_cond_broadcast(&gate->event_condition);
        while (!gate->callback_release) {
            if (pthread_cond_wait(&gate->event_condition,
                                  &gate->event_mutex) != 0) {
                (void)pthread_mutex_unlock(&gate->event_mutex);
                return UCN_DRIVER_SUBMIT_UNKNOWN;
            }
        }
        (void)pthread_mutex_unlock(&gate->event_mutex);
    }
    return UCN_DRIVER_SUBMITTED;
}

static ucn_result_t cancel(void *context, ucn_driver_token_t token)
{
    (void)context;
    (void)token;
    return UCN_OK;
}

static void make_ports(ucn_ports_t *ports,
                       ucn_link_port_t *link,
                       adapter_fixture_t *fixture)
{
    memset(ports, 0, sizeof(*ports));
    memset(link, 0, sizeof(*link));
    link->struct_size = sizeof(*link);
    link->api_version = UCN_API_VERSION;
    link->link_instance = 1U;
    link->frame_mtu = 64U;
    link->context = fixture;
    link->tx.struct_size = sizeof(link->tx);
    link->tx.api_version = UCN_API_VERSION;
    link->tx.submit = submit;
    link->tx.cancel = cancel;
    ports->struct_size = sizeof(*ports);
    ports->api_version = UCN_API_VERSION;
    ports->links = link;
    ports->link_count = 1U;
    ports->state_lock.struct_size = sizeof(ports->state_lock);
    ports->state_lock.api_version = UCN_API_VERSION;
    ports->state_lock.context = fixture;
    ports->state_lock.enter = state_enter;
    ports->state_lock.leave = state_leave;
    ports->driver_callback_gate.struct_size =
        sizeof(ports->driver_callback_gate);
    ports->driver_callback_gate.api_version = UCN_API_VERSION;
    ports->driver_callback_gate.context = fixture->shared;
    ports->driver_callback_gate.enter = callback_gate_enter;
    ports->driver_callback_gate.leave = callback_gate_leave;
}

static void *submit_thread(void *argument)
{
    submit_thread_context_t *context =
        (submit_thread_context_t *)argument;

    context->result = ucn_i_adapter_tx_submit(
        context->adapter, context->token, &context->frame, 1U,
        &context->view);
    return NULL;
}

int main(void)
{
    shared_gate_t gate;
    adapter_fixture_t fixture_a;
    adapter_fixture_t fixture_b;
    ucn_i_adapter_t adapter_a;
    ucn_i_adapter_t adapter_b;
    ucn_ports_t ports_a;
    ucn_ports_t ports_b;
    ucn_link_port_t link_a;
    ucn_link_port_t link_b;
    ucn_driver_token_t token_a;
    ucn_driver_token_t token_b;
    ucn_i_adapter_tx_view_t view_b;
    ucn_i_adapter_tx_view_t before_b;
    submit_thread_context_t thread_context;
    pthread_t thread;
    uint8_t frame_b = 0xB2U;

    memset(&gate, 0, sizeof(gate));
    memset(&fixture_a, 0, sizeof(fixture_a));
    memset(&fixture_b, 0, sizeof(fixture_b));
    memset(&adapter_a, 0, sizeof(adapter_a));
    memset(&adapter_b, 0, sizeof(adapter_b));
    memset(&thread_context, 0, sizeof(thread_context));
    CHECK(pthread_mutex_init(&gate.callback_mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&gate.event_mutex, NULL) == 0);
    CHECK(pthread_cond_init(&gate.event_condition, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture_a.state_mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture_b.state_mutex, NULL) == 0);
    fixture_a.shared = &gate;
    fixture_a.wait_in_submit = 1U;
    fixture_b.shared = &gate;
    make_ports(&ports_a, &link_a, &fixture_a);
    make_ports(&ports_b, &link_b, &fixture_b);
    CHECK(ucn_i_adapter_init(&adapter_a, 31U, 1U, &ports_a) == UCN_OK);
    CHECK(ucn_i_adapter_init(&adapter_b, 32U, 1U, &ports_b) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter_a, 0U, 1U, 1U, &token_a) ==
          UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter_b, 0U, 1U, 1U, &token_b) ==
          UCN_OK);
    thread_context.adapter = &adapter_a;
    thread_context.token = token_a;
    thread_context.frame = 0xA1U;
    CHECK(pthread_create(&thread, NULL, submit_thread, &thread_context) == 0);

    CHECK(pthread_mutex_lock(&gate.event_mutex) == 0);
    while (!gate.callback_entered) {
        CHECK(pthread_cond_wait(&gate.event_condition, &gate.event_mutex) ==
              0);
    }
    CHECK(pthread_mutex_unlock(&gate.event_mutex) == 0);

    memset(&view_b, 0xA5, sizeof(view_b));
    before_b = view_b;
    CHECK(ucn_i_adapter_tx_submit(&adapter_b, token_b, &frame_b, 1U,
                                  &view_b) == UCN_ERR_STATE);
    CHECK(memcmp(&view_b, &before_b, sizeof(view_b)) == 0);
    CHECK(fixture_b.submit_calls == 0U);
    CHECK(ucn_i_adapter_tx_view(&adapter_b, token_b, &view_b) == UCN_OK);
    CHECK(view_b.state == UCN_I_ADAPTER_TX_RESERVED);

    CHECK(pthread_mutex_lock(&gate.event_mutex) == 0);
    gate.callback_release = 1U;
    CHECK(pthread_cond_broadcast(&gate.event_condition) == 0);
    CHECK(pthread_mutex_unlock(&gate.event_mutex) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(thread_context.result == UCN_OK);
    CHECK(thread_context.view.state == UCN_I_ADAPTER_TX_SUBMITTED);
    CHECK(fixture_a.submit_calls == 1U);

    CHECK(ucn_i_adapter_tx_complete(&adapter_a, token_a, UCN_OK, NULL) ==
          UCN_OK);
    CHECK(ucn_i_adapter_tx_retire(&adapter_a, token_a) == UCN_OK);
    CHECK(ucn_i_adapter_tx_cancel(&adapter_b, token_b) == UCN_OK);
    CHECK(ucn_i_adapter_tx_retire(&adapter_b, token_b) == UCN_OK);
    while (ucn_i_owner_mailbox_take(
               &adapter_a.mailbox, &(ucn_i_owner_work_hint_t){0}) == UCN_OK) {
    }
    while (ucn_i_owner_mailbox_take(
               &adapter_b.mailbox, &(ucn_i_owner_work_hint_t){0}) == UCN_OK) {
    }
    CHECK(ucn_i_adapter_destroy(&adapter_a) == UCN_OK);
    CHECK(ucn_i_adapter_destroy(&adapter_b) == UCN_OK);
    CHECK(pthread_mutex_destroy(&fixture_b.state_mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture_a.state_mutex) == 0);
    CHECK(pthread_cond_destroy(&gate.event_condition) == 0);
    CHECK(pthread_mutex_destroy(&gate.event_mutex) == 0);
    CHECK(pthread_mutex_destroy(&gate.callback_mutex) == 0);
    puts("UCN simplified shared Driver gate concurrency tests passed");
    return 0;
}
