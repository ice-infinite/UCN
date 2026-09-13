#ifndef UCN_TEST_FAKE_PERSISTENCE_PROVIDER_H
#define UCN_TEST_FAKE_PERSISTENCE_PROVIDER_H

#include "ucn/ucn_persistence.h"

typedef struct fake_persist_domain_store {
    ucn_persist_domain_key_t key;
    uint8_t slots[UCN_PERSIST_SLOT_COUNT][UCN_PERSIST_SLOT_BYTES];
    uint32_t slot_bytes;
    uint64_t witness_generation;
    uint8_t witness_state;
} fake_persist_domain_store_t;

typedef struct fake_persist_pending_io {
    uint8_t valid;
    uint8_t phase;
    uint8_t slot;
    uint8_t reserved_zero;
    uint64_t token;
    ucn_persist_domain_key_t domain;
    uint8_t *output_buffer;
    const uint8_t *input_buffer;
    size_t exact_bytes;
    ucn_persist_witness_view_t *witness_output;
    uint64_t expected_old;
    uint64_t exact_new;
} fake_persist_pending_io_t;

typedef struct fake_persist_provider {
    fake_persist_domain_store_t domains[UCN_PERSIST_DOMAIN_COUNT];
    uint8_t domain_count;
    uint8_t pending_once_phase;
    uint8_t fail_once_phase;
    uint8_t corrupt_readback_once;
    uint8_t invalid_blob_once_phase;
    uint8_t invalid_blob_state;
    uint8_t poll_pending_remaining;
    uint8_t reserved_zero[3];
    uint32_t calls[7];
    fake_persist_pending_io_t pending;
    void (*reenter_hook)(void *context);
    void *reenter_context;
} fake_persist_provider_t;

void fake_persist_provider_init(fake_persist_provider_t *provider,
                                const ucn_persist_manifest_t *manifest,
                                uint8_t erased_value);
void fake_persist_provider_make_public(
    fake_persist_provider_t *provider,
    uint8_t erased_value,
    ucn_persistence_provider_t *public_provider_out);
const ucn_persistence_provider_vtable_t *fake_persist_provider_vtable(void);

#endif
