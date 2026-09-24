#include "internal/ucn_cluster.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression_)                                                     \
    do {                                                                       \
        if (!(expression_)) {                                                  \
            fprintf(stderr, "check failed at %d: %s\n", __LINE__,            \
                    #expression_);                                             \
            return __LINE__;                                                   \
        }                                                                      \
    } while (0)

typedef struct worker { uint32_t failures; } worker_t;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static ucn_i_cluster_owner_t owner;

static ucn_result_t lock_enter(void *context)
{
    return pthread_mutex_lock((pthread_mutex_t *)context) == 0 ?
           UCN_OK : UCN_ERR_STATE;
}
static void lock_leave(void *context)
{
    (void)pthread_mutex_unlock((pthread_mutex_t *)context);
}

static void principal(uint8_t out[UCN_I_CLUSTER_PRINCIPAL_BYTES],
                      uint8_t seed)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_PRINCIPAL_BYTES; ++index) {
        out[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_cluster_member_fact_t fact(uint64_t deadline)
{
    ucn_i_cluster_member_fact_t value;
    memset(&value, 0, sizeof(value));
    value.lease_deadline_us = deadline;
    value.capability_deadline_us = deadline;
    value.binding_generation = 7U;
    value.session_generation = 4U;
    value.capability_generation = 5U;
    value.route_generation = 2U;
    value.link_generation = 3U;
    value.link_id = 1U;
    value.authenticated = 1U;
    value.current = 1U;
    principal(value.principal, 0x10U);
    memset(value.capability_digest, 0xA5,
           sizeof(value.capability_digest));
    return value;
}

static ucn_i_cluster_directory_fact_t directory_fact(uint64_t sequence)
{
    ucn_i_cluster_directory_fact_t value;
    memset(&value, 0, sizeof(value));
    value.remote_epoch.cluster_id = 9U;
    value.remote_epoch.term = 3U;
    value.remote_epoch.head_binding_generation = 17U;
    principal(value.remote_epoch.head_principal, 0x40U);
    value.origin_sequence = sequence;
    value.authority_deadline_us = UINT64_C(100000);
    value.capability_deadline_us = UINT64_C(100000);
    value.flow_deadline_us = UINT64_C(100000);
    value.remote_config_id = 3U;
    value.remote_config_generation = 2U;
    value.authority_generation = 4U;
    value.source_session_generation = 8U;
    value.capability_generation = 9U;
    value.route_generation = 10U;
    value.path_generation = 11U;
    value.link_generation = 12U;
    value.path_id = 13U;
    value.link_id = 14U;
    memset(value.capability_digest, 0x61,
           sizeof(value.capability_digest));
    memset(value.authority_digest, 0x71,
           sizeof(value.authority_digest));
    value.authenticated = 1U;
    value.quorum_verified = 1U;
    value.flow_active = 1U;
    return value;
}

static int setup(void)
{
    ucn_i_cluster_config_t init;
    ucn_i_cluster_epoch_t epoch;
    ucn_i_cluster_config_view_t config;
    ucn_i_cluster_durability_t durability;
    ucn_i_cluster_requirement_t requirement;
    ucn_i_cluster_proof_t proof;
    ucn_handle_t persistence;

    memset(&owner, 0, sizeof(owner));
    memset(&init, 0, sizeof(init));
    init.runtime_instance = 1U;
    init.owner_instance = 8U;
    principal(init.local_principal, 0x10U);
    init.state_lock.struct_size = sizeof(init.state_lock);
    init.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    init.state_lock.context = &mutex;
    init.state_lock.enter = lock_enter;
    init.state_lock.leave = lock_leave;
    CHECK(ucn_i_cluster_owner_init(&owner, &init) == UCN_OK);
    memset(&epoch, 0, sizeof(epoch));
    epoch.cluster_id = 2U;
    epoch.term = 1U;
    epoch.head_binding_generation = 7U;
    principal(epoch.head_principal, 0x10U);
    memset(&config, 0, sizeof(config));
    config.config_id = 1U;
    config.generation = 1U;
    config.member_count = 1U;
    config.members[0].binding_generation = 7U;
    config.members[0].flags = UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
                              UCN_I_CLUSTER_MEMBER_FLAG_VOTER;
    principal(config.members[0].principal, 0x10U);
    memset(&durability, 0, sizeof(durability));
    durability.domain_id = 9U;
    durability.foundation_transaction_id = 1U;
    durability.absolute_deadline_us = UINT64_C(1000000);
    durability.persistence_domain_generation = 2U;
    durability.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    durability.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    CHECK(ucn_i_cluster_create_prepare(
              &owner, &epoch, &config, UCN_I_CLUSTER_HEAD,
              &durability, &requirement) == UCN_OK);
    memset(&persistence, 0, sizeof(persistence));
    persistence.runtime_instance = 1U;
    persistence.owner_instance = 9U;
    persistence.slot = 1U;
    persistence.generation = 1U;
    persistence.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    CHECK(ucn_i_cluster_bind_persistence(
              &owner, persistence,
              requirement.canonical_body_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence;
    proof.domain_id = 9U;
    proof.foundation_transaction_id = 1U;
    proof.record_generation = 1U;
    proof.witness_generation = 1U;
    proof.runtime_instance = 1U;
    proof.body_bytes = UCN_I_CLUSTER_RECORD_BYTES;
    proof.persistence_domain_generation = 2U;
    proof.persistence_owner_instance = 9U;
    proof.caller_owner_instance = 8U;
    proof.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    proof.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    proof.operation_kind = requirement.operation_kind;
    memcpy(proof.body_digest, requirement.canonical_body_digest,
           sizeof(proof.body_digest));
    CHECK(ucn_i_cluster_accept_proof(&owner, &proof, 1U) == UCN_OK);
    return 0;
}

static void *writer(void *argument)
{
    worker_t *worker = argument;
    uint32_t iteration;
    for (iteration = 0U; iteration < 2000U; ++iteration) {
        ucn_i_cluster_member_fact_t value =
            fact(UINT64_C(100000) + iteration);
        ucn_i_cluster_directory_fact_t remote =
            directory_fact((uint64_t)iteration + 1U);
        if (ucn_i_cluster_member_observe(&owner, &value,
                                         iteration + 1U) != UCN_OK) {
            worker->failures++;
        }
        if (ucn_i_cluster_directory_install(&owner, &remote,
                                            iteration + 1U) != UCN_OK) {
            worker->failures++;
        }
    }
    return NULL;
}

static void *reader(void *argument)
{
    worker_t *worker = argument;
    uint32_t iteration;
    for (iteration = 0U; iteration < 2000U; ++iteration) {
        ucn_i_cluster_authority_view_t view;
        ucn_i_cluster_directory_fact_t remote;
        ucn_result_t result = ucn_i_cluster_authority_preflight(
            &owner, iteration + 1U, &view);
        if (result != UCN_OK && result != UCN_ERR_ACCESS) {
            worker->failures++;
        }
        result = ucn_i_cluster_directory_copy(
            &owner, 9U, iteration + 1U, &remote);
        if (result != UCN_OK && result != UCN_ERR_NOT_FOUND &&
            result != UCN_ERR_ACCESS) {
            worker->failures++;
        }
    }
    return NULL;
}

int main(void)
{
    pthread_t writer_thread;
    pthread_t reader_thread;
    worker_t writer_result = {0U};
    worker_t reader_result = {0U};
    CHECK(setup() == 0);
    CHECK(pthread_create(&writer_thread, NULL, writer,
                         &writer_result) == 0);
    CHECK(pthread_create(&reader_thread, NULL, reader,
                         &reader_result) == 0);
    CHECK(pthread_join(writer_thread, NULL) == 0);
    CHECK(pthread_join(reader_thread, NULL) == 0);
    CHECK(writer_result.failures == 0U);
    CHECK(reader_result.failures == 0U);
    printf("cluster concurrency: member/directory writer=2000 "
           "authority/directory reader=2000\n");
    return 0;
}
