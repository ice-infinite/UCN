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
    bool corrupt_reload;
    ucn_v6_operation_journal_t *reenter_journal;
    ucn_v6_operation_key_t reenter_key;
    uint8_t reenter_digest[UCN_V6_OPERATION_DIGEST_BYTES];
    ucn_v6_result_t reenter_result;
    ucn_v6_operation_id_allocator_t *reenter_allocator;
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
    if (store->fail_submit) {
        return UCN_V6_ERR_STATE;
    }
    store->previous_durable = store->durable;
    store->has_previous_journal = store->has_journal;
    store->durable = *snapshot;
    store->has_journal = true;
    return UCN_V6_OK;
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
    ucn_v6_callback_gate_t gate;
    ucn_v6_operation_id_allocator_storage_t allocator_storage;
    ucn_v6_operation_id_allocator_storage_t restarted_storage;
    ucn_v6_operation_id_allocator_t *allocator = NULL;
    ucn_v6_operation_id_allocator_t *restarted = NULL;
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
    CHECK(ucn_v6_operation_id_take(restarted, &id) == UCN_V6_OK && id == 9U);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    return 0;
}

static int test_message_opaque_storage_preflight(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate;
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
    ucn_v6_callback_gate_t gate;
    ucn_v6_operation_journal_storage_t journal_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t key = make_key(0x10U, 7U);
    ucn_v6_operation_slot_t slot;
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
    CHECK(ucn_v6_operation_tombstone_result(journal, &key, false, true) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_operation_tombstone_result(journal, &key, true, true) ==
          UCN_V6_OK);
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &key, 8U, false) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_operation_reclaim_tombstone(journal, &key, 8U, true) ==
          UCN_V6_OK);
    admission = (ucn_v6_operation_admission_t)0;
    CHECK(ucn_v6_operation_prepare(
              journal, &key, 42U,
              UCN_V6_ENDPOINT_DURABLE_AT_MOST_ONCE,
              request_digest, &admission) == UCN_V6_ERR_REPLAY);
    CHECK(admission == (ucn_v6_operation_admission_t)0);
    return 0;
}

static int test_reboot_moves_executing_to_in_doubt(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate;
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
    CHECK(ucn_v6_operation_resolve_in_doubt(
              restarted, &key, false, false, -1, NULL, 0U,
              terminal_digest) == UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_operation_copy_slot(restarted, &key, &slot) == UCN_V6_OK);
    CHECK(slot.phase == UCN_V6_OPERATION_PHASE_IN_DOUBT);
    CHECK(ucn_v6_operation_resolve_in_doubt(
              restarted, &key, true, false, -1, NULL, 0U,
              terminal_digest) == UCN_V6_OK);
    CHECK(ucn_v6_operation_copy_slot(restarted, &key, &slot) == UCN_V6_OK);
    CHECK(slot.phase == UCN_V6_OPERATION_PHASE_TOMBSTONED);
    return 0;
}

static int test_committed_witness_rejects_journal_rollback(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate;
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
    ucn_v6_callback_gate_t gate;
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
    ucn_v6_callback_gate_t gate;
    ucn_v6_operation_journal_storage_t journal_storage;
    ucn_v6_operation_journal_t *journal = NULL;
    ucn_v6_operation_key_t key;
    ucn_v6_operation_admission_t admission;
    uint8_t digest[UCN_V6_OPERATION_DIGEST_BYTES];
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
    CHECK(ucn_v6_operation_abort_prepared(journal, &key, -2, digest) ==
          UCN_V6_OK);
    CHECK(store.reenter_result == UCN_V6_ERR_STATE);
    return 0;
}

static int test_invalid_record_and_store_failure_fail_closed(void)
{
    fake_store_t store;
    ucn_v6_message_store_ops_t ops;
    ucn_v6_callback_gate_t gate;
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

int main(void)
{
    CHECK(test_message_axes_are_orthogonal() == 0);
    CHECK(test_operation_id_reservation_survives_restart() == 0);
    CHECK(test_message_opaque_storage_preflight() == 0);
    CHECK(test_durable_journal_lifecycle_and_gc() == 0);
    CHECK(test_reboot_moves_executing_to_in_doubt() == 0);
    CHECK(test_committed_witness_rejects_journal_rollback() == 0);
    CHECK(test_pending_witness_restart_resolution() == 0);
    CHECK(test_witness_transition_contract() == 0);
    CHECK(test_fixed_capacity_and_provider_reentrancy() == 0);
    CHECK(test_invalid_record_and_store_failure_fail_closed() == 0);
    puts("ucn v6 message tests passed");
    return 0;
}
