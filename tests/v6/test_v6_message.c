#include "ucn/v6/ucn_v6_message.h"
#include "ucn/v6/ucn_v6_config.h"

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

typedef struct fake_store {
    ucn_v6_message_witness_t witness;
    bool has_witness;
    ucn_v6_operation_journal_snapshot_t durable;
    bool has_journal;
    ucn_v6_operation_journal_snapshot_t previous_durable;
    bool has_previous_journal;
    unsigned loads;
    unsigned submits;
    unsigned witness_loads;
    unsigned witness_submits;
    bool fail_submit;
    unsigned fail_journal_submit_at;
    bool corrupt_reload;
    ucn_v6_operation_journal_t *reenter_journal;
    ucn_v6_operation_key_t reenter_key;
    uint8_t reenter_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    ucn_v6_result_t reenter_result;
    ucn_v6_operation_id_allocator_t *reenter_allocator;
    unsigned lifecycle_calls;
    bool authorize_prepared_abort;
    bool authorize_result_retirement;
    bool authorize_tombstone_reclaim;
    bool authorize_history_release;
    uint64_t lifecycle_high_water;
    int32_t abort_result_code;
    uint8_t abort_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    ucn_v6_operation_reconciliation_t reconciliation;
    ucn_v6_result_t lifecycle_result;
} fake_store_t;

static void no_lock(void *context)
{
    (void)context;
}

static ucn_v6_result_t fake_load_witness(
    void *context, ucn_v6_message_witness_t *witness)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->witness_loads;
    if (!store->has_witness) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *witness = store->witness;
    return UCN_V6_OK;
}

static ucn_v6_result_t fake_reserve_witness(
    void *context, const ucn_v6_message_witness_t *witness)
{
    fake_store_t *store = (fake_store_t *)context;
    ucn_v6_message_witness_t previous;
    uint64_t ignored = 0U;
    ++store->witness_submits;
    if (store->reenter_allocator != NULL) {
        store->reenter_result = ucn_v6_operation_id_take(
            store->reenter_allocator, &ignored);
    }
    if (store->fail_submit) {
        return UCN_V6_ERR_STATE;
    }
    if (store->has_witness) {
        previous = store->witness;
    } else {
        memset(&previous, 0, sizeof(previous));
        previous.magic = UCN_V6_MESSAGE_WITNESS_MAGIC;
        previous.schema = UCN_V6_MESSAGE_WITNESS_SCHEMA;
    }
    if (ucn_v6_message_witness_transition_validate(
            &previous, witness) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    store->witness = *witness;
    store->has_witness = true;
    return UCN_V6_OK;
}

static ucn_v6_result_t fake_load_journal(
    void *context,
    ucn_v6_operation_journal_snapshot_t *snapshot)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->loads;
    if (!store->has_journal) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *snapshot = store->durable;
    if (store->corrupt_reload) {
        snapshot->magic ^= 1U;
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t fake_submit_journal(
    void *context,
    const ucn_v6_operation_journal_snapshot_t *snapshot)
{
    fake_store_t *store = (fake_store_t *)context;
    ucn_v6_operation_admission_t admission =
        (ucn_v6_operation_admission_t)0;
    ++store->submits;
    if (store->reenter_journal != NULL) {
        store->reenter_result = ucn_v6_operation_prepare(
            store->reenter_journal, &store->reenter_key, 7U,
            UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
            store->reenter_digest, &admission);
    }
    if (store->fail_submit ||
        (store->fail_journal_submit_at != 0U &&
         store->submits == store->fail_journal_submit_at)) {
        return UCN_V6_ERR_STATE;
    }
    store->previous_durable = store->durable;
    store->has_previous_journal = store->has_journal;
    store->durable = *snapshot;
    store->has_journal = true;
    return UCN_V6_OK;
}

static void fake_lifecycle_try_reenter(fake_store_t *store)
{
    ucn_v6_operation_admission_t admission =
        (ucn_v6_operation_admission_t)0;
    if (store->reenter_journal != NULL) {
        store->reenter_result = ucn_v6_operation_prepare(
            store->reenter_journal, &store->reenter_key, 7U,
            UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
            store->reenter_digest, &admission);
    }
}

static ucn_v6_result_t fake_reconcile_in_doubt(
    void *context,
    const ucn_v6_operation_slot_t *durable_slot,
    ucn_v6_operation_reconciliation_t *reconciliation)
{
    fake_store_t *store = (fake_store_t *)context;
    if (store == NULL || durable_slot == NULL || reconciliation == NULL ||
        durable_slot->phase != UCN_V6_OPERATION_PHASE_IN_DOUBT) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ++store->lifecycle_calls;
    fake_lifecycle_try_reenter(store);
    if (store->lifecycle_result != UCN_V6_OK) {
        return store->lifecycle_result;
    }
    *reconciliation = store->reconciliation;
    return UCN_V6_OK;
}

static ucn_v6_result_t fake_authorize_prepared_abort(
    void *context,
    const ucn_v6_operation_slot_t *durable_slot,
    int32_t *terminal_result_code,
    uint8_t terminal_digest[UCN_V6_OPERATION_DIGEST_BYTES])
{
    fake_store_t *store = (fake_store_t *)context;
    if (store == NULL || durable_slot == NULL ||
        terminal_result_code == NULL || terminal_digest == NULL ||
        durable_slot->phase != UCN_V6_OPERATION_PHASE_PREPARED) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ++store->lifecycle_calls;
    fake_lifecycle_try_reenter(store);
    if (store->lifecycle_result != UCN_V6_OK) {
        return store->lifecycle_result;
    }
    if (!store->authorize_prepared_abort) {
        return UCN_V6_ERR_ACCESS;
    }
    *terminal_result_code = store->abort_result_code;
    memcpy(terminal_digest, store->abort_digest,
           UCN_V6_OPERATION_DIGEST_BYTES);
    return UCN_V6_OK;
}

static ucn_v6_result_t fake_authorize_result_retirement(
    void *context,
    const ucn_v6_operation_slot_t *durable_slot)
{
    fake_store_t *store = (fake_store_t *)context;
    if (store == NULL || durable_slot == NULL ||
        durable_slot->phase != UCN_V6_OPERATION_PHASE_COMMITTED_RESULT) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ++store->lifecycle_calls;
    fake_lifecycle_try_reenter(store);
    return store->lifecycle_result != UCN_V6_OK ? store->lifecycle_result :
           (store->authorize_result_retirement ? UCN_V6_OK :
                                                UCN_V6_ERR_ACCESS);
}

static ucn_v6_result_t fake_authorize_tombstone_reclaim(
    void *context,
    const ucn_v6_operation_slot_t *durable_slot,
    uint64_t *durable_initiator_high_water)
{
    fake_store_t *store = (fake_store_t *)context;
    if (store == NULL || durable_slot == NULL ||
        durable_initiator_high_water == NULL ||
        durable_slot->phase != UCN_V6_OPERATION_PHASE_TOMBSTONED) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ++store->lifecycle_calls;
    fake_lifecycle_try_reenter(store);
    if (store->lifecycle_result != UCN_V6_OK) {
        return store->lifecycle_result;
    }
    if (!store->authorize_tombstone_reclaim) {
        return UCN_V6_ERR_ACCESS;
    }
    *durable_initiator_high_water = store->lifecycle_high_water;
    return UCN_V6_OK;
}

static ucn_v6_result_t fake_authorize_history_release(
    void *context,
    const ucn_v6_operation_high_water_t *durable_high_water)
{
    fake_store_t *store = (fake_store_t *)context;
    if (store == NULL || durable_high_water == NULL ||
        !durable_high_water->occupied) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ++store->lifecycle_calls;
    fake_lifecycle_try_reenter(store);
    return store->lifecycle_result != UCN_V6_OK ? store->lifecycle_result :
           (store->authorize_history_release ? UCN_V6_OK :
                                               UCN_V6_ERR_ACCESS);
}

static ucn_v6_message_store_ops_t fake_ops(fake_store_t *store)
{
    ucn_v6_message_store_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = store;
    ops.load_witness = fake_load_witness;
    ops.reserve_witness = fake_reserve_witness;
    ops.load_journal = fake_load_journal;
    ops.submit_journal = fake_submit_journal;
    ops.lifecycle.context = store;
    ops.lifecycle.api_version =
        UCN_V6_MESSAGE_LIFECYCLE_PROVIDER_API_VERSION;
    ops.lifecycle.authorize_prepared_abort =
        fake_authorize_prepared_abort;
    ops.lifecycle.reconcile_in_doubt = fake_reconcile_in_doubt;
    ops.lifecycle.authorize_result_retirement =
        fake_authorize_result_retirement;
    ops.lifecycle.authorize_tombstone_reclaim =
        fake_authorize_tombstone_reclaim;
    ops.lifecycle.authorize_history_release =
        fake_authorize_history_release;
    return ops;
}

static void fill_principal(ucn_v6_principal_t *principal, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < sizeof(principal->bytes); ++index) {
        principal->bytes[index] = (uint8_t)(seed + index);
    }
}

static void fill_digest(uint8_t *digest, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < UCN_V6_OPERATION_DIGEST_BYTES; ++index) {
        digest[index] = (uint8_t)(seed + index);
    }
}

static ucn_v6_operation_key_t make_key(uint8_t seed, uint64_t operation_id)
{
    ucn_v6_operation_key_t key;
    memset(&key, 0, sizeof(key));
    fill_principal(&key.initiator_principal, seed);
    key.operation_id = operation_id;
    return key;
}

typedef struct fake_executor {
    unsigned calls;
    ucn_v6_result_t callback_result;
    ucn_v6_operation_journal_t *reenter_journal;
    ucn_v6_operation_key_t reenter_key;
    uint8_t reenter_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    ucn_v6_result_t reenter_result;
} fake_executor_t;

typedef struct fake_digest_provider {
    unsigned calls;
    unsigned fail_at;
    ucn_v6_operation_journal_t *reenter_journal;
    ucn_v6_operation_key_t reenter_key;
    uint8_t reenter_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    ucn_v6_result_t reenter_result;
} fake_digest_provider_t;

static void fake_digest_mix(uint64_t *hash, const uint8_t *bytes,
                            size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) {
        *hash ^= bytes[index];
        *hash *= UINT64_C(1099511628211);
    }
}

static ucn_v6_result_t fake_digest_compute(
    void *context,
    const uint8_t *canonical_prefix,
    size_t canonical_prefix_length,
    const uint8_t *variable_bytes,
    size_t variable_length,
    uint8_t digest[UCN_V6_OPERATION_DIGEST_BYTES])
{
    fake_digest_provider_t *provider = (fake_digest_provider_t *)context;
    ucn_v6_operation_admission_t admission =
        (ucn_v6_operation_admission_t)0;
    size_t lane;

    if (provider == NULL || canonical_prefix == NULL ||
        canonical_prefix_length == 0U || digest == NULL ||
        (variable_length != 0U && variable_bytes == NULL)) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ++provider->calls;
    if (provider->reenter_journal != NULL) {
        provider->reenter_result = ucn_v6_operation_prepare(
            provider->reenter_journal, &provider->reenter_key, 43U,
            UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
            provider->reenter_digest, &admission);
    }
    if (provider->fail_at != 0U && provider->calls == provider->fail_at) {
        return UCN_V6_ERR_STATE;
    }
    for (lane = 0U; lane < 4U; ++lane) {
        uint64_t hash = UINT64_C(1469598103934665603) ^
                        ((uint64_t)lane * UINT64_C(0x9E3779B97F4A7C15));
        size_t byte_index;
        fake_digest_mix(&hash, canonical_prefix, canonical_prefix_length);
        fake_digest_mix(&hash, variable_bytes, variable_length);
        for (byte_index = 0U; byte_index < 8U; ++byte_index) {
            digest[lane * 8U + byte_index] =
                (uint8_t)(hash >> (56U - byte_index * 8U));
        }
    }
    return UCN_V6_OK;
}

static ucn_v6_message_digest_ops_t fake_digest_ops(
    fake_digest_provider_t *provider)
{
    ucn_v6_message_digest_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = provider;
    ops.api_version = UCN_V6_MESSAGE_DIGEST_PROVIDER_API_VERSION;
    ops.algorithm_id = UCN_V6_MESSAGE_DIGEST_SHA256;
    ops.digest_bytes = (uint16_t)UCN_V6_OPERATION_DIGEST_BYTES;
    ops.compute = fake_digest_compute;
    return ops;
}

static ucn_v6_result_t execute_request(
    void *context,
    const ucn_v6_message_descriptor_t *message,
    const uint8_t *payload,
    uint16_t payload_length,
    ucn_v6_endpoint_execution_result_t *result)
{
    fake_executor_t *executor = (fake_executor_t *)context;
    ucn_v6_operation_admission_t admission =
        (ucn_v6_operation_admission_t)0;
    if (executor == NULL || message == NULL || result == NULL ||
        payload == NULL || payload_length != 3U || payload[0] != 0xA1U) {
        return UCN_V6_ERR_ARGUMENT;
    }
    ++executor->calls;
    if (executor->reenter_journal != NULL) {
        executor->reenter_result = ucn_v6_operation_prepare(
            executor->reenter_journal, &executor->reenter_key, 44U,
            UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
            executor->reenter_digest, &admission);
    }
    if (executor->callback_result != UCN_V6_OK) {
        return executor->callback_result;
    }
    memset(result, 0, sizeof(*result));
    result->result_code = 17;
    result->result_length = 2U;
    result->result[0] = 0x51U;
    result->result[1] = 0x52U;
    return UCN_V6_OK;
}

static void make_dispatch_contract(
    ucn_v6_endpoint_contract_t *endpoint,
    ucn_v6_message_descriptor_t *message)
{
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->endpoint_id = 42U;
    endpoint->traffic_class_mask = UINT8_C(1) << UCN_V6_TRAFFIC_Q2;
    endpoint->delivery_guarantee_mask =
        UINT8_C(1) << UCN_V6_DELIVERY_RELIABLE;
    endpoint->interaction_role_mask =
        UINT8_C(1) << UCN_V6_INTERACTION_REQUEST;
    endpoint->max_payload_bytes = 16U;
    endpoint->max_result_bytes = 16U;
    endpoint->execution_contract = UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE;
    memset(message, 0, sizeof(*message));
    message->traffic_class = UCN_V6_TRAFFIC_Q2;
    message->delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    message->interaction_role = UCN_V6_INTERACTION_REQUEST;
    message->source_endpoint = 7U;
    message->destination_endpoint = endpoint->endpoint_id;
    message->operation_id = 55U;
    message->payload_length = 3U;
}

static int test_message_axes_are_orthogonal(void)
{
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_message_descriptor_t message;

    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.endpoint_id = 42U;
    endpoint.traffic_class_mask = 0x0FU;
    endpoint.delivery_guarantee_mask = 0x07U;
    endpoint.interaction_role_mask = 0x0FU;
    endpoint.max_payload_bytes = 128U;
    endpoint.max_result_bytes = 32U;
    endpoint.execution_contract = UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE;
    memset(&message, 0, sizeof(message));
    message.traffic_class = UCN_V6_TRAFFIC_Q2;
    message.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    message.interaction_role = UCN_V6_INTERACTION_REQUEST;
    message.source_endpoint = 7U;
    message.destination_endpoint = 42U;
    message.operation_id = 9U;
    message.payload_length = 64U;
    CHECK(ucn_v6_message_validate(&message, &endpoint) == UCN_V6_OK);
    message.traffic_class = UCN_V6_TRAFFIC_Q1;
    message.delivery_guarantee = UCN_V6_DELIVERY_LATEST;
    message.interaction_role = UCN_V6_INTERACTION_RESULT;
    message.payload_length = 32U;
    CHECK(ucn_v6_message_validate(&message, &endpoint) == UCN_V6_OK);
    message.interaction_role = UCN_V6_INTERACTION_ONE_WAY;
    CHECK(ucn_v6_message_validate(&message, &endpoint) ==
          UCN_V6_ERR_ARGUMENT);
    message.operation_id = 0U;
    CHECK(ucn_v6_message_validate(&message, &endpoint) == UCN_V6_OK);
    message.traffic_class = (ucn_v6_traffic_class_t)-1;
    CHECK(ucn_v6_message_validate(&message, &endpoint) == UCN_V6_ERR_ACCESS);
    message.traffic_class = (ucn_v6_traffic_class_t)-255;
    CHECK(ucn_v6_message_validate(&message, &endpoint) == UCN_V6_ERR_ACCESS);
    message.traffic_class = (ucn_v6_traffic_class_t)-256;
    CHECK(ucn_v6_message_validate(&message, &endpoint) == UCN_V6_ERR_ACCESS);
    message.traffic_class = UCN_V6_TRAFFIC_Q0;
    endpoint.traffic_class_mask = 1U << UCN_V6_TRAFFIC_Q2;
    CHECK(ucn_v6_message_validate(&message, &endpoint) == UCN_V6_ERR_ACCESS);
    endpoint.traffic_class_mask = 0x0FU;
    endpoint.execution_contract = UCN_V6_ENDPOINT_NON_RETRYABLE;
    message.delivery_guarantee = UCN_V6_DELIVERY_RELIABLE;
    CHECK(ucn_v6_message_validate(&message, &endpoint) == UCN_V6_ERR_ACCESS);
    return 0;
}

static int test_operation_id_reservation_survives_restart(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_id_allocator_storage_t allocator_storage;
    ucn_v6_operation_id_allocator_storage_t restarted_storage;
    ucn_v6_operation_id_allocator_t *allocator = NULL;
    ucn_v6_operation_id_allocator_t *restarted = NULL;
    ucn_v6_operation_id_allocator_view_t view;
    uint64_t id = 0U;

    memset(&store, 0, sizeof(store));
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_id_allocator_init_in_place(
              allocator_storage.bytes, sizeof(allocator_storage),
              ucn_v6_compiled_manifest(), &ops, &gate, 4U,
              &allocator) == UCN_V6_OK);
    CHECK(ucn_v6_operation_id_take(allocator, &id) == UCN_V6_OK);
    CHECK(id == 1U && store.witness.operation_id_high_water == 4U);
    CHECK(ucn_v6_operation_id_take(allocator, &id) == UCN_V6_OK);
    CHECK(id == 2U && store.witness_submits == 1U);
    CHECK(ucn_v6_operation_id_allocator_init_in_place(
              restarted_storage.bytes, sizeof(restarted_storage),
              ucn_v6_compiled_manifest(), &ops, &gate, 4U,
              &restarted) == UCN_V6_OK);
    CHECK(ucn_v6_operation_id_take(restarted, &id) == UCN_V6_OK);
    CHECK(id == 5U && store.witness.operation_id_high_water == 8U);
    CHECK(ucn_v6_operation_id_take(restarted, &id) == UCN_V6_OK && id == 6U);
    CHECK(ucn_v6_operation_id_take(restarted, &id) == UCN_V6_OK && id == 7U);
    CHECK(ucn_v6_operation_id_take(restarted, &id) == UCN_V6_OK && id == 8U);
    store.reenter_allocator = restarted;
    CHECK(ucn_v6_operation_id_take(restarted, &id) == UCN_V6_ERR_STATE);
    CHECK(id == 8U);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_id_allocator_copy_view(restarted, &view) ==
          UCN_V6_OK);
    CHECK(view.faulted && view.next_id == 9U && view.reserved_through == 8U);
    return 0;
}

static int test_message_opaque_storage_preflight(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_store_ops_t bad_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_feature_manifest_t bad_manifest = *ucn_v6_compiled_manifest();
    ucn_v6_operation_id_allocator_storage_t allocator_storage;
    ucn_v6_operation_id_allocator_storage_t allocator_before;
    ucn_v6_operation_journal_storage_t journal_storage;
    ucn_v6_operation_journal_storage_t journal_before;
    ucn_v6_operation_id_allocator_t *allocator = NULL;
    ucn_v6_operation_journal_t *journal = NULL;
    uint64_t operation_id = UINT64_MAX;

    memset(&store, 0, sizeof(store));
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    bad_manifest.layout_hash ^= UINT64_C(1);
    memset(&allocator_storage, 0xA5, sizeof(allocator_storage));
    allocator_before = allocator_storage;
    CHECK(ucn_v6_operation_id_allocator_init_in_place(
              allocator_storage.bytes, sizeof(allocator_storage),
              &bad_manifest, &ops, &gate, 4U,
              &allocator) == UCN_V6_ERR_CONFIG);
    CHECK(allocator == NULL && store.witness_loads == 0U);
    CHECK(memcmp(&allocator_storage, &allocator_before,
                 sizeof(allocator_storage)) == 0);
    CHECK(ucn_v6_operation_id_allocator_init_in_place(
              allocator_storage.bytes, 1U,
              ucn_v6_compiled_manifest(), &ops, &gate, 4U,
              &allocator) == UCN_V6_ERR_CONFIG);
    CHECK(memcmp(&allocator_storage, &allocator_before,
                 sizeof(allocator_storage)) == 0);

    memset(&journal_storage, 0x5A, sizeof(journal_storage));
    journal_before = journal_storage;
    CHECK(ucn_v6_operation_journal_init_in_place(
              journal_storage.bytes, sizeof(journal_storage),
              &bad_manifest, &ops, &gate,
              &journal) == UCN_V6_ERR_CONFIG);
    CHECK(journal == NULL && store.loads == 0U);
    CHECK(memcmp(&journal_storage, &journal_before,
                 sizeof(journal_storage)) == 0);

    bad_ops = ops;
    bad_ops.lifecycle.api_version = 0U;
    CHECK(ucn_v6_operation_journal_init_in_place(
              journal_storage.bytes, sizeof(journal_storage),
              ucn_v6_compiled_manifest(), &bad_ops, &gate,
              &journal) == UCN_V6_ERR_CONFIG);
    CHECK(journal == NULL && store.loads == 0U);
    CHECK(memcmp(&journal_storage, &journal_before,
                 sizeof(journal_storage)) == 0);

    CHECK(ucn_v6_operation_id_allocator_init_in_place(
              allocator_storage.bytes, sizeof(allocator_storage),
              ucn_v6_compiled_manifest(), &ops, &gate, 4U,
              &allocator) == UCN_V6_OK);
    allocator_storage.bytes[0] ^= 1U;
    CHECK(ucn_v6_operation_id_take(
              allocator, &operation_id) == UCN_V6_ERR_STATE);
    CHECK(operation_id == UINT64_MAX);
    return 0;
}

static int test_durable_journal_lifecycle_and_gc(void)
{
    static const uint8_t result_bytes[] = { 0x11U, 0x22U, 0x33U };
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t journal_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t key = make_key(0x10U, 7U);
    ucn_v6_operation_slot_t slot;
    ucn_v6_operation_slot_t before;
    ucn_v6_operation_admission_t admission =
        (ucn_v6_operation_admission_t)0;
    uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    uint8_t other_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES];

    memset(&store, 0, sizeof(store));
    fill_digest(request_digest, 0x20U);
    fill_digest(other_digest, 0x30U);
    fill_digest(result_digest, 0x40U);
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              journal_storage.bytes, sizeof(journal_storage),
              ucn_v6_compiled_manifest(), &ops, &gate,
              &journal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    CHECK(admission == UCN_V6_OPERATION_ADMISSION_NEW);
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    CHECK(admission == UCN_V6_OPERATION_ADMISSION_PREPARED);
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              other_digest, &admission) == UCN_V6_ERR_REPLAY);
    CHECK(ucn_v6_operation_mark_executing(journal, &key) == UCN_V6_OK);
    CHECK(ucn_v6_operation_mark_executing(journal, &key) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_commit_result(
              journal, &key, 0, result_bytes,
              (uint16_t)sizeof(result_bytes), result_digest) == UCN_V6_OK);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK);
    CHECK(slot.phase == UCN_V6_OPERATION_PHASE_COMMITTED_RESULT);
    CHECK(memcmp(slot.result, result_bytes, sizeof(result_bytes)) == 0);
    CHECK(ucn_v6_operation_tombstone_result(journal, &key) ==
          UCN_V6_ERR_ACCESS);
    store.authorize_result_retirement = true;
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &before) == UCN_V6_OK);
    store.reenter_journal = journal;
    store.reenter_key = make_key(0x11U, 8U);
    memcpy(store.reenter_digest, request_digest, sizeof(request_digest));
    CHECK(ucn_v6_operation_tombstone_result(journal, &key) ==
          UCN_V6_ERR_STATE);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK &&
          memcmp(&slot, &before, sizeof(slot)) == 0);
    store.reenter_journal = NULL;
    CHECK(ucn_v6_operation_tombstone_result(journal, &key) == UCN_V6_OK);
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &key) ==
          UCN_V6_ERR_ACCESS);
    store.authorize_tombstone_reclaim = true;
    store.lifecycle_high_water = key.operation_id;
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &key) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK &&
          slot.phase == UCN_V6_OPERATION_PHASE_TOMBSTONED);
    store.lifecycle_high_water = 8U;
    before = slot;
    store.reenter_journal = journal;
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &key) ==
          UCN_V6_ERR_STATE);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK &&
          memcmp(&slot, &before, sizeof(slot)) == 0);
    store.reenter_journal = NULL;
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &key) == UCN_V6_OK);
    admission = (ucn_v6_operation_admission_t)0;
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_ERR_REPLAY);
    CHECK(admission == (ucn_v6_operation_admission_t)0);
    return 0;
}

static int test_lifecycle_provider_reconciliation_and_reentrancy(void)
{
    fake_store_t store;
    fake_digest_provider_t digest_provider;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_digest_ops_t digest_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t key = make_key(0x21U, 17U);
    ucn_v6_operation_slot_t slot;
    ucn_v6_operation_slot_t before;
    ucn_v6_operation_admission_t admission;
    uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES];

    memset(&store, 0, sizeof(store));
    memset(&digest_provider, 0, sizeof(digest_provider));
    fill_digest(request_digest, 0x31U);
    ops = fake_ops(&store);
    digest_ops = fake_digest_ops(&digest_provider);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    CHECK(ucn_v6_operation_mark_executing(journal, &key) == UCN_V6_OK);
    CHECK(ucn_v6_operation_mark_in_doubt(journal, &key) == UCN_V6_OK);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &before) == UCN_V6_OK);

    /* A malformed trusted callback result must not mutate the durable slot. */
    memset(&store.reconciliation, 0, sizeof(store.reconciliation));
    store.reconciliation.outcome =
        UCN_V6_OPERATION_RECONCILIATION_RESULT;
    store.reconciliation.result_length = 1U;
    store.reconciliation.result[0] = 0x41U;
    store.reconciliation.result[1] = 0x42U;
    CHECK(ucn_v6_operation_resolve_in_doubt(
              journal, &key, &digest_ops) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK &&
          memcmp(&slot, &before, sizeof(slot)) == 0);

    memset(&store.reconciliation, 0, sizeof(store.reconciliation));
    store.reconciliation.outcome =
        UCN_V6_OPERATION_RECONCILIATION_RESULT;
    store.reconciliation.result_code = 23;
    store.reconciliation.result_length = 2U;
    store.reconciliation.result[0] = 0x41U;
    store.reconciliation.result[1] = 0x42U;
    store.reenter_journal = journal;
    store.reenter_key = make_key(0x22U, 18U);
    fill_digest(store.reenter_digest, 0x51U);
    CHECK(ucn_v6_operation_resolve_in_doubt(
              journal, &key, &digest_ops) == UCN_V6_ERR_STATE);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK &&
          memcmp(&slot, &before, sizeof(slot)) == 0);
    store.reenter_journal = NULL;
    CHECK(ucn_v6_operation_resolve_in_doubt(
              journal, &key, &digest_ops) == UCN_V6_OK);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK);
    CHECK(slot.phase == UCN_V6_OPERATION_PHASE_COMMITTED_RESULT &&
          slot.result_code == 23 && slot.result_length == 2U &&
          slot.result[0] == 0x41U && slot.result[1] == 0x42U &&
          memcmp(slot.result_digest, request_digest,
                 sizeof(slot.result_digest)) != 0);
    return 0;
}

static int test_exact_result_replay_precedes_retirement_floor(void)
{
    static const uint8_t result_byte = 0x66U;
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t older = make_key(0x31U, 7U);
    ucn_v6_operation_key_t newer = make_key(0x31U, 8U);
    ucn_v6_operation_admission_t admission;
    uint8_t older_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    uint8_t newer_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES];

    memset(&store, 0, sizeof(store));
    fill_digest(older_digest, 0x41U);
    fill_digest(newer_digest, 0x51U);
    fill_digest(result_digest, 0x61U);
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);

    CHECK(ucn_v6_operation_prepare(
              journal, &older, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              older_digest, &admission) == UCN_V6_OK);
    CHECK(ucn_v6_operation_mark_executing(journal, &older) == UCN_V6_OK);
    CHECK(ucn_v6_operation_commit_result(
              journal, &older, 0, &result_byte, 1U,
              result_digest) == UCN_V6_OK);

    CHECK(ucn_v6_operation_prepare(
              journal, &newer, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              newer_digest, &admission) == UCN_V6_OK);
    CHECK(ucn_v6_operation_mark_executing(journal, &newer) == UCN_V6_OK);
    CHECK(ucn_v6_operation_commit_result(
              journal, &newer, 0, &result_byte, 1U,
              result_digest) == UCN_V6_OK);
    store.authorize_result_retirement = true;
    store.authorize_tombstone_reclaim = true;
    store.lifecycle_high_water = 9U;
    CHECK(ucn_v6_operation_tombstone_result(journal, &newer) == UCN_V6_OK);
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &newer) == UCN_V6_OK);

    admission = (ucn_v6_operation_admission_t)0;
    CHECK(ucn_v6_operation_prepare(
              journal, &older, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              older_digest, &admission) == UCN_V6_OK);
    CHECK(admission == UCN_V6_OPERATION_ADMISSION_RESULT_REPLAY);
    admission = (ucn_v6_operation_admission_t)0;
    CHECK(ucn_v6_operation_prepare(
              journal, &newer, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              newer_digest, &admission) == UCN_V6_ERR_REPLAY);
    CHECK(admission == (ucn_v6_operation_admission_t)0);
    return 0;
}

static int test_retirement_floor_cannot_cross_nonterminal_slot(void)
{
    static const uint8_t result_byte = 0x67U;
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t pending = make_key(0x32U, 7U);
    ucn_v6_operation_key_t terminal = make_key(0x32U, 8U);
    ucn_v6_operation_admission_t admission;
    ucn_v6_operation_slot_t terminal_slot;
    uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    unsigned submits_before;

    memset(&store, 0, sizeof(store));
    fill_digest(request_digest, 0x42U);
    fill_digest(result_digest, 0x52U);
    store.authorize_result_retirement = true;
    store.authorize_tombstone_reclaim = true;
    store.lifecycle_high_water = 9U;
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_prepare(
              journal, &pending, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    CHECK(ucn_v6_operation_prepare(
              journal, &terminal, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    CHECK(ucn_v6_operation_mark_executing(journal, &terminal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_commit_result(
              journal, &terminal, 0, &result_byte, 1U,
              result_digest) == UCN_V6_OK);
    CHECK(ucn_v6_operation_tombstone_result(journal, &terminal) ==
          UCN_V6_OK);
    submits_before = store.submits;
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &terminal) ==
          UCN_V6_ERR_STATE);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_operation_copy_slot(journal, &terminal, &terminal_slot) ==
          UCN_V6_OK);
    CHECK(terminal_slot.phase == UCN_V6_OPERATION_PHASE_TOMBSTONED);
    return 0;
}

static int test_fixed_high_water_history_release(void)
{
    static const uint8_t result_byte = 0x71U;
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t keys[UCN_V6_OPERATION_HIGH_WATER_SLOTS];
    ucn_v6_operation_key_t newcomer;
    ucn_v6_operation_key_t active_again;
    ucn_v6_operation_admission_t admission;
    ucn_v6_operation_journal_view_t view;
    uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    size_t index;
    unsigned lifecycle_calls_before;

    memset(&store, 0, sizeof(store));
    fill_digest(request_digest, 0x71U);
    fill_digest(result_digest, 0x81U);
    store.authorize_result_retirement = true;
    store.authorize_tombstone_reclaim = true;
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);

    for (index = 0U; index < UCN_V6_OPERATION_HIGH_WATER_SLOTS; ++index) {
        keys[index] = make_key((uint8_t)(0x80U + index),
                               (uint64_t)(10U + index));
        CHECK(ucn_v6_operation_prepare(
                  journal, &keys[index], 42U,
                  UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
                  request_digest, &admission) == UCN_V6_OK);
        CHECK(ucn_v6_operation_mark_executing(journal, &keys[index]) ==
              UCN_V6_OK);
        CHECK(ucn_v6_operation_commit_result(
                  journal, &keys[index], 0, &result_byte, 1U,
                  result_digest) == UCN_V6_OK);
        CHECK(ucn_v6_operation_tombstone_result(journal, &keys[index]) ==
              UCN_V6_OK);
        store.lifecycle_high_water = keys[index].operation_id + 1U;
        CHECK(ucn_v6_operation_reclaim_tombstone(journal, &keys[index]) ==
              UCN_V6_OK);
    }
    CHECK(ucn_v6_operation_journal_copy_view(journal, &view) == UCN_V6_OK);
    CHECK(view.occupied_high_waters ==
          (uint16_t)UCN_V6_OPERATION_HIGH_WATER_SLOTS);

    newcomer = make_key(0xE0U, 91U);
    CHECK(ucn_v6_operation_prepare(
              journal, &newcomer, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    CHECK(ucn_v6_operation_mark_executing(journal, &newcomer) == UCN_V6_OK);
    CHECK(ucn_v6_operation_commit_result(
              journal, &newcomer, 0, &result_byte, 1U,
              result_digest) == UCN_V6_OK);
    CHECK(ucn_v6_operation_tombstone_result(journal, &newcomer) ==
          UCN_V6_OK);
    store.lifecycle_high_water = 92U;
    lifecycle_calls_before = store.lifecycle_calls;
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &newcomer) ==
          UCN_V6_ERR_NO_SPACE);
    CHECK(store.lifecycle_calls == lifecycle_calls_before);

    CHECK(ucn_v6_operation_release_principal_history(
              journal, &keys[0].initiator_principal) == UCN_V6_ERR_ACCESS);
    store.authorize_history_release = true;
    store.reenter_journal = journal;
    store.reenter_key = make_key(0xE1U, 92U);
    fill_digest(store.reenter_digest, 0x91U);
    CHECK(ucn_v6_operation_release_principal_history(
              journal, &keys[0].initiator_principal) == UCN_V6_ERR_STATE);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_journal_copy_view(journal, &view) == UCN_V6_OK);
    CHECK(view.occupied_high_waters ==
          (uint16_t)UCN_V6_OPERATION_HIGH_WATER_SLOTS);
    store.reenter_journal = NULL;
    CHECK(ucn_v6_operation_release_principal_history(
              journal, &keys[0].initiator_principal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_copy_view(journal, &view) == UCN_V6_OK);
    CHECK(view.occupied_high_waters ==
          (uint16_t)(UCN_V6_OPERATION_HIGH_WATER_SLOTS - 1U));
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &newcomer) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_copy_view(journal, &view) == UCN_V6_OK);
    CHECK(view.occupied_high_waters ==
          (uint16_t)UCN_V6_OPERATION_HIGH_WATER_SLOTS);

    active_again = newcomer;
    active_again.operation_id = 200U;
    CHECK(ucn_v6_operation_prepare(
              journal, &active_again, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    CHECK(ucn_v6_operation_release_principal_history(
              journal, &active_again.initiator_principal) ==
          UCN_V6_ERR_STATE);
    return 0;
}

static int test_reboot_moves_executing_to_in_doubt(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t journal_storage;
    ucn_v6_operation_journal_storage_t restarted_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_journal_t *restarted = NULL;
    ucn_v6_operation_key_t key = make_key(0x51U, 99U);
    ucn_v6_operation_slot_t slot;
    ucn_v6_operation_admission_t admission;
    uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    uint8_t terminal_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    unsigned submits_before_restart;

    memset(&store, 0, sizeof(store));
    fill_digest(request_digest, 0x61U);
    fill_digest(terminal_digest, 0x71U);
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              journal_storage.bytes, sizeof(journal_storage),
              ucn_v6_compiled_manifest(), &ops, &gate,
              &journal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 8U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    CHECK(ucn_v6_operation_mark_executing(journal, &key) == UCN_V6_OK);
    submits_before_restart = store.submits;
    CHECK(ucn_v6_operation_journal_init_in_place(
              restarted_storage.bytes, sizeof(restarted_storage),
              ucn_v6_compiled_manifest(), &ops, &gate,
              &restarted) == UCN_V6_OK);
    CHECK(store.submits == submits_before_restart + 1U);
    CHECK(ucn_v6_operation_copy_slot(restarted, &key, &slot) == UCN_V6_OK);
    CHECK(slot.phase == UCN_V6_OPERATION_PHASE_IN_DOUBT);
    store.lifecycle_result = UCN_V6_ERR_ACCESS;
    CHECK(ucn_v6_operation_resolve_in_doubt(
              restarted, &key, NULL) == UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_operation_copy_slot(restarted, &key, &slot) == UCN_V6_OK);
    CHECK(slot.phase == UCN_V6_OPERATION_PHASE_IN_DOUBT);
    store.lifecycle_result = UCN_V6_OK;
    memset(&store.reconciliation, 0, sizeof(store.reconciliation));
    store.reconciliation.outcome =
        UCN_V6_OPERATION_RECONCILIATION_TOMBSTONE;
    store.reconciliation.result_code = -1;
    memcpy(store.reconciliation.terminal_digest, terminal_digest,
           sizeof(terminal_digest));
    CHECK(ucn_v6_operation_resolve_in_doubt(
              restarted, &key, NULL) == UCN_V6_OK);
    CHECK(ucn_v6_operation_copy_slot(restarted, &key, &slot) == UCN_V6_OK);
    CHECK(slot.phase == UCN_V6_OPERATION_PHASE_TOMBSTONED);
    return 0;
}

static int test_committed_witness_rejects_journal_rollback(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_storage_t restarted_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_journal_t *restarted = NULL;
    ucn_v6_operation_journal_snapshot_t prepared;
    ucn_v6_operation_key_t key = make_key(0xB1U, 77U);
    ucn_v6_operation_admission_t admission;
    uint8_t request_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    uint8_t result_digest[UCN_V6_OPERATION_DIGEST_BYTES];

    memset(&store, 0, sizeof(store));
    fill_digest(request_digest, 0x41U);
    fill_digest(result_digest, 0x61U);
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 7U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_OK);
    prepared = store.durable;
    CHECK(ucn_v6_operation_mark_executing(journal, &key) == UCN_V6_OK);
    CHECK(ucn_v6_operation_commit_result(
              journal, &key, 0, NULL, 0U, result_digest) == UCN_V6_OK);
    CHECK(store.witness.journal_committed_generation ==
          store.durable.snapshot_generation);

    /* Simulate a dual-slot store silently falling back to an older but
     * internally valid PREPARED record.  The independent witness must make
     * this a permanent fail-closed startup, never a second execution. */
    store.durable = prepared;
    CHECK(ucn_v6_operation_journal_init_in_place(
              restarted_storage.bytes, sizeof(restarted_storage),
              ucn_v6_compiled_manifest(), &ops, &gate,
              &restarted) == UCN_V6_ERR_STATE);
    CHECK(restarted == NULL);
    return 0;
}

static int test_pending_witness_restart_resolution(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_storage_t committed_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_journal_t *committed_journal = NULL;
    ucn_v6_operation_journal_view_t view;

    memset(&store, 0, sizeof(store));
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    store.has_witness = true;
    store.witness.magic = UCN_V6_MESSAGE_WITNESS_MAGIC;
    store.witness.schema = UCN_V6_MESSAGE_WITNESS_SCHEMA;
    store.witness.flags = UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED;
    store.witness.witness_generation = 1U;
    store.witness.journal_pending_generation = 1U;

    /* Crash before the journal submit: recovery cancels the unpublished
     * reservation while monotonically advancing the witness record. */
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    CHECK(store.witness.witness_generation == 2U);
    CHECK(store.witness.journal_pending_generation == 0U);
    CHECK(store.witness.journal_committed_generation == 0U);
    CHECK((store.witness.flags &
           UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED) == 0U);

    /* Crash after the new journal slot is durable but before the witness
     * finalization: restart must finish the pending transition, not roll the
     * journal back or allocate a second generation. */
    memset(&store, 0, sizeof(store));
    ops = fake_ops(&store);
    store.has_witness = true;
    store.witness.magic = UCN_V6_MESSAGE_WITNESS_MAGIC;
    store.witness.schema = UCN_V6_MESSAGE_WITNESS_SCHEMA;
    store.witness.flags = UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED;
    store.witness.witness_generation = 1U;
    store.witness.journal_pending_generation = 1U;
    store.has_journal = true;
    store.durable.magic = UCN_V6_OPERATION_JOURNAL_MAGIC;
    store.durable.schema = UCN_V6_OPERATION_JOURNAL_SCHEMA;
    store.durable.slot_count = UCN_V6_OPERATION_JOURNAL_SLOTS;
    store.durable.snapshot_generation = 1U;
    CHECK(ucn_v6_operation_journal_init_in_place(
              committed_storage.bytes, sizeof(committed_storage),
              ucn_v6_compiled_manifest(), &ops, &gate,
              &committed_journal) == UCN_V6_OK);
    CHECK(store.witness.witness_generation == 2U);
    CHECK(store.witness.journal_pending_generation == 0U);
    CHECK(store.witness.journal_committed_generation == 1U);
    CHECK(ucn_v6_operation_journal_copy_view(
              committed_journal, &view) == UCN_V6_OK);
    CHECK(view.committed_generation == 1U &&
          view.pending_generation == 0U);
    return 0;
}

static int test_witness_transition_contract(void)
{
    ucn_v6_message_witness_t factory;
    ucn_v6_message_witness_t allocated;
    ucn_v6_message_witness_t pending;
    ucn_v6_message_witness_t committed;
    ucn_v6_message_witness_t invalid;

    memset(&factory, 0, sizeof(factory));
    factory.magic = UCN_V6_MESSAGE_WITNESS_MAGIC;
    factory.schema = UCN_V6_MESSAGE_WITNESS_SCHEMA;
    allocated = factory;
    allocated.flags = UCN_V6_MESSAGE_WITNESS_ALLOCATOR_COMMISSIONED;
    allocated.witness_generation = 1U;
    allocated.operation_id_high_water = 4U;
    CHECK(ucn_v6_message_witness_transition_validate(
              &factory, &allocated) == UCN_V6_OK);

    pending = allocated;
    pending.flags |= UCN_V6_MESSAGE_WITNESS_JOURNAL_COMMISSIONED;
    pending.witness_generation = 2U;
    pending.journal_pending_generation = 1U;
    CHECK(ucn_v6_message_witness_transition_validate(
              &allocated, &pending) == UCN_V6_OK);
    committed = pending;
    committed.witness_generation = 3U;
    committed.journal_committed_generation = 1U;
    committed.journal_pending_generation = 0U;
    CHECK(ucn_v6_message_witness_transition_validate(
              &pending, &committed) == UCN_V6_OK);

    invalid = committed;
    invalid.witness_generation = 4U;
    invalid.operation_id_high_water = 3U;
    CHECK(ucn_v6_message_witness_transition_validate(
              &committed, &invalid) == UCN_V6_ERR_STATE);
    invalid = committed;
    invalid.witness_generation = 4U;
    invalid.journal_committed_generation = 0U;
    CHECK(ucn_v6_message_witness_transition_validate(
              &committed, &invalid) == UCN_V6_ERR_STATE);
    return 0;
}

static int test_fixed_capacity_and_provider_reentrancy(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t journal_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t key;
    ucn_v6_operation_admission_t admission;
    uint8_t digest[UCN_V6_OPERATION_DIGEST_BYTES];
    ucn_v6_operation_slot_t before_abort;
    ucn_v6_operation_slot_t after_abort;
    size_t index;
    unsigned submits_before;

    memset(&store, 0, sizeof(store));
    fill_digest(digest, 0x81U);
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              journal_storage.bytes, sizeof(journal_storage),
              ucn_v6_compiled_manifest(), &ops, &gate,
              &journal) == UCN_V6_OK);
    for (index = 0U; index < UCN_V6_OPERATION_JOURNAL_SLOTS; ++index) {
        key = make_key((uint8_t)(0x11U + index), index + 1U);
        CHECK(ucn_v6_operation_prepare(
                  journal, &key, 9U,
                  UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
                  digest, &admission) == UCN_V6_OK);
    }
    key = make_key(0xE0U, 99U);
    submits_before = store.submits;
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 9U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              digest, &admission) == UCN_V6_ERR_NO_SPACE);
    CHECK(store.submits == submits_before);

    store.reenter_journal = journal;
    store.reenter_key = make_key(0xF0U, 100U);
    memcpy(store.reenter_digest, digest, sizeof(digest));
    key = make_key(0x11U, 1U);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &before_abort) ==
          UCN_V6_OK);
    submits_before = store.submits;
    CHECK(ucn_v6_operation_abort_prepared(journal, &key) ==
          UCN_V6_ERR_STATE);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &after_abort) ==
          UCN_V6_OK);
    CHECK(memcmp(&before_abort, &after_abort, sizeof(before_abort)) == 0);

    store.authorize_prepared_abort = true;
    store.abort_result_code = -2;
    memset(store.abort_digest, 0, sizeof(store.abort_digest));
    CHECK(ucn_v6_operation_abort_prepared(journal, &key) ==
          UCN_V6_ERR_STATE);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &after_abort) ==
          UCN_V6_OK);
    CHECK(memcmp(&before_abort, &after_abort, sizeof(before_abort)) == 0);

    memcpy(store.abort_digest, digest, sizeof(store.abort_digest));
    CHECK(ucn_v6_operation_abort_prepared(journal, &key) ==
          UCN_V6_ERR_STATE);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(store.submits == submits_before);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &after_abort) ==
          UCN_V6_OK);
    CHECK(memcmp(&before_abort, &after_abort, sizeof(before_abort)) == 0);
    store.reenter_journal = NULL;
    CHECK(ucn_v6_operation_abort_prepared(journal, &key) == UCN_V6_OK);
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &after_abort) ==
          UCN_V6_OK);
    CHECK(after_abort.phase == UCN_V6_OPERATION_PHASE_TOMBSTONED &&
          after_abort.result_code == -2 &&
          memcmp(after_abort.result_digest, digest,
                 sizeof(after_abort.result_digest)) == 0);
    return 0;
}

static int test_invalid_record_and_store_failure_fail_closed(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t journal_storage;
    ucn_v6_operation_journal_storage_t before_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_journal_view_t before_view;
    ucn_v6_operation_journal_view_t after_view;
    ucn_v6_operation_key_t key = make_key(0x91U, 1U);
    ucn_v6_operation_admission_t admission =
        (ucn_v6_operation_admission_t)0;
    uint8_t digest[UCN_V6_OPERATION_DIGEST_BYTES];

    memset(&store, 0, sizeof(store));
    memset(&journal_storage, 0xA5, sizeof(journal_storage));
    before_storage = journal_storage;
    fill_digest(digest, 0xA1U);
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    store.has_journal = true;
    CHECK(ucn_v6_operation_journal_init_in_place(
              journal_storage.bytes, sizeof(journal_storage),
              ucn_v6_compiled_manifest(), &ops, &gate,
              &journal) == UCN_V6_ERR_STATE);
    CHECK(memcmp(&journal_storage, &before_storage,
                 sizeof(journal_storage)) == 0);

    memset(&store, 0, sizeof(store));
    ops = fake_ops(&store);
    CHECK(ucn_v6_operation_journal_init_in_place(
              journal_storage.bytes, sizeof(journal_storage),
              ucn_v6_compiled_manifest(), &ops, &gate,
              &journal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_copy_view(
              journal, &before_view) == UCN_V6_OK);
    store.fail_submit = true;
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 12U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              digest, &admission) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_operation_journal_copy_view(
              journal, &after_view) == UCN_V6_OK);
    CHECK(after_view.faulted);
    CHECK(after_view.committed_generation == before_view.committed_generation);
    CHECK(after_view.occupied_slots == before_view.occupied_slots);
    CHECK(!store.has_journal);
    CHECK(admission == (ucn_v6_operation_admission_t)0);
    return 0;
}

static int test_endpoint_dispatch_durable_closure(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xA2U, 0xA3U };
    fake_store_t store;
    fake_executor_t executor;
    fake_digest_provider_t digest_provider;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_digest_ops_t digest_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_message_descriptor_t message;
    ucn_v6_principal_t principal;
    ucn_v6_endpoint_dispatch_result_t result;
    ucn_v6_endpoint_dispatch_result_t replay;

    memset(&store, 0, sizeof(store));
    memset(&executor, 0, sizeof(executor));
    memset(&digest_provider, 0, sizeof(digest_provider));
    make_dispatch_contract(&endpoint, &message);
    fill_principal(&principal, 0x31U);
    ops = fake_ops(&store);
    digest_ops = fake_digest_ops(&digest_provider);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_OK);
    CHECK(executor.calls == 1U && store.submits == 3U);
    CHECK(result.action == UCN_V6_ENDPOINT_DISPATCH_EXECUTED &&
          result.result_code == 17 && result.result_length == 2U &&
          result.result[0] == 0x51U && result.result[1] == 0x52U);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &replay) == UCN_V6_OK);
    CHECK(executor.calls == 1U && store.submits == 3U);
    CHECK(replay.action == UCN_V6_ENDPOINT_DISPATCH_RESULT_REPLAY &&
          replay.result_code == result.result_code &&
          replay.result_length == result.result_length &&
          memcmp(replay.result, result.result, result.result_length) == 0 &&
          memcmp(replay.result_digest, result.result_digest,
                 sizeof(replay.result_digest)) == 0);
    return 0;
}

static int test_endpoint_callback_hidden_reentry_fails_closed(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xA2U, 0xA3U };
    fake_store_t store;
    fake_executor_t executor;
    fake_digest_provider_t digest_provider;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_digest_ops_t digest_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_message_descriptor_t message;
    ucn_v6_principal_t principal;
    ucn_v6_endpoint_dispatch_result_t result;
    ucn_v6_endpoint_dispatch_result_t sentinel;
    ucn_v6_operation_key_t key;
    ucn_v6_operation_slot_t slot;

    memset(&store, 0, sizeof(store));
    memset(&executor, 0, sizeof(executor));
    memset(&digest_provider, 0, sizeof(digest_provider));
    make_dispatch_contract(&endpoint, &message);
    fill_principal(&principal, 0x41U);
    ops = fake_ops(&store);
    digest_ops = fake_digest_ops(&digest_provider);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);

    memset(&result, 0xA5, sizeof(result));
    sentinel = result;
    digest_provider.reenter_journal = journal;
    digest_provider.reenter_key = make_key(0x42U, 99U);
    fill_digest(digest_provider.reenter_digest, 0x62U);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_STATE);
    CHECK(digest_provider.reenter_result == UCN_V6_ERR_STATE);
    CHECK(executor.calls == 0U && store.submits == 0U);
    CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);

    digest_provider.reenter_journal = NULL;
    executor.reenter_journal = journal;
    executor.reenter_key = make_key(0x43U, 100U);
    fill_digest(executor.reenter_digest, 0x72U);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_STATE);
    CHECK(executor.reenter_result == UCN_V6_ERR_STATE);
    CHECK(executor.calls == 1U && store.submits == 3U);
    CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
    key.initiator_principal = principal;
    key.operation_id = message.operation_id;
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK);
    CHECK(slot.phase == UCN_V6_OPERATION_PHASE_IN_DOUBT);
    return 0;
}

static int test_store_provider_hidden_reentry_fails_closed(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t key = make_key(0x51U, 1U);
    ucn_v6_operation_admission_t admission =
        (ucn_v6_operation_admission_t)0;
    ucn_v6_operation_journal_view_t before;
    ucn_v6_operation_journal_view_t after;
    uint8_t digest[UCN_V6_OPERATION_DIGEST_BYTES];

    memset(&store, 0, sizeof(store));
    fill_digest(digest, 0x81U);
    ops = fake_ops(&store);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_copy_view(journal, &before) == UCN_V6_OK);
    store.reenter_journal = journal;
    store.reenter_key = make_key(0x52U, 2U);
    fill_digest(store.reenter_digest, 0x91U);
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              digest, &admission) == UCN_V6_ERR_STATE);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    CHECK(admission == (ucn_v6_operation_admission_t)0);
    CHECK(ucn_v6_operation_journal_copy_view(journal, &after) == UCN_V6_OK);
    CHECK(after.faulted &&
          after.committed_generation == before.committed_generation &&
          after.occupied_slots == before.occupied_slots);
    return 0;
}

static int test_endpoint_dispatch_canonical_request_binding(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xA2U, 0xA3U };
    static const uint8_t changed_payload[] = { 0xA1U, 0xA2U, 0xB3U };
    fake_store_t store;
    fake_executor_t executor;
    fake_digest_provider_t digest_provider;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_digest_ops_t digest_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_endpoint_contract_t changed_endpoint;
    ucn_v6_message_descriptor_t message;
    ucn_v6_message_descriptor_t changed_message;
    ucn_v6_principal_t principal;
    ucn_v6_principal_t other_principal;
    ucn_v6_operation_key_t key;
    ucn_v6_operation_slot_t first_slot;
    ucn_v6_operation_slot_t other_slot;
    ucn_v6_endpoint_dispatch_result_t result;
    ucn_v6_endpoint_dispatch_result_t before;

    memset(&store, 0, sizeof(store));
    memset(&executor, 0, sizeof(executor));
    memset(&digest_provider, 0, sizeof(digest_provider));
    make_dispatch_contract(&endpoint, &message);
    fill_principal(&principal, 0x35U);
    ops = fake_ops(&store);
    digest_ops = fake_digest_ops(&digest_provider);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_OK);
    CHECK(executor.calls == 1U);
    key.initiator_principal = principal;
    key.operation_id = message.operation_id;
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &first_slot) ==
          UCN_V6_OK);

    memset(&result, 0xA5, sizeof(result));
    before = result;
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, changed_payload,
              (uint16_t)sizeof(changed_payload), &digest_ops,
              execute_request, &executor, &result) == UCN_V6_ERR_REPLAY);
    CHECK(executor.calls == 1U &&
          memcmp(&result, &before, sizeof(result)) == 0);

    changed_message = message;
    changed_message.source_endpoint = 8U;
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &changed_message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_REPLAY);
    CHECK(executor.calls == 1U &&
          memcmp(&result, &before, sizeof(result)) == 0);

    changed_endpoint = endpoint;
    changed_endpoint.max_result_bytes = 15U;
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &changed_endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_REPLAY);
    CHECK(executor.calls == 1U &&
          memcmp(&result, &before, sizeof(result)) == 0);

    fill_principal(&other_principal, 0x75U);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &other_principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_OK);
    key.initiator_principal = other_principal;
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &other_slot) ==
          UCN_V6_OK);
    CHECK(executor.calls == 2U &&
          memcmp(first_slot.request_digest, other_slot.request_digest,
                 sizeof(first_slot.request_digest)) != 0);
    return 0;
}

static int test_endpoint_dispatch_rejects_inconsistent_durable_result(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xA2U, 0xA3U };
    fake_store_t store;
    fake_executor_t executor;
    fake_digest_provider_t digest_provider;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_digest_ops_t digest_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_storage_t restarted_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_journal_t *restarted = NULL;
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_message_descriptor_t message;
    ucn_v6_principal_t principal;
    ucn_v6_endpoint_dispatch_result_t result;
    ucn_v6_endpoint_dispatch_result_t before;
    ucn_v6_operation_journal_view_t view;
    ucn_v6_operation_journal_snapshot_t pristine;
    unsigned mutation;

    memset(&store, 0, sizeof(store));
    memset(&executor, 0, sizeof(executor));
    memset(&digest_provider, 0, sizeof(digest_provider));
    make_dispatch_contract(&endpoint, &message);
    fill_principal(&principal, 0x36U);
    ops = fake_ops(&store);
    digest_ops = fake_digest_ops(&digest_provider);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_OK);
    CHECK(store.has_journal && store.durable.slots[0].result_length == 2U);
    pristine = store.durable;

    /* Structurally valid records with a changed code, length, or result byte
     * must all fail closed on replay. / 结构仍合法、但结果码、长度或结果字节
     * 任一变化的持久记录，重放时都必须失败关闭。 */
    for (mutation = 0U; mutation < 3U; ++mutation) {
        store.durable = pristine;
        if (mutation == 0U) {
            ++store.durable.slots[0].result_code;
        } else if (mutation == 1U) {
            store.durable.slots[0].result_length = 3U;
        } else {
            store.durable.slots[0].result[0] ^= UINT8_C(1);
        }
        restarted = NULL;
        CHECK(ucn_v6_operation_journal_init_in_place(
                  restarted_storage.bytes, sizeof(restarted_storage),
                  ucn_v6_compiled_manifest(), &ops, &gate,
                  &restarted) == UCN_V6_OK);
        memset(&result, 0x5A, sizeof(result));
        before = result;
        CHECK(ucn_v6_endpoint_dispatch_request(
                  restarted, &endpoint, &message, &principal, payload,
                  (uint16_t)sizeof(payload), &digest_ops, execute_request,
                  &executor, &result) == UCN_V6_ERR_STATE);
        CHECK(executor.calls == 1U &&
              memcmp(&result, &before, sizeof(result)) == 0);
        CHECK(ucn_v6_operation_journal_copy_view(restarted, &view) ==
              UCN_V6_OK);
        CHECK(view.faulted);
    }
    return 0;
}

static int test_endpoint_dispatch_uncertain_never_reexecutes(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xA2U, 0xA3U };
    fake_store_t store;
    fake_executor_t executor;
    fake_digest_provider_t digest_provider;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_digest_ops_t digest_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_message_descriptor_t message;
    ucn_v6_principal_t principal;
    ucn_v6_operation_key_t key;
    ucn_v6_operation_slot_t slot;
    ucn_v6_endpoint_dispatch_result_t result;
    ucn_v6_endpoint_dispatch_result_t before;

    memset(&store, 0, sizeof(store));
    memset(&executor, 0, sizeof(executor));
    memset(&digest_provider, 0, sizeof(digest_provider));
    make_dispatch_contract(&endpoint, &message);
    fill_principal(&principal, 0x41U);
    ops = fake_ops(&store);
    digest_ops = fake_digest_ops(&digest_provider);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    executor.callback_result = UCN_V6_ERR_TIMEOUT;
    memset(&result, 0xA5, sizeof(result));
    before = result;
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_TIMEOUT);
    CHECK(memcmp(&result, &before, sizeof(result)) == 0 &&
          executor.calls == 1U);
    key.initiator_principal = principal;
    key.operation_id = message.operation_id;
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK &&
          slot.phase == UCN_V6_OPERATION_PHASE_IN_DOUBT);
    executor.callback_result = UCN_V6_OK;
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_OK);
    CHECK(result.action == UCN_V6_ENDPOINT_DISPATCH_IN_DOUBT &&
          executor.calls == 1U);
    return 0;
}

static int test_endpoint_dispatch_digest_failure_boundaries(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xA2U, 0xA3U };
    fake_store_t store;
    fake_executor_t executor;
    fake_digest_provider_t digest_provider;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_digest_ops_t digest_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_message_descriptor_t message;
    ucn_v6_principal_t principal;
    ucn_v6_operation_key_t key;
    ucn_v6_operation_slot_t slot;
    ucn_v6_operation_journal_view_t before_view;
    ucn_v6_operation_journal_view_t after_view;
    ucn_v6_endpoint_dispatch_result_t result;
    ucn_v6_endpoint_dispatch_result_t before;

    memset(&store, 0, sizeof(store));
    memset(&executor, 0, sizeof(executor));
    memset(&digest_provider, 0, sizeof(digest_provider));
    make_dispatch_contract(&endpoint, &message);
    fill_principal(&principal, 0x45U);
    ops = fake_ops(&store);
    digest_ops = fake_digest_ops(&digest_provider);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);

    digest_ops.algorithm_id = 0U;
    CHECK(ucn_v6_operation_journal_copy_view(journal, &before_view) ==
          UCN_V6_OK);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_ARGUMENT);
    CHECK(ucn_v6_operation_journal_copy_view(journal, &after_view) ==
          UCN_V6_OK);
    CHECK(memcmp(&before_view, &after_view, sizeof(before_view)) == 0 &&
          executor.calls == 0U);

    digest_ops = fake_digest_ops(&digest_provider);
    digest_provider.calls = 0U;
    digest_provider.fail_at = 2U;
    memset(&result, 0xA5, sizeof(result));
    before = result;
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_STATE);
    CHECK(executor.calls == 1U &&
          memcmp(&result, &before, sizeof(result)) == 0);
    key.initiator_principal = principal;
    key.operation_id = message.operation_id;
    CHECK(ucn_v6_operation_copy_slot(journal, &key, &slot) == UCN_V6_OK &&
          slot.phase == UCN_V6_OPERATION_PHASE_IN_DOUBT);
    return 0;
}

static int test_endpoint_dispatch_persistence_failure_prevents_repeat(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xA2U, 0xA3U };
    fake_store_t store;
    fake_executor_t executor;
    fake_digest_provider_t digest_provider;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_message_digest_ops_t digest_ops;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_operation_journal_storage_t storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_message_descriptor_t message;
    ucn_v6_principal_t principal;
    ucn_v6_endpoint_dispatch_result_t result;
    ucn_v6_endpoint_dispatch_result_t before;

    memset(&store, 0, sizeof(store));
    memset(&executor, 0, sizeof(executor));
    memset(&digest_provider, 0, sizeof(digest_provider));
    make_dispatch_contract(&endpoint, &message);
    fill_principal(&principal, 0x51U);
    ops = fake_ops(&store);
    digest_ops = fake_digest_ops(&digest_provider);
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, no_lock, no_lock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_journal_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &ops, &gate, &journal) == UCN_V6_OK);
    store.fail_journal_submit_at = 3U;
    memset(&result, 0x5A, sizeof(result));
    before = result;
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_STATE);
    CHECK(executor.calls == 1U && memcmp(&result, &before, sizeof(result)) == 0);
    CHECK(store.has_journal &&
          store.durable.slots[0].phase == UCN_V6_OPERATION_PHASE_EXECUTING);
    CHECK(ucn_v6_endpoint_dispatch_request(
              journal, &endpoint, &message, &principal, payload,
              (uint16_t)sizeof(payload), &digest_ops, execute_request,
              &executor, &result) == UCN_V6_ERR_STATE);
    CHECK(executor.calls == 1U);
    return 0;
}

static int test_endpoint_dispatch_non_durable_direct_path(void)
{
    static const uint8_t payload[] = { 0xA1U, 0xA2U, 0xA3U };
    fake_executor_t executor;
    ucn_v6_endpoint_contract_t endpoint;
    ucn_v6_message_descriptor_t message;
    ucn_v6_endpoint_dispatch_result_t result;

    memset(&executor, 0, sizeof(executor));
    make_dispatch_contract(&endpoint, &message);
    endpoint.execution_contract = UCN_V6_ENDPOINT_IDEMPOTENT_REPLAYABLE;
    CHECK(ucn_v6_endpoint_dispatch_request(
              NULL, &endpoint, &message, NULL, payload,
              (uint16_t)sizeof(payload), NULL, execute_request, &executor,
              &result) == UCN_V6_OK);
    CHECK(executor.calls == 1U &&
          result.action == UCN_V6_ENDPOINT_DISPATCH_EXECUTED);
    return 0;
}

int main(void)
{
    CHECK(test_message_axes_are_orthogonal() == 0);
    CHECK(test_operation_id_reservation_survives_restart() == 0);
    CHECK(test_message_opaque_storage_preflight() == 0);
    CHECK(test_durable_journal_lifecycle_and_gc() == 0);
    CHECK(test_lifecycle_provider_reconciliation_and_reentrancy() == 0);
    CHECK(test_exact_result_replay_precedes_retirement_floor() == 0);
    CHECK(test_retirement_floor_cannot_cross_nonterminal_slot() == 0);
    CHECK(test_fixed_high_water_history_release() == 0);
    CHECK(test_reboot_moves_executing_to_in_doubt() == 0);
    CHECK(test_committed_witness_rejects_journal_rollback() == 0);
    CHECK(test_pending_witness_restart_resolution() == 0);
    CHECK(test_witness_transition_contract() == 0);
    CHECK(test_fixed_capacity_and_provider_reentrancy() == 0);
    CHECK(test_invalid_record_and_store_failure_fail_closed() == 0);
    CHECK(test_endpoint_dispatch_durable_closure() == 0);
    CHECK(test_endpoint_callback_hidden_reentry_fails_closed() == 0);
    CHECK(test_store_provider_hidden_reentry_fails_closed() == 0);
    CHECK(test_endpoint_dispatch_canonical_request_binding() == 0);
    CHECK(test_endpoint_dispatch_rejects_inconsistent_durable_result() == 0);
    CHECK(test_endpoint_dispatch_uncertain_never_reexecutes() == 0);
    CHECK(test_endpoint_dispatch_digest_failure_boundaries() == 0);
    CHECK(test_endpoint_dispatch_persistence_failure_prevents_repeat() == 0);
    CHECK(test_endpoint_dispatch_non_durable_direct_path() == 0);
    puts("ucn v6 message tests passed");
    return 0;
}
