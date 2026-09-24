#include "internal/ucn_security.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                   \
    do {                                                                    \
        if (!(expression)) {                                                \
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

typedef struct concurrency_fixture {
    ucn_i_security_owner_t owner[2];
    ucn_i_security_candidate_t candidate[2];
    ucn_i_security_domain_rule_t rule[2];
    ucn_i_security_acl_rule_t acl[2];
    ucn_i_security_config_t config[2];
    ucn_i_security_durability_base_t durability[2];
    ucn_i_security_handle_t handle[2];
    pthread_lock_t owner_lock[2];
    pthread_lock_t gate_lock;
    ucn_i_callback_gate_t gate;
    callback_block_t block;
    ucn_result_t first_result;
} concurrency_fixture_t;

static concurrency_fixture_t fixture;

static ucn_result_t mutex_enter(void *context)
{
    pthread_lock_t *lock = (pthread_lock_t *)context;

    return lock != NULL && pthread_mutex_lock(&lock->mutex) == 0 ?
               UCN_OK : UCN_ERR_STATE;
}

static void mutex_leave(void *context)
{
    pthread_lock_t *lock = (pthread_lock_t *)context;

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

static void fill_bytes(uint8_t *bytes, size_t length, uint8_t seed)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = (uint8_t)(seed + index);
    }
}

static ucn_result_t blocking_verify(
    void *context,
    const ucn_i_security_candidate_t *candidate,
    const uint8_t *proof,
    size_t proof_bytes)
{
    callback_block_t *block = (callback_block_t *)context;

    if (candidate == NULL || proof == NULL || proof_bytes != 1U ||
        proof[0] != 0xC3U || pthread_mutex_lock(&block->mutex) != 0) {
        return UCN_ERR_SECURITY;
    }
    block->entered = 1U;
    (void)pthread_cond_broadcast(&block->condition);
    while (block->release == 0U) {
        if (pthread_cond_wait(&block->condition, &block->mutex) != 0) {
            (void)pthread_mutex_unlock(&block->mutex);
            return UCN_ERR_STATE;
        }
    }
    (void)pthread_mutex_unlock(&block->mutex);
    return UCN_OK;
}

static ucn_result_t unused_protect(
    void *context,
    const ucn_i_security_origin_protect_request_t *request)
{
    (void)context;
    (void)request;
    return UCN_ERR_UNSUPPORTED;
}

static ucn_result_t unused_open(
    void *context,
    const ucn_i_security_origin_open_request_t *request)
{
    (void)context;
    (void)request;
    return UCN_ERR_UNSUPPORTED;
}

static ucn_result_t unused_hop_protect(
    void *context,
    const ucn_i_security_hop_request_t *request)
{
    (void)context;
    (void)request;
    return UCN_ERR_UNSUPPORTED;
}

static ucn_result_t unused_hop_verify(
    void *context,
    const ucn_i_security_hop_verify_request_t *request)
{
    (void)context;
    (void)request;
    return UCN_ERR_UNSUPPORTED;
}

static void make_candidate(unsigned index)
{
    ucn_i_security_candidate_t *candidate = &fixture.candidate[index];
    ucn_i_security_domain_rule_t *rule = &fixture.rule[index];
    ucn_i_security_acl_rule_t *acl = &fixture.acl[index];

    memset(candidate, 0, sizeof(*candidate));
    candidate->local.address = 1U;
    candidate->local.binding_generation = 1U;
    fill_bytes(candidate->local.principal,
               sizeof(candidate->local.principal), 0x10U);
    candidate->peer.address = (uint32_t)(2U + index);
    candidate->peer.binding_generation = 1U;
    fill_bytes(candidate->peer.principal,
               sizeof(candidate->peer.principal),
               (uint8_t)(0x30U + index));
    candidate->expires_at_us = 5000U;
    candidate->link_generation = 1U;
    candidate->session_generation = 1U;
    candidate->policy_generation = 1U;
    candidate->origin_tx.key_generation = 1U;
    candidate->origin_tx.key_id = 1U;
    candidate->origin_tx.suite_id = 1U;
    candidate->origin_rx.key_generation = 1U;
    candidate->origin_rx.key_id = 2U;
    candidate->origin_rx.suite_id = 1U;
    fill_bytes(candidate->origin_fingerprint,
               sizeof(candidate->origin_fingerprint), 0x50U);
    fill_bytes(candidate->hop_fingerprint,
               sizeof(candidate->hop_fingerprint), 0x60U);
    fill_bytes(candidate->transcript_digest,
               sizeof(candidate->transcript_digest), 0x70U);
    candidate->origin_level = UCN_I_SECURITY_AUTHENTICATED;
    candidate->hop_profile = UCN_I_HOP_PROFILE_H0;
    candidate->address_width = 1U;

    memset(rule, 0, sizeof(*rule));
    rule->domain_id = UINT64_C(0x8000) + index;
    memcpy(rule->peer_principal, candidate->peer.principal,
           sizeof(rule->peer_principal));

    memset(acl, 0, sizeof(*acl));
    memcpy(acl->peer_principal, candidate->peer.principal,
           sizeof(acl->peer_principal));
    memcpy(acl->context_fingerprint, candidate->origin_fingerprint,
           sizeof(acl->context_fingerprint));
    acl->peer_binding_generation = candidate->peer.binding_generation;
    acl->service_id = 1U;
    acl->direction = UCN_I_SECURITY_ACCESS_OUTBOUND;

    memset(&fixture.durability[index], 0,
           sizeof(fixture.durability[index]));
    fixture.durability[index].domain_id = rule->domain_id;
    fixture.durability[index].next_transaction_id = 1U;
    fixture.durability[index].expected_record_generation = 1U;
    fixture.durability[index].absolute_deadline_us = 4000U;
    fixture.durability[index].domain_generation = 1U;
    fixture.durability[index].volatile_continuation = 1U;
    fill_bytes(fixture.durability[index].expected_body_digest,
               sizeof(fixture.durability[index].expected_body_digest),
               0x90U);
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
    CHECK(ucn_i_callback_gate_init(&fixture.gate, 90U, &gate_ops) == UCN_OK);

    for (index = 0U; index < 2U; ++index) {
        ucn_i_security_config_t *config = &fixture.config[index];

        make_candidate(index);
        memset(config, 0, sizeof(*config));
        config->struct_size = sizeof(*config);
        config->api_version = UCN_API_VERSION;
        config->runtime_instance = (uint32_t)(index + 1U);
        config->realm_id = 1U;
        config->owner_instance = (uint16_t)(index + 1U);
        config->persistence_business_owner_instance =
            (uint16_t)(index + 11U);
        config->address_width = 1U;
        config->domain_rules = &fixture.rule[index];
        config->domain_rule_count = 1U;
        config->acl_rules = &fixture.acl[index];
        config->acl_rule_count = 1U;
        config->replay_reservation_lifetime_us = 100U;
        config->provider.struct_size = sizeof(config->provider);
        config->provider.api_version = 1U;
        config->provider.context = &fixture.block;
        config->provider.verify_session = blocking_verify;
        config->provider.protect_origin = unused_protect;
        config->provider.open_origin = unused_open;
        config->provider.protect_hop = unused_hop_protect;
        config->provider.verify_hop = unused_hop_verify;
        config->state_lock = make_lock(&fixture.owner_lock[index]);
        config->provider_gate = &fixture.gate;
        CHECK(ucn_i_security_owner_init(&fixture.owner[index], config) ==
              UCN_OK);
    }
    return 0;
}

static void *prepare_first_owner(void *unused)
{
    const uint8_t proof = 0xC3U;

    (void)unused;
    fixture.first_result = ucn_i_security_prepare_static_session(
        &fixture.owner[0], &fixture.candidate[0], &proof, 1U,
        &fixture.durability[0], 1000U, &fixture.handle[0]);
    return NULL;
}

int main(void)
{
    const uint8_t proof = 0xC3U;
    ucn_i_security_handle_t sentinel;
    ucn_i_security_handle_t before;
    pthread_t thread;
    int setup_result = configure_fixture();

    if (setup_result != 0) {
        return setup_result;
    }
    CHECK(pthread_create(&thread, NULL, prepare_first_owner, NULL) == 0);
    CHECK(pthread_mutex_lock(&fixture.block.mutex) == 0);
    while (fixture.block.entered == 0U) {
        CHECK(pthread_cond_wait(&fixture.block.condition,
                                &fixture.block.mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&fixture.block.mutex) == 0);

    memset(&sentinel, 0xA5, sizeof(sentinel));
    before = sentinel;
    CHECK(ucn_i_security_prepare_static_session(
              &fixture.owner[1], &fixture.candidate[1], &proof, 1U,
              &fixture.durability[1], 1000U, &sentinel) == UCN_ERR_STATE);
    CHECK(memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
    CHECK(fixture.owner[1].sessions[0].occupied == 0U);

    CHECK(pthread_mutex_lock(&fixture.block.mutex) == 0);
    fixture.block.release = 1U;
    CHECK(pthread_cond_broadcast(&fixture.block.condition) == 0);
    CHECK(pthread_mutex_unlock(&fixture.block.mutex) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(fixture.first_result == UCN_OK);
    CHECK(fixture.owner[0].sessions[0].occupied == 1U);
    CHECK(ucn_i_security_fence(&fixture.owner[0], fixture.handle[0]) ==
          UCN_OK);
    CHECK(ucn_i_security_retire_fenced(&fixture.owner[0],
                                       fixture.handle[0]) == UCN_OK);
    CHECK(ucn_i_security_owner_destroy(&fixture.owner[1]) == UCN_OK);
    CHECK(ucn_i_security_owner_destroy(&fixture.owner[0]) == UCN_OK);
    CHECK(ucn_i_callback_gate_destroy(&fixture.gate) == UCN_OK);
    CHECK(pthread_cond_destroy(&fixture.block.condition) == 0);
    CHECK(pthread_mutex_destroy(&fixture.block.mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.owner_lock[1].mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.owner_lock[0].mutex) == 0);
    CHECK(pthread_mutex_destroy(&fixture.gate_lock.mutex) == 0);
    puts("UCN simplified Security concurrency tests passed");
    return 0;
}
