#include "fake_persistence_provider.h"
#include "internal/ucn_cluster.h"
#include "internal/ucn_persistence.h"

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

typedef struct test_lock { uint8_t held; } test_lock_t;
typedef struct fixture {
    ucn_persist_manifest_entry_t entry;
    ucn_persist_domain_binding_t binding;
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake;
    ucn_persistence_provider_t provider;
    ucn_persistence_config_t persist_config;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace;
    ucn_persistence_storage_t persistence_storage;
    ucn_persist_callback_gate_t *gate;
    ucn_persistence_owner_t *persistence;
    ucn_i_cluster_owner_t cluster;
    ucn_i_cluster_owner_t reload;
    test_lock_t persist_lock;
    test_lock_t gate_lock;
    test_lock_t cluster_lock;
    test_lock_t reload_lock;
} fixture_t;

static fixture_t fixture;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}
static void lock_leave(void *context) { ((test_lock_t *)context)->held = 0U; }

static ucn_lock_ops_t public_lock(test_lock_t *state)
{
    ucn_lock_ops_t lock;
    memset(&lock, 0, sizeof(lock));
    lock.struct_size = sizeof(lock);
    lock.api_version = UCN_API_VERSION;
    lock.context = state;
    lock.enter = lock_enter;
    lock.leave = lock_leave;
    return lock;
}

static ucn_i_lock_ops_t private_lock(test_lock_t *state)
{
    ucn_i_lock_ops_t lock;
    memset(&lock, 0, sizeof(lock));
    lock.struct_size = sizeof(lock);
    lock.api_version = UCN_I_LOCK_OPS_VERSION;
    lock.context = state;
    lock.enter = lock_enter;
    lock.leave = lock_leave;
    return lock;
}

static void principal(uint8_t out[UCN_I_CLUSTER_PRINCIPAL_BYTES],
                      uint8_t seed)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_CLUSTER_PRINCIPAL_BYTES; ++index) {
        out[index] = (uint8_t)(seed + index);
    }
}

static int cluster_init(ucn_i_cluster_owner_t *owner, test_lock_t *lock)
{
    ucn_i_cluster_config_t config;
    memset(&config, 0, sizeof(config));
    config.runtime_instance = 1U;
    config.owner_instance = 8U;
    principal(config.local_principal, 0x10U);
    config.state_lock = private_lock(lock);
    CHECK(ucn_i_cluster_owner_init(owner, &config) == UCN_OK);
    return 0;
}

static int fixture_init(void)
{
    ucn_lock_ops_t persist_lock;
    ucn_lock_ops_t gate_lock;
    uint8_t digest[UCN_PERSIST_DIGEST_BYTES];
    uint16_t iteration;

    memset(&fixture, 0, sizeof(fixture));
    fixture.entry.struct_size = sizeof(fixture.entry);
    fixture.entry.api_version = UCN_PERSIST_API_VERSION;
    fixture.entry.domain.domain_kind = UCN_PERSIST_DOMAIN_CLUSTER;
    fixture.entry.domain.domain_id = 902U;
    fixture.entry.body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
    fixture.entry.slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
    fixture.entry.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    fixture.entry.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    fixture.entry.digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
    fixture.entry.witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
    fixture.entry.provider_atomicity_class =
        UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
    fixture.binding.struct_size = sizeof(fixture.binding);
    fixture.binding.api_version = UCN_PERSIST_API_VERSION;
    fixture.binding.domain = fixture.entry.domain;
    fixture.binding.business_owner_instance = 8U;
    fixture.binding.domain_generation = 5U;
    fixture.manifest.struct_size = sizeof(fixture.manifest);
    fixture.manifest.api_version = UCN_PERSIST_API_VERSION;
    fixture.manifest.protocol_manifest_version =
        UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    fixture.manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    fixture.manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    fixture.manifest.entries = &fixture.entry;
    fixture.manifest.entry_count = 1U;
    fixture.manifest.profile_id = UCN_PROFILE;
    CHECK(ucn_persistence_manifest_digest(&fixture.manifest,
                                          &fixture.digest_workspace,
                                          digest) == UCN_OK);
    memcpy(fixture.manifest.expected_digest, digest, sizeof(digest));
    fake_persist_provider_init(&fixture.fake, &fixture.manifest, 0xFFU);
    fake_persist_provider_make_public(&fixture.fake, 0xFFU,
                                      &fixture.provider);
    persist_lock = public_lock(&fixture.persist_lock);
    gate_lock = public_lock(&fixture.gate_lock);
    CHECK(ucn_persist_callback_gate_init_in_place(
              &fixture.gate_storage, sizeof(fixture.gate_storage),
              &gate_lock, &fixture.gate) == UCN_OK);
    fixture.persist_config.struct_size = sizeof(fixture.persist_config);
    fixture.persist_config.api_version = UCN_PERSIST_API_VERSION;
    fixture.persist_config.runtime_instance = 1U;
    fixture.persist_config.owner_instance = 9U;
    fixture.persist_config.required_domain_mask = 1U;
    fixture.persist_config.manifest = &fixture.manifest;
    fixture.persist_config.domain_bindings = &fixture.binding;
    fixture.persist_config.domain_binding_count = 1U;
    fixture.persist_config.provider = &fixture.provider;
    fixture.persist_config.state_lock = persist_lock;
    fixture.persist_config.shared_callback_gate = fixture.gate;
    fixture.persist_config.digest_workspace = &fixture.digest_workspace;
    CHECK(ucn_persistence_init_in_place(
              &fixture.persistence_storage,
              sizeof(fixture.persistence_storage),
              &fixture.persist_config, &fixture.persistence) == UCN_OK);
    CHECK(ucn_i_persistence_start_recovery(fixture.persistence) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence, iteration + 1U,
                                     8U, &step) == UCN_OK);
        if (step.owner_ready != 0U && step.made_progress == 0U) break;
    }
    CHECK(iteration < 32U);
    CHECK(cluster_init(&fixture.cluster, &fixture.cluster_lock) == 0);
    CHECK(cluster_init(&fixture.reload, &fixture.reload_lock) == 0);
    return 0;
}

static int persist_cluster_requirement(
    ucn_i_cluster_owner_t *cluster,
    const ucn_i_cluster_requirement_t *requirement, uint64_t now_us,
    ucn_i_cluster_proof_t *cluster_proof_out)
{
    ucn_persistence_request_t request;
    ucn_persistence_proof_t foundation;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_handle_t persistence_handle;
    uint8_t published_digest[UCN_PERSIST_DIGEST_BYTES];
    uint16_t iteration;
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.api_version = UCN_PERSIST_API_VERSION;
    request.runtime_instance = requirement->runtime_instance;
    request.caller_owner_instance = requirement->caller_owner_instance;
    request.domain_generation =
        requirement->durability.persistence_domain_generation;
    request.domain = fixture.entry.domain;
    request.foundation_transaction_id =
        requirement->durability.foundation_transaction_id;
    request.expected_record_generation =
        requirement->durability.expected_record_generation;
    request.absolute_deadline_us =
        requirement->durability.absolute_deadline_us;
    request.business_transition_digest =
        ((uint64_t)requirement->operation_kind << 48U) |
        requirement->durability.foundation_transaction_id;
    request.canonical_body = requirement->body;
    request.body_bytes = requirement->body_bytes;
    request.schema_id = requirement->durability.schema_id;
    request.schema_version = requirement->durability.schema_version;
    request.operation_kind = requirement->operation_kind;
    request.volatile_continuation =
        requirement->durability.volatile_continuation;
    memcpy(request.expected_body_digest, requirement->expected_body_digest,
           sizeof(request.expected_body_digest));
    memset(&meta, 0, sizeof(meta));
    memset(&codec, 0, sizeof(codec));
    meta.domain = request.domain;
    meta.record_generation = request.expected_record_generation + 1U;
    meta.transaction_id = request.foundation_transaction_id;
    meta.body_bytes = request.body_bytes;
    meta.schema_id = request.schema_id;
    meta.schema_version = request.schema_version;
    meta.operation_kind = request.operation_kind;
    CHECK(ucn_i_persist_body_digest(&meta, request.canonical_body,
                                    published_digest, &codec) == UCN_OK);
    CHECK(ucn_i_persistence_submit(fixture.persistence, &request, now_us,
                                   &persistence_handle) == UCN_OK);
    CHECK(ucn_i_cluster_bind_persistence(cluster, persistence_handle,
                                         published_digest) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence,
                                     now_us + 1U + iteration, 8U,
                                     &step) == UCN_OK);
        if (step.proofs_ready != 0U) break;
    }
    CHECK(iteration < 32U);
    CHECK(ucn_i_persistence_proof_get(fixture.persistence,
                                      persistence_handle,
                                      &foundation) == UCN_OK);
    memset(cluster_proof_out, 0, sizeof(*cluster_proof_out));
    cluster_proof_out->persistence_handle = persistence_handle;
    cluster_proof_out->domain_id = foundation.domain.domain_id;
    cluster_proof_out->foundation_transaction_id =
        foundation.foundation_transaction_id;
    cluster_proof_out->record_generation = foundation.record_generation;
    cluster_proof_out->witness_generation = foundation.witness_generation;
    cluster_proof_out->runtime_instance = foundation.runtime_instance;
    cluster_proof_out->body_bytes = foundation.body_bytes;
    cluster_proof_out->persistence_domain_generation =
        foundation.domain_generation;
    cluster_proof_out->persistence_owner_instance =
        foundation.persistence_owner_instance;
    cluster_proof_out->caller_owner_instance =
        foundation.caller_owner_instance;
    cluster_proof_out->schema_id = request.schema_id;
    cluster_proof_out->schema_version = request.schema_version;
    cluster_proof_out->operation_kind = foundation.operation_kind;
    memcpy(cluster_proof_out->body_digest, foundation.body_digest,
           sizeof(cluster_proof_out->body_digest));
    CHECK(ucn_i_cluster_accept_proof(cluster, cluster_proof_out,
                                     now_us + 40U) == UCN_OK);
    CHECK(ucn_i_persistence_proof_retire(fixture.persistence,
                                         persistence_handle) == UCN_OK);
    return 0;
}

static int test_foundation_round_trip(void)
{
    ucn_i_cluster_epoch_t epoch;
    ucn_i_cluster_config_view_t config;
    ucn_i_cluster_durability_t durability;
    ucn_i_cluster_requirement_t requirement;
    ucn_i_cluster_proof_t proof;
    ucn_persistence_request_t request;
    ucn_persistence_proof_t foundation;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_handle_t persistence_handle;
    uint8_t published_digest[UCN_PERSIST_DIGEST_BYTES];
    uint8_t corrupt[UCN_I_CLUSTER_RECORD_BYTES];
    ucn_i_cluster_owner_t reload_snapshot;
    uint16_t iteration;

    memset(&epoch, 0, sizeof(epoch));
    epoch.cluster_id = 3U;
    epoch.term = 1U;
    epoch.head_binding_generation = 7U;
    principal(epoch.head_principal, 0x10U);
    memset(&config, 0, sizeof(config));
    config.config_id = 2U;
    config.generation = 1U;
    config.member_count = 1U;
    config.members[0].binding_generation = 7U;
    config.members[0].flags = UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
                              UCN_I_CLUSTER_MEMBER_FLAG_VOTER;
    principal(config.members[0].principal, 0x10U);
    memset(&durability, 0, sizeof(durability));
    durability.domain_id = fixture.entry.domain.domain_id;
    durability.foundation_transaction_id = 1U;
    durability.expected_record_generation = 0U;
    durability.absolute_deadline_us = 8000U;
    durability.persistence_domain_generation = 5U;
    durability.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    durability.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    CHECK(ucn_i_cluster_create_prepare(
              &fixture.cluster, &epoch, &config, UCN_I_CLUSTER_HEAD,
              &durability, &requirement) == UCN_OK);
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.api_version = UCN_PERSIST_API_VERSION;
    request.runtime_instance = requirement.runtime_instance;
    request.caller_owner_instance = requirement.caller_owner_instance;
    request.domain_generation = durability.persistence_domain_generation;
    request.domain = fixture.entry.domain;
    request.foundation_transaction_id = durability.foundation_transaction_id;
    request.expected_record_generation = durability.expected_record_generation;
    request.absolute_deadline_us = durability.absolute_deadline_us;
    request.business_transition_digest = UINT64_C(0x0830000000000001);
    request.canonical_body = requirement.body;
    request.body_bytes = requirement.body_bytes;
    request.schema_id = durability.schema_id;
    request.schema_version = durability.schema_version;
    request.operation_kind = requirement.operation_kind;
    request.volatile_continuation =
        requirement.durability.volatile_continuation;
    memcpy(request.expected_body_digest, requirement.expected_body_digest,
           sizeof(request.expected_body_digest));
    memset(&meta, 0, sizeof(meta));
    memset(&codec, 0, sizeof(codec));
    meta.domain = request.domain;
    meta.record_generation = 1U;
    meta.transaction_id = request.foundation_transaction_id;
    meta.body_bytes = request.body_bytes;
    meta.schema_id = request.schema_id;
    meta.schema_version = request.schema_version;
    meta.operation_kind = request.operation_kind;
    CHECK(ucn_i_persist_body_digest(&meta, request.canonical_body,
                                    published_digest, &codec) == UCN_OK);
    CHECK(ucn_i_persistence_submit(fixture.persistence, &request, 100U,
                                   &persistence_handle) == UCN_OK);
    CHECK(ucn_i_cluster_bind_persistence(
              &fixture.cluster, persistence_handle,
              published_digest) == UCN_OK);
    for (iteration = 0U; iteration < 32U; ++iteration) {
        ucn_persistence_step_result_t step;
        CHECK(ucn_i_persistence_step(fixture.persistence,
                                     101U + iteration, 8U, &step) == UCN_OK);
        if (step.proofs_ready != 0U) break;
    }
    CHECK(iteration < 32U);
    CHECK(ucn_i_persistence_proof_get(fixture.persistence,
                                      persistence_handle,
                                      &foundation) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence_handle;
    proof.domain_id = foundation.domain.domain_id;
    proof.foundation_transaction_id = foundation.foundation_transaction_id;
    proof.record_generation = foundation.record_generation;
    proof.witness_generation = foundation.witness_generation;
    proof.runtime_instance = foundation.runtime_instance;
    proof.body_bytes = foundation.body_bytes;
    proof.persistence_domain_generation = foundation.domain_generation;
    proof.persistence_owner_instance = foundation.persistence_owner_instance;
    proof.caller_owner_instance = foundation.caller_owner_instance;
    proof.schema_id = request.schema_id;
    proof.schema_version = request.schema_version;
    proof.operation_kind = foundation.operation_kind;
    memcpy(proof.body_digest, foundation.body_digest,
           sizeof(proof.body_digest));
    CHECK(ucn_i_cluster_accept_proof(&fixture.cluster, &proof, 200U) ==
          UCN_OK);
    CHECK(fixture.cluster.state.epoch.cluster_id == 3U);
    CHECK(ucn_i_cluster_import(&fixture.reload, requirement.body,
                               requirement.body_bytes, &durability,
                               foundation.body_digest) == UCN_OK);
    CHECK(fixture.reload.state.epoch.cluster_id == 3U &&
          fixture.reload.authority_active == 0U);
    reload_snapshot = fixture.reload;
    memcpy(corrupt, requirement.body, sizeof(corrupt));
    corrupt[0] ^= 1U;
    CHECK(ucn_i_cluster_import(&fixture.reload, corrupt, sizeof(corrupt),
                               &durability, foundation.body_digest) ==
          UCN_ERR_MALFORMED);
    CHECK(memcmp(&fixture.reload, &reload_snapshot,
                 sizeof(reload_snapshot)) == 0);
    memcpy(corrupt, requirement.body, sizeof(corrupt));
    corrupt[47] = 1U;
    CHECK(ucn_i_cluster_import(&fixture.reload, corrupt, sizeof(corrupt),
                               &durability, foundation.body_digest) ==
          UCN_ERR_MALFORMED);
    CHECK(memcmp(&fixture.reload, &reload_snapshot,
                 sizeof(reload_snapshot)) == 0);
    memcpy(corrupt, requirement.body, sizeof(corrupt));
    corrupt[56] = 1U;
    CHECK(ucn_i_cluster_import(&fixture.reload, corrupt, sizeof(corrupt),
                               &durability, foundation.body_digest) ==
          UCN_ERR_MALFORMED);
    CHECK(memcmp(&fixture.reload, &reload_snapshot,
                 sizeof(reload_snapshot)) == 0);
    CHECK(ucn_i_persistence_proof_retire(fixture.persistence,
                                         persistence_handle) == UCN_OK);

    {
        ucn_i_cluster_member_fact_t local;
        ucn_i_cluster_epoch_t successor;
        ucn_i_cluster_config_view_t successor_config;
        ucn_i_cluster_handover_ready_t ready;
        ucn_i_cluster_proof_t lineage_proof;

        memset(&local, 0, sizeof(local));
        local.lease_deadline_us = 8000U;
        local.capability_deadline_us = 8000U;
        local.binding_generation = 7U;
        local.session_generation = 2U;
        local.capability_generation = 3U;
        local.route_generation = 4U;
        local.link_generation = 5U;
        local.link_id = 1U;
        local.authenticated = 1U;
        local.current = 1U;
        principal(local.principal, 0x10U);
        memset(local.capability_digest, 0xA5,
               sizeof(local.capability_digest));
        CHECK(ucn_i_cluster_member_observe(
                  &fixture.cluster, &local, 300U) == UCN_OK);
        successor = epoch;
        successor.cluster_id = 4U;
        successor.term = 1U;
        successor_config = config;
        successor_config.config_id = 3U;
        successor_config.generation = 2U;
        memset(&durability, 0, sizeof(durability));
        durability.domain_id = fixture.entry.domain.domain_id;
        durability.foundation_transaction_id = 2U;
        durability.expected_record_generation = 1U;
        durability.absolute_deadline_us = 8000U;
        durability.persistence_domain_generation = 5U;
        durability.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
        durability.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
        CHECK(ucn_i_cluster_rekey_prepare(
                  &fixture.cluster, &successor, &successor_config,
                  1U, 310U, &durability, &requirement) == UCN_OK);
        CHECK(persist_cluster_requirement(
                  &fixture.cluster, &requirement, 320U,
                  &lineage_proof) == 0);
        CHECK(fixture.cluster.state.epoch.cluster_id == 4U &&
              fixture.cluster.state.retired_cluster_high_water == 3U &&
              fixture.cluster.state.lineage_generation == 1U);
        CHECK(ucn_i_cluster_import(
                  &fixture.reload, requirement.body,
                  requirement.body_bytes, &durability,
                  lineage_proof.body_digest) == UCN_OK);
        CHECK(fixture.reload.state.epoch.cluster_id == 4U &&
              fixture.reload.state.retired_cluster_high_water == 3U);

        successor.cluster_id = 9U;
        successor.term = 1U;
        successor.head_binding_generation = 20U;
        principal(successor.head_principal, 0x40U);
        memset(&successor_config, 0, sizeof(successor_config));
        successor_config.config_id = 40U;
        successor_config.generation = 1U;
        successor_config.member_count = 1U;
        successor_config.members[0].binding_generation = 20U;
        successor_config.members[0].flags =
            UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
            UCN_I_CLUSTER_MEMBER_FLAG_VOTER;
        principal(successor_config.members[0].principal, 0x40U);
        memset(&ready, 0, sizeof(ready));
        ready.transaction_id = 2U;
        ready.lease_deadline_us = 7000U;
        ready.target_cluster_id = successor.cluster_id;
        ready.target_term = successor.term;
        ready.target_binding_generation =
            successor.head_binding_generation;
        ready.target_config_id = successor_config.config_id;
        ready.target_config_generation = successor_config.generation;
        ready.capability_generation = 8U;
        ready.authority_generation = 9U;
        ready.authenticated = 1U;
        ready.quorum_verified = 1U;
        ready.durable_continuation = 1U;
        principal(ready.target_principal, 0x40U);
        memset(ready.proof_digest, 0xC7, sizeof(ready.proof_digest));
        memset(&durability, 0, sizeof(durability));
        durability.domain_id = fixture.entry.domain.domain_id;
        durability.foundation_transaction_id = 3U;
        durability.expected_record_generation = 2U;
        durability.absolute_deadline_us = 8000U;
        durability.persistence_domain_generation = 5U;
        durability.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
        durability.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
        CHECK(ucn_i_cluster_merge_retire_prepare(
                  &fixture.cluster, &successor, &successor_config,
                  &ready, 2U, 400U, &durability,
                  &requirement) == UCN_OK);
        CHECK(persist_cluster_requirement(
                  &fixture.cluster, &requirement, 410U,
                  &lineage_proof) == 0);
        CHECK(fixture.cluster.state.role == UCN_I_CLUSTER_FENCED &&
              fixture.cluster.state.retired_cluster_high_water == 4U);
        CHECK(ucn_i_cluster_import(
                  &fixture.reload, requirement.body,
                  requirement.body_bytes, &durability,
                  lineage_proof.body_digest) == UCN_OK);
        CHECK(fixture.reload.state.role == UCN_I_CLUSTER_FENCED &&
              ucn_i_cluster_merge_continuation_preflight(
                  &fixture.reload, 2U, 500U) == UCN_ERR_ACCESS);
        CHECK(ucn_i_cluster_merge_ready_accept(
                  &fixture.reload, &ready, 500U) == UCN_OK);
        CHECK(ucn_i_cluster_merge_continuation_preflight(
                  &fixture.reload, 2U, 501U) == UCN_OK);
    }
    return 0;
}

int main(void)
{
    int result = fixture_init();
    if (result == 0) result = test_foundation_round_trip();
    if (result != 0) fprintf(stderr, "cluster persistence failed: %d\n", result);
    return result;
}
