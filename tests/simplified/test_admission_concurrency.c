#include "internal/ucn_admission.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr_)                                                        \
    do {                                                                    \
        if (!(expr_)) {                                                     \
            return __LINE__;                                               \
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

typedef struct fixture {
    ucn_i_admission_owner_t owner[2];
    ucn_i_admission_config_t config[2];
    pthread_lock_t owner_lock[2];
    pthread_lock_t gate_lock;
    ucn_i_callback_gate_t gate;
    callback_block_t block;
    ucn_i_admission_hello_t hello;
    ucn_i_admission_link_t link;
    ucn_i_admission_cookie_challenge_t first_output;
    ucn_result_t first_result;
} fixture_t;

static fixture_t fixture;

static ucn_result_t mutex_enter(void *context)
{
    pthread_lock_t *lock = context;

    return lock != NULL && pthread_mutex_lock(&lock->mutex) == 0 ?
               UCN_OK : UCN_ERR_STATE;
}

static void mutex_leave(void *context)
{
    pthread_lock_t *lock = context;

    if (lock != NULL) {
        (void)pthread_mutex_unlock(&lock->mutex);
    }
}

static ucn_i_lock_ops_t make_lock(pthread_lock_t *lock)
{
    ucn_i_lock_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = mutex_enter;
    ops.leave = mutex_leave;
    return ops;
}

static ucn_result_t blocking_issue_cookie(
    void *context,
    const ucn_i_admission_hello_t *hello,
    ucn_i_admission_link_t link,
    uint32_t cookie_time_bucket,
    uint8_t output[UCN_I_ADMISSION_COOKIE_BYTES],
    uint8_t *output_bytes)
{
    callback_block_t *block = context;

    if (hello == NULL || link.id == 0U || cookie_time_bucket == 0U ||
        output == NULL || output_bytes == NULL ||
        pthread_mutex_lock(&block->mutex) != 0) {
        return UCN_ERR_ARGUMENT;
    }
    block->entered++;
    (void)pthread_cond_broadcast(&block->condition);
    while (block->release == 0U) {
        if (pthread_cond_wait(&block->condition, &block->mutex) != 0) {
            (void)pthread_mutex_unlock(&block->mutex);
            return UCN_ERR_STATE;
        }
    }
    (void)pthread_mutex_unlock(&block->mutex);
    memset(output, 0x5AU, UCN_I_ADMISSION_COOKIE_BYTES);
    *output_bytes = UCN_I_ADMISSION_COOKIE_BYTES;
    return UCN_OK;
}

static ucn_result_t unused_verify(
    void *context,
    const ucn_i_admission_hello_cookie_t *hello_cookie)
{
    (void)context;
    (void)hello_cookie;
    return UCN_ERR_UNSUPPORTED;
}

static ucn_result_t unused_authorize(
    void *context,
    ucn_i_admission_event_t event,
    const ucn_i_admission_key_t *key,
    const ucn_i_admission_transcript_t *transcript,
    uint64_t now_us,
    const ucn_i_admission_evidence_t *evidence)
{
    (void)context;
    (void)event;
    (void)key;
    (void)transcript;
    (void)now_us;
    (void)evidence;
    return UCN_ERR_UNSUPPORTED;
}

static int configure_fixture(void)
{
    ucn_i_lock_ops_t gate_ops;
    unsigned index;

    memset(&fixture, 0, sizeof(fixture));
    CHECK(pthread_mutex_init(&fixture.gate_lock.mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture.owner_lock[0].mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture.owner_lock[1].mutex, NULL) == 0);
    CHECK(pthread_mutex_init(&fixture.block.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&fixture.block.condition, NULL) == 0);
    gate_ops = make_lock(&fixture.gate_lock);
    CHECK(ucn_i_callback_gate_init(&fixture.gate, 80U, &gate_ops) == UCN_OK);

    fixture.hello.device_nonce = 1U;
    fixture.hello.transaction_id = 2U;
    memset(fixture.hello.identity_digest, 0x31,
           sizeof(fixture.hello.identity_digest));
    fixture.link.id = 1U;
    fixture.link.generation = 1U;
    for (index = 0U; index < 2U; ++index) {
        ucn_i_admission_config_t *config = &fixture.config[index];

        memset(config, 0, sizeof(*config));
        config->struct_size = sizeof(*config);
        config->api_version = UCN_API_VERSION;
        config->runtime_instance = (uint32_t)(index + 1U);
        config->realm_id = 1U;
        config->owner_instance = (uint16_t)(20U + index);
        config->identity_owner_instance = (uint16_t)(30U + index);
        config->persistence_owner_instance = (uint16_t)(40U + index);
        config->max_pending_per_link = 1U;
        config->token_burst = 1U;
        config->tokens_per_second = 1U;
        config->pending_timeout_us = 1000U;
        config->provider.struct_size = sizeof(config->provider);
        config->provider.api_version = 1U;
        config->provider.context = &fixture.block;
        config->provider.issue_cookie = blocking_issue_cookie;
        config->provider.verify_cookie = unused_verify;
        config->provider.authorize_event = unused_authorize;
        config->state_lock = make_lock(&fixture.owner_lock[index]);
        config->provider_gate = &fixture.gate;
        CHECK(ucn_i_admission_owner_init(&fixture.owner[index], config) ==
              UCN_OK);
    }
    return 0;
}

static void *issue_first(void *unused)
{
    (void)unused;
    fixture.first_result = ucn_i_admission_issue_cookie(
        &fixture.owner[0], &fixture.hello, fixture.link, 100U, 40U, 40U,
        1U, &fixture.first_output);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    ucn_i_admission_cookie_challenge_t sentinel;
    ucn_i_admission_cookie_challenge_t sentinel_before;
    int configured = configure_fixture();

    if (configured != 0) {
        return configured;
    }
    CHECK(pthread_create(&thread, NULL, issue_first, NULL) == 0);
    CHECK(pthread_mutex_lock(&fixture.block.mutex) == 0);
    while (fixture.block.entered == 0U) {
        CHECK(pthread_cond_wait(&fixture.block.condition,
                                &fixture.block.mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&fixture.block.mutex) == 0);

    memset(&sentinel, 0xA5, sizeof(sentinel));
    sentinel_before = sentinel;
    CHECK(ucn_i_admission_issue_cookie(
              &fixture.owner[1], &fixture.hello, fixture.link, 100U,
              40U, 40U, 1U, &sentinel) == UCN_ERR_STATE);
    CHECK(memcmp(&sentinel, &sentinel_before, sizeof(sentinel)) == 0);

    CHECK(pthread_mutex_lock(&fixture.block.mutex) == 0);
    fixture.block.release = 1U;
    CHECK(pthread_cond_broadcast(&fixture.block.condition) == 0);
    CHECK(pthread_mutex_unlock(&fixture.block.mutex) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(fixture.first_result == UCN_OK);
    CHECK(fixture.block.entered == 1U);
    CHECK(ucn_i_admission_owner_destroy(&fixture.owner[0]) == UCN_OK);
    CHECK(ucn_i_admission_owner_destroy(&fixture.owner[1]) == UCN_OK);
    CHECK(ucn_i_callback_gate_destroy(&fixture.gate) == UCN_OK);
    CHECK(pthread_cond_destroy(&fixture.block.condition) == 0);
    CHECK(pthread_mutex_destroy(&fixture.block.mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.owner_lock[0].mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.owner_lock[1].mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.gate_lock.mutex) == 0);
    puts("admission concurrency tests passed");
    return 0;
}
