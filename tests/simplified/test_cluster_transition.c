#include "internal/ucn_cluster.h"

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
static test_lock_t lock_state;
static ucn_i_cluster_owner_t owner;
static uint64_t record_generation;
static uint64_t foundation_tx;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}
static void lock_leave(void *context) { ((test_lock_t *)context)->held = 0U; }

static ucn_i_lock_ops_t lock_ops(test_lock_t *state)
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

#if UCN_I_CLUSTER_CONFIG_MEMBER_COUNT >= 2U
static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static void write_be64(uint8_t *out, uint64_t value)
{
    write_be32(out, (uint32_t)(value >> 32U));
    write_be32(&out[4], (uint32_t)value);
}

static void encode_snapshot_member(
    const ucn_i_cluster_snapshot_member_t *member, uint8_t *out)
{
    memset(out, 0, UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES);
    memcpy(&out[0], member->principal, UCN_I_CLUSTER_PRINCIPAL_BYTES);
    write_be32(&out[16], member->binding_generation);
    write_be32(&out[20], member->session_generation);
    write_be32(&out[24], member->capability_generation);
    write_be32(&out[28], member->route_generation);
    write_be32(&out[32], member->link_generation);
    write_be16(&out[36], member->link_id);
    out[38] = member->flags;
    memcpy(&out[40], member->capability_digest,
           UCN_I_CLUSTER_DIGEST_BYTES);
}

static int build_current_snapshot(
    const ucn_i_cluster_config_view_t *stable,
    const ucn_i_cluster_member_fact_t *facts,
    uint32_t assignment_generation, uint32_t sequence,
    uint64_t deadline_us)
{
    ucn_i_cluster_snapshot_header_t header;
    ucn_i_cluster_snapshot_member_t member;
    ucn_i_sha256_workspace_t workspace;
    uint8_t canonical[UCN_I_CLUSTER_SNAPSHOT_CANONICAL_BYTES];
    uint8_t index;
    size_t bytes;
    memset(&header, 0, sizeof(header));
    memset(&workspace, 0, sizeof(workspace));
    memset(canonical, 0, sizeof(canonical));
    header.absolute_deadline_us = deadline_us;
    header.cluster_id = owner.state.epoch.cluster_id;
    header.term = owner.state.epoch.term;
    header.config_id = stable->config_id;
    header.config_generation = stable->generation;
    header.assignment_generation = assignment_generation;
    header.snapshot_sequence = sequence;
    header.member_count = stable->member_count;
    for (index = 0U; index < stable->member_count; ++index) {
        if ((stable->members[index].flags &
             UCN_I_CLUSTER_MEMBER_FLAG_VOTER) != 0U) {
            header.protected_voter_bitmap |= UINT32_C(1) << index;
        }
    }
    write_be32(&canonical[0], header.cluster_id);
    write_be32(&canonical[4], header.term);
    write_be32(&canonical[8], header.config_id);
    write_be32(&canonical[12], header.config_generation);
    write_be32(&canonical[16], header.assignment_generation);
    write_be32(&canonical[20], header.snapshot_sequence);
    write_be32(&canonical[24], header.protected_voter_bitmap);
    canonical[28] = header.member_count;
    write_be64(&canonical[32], header.absolute_deadline_us);
    for (index = 0U; index < stable->member_count; ++index) {
        memset(&member, 0, sizeof(member));
        member.binding_generation = facts[index].binding_generation;
        member.session_generation = facts[index].session_generation;
        member.capability_generation = facts[index].capability_generation;
        member.route_generation = facts[index].route_generation;
        member.link_generation = facts[index].link_generation;
        member.link_id = facts[index].link_id;
        member.flags = stable->members[index].flags;
        memcpy(member.principal, facts[index].principal,
               sizeof(member.principal));
        memcpy(member.capability_digest, facts[index].capability_digest,
               sizeof(member.capability_digest));
        encode_snapshot_member(&member,
            &canonical[UCN_I_CLUSTER_SNAPSHOT_HEADER_BYTES +
                       (size_t)index *
                           UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES]);
    }
    bytes = UCN_I_CLUSTER_SNAPSHOT_HEADER_BYTES +
            (size_t)stable->member_count *
                UCN_I_CLUSTER_SNAPSHOT_MEMBER_BYTES;
    CHECK(ucn_i_sha256_128(canonical, bytes, header.expected_digest,
                           &workspace) == UCN_OK);
    CHECK(ucn_i_cluster_snapshot_begin(&owner, &header, 30U) == UCN_OK);
    for (index = 0U; index < stable->member_count; ++index) {
        memset(&member, 0, sizeof(member));
        member.binding_generation = facts[index].binding_generation;
        member.session_generation = facts[index].session_generation;
        member.capability_generation = facts[index].capability_generation;
        member.route_generation = facts[index].route_generation;
        member.link_generation = facts[index].link_generation;
        member.link_id = facts[index].link_id;
        member.flags = stable->members[index].flags;
        memcpy(member.principal, facts[index].principal,
               sizeof(member.principal));
        memcpy(member.capability_digest, facts[index].capability_digest,
               sizeof(member.capability_digest));
        CHECK(ucn_i_cluster_snapshot_member(&owner, &member,
                                             31U + index) == UCN_OK);
    }
    CHECK(ucn_i_cluster_snapshot_end(&owner, 40U) == UCN_OK);
    return 0;
}
#endif

static ucn_i_cluster_config_view_t two_voter_config(void)
{
    ucn_i_cluster_config_view_t config;
    memset(&config, 0, sizeof(config));
    config.config_id = 5U;
    config.generation = 1U;
#if UCN_I_CLUSTER_CONFIG_MEMBER_COUNT >= 2U
    config.member_count = 2U;
#else
    config.member_count = 1U;
#endif
    config.members[0].binding_generation = 7U;
    config.members[0].flags = UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
                              UCN_I_CLUSTER_MEMBER_FLAG_VOTER;
    principal(config.members[0].principal, 0x10U);
#if UCN_I_CLUSTER_CONFIG_MEMBER_COUNT >= 2U
    config.members[1].binding_generation = 8U;
    config.members[1].flags = UCN_I_CLUSTER_MEMBER_FLAG_MEMBER |
                              UCN_I_CLUSTER_MEMBER_FLAG_VOTER |
                              UCN_I_CLUSTER_MEMBER_FLAG_BACKUP;
    principal(config.members[1].principal, 0x30U);
#endif
    return config;
}

static ucn_i_cluster_epoch_t epoch(uint32_t term, uint8_t head_seed,
                                    uint32_t binding)
{
    ucn_i_cluster_epoch_t value;
    memset(&value, 0, sizeof(value));
    value.cluster_id = 4U;
    value.term = term;
    value.head_binding_generation = binding;
    principal(value.head_principal, head_seed);
    return value;
}

static ucn_i_cluster_durability_t next_durability(void)
{
    ucn_i_cluster_durability_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = 801U;
    value.foundation_transaction_id = ++foundation_tx;
    value.expected_record_generation = record_generation;
    value.absolute_deadline_us = 9000U;
    value.persistence_domain_generation = 3U;
    value.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    value.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    return value;
}

static int publish(ucn_i_cluster_requirement_t *requirement,
                   uint64_t now_us)
{
    ucn_i_cluster_proof_t proof;
    ucn_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = 1U;
    handle.owner_instance = 99U;
    handle.slot = 1U;
    handle.generation = (uint16_t)foundation_tx;
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    CHECK(ucn_i_cluster_bind_persistence(
              &owner, handle,
              requirement->canonical_body_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = handle;
    proof.domain_id = requirement->durability.domain_id;
    proof.foundation_transaction_id =
        requirement->durability.foundation_transaction_id;
    proof.record_generation = ++record_generation;
    proof.witness_generation = proof.record_generation;
    proof.runtime_instance = 1U;
    proof.body_bytes = UCN_I_CLUSTER_RECORD_BYTES;
    proof.persistence_domain_generation = 3U;
    proof.persistence_owner_instance = handle.owner_instance;
    proof.caller_owner_instance = 8U;
    proof.schema_id = UCN_I_CLUSTER_RECORD_SCHEMA_ID;
    proof.schema_version = UCN_I_CLUSTER_RECORD_SCHEMA;
    proof.operation_kind = requirement->operation_kind;
    memcpy(proof.body_digest, requirement->canonical_body_digest,
           sizeof(proof.body_digest));
    CHECK(ucn_i_cluster_accept_proof(&owner, &proof, now_us) == UCN_OK);
    return 0;
}

static ucn_i_cluster_member_fact_t member_fact(uint8_t seed,
                                                uint32_t binding,
                                                uint32_t session)
{
    ucn_i_cluster_member_fact_t fact;
    memset(&fact, 0, sizeof(fact));
    fact.lease_deadline_us = 8000U;
    fact.capability_deadline_us = 8000U;
    fact.binding_generation = binding;
    fact.session_generation = session;
    fact.capability_generation = session + 10U;
    fact.route_generation = 2U;
    fact.link_generation = 3U;
    fact.link_id = (uint16_t)(seed + 1U);
    fact.authenticated = 1U;
    fact.current = 1U;
    fact.backup_eligible = seed == 0x30U ? 1U : 0U;
    principal(fact.principal, seed);
    memset(fact.capability_digest, (int)(uint8_t)(seed ^ session),
           sizeof(fact.capability_digest));
    return fact;
}

static ucn_i_cluster_vote_evidence_t vote_from(
    const ucn_i_cluster_member_fact_t *fact, uint8_t index,
    uint32_t vote_id)
{
    ucn_i_cluster_vote_evidence_t vote;
    memset(&vote, 0, sizeof(vote));
    vote.binding_generation = fact->binding_generation;
    vote.session_generation = fact->session_generation;
    vote.capability_generation = fact->capability_generation;
    vote.vote_id = vote_id;
    vote.valid = 1U;
    vote.voter_index = index;
    memcpy(vote.principal, fact->principal, sizeof(vote.principal));
    memcpy(vote.canonical_digest, fact->capability_digest,
           sizeof(vote.canonical_digest));
    return vote;
}

static int configure_with(uint8_t local_seed, uint8_t role,
                          const ucn_i_cluster_config_view_t *stable)
{
    ucn_i_cluster_config_t config;
    ucn_i_cluster_epoch_t active = epoch(1U, 0x10U, 7U);
    ucn_i_cluster_durability_t durable;
    ucn_i_cluster_requirement_t requirement;
    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&config, 0, sizeof(config));
    record_generation = 0U;
    foundation_tx = 0U;
    config.runtime_instance = 1U;
    config.owner_instance = 8U;
    principal(config.local_principal, local_seed);
    config.state_lock = lock_ops(&lock_state);
    CHECK(ucn_i_cluster_owner_init(&owner, &config) == UCN_OK);
    durable = next_durability();
    CHECK(ucn_i_cluster_create_prepare(
              &owner, &active, stable, role, &durable,
              &requirement) == UCN_OK);
    CHECK(publish(&requirement, 10U) == 0);
    return 0;
}

#if UCN_I_CLUSTER_CONFIG_MEMBER_COUNT >= 2U
static int configure(uint8_t local_seed, uint8_t role)
{
    ucn_i_cluster_config_view_t stable = two_voter_config();
    return configure_with(local_seed, role, &stable);
}
#endif

static int persist_vote(const ucn_i_cluster_vote_evidence_t *vote,
                        uint64_t now_us)
{
    ucn_i_cluster_durability_t durable = next_durability();
    ucn_i_cluster_requirement_t requirement;
    CHECK(ucn_i_cluster_transition_vote_prepare(
              &owner, vote, false, now_us, &durable,
              &requirement) == UCN_OK);
    CHECK(publish(&requirement, now_us + 1U) == 0);
    return 0;
}

static int test_recovery_single_voter(void)
{
    ucn_i_cluster_config_view_t stable = two_voter_config();
    ucn_i_cluster_epoch_t target = epoch(2U, 0x10U, 7U);
    ucn_i_cluster_member_fact_t local = member_fact(0x10U, 7U, 3U);
    ucn_i_cluster_vote_evidence_t vote = vote_from(&local, 0U, 31U);
    ucn_i_cluster_durability_t durable;
    ucn_i_cluster_requirement_t requirement;

    stable.member_count = 1U;
#if UCN_I_CLUSTER_CONFIG_MEMBER_COUNT >= 2U
    memset(&stable.members[1], 0,
           sizeof(stable.members) - sizeof(stable.members[0]));
#endif
    CHECK(configure_with(0x10U, UCN_I_CLUSTER_VOTER, &stable) == 0);
    CHECK(ucn_i_cluster_member_observe(&owner, &local, 20U) == UCN_OK);
    CHECK(ucn_i_cluster_transition_begin(
              &owner, UCN_I_CLUSTER_TRANSITION_RECOVERY,
              &target, &stable, 1U, 7000U, 100U) == UCN_OK);
    CHECK(persist_vote(&vote, 110U) == 0);
    CHECK(owner.state.phase == UCN_I_CLUSTER_QUORUM);
    durable = next_durability();
    CHECK(ucn_i_cluster_transition_commit_prepare(
              &owner, 1U, 120U, &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 130U) == 0);
    CHECK(owner.state.phase == UCN_I_CLUSTER_EPOCH_DURABLE &&
          owner.state.role == UCN_I_CLUSTER_HEAD &&
          owner.state.epoch.term == 2U &&
          owner.state.backup_ready == 0U);
    return 0;
}

#if UCN_I_CLUSTER_CONFIG_MEMBER_COUNT >= 2U

static int test_takeover_revalidation(void)
{
    ucn_i_cluster_config_view_t stable = two_voter_config();
    ucn_i_cluster_epoch_t target = epoch(2U, 0x30U, 8U);
    ucn_i_cluster_member_fact_t head = member_fact(0x10U, 7U, 1U);
    ucn_i_cluster_member_fact_t backup = member_fact(0x30U, 8U, 2U);
    ucn_i_cluster_member_fact_t facts[2];
    ucn_i_cluster_vote_evidence_t head_vote;
    ucn_i_cluster_vote_evidence_t backup_vote;
    ucn_i_cluster_durability_t durable;
    ucn_i_cluster_requirement_t requirement;
    ucn_i_cluster_step_result_t step;
    uint8_t snapshot_digest[UCN_I_CLUSTER_DIGEST_BYTES];
    const uint8_t zero_digest[UCN_I_CLUSTER_DIGEST_BYTES] = {0};

    CHECK(configure(0x30U, UCN_I_CLUSTER_BACKUP) == 0);
    CHECK(ucn_i_cluster_member_observe(&owner, &head, 20U) == UCN_OK);
    CHECK(ucn_i_cluster_member_observe(&owner, &backup, 20U) == UCN_OK);
    memset(snapshot_digest, 0x5AU, sizeof(snapshot_digest));
    {
        ucn_i_cluster_config_view_t wrong = stable;
        wrong.generation++;
        durable = next_durability();
        CHECK(ucn_i_cluster_backup_ready_prepare(
                  &owner, 1U, &wrong, 3U, snapshot_digest, &durable,
                  &requirement) == UCN_ERR_STATE);
        CHECK(owner.state.backup_ready == 0U && owner.pending_valid == 0U);
    }
    durable = next_durability();
    CHECK(ucn_i_cluster_backup_ready_prepare(
              &owner, 1U, &stable, 1U, snapshot_digest, &durable,
              &requirement) == UCN_ERR_STATE);
    memset(snapshot_digest, 0, sizeof(snapshot_digest));
    CHECK(ucn_i_cluster_backup_ready_prepare(
              &owner, 1U, &stable, 3U, snapshot_digest, &durable,
              &requirement) == UCN_ERR_ARGUMENT);
    memset(snapshot_digest, 0x5AU, sizeof(snapshot_digest));
    facts[0] = head;
    facts[1] = backup;
    CHECK(build_current_snapshot(&stable, facts, 1U, 1U, 7000U) == 0);
    durable = next_durability();
    CHECK(ucn_i_cluster_backup_ready_from_mirror_prepare(
              &owner, &durable, &requirement) == UCN_OK);
    CHECK(owner.state.backup_ready == 0U);
    CHECK(publish(&requirement, 90U) == 0);
    CHECK(owner.state.backup_ready == 1U &&
          memcmp(owner.state.backup_snapshot_digest,
                 owner.snapshot_sync.buffers[
                     owner.snapshot_sync.active_index].header.expected_digest,
                 sizeof(snapshot_digest)) == 0);
    CHECK(ucn_i_cluster_transition_begin(
              &owner, UCN_I_CLUSTER_TRANSITION_TAKEOVER,
              &target, &stable, 1U, 7000U, 100U) == UCN_OK);
    head_vote = vote_from(&head, 0U, 11U);
    backup_vote = vote_from(&backup, 1U, 12U);
    CHECK(persist_vote(&head_vote, 110U) == 0);
    CHECK(persist_vote(&backup_vote, 120U) == 0);
    CHECK(owner.state.phase == UCN_I_CLUSTER_QUORUM);

    head = member_fact(0x10U, 7U, 9U);
    CHECK(ucn_i_cluster_member_observe(&owner, &head, 130U) == UCN_OK);
    CHECK(owner.state.phase == UCN_I_CLUSTER_COLLECTING);
    durable = next_durability();
    CHECK(ucn_i_cluster_transition_commit_prepare(
              &owner, 1U, 140U, &durable,
              &requirement) == UCN_ERR_STATE);
    head_vote = vote_from(&head, 0U, 13U);
    CHECK(persist_vote(&head_vote, 150U) == 0);
    CHECK(owner.state.phase == UCN_I_CLUSTER_QUORUM);
    durable = next_durability();
    CHECK(ucn_i_cluster_transition_commit_prepare(
              &owner, 1U, 160U, &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 170U) == 0);
    CHECK(owner.state.phase == UCN_I_CLUSTER_EPOCH_DURABLE &&
          owner.state.role == UCN_I_CLUSTER_HEAD &&
          owner.state.epoch.term == 2U);
    CHECK(owner.state.backup_ready == 0U &&
          memcmp(owner.state.backup_snapshot_digest,
                 zero_digest,
                 UCN_I_CLUSTER_DIGEST_BYTES) == 0);
    CHECK(owner.snapshot_sync.buffers[0].valid == 0U &&
          owner.snapshot_sync.buffers[1].valid == 0U);
    CHECK(ucn_i_cluster_transition_vote_prepare(
              &owner, &backup_vote, false, 180U, &durable,
              &requirement) == UCN_ERR_STATE);
    memset(&step, 0xA5, sizeof(step));
    CHECK(ucn_i_cluster_step(&owner, 8000U, 4U, &step) == UCN_OK);
    CHECK(owner.state.phase == UCN_I_CLUSTER_EPOCH_DURABLE);
    return 0;
}

static int test_planned_handover_fence(void)
{
    ucn_i_cluster_config_view_t stable = two_voter_config();
    ucn_i_cluster_epoch_t target = epoch(2U, 0x30U, 8U);
    ucn_i_cluster_member_fact_t head = member_fact(0x10U, 7U, 1U);
    ucn_i_cluster_member_fact_t backup = member_fact(0x30U, 8U, 2U);
    ucn_i_cluster_vote_evidence_t vote0 = vote_from(&head, 0U, 21U);
    ucn_i_cluster_vote_evidence_t vote1 = vote_from(&backup, 1U, 22U);
    ucn_i_cluster_handover_ready_t ready;
    ucn_i_cluster_durability_t durable;
    ucn_i_cluster_requirement_t requirement;
    ucn_i_cluster_authority_view_t view;

    CHECK(configure(0x10U, UCN_I_CLUSTER_HEAD) == 0);
    CHECK(ucn_i_cluster_member_observe(&owner, &head, 20U) == UCN_OK);
    CHECK(ucn_i_cluster_member_observe(&owner, &backup, 20U) == UCN_OK);
    CHECK(ucn_i_cluster_transition_begin(
              &owner, UCN_I_CLUSTER_TRANSITION_HANDOVER,
              &target, &stable, 1U, 7000U, 100U) == UCN_OK);
    CHECK(persist_vote(&vote0, 110U) == 0);
    CHECK(persist_vote(&vote1, 120U) == 0);
    memset(&ready, 0, sizeof(ready));
    ready.transaction_id = 1U;
    ready.lease_deadline_us = 6000U;
    ready.target_cluster_id = target.cluster_id;
    ready.target_term = target.term;
    ready.target_binding_generation = target.head_binding_generation;
    ready.target_config_id = stable.config_id;
    ready.target_config_generation = stable.generation;
    ready.capability_generation = backup.capability_generation;
    ready.authority_generation = 2U;
    principal(ready.target_principal, 0x30U);
    memset(ready.proof_digest, 0xCC, sizeof(ready.proof_digest));
    ready.authenticated = 1U;
    ready.quorum_verified = 1U;
    ready.durable_continuation = 1U;
    {
        ucn_i_cluster_owner_t unchanged = owner;
        ready.target_term++;
        CHECK(ucn_i_cluster_handover_ready_accept(
                  &owner, &ready, 130U) == UCN_ERR_STATE);
        CHECK(memcmp(&owner, &unchanged, sizeof(owner)) == 0);
        ready.target_term--;
    }
    CHECK(ucn_i_cluster_handover_ready_accept(&owner, &ready, 130U) ==
          UCN_OK);
    durable = next_durability();
    CHECK(ucn_i_cluster_transition_commit_prepare(
              &owner, 1U, 140U, &durable, &requirement) == UCN_OK);
    CHECK(publish(&requirement, 150U) == 0);
    CHECK(owner.state.role == UCN_I_CLUSTER_FENCED &&
          owner.state.authority_fenced == 1U &&
          owner.state.phase == UCN_I_CLUSTER_EPOCH_DURABLE);
    CHECK(ucn_i_cluster_handover_continuation_preflight(
              &owner, 1U, 151U) == UCN_ERR_ACCESS);
    {
        ucn_i_cluster_handover_ready_t wrong = ready;
        wrong.proof_digest[0] ^= 1U;
        CHECK(ucn_i_cluster_handover_ready_accept(
                  &owner, &wrong, 152U) == UCN_ERR_SECURITY);
    }
    CHECK(ucn_i_cluster_handover_ready_accept(&owner, &ready, 155U) ==
          UCN_OK);
    CHECK(ucn_i_cluster_handover_continuation_preflight(
              &owner, 1U, 160U) == UCN_OK);
    CHECK(ucn_i_cluster_handover_continuation_preflight(
              &owner, 2U, 160U) == UCN_ERR_ACCESS);
    CHECK(ucn_i_cluster_handover_continuation_preflight(
              &owner, 1U, 7000U) == UCN_ERR_ACCESS);
    CHECK(ucn_i_cluster_authority_preflight(&owner, 160U, &view) ==
          UCN_ERR_ACCESS);
    return 0;
}
#endif

int main(void)
{
    int result = test_recovery_single_voter();
#if UCN_I_CLUSTER_CONFIG_MEMBER_COUNT >= 2U
    if (result == 0) result = test_takeover_revalidation();
    if (result == 0) result = test_planned_handover_fence();
#endif
    if (result != 0) fprintf(stderr, "cluster transition failed: %d\n", result);
    return result;
}
