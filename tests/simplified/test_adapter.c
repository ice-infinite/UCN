#include "internal/ucn_adapter.h"

#include <stdio.h>
#include <string.h>

typedef struct fixture {
    ucn_i_adapter_t *adapter;
    ucn_driver_submit_result_t submit_result;
    ucn_result_t synchronous_completion;
    ucn_result_t cancel_result;
    uint32_t submit_calls;
    uint32_t cancel_calls;
    uint8_t publish_synchronously;
    uint8_t complete_during_cancel;
    uint8_t link_down_during_submit;
    uint8_t driver_gate_active;
    uint8_t nested_submit_during_submit;
    uint8_t nested_submit_during_cancel;
    uint8_t nested_frame;
    ucn_driver_token_t nested_token;
    ucn_i_adapter_tx_view_t nested_view;
    ucn_result_t nested_result;
} fixture_t;

static int failures;

#define CHECK(condition_)                                                     \
    do {                                                                      \
        if (!(condition_)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition_);    \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static ucn_result_t lock_enter(void *context)
{
    (void)context;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    (void)context;
}

static ucn_result_t driver_gate_enter(void *context)
{
    fixture_t *fixture = (fixture_t *)context;

    if (fixture == NULL || fixture->driver_gate_active) {
        return UCN_ERR_STATE;
    }
    fixture->driver_gate_active = 1U;
    return UCN_OK;
}

static void driver_gate_leave(void *context)
{
    fixture_t *fixture = (fixture_t *)context;

    if (fixture != NULL) {
        fixture->driver_gate_active = 0U;
    }
}

static ucn_driver_submit_result_t submit(void *context,
                                         ucn_link_handle_t link,
                                         const uint8_t *bytes,
                                         size_t length,
                                         ucn_driver_token_t token)
{
    fixture_t *fixture = (fixture_t *)context;

    CHECK(link.object_kind == UCN_OBJECT_KIND_LINK);
    CHECK(bytes != NULL);
    CHECK(length != 0U);
    ++fixture->submit_calls;
    if (fixture->nested_submit_during_submit) {
        fixture->nested_submit_during_submit = 0U;
        fixture->nested_result = ucn_i_adapter_tx_submit(
            fixture->adapter, fixture->nested_token,
            &fixture->nested_frame, 1U, &fixture->nested_view);
    }
    if (fixture->link_down_during_submit) {
        CHECK(ucn_i_adapter_link_event(fixture->adapter, link,
                                       UCN_DRIVER_LINK_DOWN, NULL) == UCN_OK);
    }
    if (fixture->publish_synchronously) {
        CHECK(ucn_i_adapter_tx_complete(fixture->adapter, token,
                                        fixture->synchronous_completion,
                                        NULL) == UCN_OK);
    }
    return fixture->submit_result;
}

static ucn_result_t cancel(void *context, ucn_driver_token_t token)
{
    fixture_t *fixture = (fixture_t *)context;
    ++fixture->cancel_calls;
    if (fixture->nested_submit_during_cancel) {
        fixture->nested_submit_during_cancel = 0U;
        fixture->nested_result = ucn_i_adapter_tx_submit(
            fixture->adapter, fixture->nested_token,
            &fixture->nested_frame, 1U, &fixture->nested_view);
    }
    if (fixture->complete_during_cancel) {
        CHECK(ucn_i_adapter_tx_complete(fixture->adapter, token,
                                        fixture->synchronous_completion,
                                        NULL) == UCN_OK);
    }
    return fixture->cancel_result;
}

static void make_ports(ucn_ports_t *ports,
                       ucn_link_port_t *link,
                       fixture_t *fixture)
{
    memset(ports, 0, sizeof(*ports));
    memset(link, 0, sizeof(*link));
    link->struct_size = sizeof(*link);
    link->api_version = UCN_API_VERSION;
    link->link_instance = 10U;
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
    ports->state_lock.enter = lock_enter;
    ports->state_lock.leave = lock_leave;
    ports->driver_callback_gate.struct_size =
        sizeof(ports->driver_callback_gate);
    ports->driver_callback_gate.api_version = UCN_API_VERSION;
    ports->driver_callback_gate.context = fixture;
    ports->driver_callback_gate.enter = driver_gate_enter;
    ports->driver_callback_gate.leave = driver_gate_leave;
}

static void test_submit_lifecycle(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_i_adapter_tx_view_t view;
    uint8_t frame[3] = {1U, 2U, 3U};

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_SUBMITTED;
    fixture.synchronous_completion = UCN_OK;
    fixture.cancel_result = UCN_OK;
    fixture.publish_synchronously = 1U;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 9U, 7U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 4U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, frame, sizeof(frame),
                                  &view) == UCN_OK);
    CHECK(fixture.submit_calls == 1U);
    CHECK(view.state == UCN_I_ADAPTER_TX_COMPLETED);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(ucn_i_adapter_tx_complete(&adapter, token, UCN_OK, NULL) == UCN_OK);
    CHECK(ucn_i_adapter_tx_complete(&adapter, token, UCN_ERR_STATE, NULL) ==
          UCN_ERR_IN_DOUBT);
    CHECK(ucn_i_adapter_tx_view(&adapter, token, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_IN_DOUBT);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_ERR_STATE);

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_SUBMITTED;
    fixture.synchronous_completion = UCN_OK;
    fixture.cancel_result = UCN_OK;
    fixture.publish_synchronously = 1U;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 9U, 7U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 4U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, frame, sizeof(frame),
                                  &view) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, frame, sizeof(frame),
                                  &view) == UCN_ERR_STATE);
    CHECK(ucn_i_adapter_tx_cancel(&adapter, token) == UCN_ERR_STATE);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_ERR_NOT_FOUND);
    CHECK(ucn_i_adapter_destroy(&adapter) == UCN_ERR_STATE);
    while (ucn_i_owner_mailbox_take(&adapter.mailbox,
                                    &(ucn_i_owner_work_hint_t){0}) == UCN_OK) {
    }
    CHECK(ucn_i_adapter_destroy(&adapter) == UCN_OK);
}

static void test_submit_outcomes_and_link_fence(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_driver_token_t reserved;
    ucn_driver_token_t blocked;
    ucn_i_adapter_tx_view_t view;
    ucn_link_handle_t link_handle;
    ucn_link_event_meta_t reopen;
    uint8_t frame = 0xA5U;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_NOT_SUBMITTED;
    fixture.cancel_result = UCN_OK;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 2U, 3U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_link_handle(&adapter, 0U, &link_handle) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 1U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, &frame, 1U, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED);
    CHECK(view.terminal_latched == 0U);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_OK);

    fixture.submit_result = UCN_DRIVER_SUBMITTED;
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 2U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, &frame, 1U, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_SUBMITTED);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 3U, &reserved) == UCN_OK);
    CHECK(ucn_i_adapter_link_event(&adapter, link_handle,
                                   UCN_DRIVER_LINK_DOWN, NULL) == UCN_OK);
    CHECK(ucn_i_adapter_tx_view(&adapter, token, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_IN_DOUBT);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_ERR_STATE);
    CHECK(ucn_i_adapter_tx_view(&adapter, reserved, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED);
    CHECK(ucn_i_adapter_link_event(&adapter, link_handle,
                                   UCN_DRIVER_LINK_UP, NULL) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 4U, &blocked) ==
          UCN_ERR_STATE);
    memset(&reopen, 0, sizeof(reopen));
    reopen.struct_size = sizeof(reopen);
    reopen.api_version = UCN_API_VERSION;
    reopen.new_link_instance = 11U;
    CHECK(ucn_i_adapter_link_event(&adapter, link_handle,
                                   UCN_DRIVER_LINK_REOPENED, &reopen) ==
          UCN_OK);
    CHECK(ucn_i_adapter_tx_view(&adapter, token, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_COMPLETED);
    CHECK(view.terminal_result == UCN_ERR_IN_DOUBT);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_retire(&adapter, reserved) == UCN_OK);
}

static void test_cancel_does_not_overwrite_synchronous_completion(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_i_adapter_tx_view_t view;
    uint8_t frame = 0x5AU;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_SUBMITTED;
    fixture.cancel_result = UCN_OK;
    fixture.synchronous_completion = UCN_OK;
    fixture.complete_during_cancel = 1U;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 11U, 3U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 9U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, &frame, 1U, &view) ==
          UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_SUBMITTED);
    CHECK(ucn_i_adapter_tx_cancel(&adapter, token) == UCN_ERR_STATE);
    CHECK(ucn_i_adapter_tx_view(&adapter, token, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_COMPLETED);
    CHECK(view.terminal_result == UCN_OK);
}

static void test_synchronous_terminal_proof_wins_conflicting_submit_result(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_driver_token_t blocked;
    ucn_i_adapter_tx_view_t view;
    uint8_t frame = 0x5BU;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_NOT_SUBMITTED;
    fixture.synchronous_completion = UCN_OK;
    fixture.publish_synchronously = 1U;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 18U, 9U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 1U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, &frame, 1U, &view) ==
          UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_COMPLETED);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 2U, &blocked) ==
          UCN_ERR_STATE);

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_SUBMIT_UNKNOWN;
    fixture.synchronous_completion = UCN_ERR_CANCELLED;
    fixture.publish_synchronously = 1U;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 19U, 9U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 1U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, &frame, 1U, &view) ==
          UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_COMPLETED);
    CHECK(view.terminal_result == UCN_ERR_CANCELLED);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 2U, &blocked) ==
          UCN_ERR_STATE);
}

static void test_link_invalidation_during_submit_is_not_adapter_fault(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_i_adapter_tx_view_t view;
    ucn_link_handle_t old_link;
    ucn_link_event_meta_t reopen;
    uint8_t frame = 0x33U;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_SUBMITTED;
    fixture.cancel_result = UCN_OK;
    fixture.link_down_during_submit = 1U;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 12U, 4U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_link_handle(&adapter, 0U, &old_link) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 3U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, &frame, 1U, &view) ==
          UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_IN_DOUBT);
    CHECK(adapter.faulted == 0U);
    memset(&reopen, 0, sizeof(reopen));
    reopen.struct_size = sizeof(reopen);
    reopen.api_version = UCN_API_VERSION;
    reopen.new_link_instance = 11U;
    CHECK(ucn_i_adapter_link_event(&adapter, old_link,
                                   UCN_DRIVER_LINK_REOPENED, &reopen) ==
          UCN_OK);
    CHECK(ucn_i_adapter_tx_view(&adapter, token, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_COMPLETED);
    CHECK(view.terminal_result == UCN_ERR_IN_DOUBT);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_OK);
}

static void test_explicit_submit_unknown_fences_link(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_driver_token_t second;
    ucn_i_adapter_tx_view_t view;
    uint8_t frame = 0x44U;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_ERR_IN_DOUBT;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 13U, 4U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 3U, &token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_submit(&adapter, token, &frame, 1U, &view) ==
          UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_IN_DOUBT);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 4U, &second) ==
          UCN_ERR_STATE);
}

static void test_rx_atomic_record(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_link_handle_t link_handle;
    ucn_rx_meta_t meta;
    ucn_i_adapter_rx_view_t view;
    uint8_t frame[5] = {9U, 8U, 7U, 6U, 5U};
    size_t index;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    make_ports(&ports, &link, &fixture);
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    meta.api_version = UCN_API_VERSION;
    meta.timestamp_us = 123U;
    meta.sender_discriminator = 77U;
    CHECK(ucn_i_adapter_init(&adapter, 4U, 5U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_link_handle(&adapter, 0U, &link_handle) == UCN_OK);
    CHECK(ucn_i_adapter_rx_publish(&adapter, link_handle, frame,
                                   sizeof(frame), &meta) == UCN_ERR_STATE);
    CHECK(ucn_i_adapter_set_rx_enabled(&adapter, true) == UCN_OK);
    for (index = 0U; index < UCN_ADAPTER_RX_SLOT_COUNT; ++index) {
        frame[0] = (uint8_t)index;
        CHECK(ucn_i_adapter_rx_publish(&adapter, link_handle, frame,
                                       sizeof(frame), &meta) == UCN_OK);
    }
    CHECK(ucn_i_adapter_rx_publish(&adapter, link_handle, frame,
                                   sizeof(frame), &meta) == UCN_ERR_NO_SPACE);
    for (index = 0U; index < UCN_ADAPTER_RX_SLOT_COUNT; ++index) {
        CHECK(ucn_i_adapter_rx_claim(&adapter, &view) == UCN_OK);
        CHECK(view.frame_bytes == sizeof(frame));
        CHECK(view.frame[0] == (uint8_t)index);
        CHECK(view.meta.timestamp_us == 123U);
        CHECK(view.meta.sender_discriminator == 77U);
        CHECK(ucn_i_adapter_rx_retire(&adapter, view.token) == UCN_OK);
        CHECK(ucn_i_adapter_rx_retire(&adapter, view.token) ==
              UCN_ERR_NOT_FOUND);
    }
    CHECK(ucn_i_adapter_rx_claim(&adapter, &view) == UCN_ERR_NOT_FOUND);
}

static void test_tx_reservations_rotate_and_cancel_is_idempotent(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    uint16_t index;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 15U, 6U, &ports) == UCN_OK);
    for (index = 0U; index < UCN_ADAPTER_TX_SLOT_COUNT; ++index) {
        CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, index, &token) == UCN_OK);
        CHECK(token.slot == (uint16_t)(index + 1U));
        CHECK(ucn_i_adapter_tx_cancel(&adapter, token) == UCN_OK);
        CHECK(ucn_i_adapter_tx_cancel(&adapter, token) == UCN_OK);
        CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_OK);
    }
}

static void test_exhausted_slots_do_not_hide_reusable_capacity(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_link_handle_t link_handle;
    ucn_i_adapter_rx_view_t rx_view;
    ucn_rx_meta_t meta;
    uint8_t frame = 0x5AU;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 16U, 8U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_link_handle(&adapter, 0U, &link_handle) == UCN_OK);

    adapter.tx_tokens[0].generation = UINT16_MAX;
    adapter.tx_allocate_cursor = 0U;
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 7U, &token) == UCN_OK);
    CHECK(token.slot == 2U);
    CHECK(ucn_i_adapter_tx_cancel(&adapter, token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_OK);

    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    meta.api_version = UCN_API_VERSION;
    adapter.rx_slots[0].generation = UINT16_MAX;
    adapter.rx_allocate_cursor = 0U;
    CHECK(ucn_i_adapter_set_rx_enabled(&adapter, true) == UCN_OK);
    CHECK(ucn_i_adapter_rx_publish(&adapter, link_handle, &frame,
                                   sizeof(frame), &meta) == UCN_OK);
    CHECK(ucn_i_adapter_rx_claim(&adapter, &rx_view) == UCN_OK);
    CHECK(rx_view.token.slot == 2U);
    CHECK(ucn_i_adapter_rx_retire(&adapter, rx_view.token) == UCN_OK);
}

static void test_link_generation_exhaustion_faults_closed(void)
{
    ucn_i_adapter_t adapter;
    ucn_i_adapter_t before;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_driver_token_t token_before;
    ucn_link_handle_t link_handle;
    ucn_link_event_meta_t reopen;
    bool faulted = false;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 20U, 10U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_faulted(&adapter, &faulted) == UCN_OK);
    CHECK(!faulted);
    memset(&token, 0xA5, sizeof(token));
    token_before = token;
    before = adapter;
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 9U, 1U, &token) ==
          UCN_ERR_NOT_FOUND);
    CHECK(memcmp(&token, &token_before, sizeof(token)) == 0);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
    adapter.links[0].handle_generation = UINT16_MAX;
    CHECK(ucn_i_adapter_link_handle(&adapter, 0U, &link_handle) == UCN_OK);
    memset(&reopen, 0, sizeof(reopen));
    reopen.struct_size = sizeof(reopen);
    reopen.api_version = UCN_API_VERSION;
    reopen.new_link_instance = 11U;
    CHECK(ucn_i_adapter_link_event(&adapter, link_handle,
                                   UCN_DRIVER_LINK_REOPENED, &reopen) ==
          UCN_ERR_EXHAUSTED);
    CHECK(adapter.faulted == 1U);
    CHECK(adapter.links[0].fenced == 1U);
    CHECK(ucn_i_adapter_faulted(&adapter, &faulted) == UCN_OK);
    CHECK(faulted);

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    make_ports(&ports, &link, &fixture);
    link.link_instance = UINT32_MAX;
    CHECK(ucn_i_adapter_init(&adapter, 21U, 10U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_link_handle(&adapter, 0U, &link_handle) == UCN_OK);
    reopen.new_link_instance = UINT32_MAX;
    CHECK(ucn_i_adapter_link_event(&adapter, link_handle,
                                   UCN_DRIVER_LINK_REOPENED, &reopen) ==
          UCN_ERR_EXHAUSTED);
    CHECK(adapter.faulted == 1U);
    CHECK(adapter.links[0].fenced == 1U);
    CHECK(ucn_i_adapter_faulted(&adapter, &faulted) == UCN_OK);
    CHECK(faulted);
}

static void test_all_token_generations_exhaust_faults_closed(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_driver_token_t token_before;
    ucn_link_handle_t link_handle;
    ucn_rx_meta_t meta;
    bool faulted = false;
    uint16_t index;
    uint8_t frame = 0x5AU;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 24U, 13U, &ports) == UCN_OK);
    for (index = 0U; index < UCN_ADAPTER_TX_SLOT_COUNT; ++index) {
        adapter.tx_tokens[index].generation = UINT16_MAX;
    }
    memset(&token, 0xA5, sizeof(token));
    token_before = token;
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 1U, &token) ==
          UCN_ERR_EXHAUSTED);
    CHECK(memcmp(&token, &token_before, sizeof(token)) == 0);
    CHECK(ucn_i_adapter_faulted(&adapter, &faulted) == UCN_OK);
    CHECK(faulted);

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 25U, 14U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_link_handle(&adapter, 0U, &link_handle) == UCN_OK);
    CHECK(ucn_i_adapter_set_rx_enabled(&adapter, true) == UCN_OK);
    for (index = 0U; index < UCN_ADAPTER_RX_SLOT_COUNT; ++index) {
        adapter.rx_slots[index].generation = UINT16_MAX;
    }
    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    meta.api_version = UCN_API_VERSION;
    CHECK(ucn_i_adapter_rx_publish(&adapter, link_handle, &frame,
                                   sizeof(frame), &meta) ==
          UCN_ERR_EXHAUSTED);
    CHECK(ucn_i_adapter_faulted(&adapter, &faulted) == UCN_OK);
    CHECK(faulted);
}

static void test_alias_rejection_is_atomic(void)
{
    ucn_i_adapter_t adapter;
    ucn_i_adapter_t adapter_before;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t token;
    ucn_i_adapter_tx_view_t tx_view;
    ucn_i_adapter_tx_view_t tx_before;
    ucn_i_adapter_tx_view_t reserved_view;
    ucn_link_handle_t link_handle;
    ucn_rx_meta_t meta;
    uint32_t instance = UINT32_C(0xA5A5A5A5);
    uint16_t mtu = UINT16_C(0x5A5A);
    bool ready = true;
    bool faulted = true;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_SUBMITTED;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 22U, 11U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_link_handle(&adapter, 0U, &link_handle) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 17U, &token) == UCN_OK);

    memset(&tx_view, 0xA5, sizeof(tx_view));
    tx_before = tx_view;
    CHECK(ucn_i_adapter_tx_submit(
              &adapter, token, (const uint8_t *)(const void *)&tx_view, 1U,
              &tx_view) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&tx_view, &tx_before, sizeof(tx_view)) == 0);
    CHECK(fixture.submit_calls == 0U);
    CHECK(ucn_i_adapter_tx_view(&adapter, token, &reserved_view) == UCN_OK);
    CHECK(reserved_view.state == UCN_I_ADAPTER_TX_RESERVED);

    CHECK(ucn_i_adapter_link_snapshot(
              &adapter, 0U, &instance, (uint16_t *)(void *)&instance,
              &ready) == UCN_ERR_ARGUMENT);
    CHECK(instance == UINT32_C(0xA5A5A5A5));
    CHECK(mtu == UINT16_C(0x5A5A));
    CHECK(ready);

    CHECK(ucn_i_adapter_faulted(
              &adapter, (bool *)(void *)((uint8_t *)&adapter + 1U)) ==
          UCN_ERR_ARGUMENT);
    CHECK(ucn_i_adapter_faulted(&adapter, &faulted) == UCN_OK);
    CHECK(!faulted);

    memset(&meta, 0, sizeof(meta));
    meta.struct_size = sizeof(meta);
    meta.api_version = UCN_API_VERSION;
    CHECK(ucn_i_adapter_set_rx_enabled(&adapter, true) == UCN_OK);
    adapter_before = adapter;
    CHECK(ucn_i_adapter_rx_publish(
              &adapter, link_handle,
              ((const uint8_t *)(const void *)&meta) + 1U, 1U, &meta) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&adapter, &adapter_before, sizeof(adapter)) == 0);

    CHECK(ucn_i_adapter_tx_cancel(&adapter, token) == UCN_OK);
    CHECK(ucn_i_adapter_tx_retire(&adapter, token) == UCN_OK);
}

static void test_driver_callback_gate_blocks_recursive_control(void)
{
    ucn_i_adapter_t adapter;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;
    ucn_driver_token_t outer;
    ucn_driver_token_t nested;
    ucn_i_adapter_tx_view_t view;
    ucn_i_adapter_tx_view_t nested_before;
    uint8_t frame = 0xA1U;

    memset(&adapter, 0, sizeof(adapter));
    memset(&fixture, 0, sizeof(fixture));
    fixture.adapter = &adapter;
    fixture.submit_result = UCN_DRIVER_SUBMITTED;
    fixture.cancel_result = UCN_OK;
    fixture.nested_frame = 0xB2U;
    make_ports(&ports, &link, &fixture);
    CHECK(ucn_i_adapter_init(&adapter, 23U, 12U, &ports) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 1U, &outer) == UCN_OK);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 2U, &nested) == UCN_OK);
    fixture.nested_token = nested;
    memset(&fixture.nested_view, 0xA5, sizeof(fixture.nested_view));
    nested_before = fixture.nested_view;
    fixture.nested_submit_during_submit = 1U;
    CHECK(ucn_i_adapter_tx_submit(&adapter, outer, &frame, 1U, &view) ==
          UCN_OK);
    CHECK(fixture.nested_result == UCN_ERR_STATE);
    CHECK(memcmp(&fixture.nested_view, &nested_before,
                 sizeof(nested_before)) == 0);
    CHECK(ucn_i_adapter_tx_view(&adapter, nested, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_RESERVED);

    CHECK(ucn_i_adapter_tx_complete(&adapter, outer, UCN_OK, NULL) ==
          UCN_OK);
    CHECK(ucn_i_adapter_tx_view(&adapter, outer, &view) == UCN_OK);
    CHECK(ucn_i_adapter_tx_retire(&adapter, outer) == UCN_OK);

    CHECK(ucn_i_adapter_tx_submit(&adapter, nested, &frame, 1U, &view) ==
          UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_SUBMITTED);
    CHECK(ucn_i_adapter_tx_reserve(&adapter, 0U, 10U, 3U, &outer) == UCN_OK);
    fixture.nested_token = outer;
    memset(&fixture.nested_view, 0x5A, sizeof(fixture.nested_view));
    nested_before = fixture.nested_view;
    fixture.nested_submit_during_cancel = 1U;
    CHECK(ucn_i_adapter_tx_cancel(&adapter, nested) == UCN_OK);
    CHECK(fixture.nested_result == UCN_ERR_STATE);
    CHECK(memcmp(&fixture.nested_view, &nested_before,
                 sizeof(nested_before)) == 0);
    CHECK(ucn_i_adapter_tx_view(&adapter, outer, &view) == UCN_OK);
    CHECK(view.state == UCN_I_ADAPTER_TX_RESERVED);
    CHECK(ucn_i_adapter_tx_retire(&adapter, nested) == UCN_OK);
    CHECK(ucn_i_adapter_tx_cancel(&adapter, outer) == UCN_OK);
    CHECK(ucn_i_adapter_tx_retire(&adapter, outer) == UCN_OK);
}

static void test_failure_no_write(void)
{
    ucn_i_adapter_t adapter;
    ucn_i_adapter_t before;
    ucn_ports_t ports;
    ucn_link_port_t link;
    fixture_t fixture;

    memset(&adapter, 0, sizeof(adapter));
    before = adapter;
    memset(&fixture, 0, sizeof(fixture));
    make_ports(&ports, &link, &fixture);
    link.frame_mtu = (uint16_t)(UCN_ADAPTER_FRAME_BYTES + 1U);
    CHECK(ucn_i_adapter_init(&adapter, 1U, 1U, &ports) == UCN_ERR_CONFIG);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);

    link.frame_mtu = 64U;
    ports.driver_callback_gate = ports.state_lock;
    CHECK(ucn_i_adapter_init(&adapter, 1U, 1U, &ports) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&adapter, &before, sizeof(adapter)) == 0);
}

int main(void)
{
    test_submit_lifecycle();
    test_submit_outcomes_and_link_fence();
    test_cancel_does_not_overwrite_synchronous_completion();
    test_synchronous_terminal_proof_wins_conflicting_submit_result();
    test_link_invalidation_during_submit_is_not_adapter_fault();
    test_explicit_submit_unknown_fences_link();
    test_rx_atomic_record();
    test_tx_reservations_rotate_and_cancel_is_idempotent();
    test_exhausted_slots_do_not_hide_reusable_capacity();
    test_link_generation_exhaustion_faults_closed();
    test_all_token_generations_exhaust_faults_closed();
    test_alias_rejection_is_atomic();
    test_driver_callback_gate_blocks_recursive_control();
    test_failure_no_write();
    if (failures != 0) {
        printf("adapter failures=%d\n", failures);
        return 1;
    }
    printf("adapter tests passed\n");
    return 0;
}
