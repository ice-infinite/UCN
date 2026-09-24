#include "internal/ucn_group.h"

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
static test_lock_t reload_lock_state;
static ucn_i_group_owner_t owner;
static ucn_i_group_owner_t reload_owner;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    ((test_lock_t *)context)->held = 0U;
}

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

static void fill_principal(uint8_t value[UCN_I_GROUP_PRINCIPAL_BYTES],
                           uint8_t seed)
{
    uint8_t index;
    for (index = 0U; index < UCN_I_GROUP_PRINCIPAL_BYTES; ++index) {
        value[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_group_context_config_t group_config(uint8_t mode,
                                                  uint32_t group_id,
                                                  bool secure)
{
    ucn_i_group_context_config_t config;
    uint8_t index;

    memset(&config, 0, sizeof(config));
    config.realm_id = 9U;
    config.group_id = group_id;
    config.group_generation = 1U;
    config.policy_generation = 1U;
    config.member_generation = 1U;
    config.endpoint = 0x1201U;
    config.opcode = 0x2201U;
    config.mode = mode;
    config.member_count = 2U;
    config.unicast_fanout_limit = 2U;
    config.quorum_weight = 2U;
    config.allowed_scope_mask =
        (uint8_t)((1U << UCN_I_GROUP_SCOPE_COUNT) - 1U);
    config.secure_required = secure ? 1U : 0U;
    config.public_static = secure ? 0U : 1U;
    if (secure) {
        config.security.runtime_instance = 1U;
        config.security.key_generation = 7U;
        config.security.owner_instance = 10U;
        config.security.slot = 2U;
        config.security.generation = 3U;
        config.security.valid = 1U;
        config.security.exact_principal = 1U;
        memset(config.security.context_digest, 0xA5,
               sizeof(config.security.context_digest));
    }
    for (index = 0U; index < config.member_count; ++index) {
        config.members[index].address = (uint32_t)(100U + index);
        config.members[index].binding_generation = (uint32_t)(20U + index);
        config.members[index].sender_slot = (uint16_t)(index + 1U);
        config.members[index].weight = 1U;
        fill_principal(config.members[index].principal,
                       (uint8_t)(0x20U + index * 0x20U));
    }
    return config;
}

static ucn_i_group_authority_facts_t authority_facts(void)
{
    ucn_i_group_authority_facts_t facts;
    memset(&facts, 0, sizeof(facts));
    facts.lease_deadline_us = 5000U;
    facts.realm_id = 9U;
    facts.authority_generation = 4U;
    facts.owner_instance = 12U;
    facts.authenticated = 1U;
    facts.quorum_met = 1U;
    facts.current = 1U;
    memset(facts.proof_digest, 0x5A, sizeof(facts.proof_digest));
    return facts;
}

static ucn_i_group_durability_t durability(uint64_t transaction_id,
                                            uint64_t record_generation)
{
    ucn_i_group_durability_t value;
    memset(&value, 0, sizeof(value));
    value.domain_id = 900U;
    value.foundation_transaction_id = transaction_id;
    value.expected_record_generation = record_generation;
    value.absolute_deadline_us = 4000U;
    value.persistence_domain_generation = 6U;
    value.schema_id = UCN_I_GROUP_RECORD_SCHEMA_ID;
    value.schema_version = UCN_I_GROUP_RECORD_SCHEMA;
    return value;
}

static ucn_handle_t persistence_handle(uint16_t generation)
{
    ucn_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = 1U;
    handle.owner_instance = 11U;
    handle.slot = 1U;
    handle.generation = generation;
    handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    return handle;
}

static int publish_dynamic(ucn_i_group_context_config_t *config,
                           uint64_t transaction_id,
                           uint64_t record_generation,
                           ucn_handle_t *handle_out,
                           ucn_i_group_requirement_t *requirement_out)
{
    ucn_i_group_authority_facts_t authority = authority_facts();
    ucn_i_group_durability_t durable =
        durability(transaction_id, record_generation);
    ucn_i_group_requirement_t requirement;
    ucn_i_group_proof_t proof;
    ucn_handle_t group;
    ucn_handle_t persistence = persistence_handle((uint16_t)transaction_id);

    CHECK(ucn_i_group_admin_prepare(&owner, config, &authority, &durable,
                                    100U, &group, &requirement) == UCN_OK);
    CHECK(ucn_i_group_bind_persistence(
              &owner, group, persistence,
              requirement.canonical_body_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence;
    proof.domain_id = durable.domain_id;
    proof.foundation_transaction_id = durable.foundation_transaction_id;
    proof.record_generation = record_generation + 1U;
    proof.witness_generation = proof.record_generation;
    proof.runtime_instance = 1U;
    proof.body_bytes = UCN_I_GROUP_RECORD_BYTES;
    proof.persistence_domain_generation =
        durable.persistence_domain_generation;
    proof.persistence_owner_instance = persistence.owner_instance;
    proof.caller_owner_instance = 8U;
    proof.schema_id = UCN_I_GROUP_RECORD_SCHEMA_ID;
    proof.schema_version = UCN_I_GROUP_RECORD_SCHEMA;
    proof.operation_kind = UCN_I_GROUP_PERSIST_KIND;
    memcpy(proof.body_digest, requirement.canonical_body_digest,
           sizeof(proof.body_digest));
    {
        ucn_i_group_proof_t wrong = proof;
        wrong.body_digest[0] ^= 1U;
        CHECK(ucn_i_group_accept_proof(&owner, group, &wrong, &authority,
                                       200U) == UCN_ERR_STATE);
    }
    CHECK(ucn_i_group_accept_proof(&owner, group, &proof, &authority,
                                   200U) == UCN_OK);
    if (handle_out != NULL) *handle_out = group;
    if (requirement_out != NULL) *requirement_out = requirement;
    return 0;
}

static int retire_dynamic(ucn_handle_t group, uint64_t transaction_id,
                          uint64_t record_generation,
                          ucn_i_group_requirement_t *requirement_out)
{
    ucn_i_group_authority_facts_t authority = authority_facts();
    ucn_i_group_durability_t durable =
        durability(transaction_id, record_generation);
    ucn_i_group_requirement_t requirement;
    ucn_i_group_proof_t proof;
    ucn_handle_t persistence = persistence_handle((uint16_t)transaction_id);

    CHECK(ucn_i_group_admin_retire_prepare(&owner, group, &authority,
                                            &durable, 100U,
                                            &requirement) == UCN_OK);
    CHECK(ucn_i_group_bind_persistence(
              &owner, group, persistence,
              requirement.canonical_body_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence;
    proof.domain_id = durable.domain_id;
    proof.foundation_transaction_id = durable.foundation_transaction_id;
    proof.record_generation = record_generation + 1U;
    proof.witness_generation = proof.record_generation;
    proof.runtime_instance = 1U;
    proof.body_bytes = UCN_I_GROUP_RECORD_BYTES;
    proof.persistence_domain_generation =
        durable.persistence_domain_generation;
    proof.persistence_owner_instance = persistence.owner_instance;
    proof.caller_owner_instance = 8U;
    proof.schema_id = UCN_I_GROUP_RECORD_SCHEMA_ID;
    proof.schema_version = UCN_I_GROUP_RECORD_SCHEMA;
    proof.operation_kind = UCN_I_GROUP_PERSIST_KIND;
    memcpy(proof.body_digest, requirement.canonical_body_digest,
           sizeof(proof.body_digest));
    CHECK(ucn_i_group_accept_proof(&owner, group, &proof, &authority,
                                   200U) == UCN_OK);
    if (requirement_out != NULL) *requirement_out = requirement;
    return 0;
}

static int configure_owner(ucn_i_group_owner_t *target, test_lock_t *lock,
                           uint16_t owner_instance)
{
    ucn_i_group_config_t config;
    memset(target, 0, sizeof(*target));
    memset(lock, 0, sizeof(*lock));
    memset(&config, 0, sizeof(config));
    config.runtime_instance = 1U;
    config.realm_id = 9U;
    config.owner_instance = owner_instance;
    config.state_lock = lock_ops(lock);
    CHECK(ucn_i_group_owner_init(target, &config) == UCN_OK);
    return 0;
}

static int test_static_context(void)
{
    ucn_i_group_context_config_t config =
        group_config(UCN_I_GROUP_STATIC, 41U, false);
    ucn_i_group_context_config_t wrong_realm;
    ucn_i_group_context_config_t view;
    ucn_i_group_dependency_event_t event;
    ucn_handle_t handle;
    ucn_handle_t old_handle;
    ucn_handle_t sentinel;
    uint8_t phase = 0U;

    wrong_realm = config;
    wrong_realm.realm_id = 8U;
    memset(&handle, 0xA5, sizeof(handle));
    sentinel = handle;
    CHECK(ucn_i_group_install_static(&owner, 0U, &wrong_realm, true, true,
                                     &handle) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&handle, &sentinel, sizeof(handle)) == 0);
    CHECK(ucn_i_group_install_static(&owner, 0U, &config, false, true,
                                     &handle) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_group_install_static(&owner, 0U, &config, true, true,
                                     &handle) == UCN_OK);
    CHECK(ucn_i_group_install_static(&owner, 0U, &config, true, true,
                                     &handle) == UCN_OK);
    CHECK(ucn_i_group_context_view(&owner, handle, &view, &phase) == UCN_OK);
    CHECK(phase == UCN_I_GROUP_ACTIVE && view.group_id == 41U &&
          view.public_static == 1U);
    old_handle = handle;
    config.member_generation++;
    CHECK(ucn_i_group_install_static(&owner, 0U, &config, true, true,
                                     &handle) == UCN_ERR_STATE);
    memset(&event, 0, sizeof(event));
    event.group_id = 41U;
    event.group_generation = 1U;
    event.policy_generation = 1U;
    event.member_generation = 2U;
    event.security_current = 1U;
    event.tree_current = 1U;
    event.authority_current = 1U;
    CHECK(ucn_i_group_dependency_change(&owner, &event) == UCN_OK);
    config.group_generation = 2U;
    config.policy_generation = 2U;
    CHECK(ucn_i_group_install_static(&owner, 0U, &config, true, true,
                                     &handle) == UCN_OK);
    CHECK(ucn_i_group_context_view(&owner, old_handle, &view, &phase) ==
          UCN_ERR_STATE);
    CHECK(ucn_i_group_context_view(&owner, handle, &view, &phase) == UCN_OK);
    CHECK(phase == UCN_I_GROUP_ACTIVE && view.group_generation == 2U &&
          view.policy_generation == 2U && view.member_generation == 2U);
    return 0;
}

static int test_dynamic_lifecycle_and_reload(void)
{
    ucn_i_group_context_config_t config =
        group_config(UCN_I_GROUP_DYNAMIC, 0U, true);
    ucn_i_group_context_config_t view;
    ucn_i_group_requirement_t requirement;
    ucn_i_group_durability_t durable = durability(1U, 0U);
    ucn_handle_t handle;
    ucn_handle_t reopened;
    uint8_t phase = 0U;
    int line;

    {
        ucn_i_group_context_config_t wrong_realm = config;
        ucn_i_group_authority_facts_t wrong_authority = authority_facts();
        ucn_i_group_requirement_t unchanged;
        ucn_i_group_requirement_t sentinel;
        ucn_handle_t unchanged_handle;
        ucn_handle_t handle_sentinel;

        wrong_realm.realm_id = 8U;
        wrong_authority.realm_id = 8U;
        memset(&unchanged, 0xA5, sizeof(unchanged));
        sentinel = unchanged;
        memset(&unchanged_handle, 0x5A, sizeof(unchanged_handle));
        handle_sentinel = unchanged_handle;
        CHECK(ucn_i_group_admin_prepare(&owner, &wrong_realm,
                                        &wrong_authority, &durable, 100U,
                                        &unchanged_handle, &unchanged) ==
              UCN_ERR_ARGUMENT);
        CHECK(memcmp(&unchanged, &sentinel, sizeof(unchanged)) == 0);
        CHECK(memcmp(&unchanged_handle, &handle_sentinel,
                     sizeof(unchanged_handle)) == 0);
    }

    line = publish_dynamic(&config, 1U, 0U, &handle, &requirement);
    if (line != 0) return line;
    CHECK(ucn_i_group_context_view(&owner, handle, &view, &phase) == UCN_OK);
    CHECK(phase == UCN_I_GROUP_ACTIVE && view.group_id == 1U &&
          view.group_generation == 1U && owner.dynamic_id_high_water == 1U);
    CHECK(requirement.body[8] == 0U && requirement.body[9] == 0U &&
          requirement.body[10] == 0U && requirement.body[11] == 1U);

    CHECK(configure_owner(&reload_owner, &reload_lock_state, 8U) == 0);
    CHECK(ucn_i_group_import(&reload_owner, requirement.body,
                             requirement.body_bytes, &durable,
                             requirement.canonical_body_digest,
                             &reopened) == UCN_OK);
    CHECK(ucn_i_group_context_view(&reload_owner, reopened, &view,
                                   &phase) == UCN_OK);
    CHECK(view.group_id == 1U && phase == UCN_I_GROUP_ACTIVE);

    config = view;
    config.group_generation = 2U;
    config.policy_generation = 2U;
    line = publish_dynamic(&config, 2U, 1U, &handle, NULL);
    if (line != 0) return line;
    CHECK(ucn_i_group_context_view(&owner, handle, &view,
                                   &phase) == UCN_ERR_STATE);
    CHECK(ucn_i_group_open(&owner, 1U, 2U, 2U, &reopened) == UCN_OK);
    CHECK(ucn_i_group_context_view(&owner, reopened, &view, &phase) == UCN_OK);
    CHECK(view.member_generation == 1U && phase == UCN_I_GROUP_ACTIVE);

    line = retire_dynamic(reopened, 3U, 2U, &requirement);
    if (line != 0) return line;
    CHECK(ucn_i_group_context_view(&owner, reopened, &view, &phase) ==
          UCN_ERR_STATE);
    CHECK(owner.dynamic_id_high_water == 1U);

    durable = durability(3U, 2U);
    memset(&reopened, 0xA5, sizeof(reopened));
    CHECK(ucn_i_group_import(&reload_owner, requirement.body,
                             requirement.body_bytes, &durable,
                             requirement.canonical_body_digest,
                             &reopened) == UCN_OK);
    CHECK(memcmp(&reopened, &(ucn_handle_t){0}, sizeof(reopened)) == 0);
    CHECK(reload_owner.dynamic_id_high_water == 1U);

    config = group_config(UCN_I_GROUP_DYNAMIC, 0U, true);
    line = publish_dynamic(&config, 4U, 3U, &handle, NULL);
    if (line != 0) return line;
    CHECK(owner.dynamic_id_high_water == 2U);
    CHECK(ucn_i_group_open(&owner, 2U, 1U, 1U, &reopened) == UCN_OK);
    return 0;
}

int main(void)
{
    int result;

    result = configure_owner(&owner, &lock_state, 8U);
    if (result != 0) return result;
    result = test_static_context();
    if (result != 0) return result;
    result = test_dynamic_lifecycle_and_reload();
    if (result != 0) return result;
    CHECK(ucn_i_group_owner_destroy(&reload_owner) == UCN_OK);
    CHECK(ucn_i_group_owner_destroy(&owner) == UCN_OK);
    printf("group_context_tests=PASS contexts=%u members=%u\n",
           (unsigned)UCN_I_GROUP_CONTEXT_COUNT,
           (unsigned)UCN_I_GROUP_MEMBER_COUNT);
    return 0;
}
