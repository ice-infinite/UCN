#include "fake_persistence_provider.h"
#include "internal/ucn_persistence.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            return __LINE__;                                                \
        }                                                                   \
    } while (0)

typedef struct pthread_lock {
    pthread_mutex_t mutex;
} pthread_lock_t;

typedef struct callback_block {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned entered;
    unsigned release;
} callback_block_t;

typedef struct concurrent_fixture {
    ucn_persist_manifest_entry_t entry;
    ucn_persist_domain_binding_t binding[2];
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake[2];
    ucn_persistence_provider_t provider[2];
    pthread_lock_t owner_lock[2];
    pthread_lock_t gate_lock;
    callback_block_t callback;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace[2];
    ucn_persistence_storage_t owner_storage[2];
    ucn_persist_callback_gate_t *gate;
    ucn_persistence_owner_t *owner[2];
    ucn_result_t thread_result;
    ucn_persistence_step_result_t thread_step;
} concurrent_fixture_t;

static concurrent_fixture_t fixture;

static ucn_result_t mutex_enter(void *context)
{
    pthread_lock_t *lock = (pthread_lock_t *)context;
    return lock != NULL && pthread_mutex_lock(&lock->mutex) == 0
               ? UCN_OK
               : UCN_ERR_STATE;
}

static void mutex_leave(void *context)
{
    pthread_lock_t *lock = (pthread_lock_t *)context;
    if (lock != NULL) {
        (void)pthread_mutex_unlock(&lock->mutex);
    }
}

static ucn_lock_ops_t make_lock(pthread_lock_t *lock)
{
    ucn_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_API_VERSION;
    ops.context = lock;
    ops.enter = mutex_enter;
    ops.leave = mutex_leave;
    return ops;
}

static void blocking_provider_hook(void *context)
{
    callback_block_t *block = (callback_block_t *)context;
    if (pthread_mutex_lock(&block->mutex) != 0) {
        return;
    }
    block->entered = 1U;
    (void)pthread_cond_broadcast(&block->condition);
    while (block->release == 0U) {
        if (pthread_cond_wait(&block->condition, &block->mutex) != 0) {
            break;
        }
    }
    (void)pthread_mutex_unlock(&block->mutex);
}

static void *step_first_owner(void *unused)
{
    (void)unused;
    fixture.thread_result = ucn_i_persistence_step(
        fixture.owner[0], 1U, 1U, &fixture.thread_step);
    return NULL;
}

static int configure_fixture(void)
{
    ucn_lock_ops_t gate_ops;
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    unsigned index;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(pthread_mutex_init(&fixture.gate_lock.mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture.owner_lock[0].mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture.owner_lock[1].mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture.callback.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&fixture.callback.condition, NULL) == 0);

    fixture.entry.struct_size = sizeof(fixture.entry);
    fixture.entry.api_version = UCN_PERSIST_API_VERSION;
    fixture.entry.domain.domain_kind = UCN_PERSIST_DOMAIN_PRODUCT_CONFIG;
    fixture.entry.domain.domain_id = 1U;
    fixture.entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    fixture.entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    fixture.entry.schema_id = 1U;
    fixture.entry.schema_version = 1U;
    fixture.entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    fixture.entry.witness_policy =
        UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    fixture.entry.provider_atomicity_class =
        UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    fixture.manifest.struct_size = sizeof(fixture.manifest);
    fixture.manifest.api_version = UCN_PERSIST_API_VERSION;
    fixture.manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    fixture.manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    fixture.manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    fixture.manifest.entries = &fixture.entry;
    fixture.manifest.entry_count = 1U;
    fixture.manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(
              &fixture.manifest, &fixture.digest_workspace[0], digest) ==
          UCN_OK);
    memcpy(fixture.manifest.expected_digest, digest, sizeof(digest));

    gate_ops = make_lock(&fixture.gate_lock);
    CHECK(ucn_persist_callback_gate_init_in_place(
              &fixture.gate_storage, sizeof(fixture.gate_storage), &gate_ops,
              &fixture.gate) == UCN_OK);
    for (index = 0U; index < 2U; ++index) {
        ucn_persistence_config_t config;
        ucn_lock_ops_t owner_ops = make_lock(&fixture.owner_lock[index]);
        fake_persist_provider_init(&fixture.fake[index], &fixture.manifest,
                                   0xFFU);
        fake_persist_provider_make_public(&fixture.fake[index], 0xFFU,
                                          &fixture.provider[index]);
        memset(&config, 0, sizeof(config));
        config.struct_size = sizeof(config);
        config.api_version = UCN_PERSIST_API_VERSION;
        config.runtime_instance = (uint32_t)index + 1U;
        config.owner_instance = (uint16_t)index + 1U;
        config.required_domain_mask = 1U;
        config.manifest = &fixture.manifest;
        fixture.binding[index].struct_size = sizeof(fixture.binding[index]);
        fixture.binding[index].api_version = UCN_PERSIST_API_VERSION;
        fixture.binding[index].domain = fixture.entry.domain;
        fixture.binding[index].business_owner_instance =
            (uint16_t)(21U + index);
        fixture.binding[index].domain_generation = 1U;
        config.domain_bindings = &fixture.binding[index];
        config.domain_binding_count = 1U;
        config.provider = &fixture.provider[index];
        config.state_lock = owner_ops;
        config.shared_callback_gate = fixture.gate;
        config.digest_workspace = &fixture.digest_workspace[index];
        CHECK(ucn_persistence_init_in_place(
                  &fixture.owner_storage[index],
                  sizeof(fixture.owner_storage[index]), &config,
                  &fixture.owner[index]) == UCN_OK);
        CHECK(ucn_i_persistence_start_recovery(fixture.owner[index]) == UCN_OK);
    }
    return 0;
}

static int finish_recovery(ucn_persistence_owner_t *owner)
{
    unsigned iteration;
    for (iteration = 0U; iteration < 16U; ++iteration) {
        ucn_persistence_step_result_t result;
        CHECK(ucn_i_persistence_step(owner, 10U + iteration, 8U, &result) ==
              UCN_OK);
        if (result.owner_ready != 0U && result.made_progress == 0U) {
            return 0;
        }
    }
    return __LINE__;
}

int main(void)
{
    pthread_t thread;
    ucn_persistence_step_result_t unchanged;
    int setup_result = configure_fixture();
    if (setup_result != 0) {
        return setup_result;
    }

    fixture.fake[0].reenter_hook = blocking_provider_hook;
    fixture.fake[0].reenter_context = &fixture.callback;
    CHECK(pthread_create(&thread, NULL, step_first_owner, NULL) == 0);
    CHECK(pthread_mutex_lock(&fixture.callback.mutex) == 0);
    while (fixture.callback.entered == 0U) {
        CHECK(pthread_cond_wait(&fixture.callback.condition,
                                &fixture.callback.mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&fixture.callback.mutex) == 0);

    memset(&unchanged, 0xA5, sizeof(unchanged));
    CHECK(ucn_i_persistence_step(fixture.owner[1], 1U, 1U, &unchanged) ==
          UCN_ERR_STATE);
    CHECK(((const uint8_t *)&unchanged)[0] == 0xA5U);
    CHECK(fixture.fake[1].calls[UCN_PERSIST_IO_LOAD_WITNESS] == 0U);

    CHECK(pthread_mutex_lock(&fixture.callback.mutex) == 0);
    fixture.callback.release = 1U;
    CHECK(pthread_cond_broadcast(&fixture.callback.condition) == 0);
    CHECK(pthread_mutex_unlock(&fixture.callback.mutex) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(fixture.thread_result == UCN_OK);
    fixture.fake[0].reenter_hook = NULL;

    CHECK(finish_recovery(fixture.owner[0]) == 0);
    CHECK(finish_recovery(fixture.owner[1]) == 0);
    CHECK(fixture.owner[0]->io.token != 0U);
    CHECK(fixture.owner[1]->io.token != 0U);
    CHECK(fixture.owner[0]->io.token != fixture.owner[1]->io.token);
    CHECK(ucn_persistence_deinit(fixture.owner[1]) == UCN_OK);
    CHECK(ucn_persistence_deinit(fixture.owner[0]) == UCN_OK);
    CHECK(ucn_persist_callback_gate_deinit(fixture.gate) == UCN_OK);
    CHECK(pthread_cond_destroy(&fixture.callback.condition) == 0);
    CHECK(pthread_mutex_destroy(&fixture.callback.mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.owner_lock[1].mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.owner_lock[0].mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.gate_lock.mutex) == 0);
    puts("UCN simplified Persistence concurrency tests passed");
    return 0;
}
