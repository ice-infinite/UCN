#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_handover.h"
#include "ucn/ucn_time.h"

#define ASSERT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                      \
            (void)fprintf(stderr, "ASSERT %s at %s:%d\\n", #condition,       \
                          __FILE__, __LINE__);                                   \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static ucn_cluster_epoch_t make_epoch(uint32_t cluster_id,
                                      uint32_t term,
                                      ucn_node_id_t head)
{
    ucn_cluster_epoch_t epoch;

    epoch.cluster_id = cluster_id;
    epoch.term = term;
    epoch.head_node_id = head;
    return epoch;
}

static ucn_cluster_handover_policy_t make_policy(void)
{
    ucn_cluster_handover_policy_t policy;

    (void)memset(&policy, 0, sizeof(policy));
    policy.improvement_percent = 10U;
    policy.required_samples = 2U;
    policy.required_capabilities = UCN_CLUSTER_HANDOVER_REQUIRED_CAPABILITIES;
    policy.head_min_tenure_ms = 20U;
    policy.merge_hold_down_ms = 50U;
    policy.retry_interval_ms = 10U;
    policy.transaction_timeout_ms = 100U;
    return policy;
}

static ucn_cluster_handover_offer_t make_offer(uint32_t cluster_id,
                                                uint32_t term,
                                                ucn_node_id_t head,
                                                uint32_t config_id,
                                                uint32_t config_hash,
                                                uint32_t nonce)
{
    ucn_cluster_handover_offer_t offer;

    (void)memset(&offer, 0, sizeof(offer));
    offer.epoch = make_epoch(cluster_id, term, head);
    offer.config_id = config_id;
    offer.config_hash = config_hash;
    offer.nonce = nonce;
    offer.head_score = 900U;
    offer.cluster_size = 3U;
    offer.available_capacity = 8U;
    offer.capabilities = UCN_CLUSTER_HANDOVER_REQUIRED_CAPABILITIES;
    offer.wire_format = UCN_CLUSTER_HANDOVER_WIRE_FORMAT_V4;
    offer.backup_policy_compatible = true;
    return offer;
}

static ucn_cluster_handover_receiver_context_t make_receiver(
    const ucn_cluster_handover_offer_t *offer,
    ucn_cluster_handover_role_t role)
{
    ucn_cluster_handover_receiver_context_t receiver;

    (void)memset(&receiver, 0, sizeof(receiver));
    receiver.local_epoch = offer->epoch;
    receiver.expected_target_epoch = offer->epoch;
    receiver.active_config_id = offer->config_id;
    receiver.active_config_hash = offer->config_hash;
    receiver.available_capacity = offer->available_capacity;
    receiver.capabilities = offer->capabilities;
    receiver.wire_format = offer->wire_format;
    receiver.local_role = (uint8_t)role;
    receiver.confirmed_backup = role == UCN_CLUSTER_HANDOVER_ROLE_BACKUP;
    receiver.backup_policy_compatible = true;
    return receiver;
}

static int test_offer_candidate_hysteresis_and_feasibility(void)
{
    ucn_cluster_epoch_t local = make_epoch(1U, 2U, 1U);
    ucn_cluster_handover_offer_t foreign =
        make_offer(2U, 100U, 2U, 10U, 11U, 1U);
    ucn_cluster_handover_policy_t policy = make_policy();
    ucn_cluster_handover_candidate_table_t table;
    ucn_cluster_handover_candidate_t *candidate = NULL;
    ucn_cluster_handover_feasibility_t feasibility;
    ucn_cluster_handover_candidate_table_t before;

    /* M11-01: foreign term 100 has no authority over local term 2. */
    ASSERT_TRUE(ucn_cluster_handover_offer_classify(
                    &local, &foreign) == UCN_CLUSTER_HANDOVER_OFFER_FOREIGN_MERGE);
    foreign.epoch.cluster_id = 1U;
    ASSERT_TRUE(ucn_cluster_handover_offer_classify(
                    &local, &foreign) ==
                UCN_CLUSTER_HANDOVER_OFFER_SAME_CLUSTER_AUTHORITY);
    foreign.epoch.cluster_id = 2U;

    ucn_cluster_handover_candidate_table_reset(&table);
    ASSERT_TRUE(ucn_cluster_handover_candidate_observe(
                    &table, &local, &foreign, &policy, 30U, &candidate) == UCN_OK);
    ASSERT_TRUE(candidate != NULL && candidate->score_samples == 1U);
    ASSERT_TRUE(!ucn_cluster_handover_candidate_is_eligible(candidate, 800U, 0U,
                                                             &policy, 30U));
    before = table;
    ASSERT_TRUE(ucn_cluster_handover_candidate_observe(
                    &table, &local, &foreign, &policy, 31U, &candidate) ==
                UCN_ERR_REPLAY);
    ASSERT_TRUE(memcmp(&table, &before, sizeof(table)) == 0);
    foreign.nonce = 2U;
    ASSERT_TRUE(ucn_cluster_handover_candidate_observe(
                    &table, &local, &foreign, &policy, 51U, &candidate) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_candidate_is_eligible(candidate, 800U, 0U,
                                                            &policy, 51U));
    ucn_cluster_handover_candidate_note_result(candidate, true, &policy, 51U);
    ASSERT_TRUE(!ucn_cluster_handover_candidate_is_eligible(candidate, 800U, 0U,
                                                             &policy, 60U));
    ucn_cluster_handover_candidate_expire(&table, 200U, 100U);
    ASSERT_TRUE(!table.slots[0].occupied);

    foreign.available_capacity = 0U;
    ASSERT_TRUE(ucn_cluster_handover_feasibility_evaluate(
                    &foreign, 3U, &policy, &feasibility) == UCN_OK &&
                !feasibility.admitted &&
                feasibility.reason == UCN_CLUSTER_HANDOVER_FEASIBILITY_CAPACITY);
    foreign.available_capacity = 8U;
    foreign.capabilities = UCN_CLUSTER_HANDOVER_CAP_BACKUP;
    ASSERT_TRUE(ucn_cluster_handover_feasibility_evaluate(
                    &foreign, 3U, &policy, &feasibility) == UCN_OK &&
                feasibility.reason == UCN_CLUSTER_HANDOVER_FEASIBILITY_CAPABILITIES);
    foreign.capabilities = UCN_CLUSTER_HANDOVER_REQUIRED_CAPABILITIES;
    ASSERT_TRUE(ucn_cluster_handover_feasibility_evaluate(
                    &foreign, 3U, &policy, &feasibility) == UCN_OK &&
                feasibility.admitted);
    return 0;
}

static int test_foreign_handover_order_retry_and_member(void)
{
    ucn_cluster_epoch_t old_epoch = make_epoch(1U, 2U, 1U);
    ucn_cluster_handover_offer_t target =
        make_offer(2U, 100U, 2U, 20U, 21U, 1U);
    ucn_cluster_handover_policy_t policy = make_policy();
    ucn_cluster_handover_receiver_context_t receiver =
        make_receiver(&target, UCN_CLUSTER_HANDOVER_ROLE_HEAD);
    ucn_cluster_handover_transaction_t losing = {0};
    ucn_cluster_handover_transaction_t winning = {0};
    ucn_cluster_handover_message_t prepare;
    ucn_cluster_handover_message_t ready;
    ucn_cluster_handover_message_t stepdown;
    ucn_cluster_handover_message_t commit;
    ucn_cluster_handover_member_result_t member;
    ucn_cluster_handover_member_result_t member_before;
    ucn_cluster_handover_transaction_t before;

    ucn_cluster_handover_transaction_reset(&losing);
    ucn_cluster_handover_transaction_reset(&winning);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 9U, 12U, 41U, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&losing, &prepare) ==
                UCN_OK);
    ASSERT_TRUE(prepare.stepdown_nonce == 0U);
    ASSERT_TRUE(ucn_cluster_handover_transaction_retry_due(&losing, 10U));
    ASSERT_TRUE(ucn_cluster_handover_transaction_note_prepare_retransmitted(
                    &losing, &policy, 10U) == UCN_OK && losing.retry_count == 1U);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 11U, &ready) == UCN_OK);
    ASSERT_TRUE(ready.sender_role == UCN_CLUSTER_HANDOVER_ROLE_HEAD &&
                ready.stepdown_nonce == 0U &&
                winning.state == UCN_CLUSTER_HANDOVER_STATE_READY_SENT);
    /* Duplicate PREPARE is a replay of exactly the same READY, not a new txn. */
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 12U, &commit) == UCN_OK &&
                memcmp(&ready, &commit, sizeof(ready)) == 0);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_ready(&losing, &ready) == UCN_OK &&
                losing.local_authority_active);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_ready(&losing, &ready) == UCN_OK &&
                losing.trace_count == 1U);
    ASSERT_TRUE(ucn_cluster_handover_transaction_revoke_authority(&losing, 51U) == UCN_OK &&
                !losing.local_authority_active);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_stepdown(&losing, &stepdown) ==
                UCN_OK);
    ASSERT_TRUE(stepdown.target_config_id == 0U && stepdown.target_config_hash == 0U);
    ASSERT_TRUE(ucn_cluster_handover_member_accept_stepdown(
                    &old_epoch, 10U, 11U, 1U,
                    UCN_CLUSTER_HANDOVER_ROLE_MEMBER, &stepdown, &member) == UCN_OK &&
                member.join_target && !member.observe_target);
    ASSERT_TRUE(ucn_cluster_handover_member_accept_stepdown(
                    &old_epoch, 10U, 11U, 1U,
                    UCN_CLUSTER_HANDOVER_ROLE_PROVISIONAL, &stepdown, &member) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_member_accept_stepdown(
                    &old_epoch, 10U, 11U, 1U,
                    UCN_CLUSTER_HANDOVER_ROLE_BACKUP, &stepdown, &member) == UCN_OK);
    member_before = member;
    stepdown.target_config_id = 20U;
    ASSERT_TRUE(ucn_cluster_handover_member_accept_stepdown(
                    &old_epoch, 10U, 11U, 1U,
                    UCN_CLUSTER_HANDOVER_ROLE_MEMBER, &stepdown, &member) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&member, &member_before, sizeof(member)) == 0);
    stepdown.target_config_id = 0U;
    ucn_cluster_handover_member_note_target_lost(&member);
    ASSERT_TRUE(!member.join_target && member.observe_target);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_commit(&losing, &commit) == UCN_OK &&
                commit.stepdown_nonce == 0U &&
                ucn_cluster_handover_trace_order_is_valid(&losing));
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_commit(&winning, &commit, 20U) ==
                UCN_OK &&
                !ucn_cluster_handover_transaction_target_authority_ready(&winning));
    ASSERT_TRUE(ucn_cluster_handover_transaction_mark_target_epoch_durable(
                    &winning, &target.epoch, 21U) == UCN_ERR_UNSUPPORTED &&
                !ucn_cluster_handover_transaction_target_authority_ready(&winning));
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_commit(&winning, &commit, 22U) ==
                UCN_OK);
    before = losing;
    ready.sender_role = UCN_CLUSTER_HANDOVER_ROLE_BACKUP;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_ready(&losing, &ready) ==
                UCN_ERR_ACCESS);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0);
    return 0;
}

static int test_same_cluster_planned_transfer_and_timeout(void)
{
    ucn_cluster_epoch_t old_epoch = make_epoch(1U, 9U, 1U);
    ucn_cluster_handover_offer_t target =
        make_offer(1U, 10U, 2U, 10U, 11U, 1U);
    ucn_cluster_handover_policy_t policy = make_policy();
    ucn_cluster_handover_receiver_context_t receiver =
        make_receiver(&target, UCN_CLUSTER_HANDOVER_ROLE_BACKUP);
    ucn_cluster_handover_transaction_t losing = {0};
    ucn_cluster_handover_transaction_t winning = {0};
    ucn_cluster_handover_message_t prepare;
    ucn_cluster_handover_message_t ready;
    ucn_cluster_handover_transaction_t before;

    /* A planned-transfer target is still the old Epoch's Backup while it
     * emits READY.  expected_target_epoch is an admitted proposal only. */
    receiver.local_epoch = old_epoch;
    ucn_cluster_handover_transaction_reset(&losing);
    ucn_cluster_handover_transaction_reset(&winning);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &winning, &old_epoch, &target, &policy, 3U, 99U, 11U, 70U, 0U) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 10U, 11U, 71U, 0U) == UCN_OK);
    ASSERT_TRUE(losing.mode == UCN_CLUSTER_HANDOVER_MODE_SAME_CLUSTER_PLANNED);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&losing, &prepare) == UCN_OK);
    receiver.local_epoch = target.epoch; /* premature self-promotion is invalid */
    before = winning;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 1U, &ready) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&winning, &before, sizeof(winning)) == 0);
    receiver.local_epoch = old_epoch;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 1U, &ready) == UCN_OK &&
                ready.sender_role == UCN_CLUSTER_HANDOVER_ROLE_BACKUP);
    before = winning;
    receiver.confirmed_backup = false;
    prepare.transaction_id++;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 2U, &ready) ==
                UCN_ERR_ACCESS);
    ASSERT_TRUE(memcmp(&winning, &before, sizeof(winning)) == 0);
    ASSERT_TRUE(ucn_cluster_handover_transaction_step(&losing, 100U) == UCN_ERR_STATE &&
                losing.state == UCN_CLUSTER_HANDOVER_STATE_ABORTED &&
                !losing.recovery_observe_required && losing.local_authority_active);
    return 0;
}

static int test_revoked_head_timeout_requires_observe(void)
{
    ucn_cluster_epoch_t old_epoch = make_epoch(1U, 2U, 1U);
    ucn_cluster_handover_offer_t target =
        make_offer(2U, 100U, 2U, 10U, 11U, 1U);
    ucn_cluster_handover_policy_t policy = make_policy();
    ucn_cluster_handover_receiver_context_t receiver =
        make_receiver(&target, UCN_CLUSTER_HANDOVER_ROLE_HEAD);
    ucn_cluster_handover_transaction_t losing = {0};
    ucn_cluster_handover_transaction_t winning = {0};
    ucn_cluster_handover_message_t prepare;
    ucn_cluster_handover_message_t ready;

    ucn_cluster_handover_transaction_reset(&losing);
    ucn_cluster_handover_transaction_reset(&winning);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 9U, 12U, 51U, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&losing, &prepare) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 1U, &ready) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_ready(&losing, &ready) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_revoke_authority(&losing, 61U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_step(&losing, 100U) == UCN_ERR_STATE &&
                losing.state == UCN_CLUSTER_HANDOVER_STATE_ABORTED &&
                losing.recovery_observe_required && !losing.local_authority_active);
    return 0;
}

static int test_r01_wire_identity_and_value_guards(void)
{
    ucn_cluster_epoch_t old_epoch = make_epoch(1U, 2U, 1U);
    ucn_cluster_handover_offer_t target =
        make_offer(2U, 100U, 2U, 20U, 21U, 1U);
    ucn_cluster_handover_policy_t policy = make_policy();
    ucn_cluster_handover_receiver_context_t receiver =
        make_receiver(&target, UCN_CLUSTER_HANDOVER_ROLE_HEAD);
    ucn_cluster_handover_transaction_t losing = {0};
    ucn_cluster_handover_transaction_t winning = {0};
    ucn_cluster_handover_transaction_t before;
    ucn_cluster_handover_message_t prepare;
    ucn_cluster_handover_message_t ready;
    ucn_cluster_handover_message_t output;
    ucn_cluster_handover_message_t output_before;

    ucn_cluster_handover_transaction_reset(&losing);
    ucn_cluster_handover_transaction_reset(&winning);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 9U, 12U, 41U, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&losing, &prepare) == UCN_OK &&
                prepare.stepdown_nonce == 0U);

    (void)memset(&output, 0xA5, sizeof(output));
    output_before = output;
    prepare.stepdown_nonce = 77U;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 1U, &output) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&winning, &(ucn_cluster_handover_transaction_t){0},
                       sizeof(winning)) == 0);
    ASSERT_TRUE(memcmp(&output, &output_before, sizeof(output)) == 0);
    prepare.stepdown_nonce = 0U;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 1U, &ready) == UCN_OK &&
                ready.stepdown_nonce == 0U);

    before = losing;
    ready.stepdown_nonce = 77U;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_ready(&losing, &ready) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0);
    ready.stepdown_nonce = 0U;

    policy.retry_interval_ms = UCN_MAX_SAFE_DURATION_MS + 1U;
    before = losing;
    ASSERT_TRUE(ucn_cluster_handover_transaction_note_prepare_retransmitted(
                    &losing, &policy, 10U) == UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0);
    ucn_cluster_handover_transaction_reset(&winning);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &winning, &old_epoch, &target, &policy, 3U, 9U, 12U, 42U, 0U) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&winning, &(ucn_cluster_handover_transaction_t){0},
                       sizeof(winning)) == 0);

    policy = make_policy();
    target.epoch.term = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    ASSERT_TRUE(ucn_cluster_handover_offer_classify(
                    &old_epoch, &target) == UCN_CLUSTER_HANDOVER_OFFER_INVALID);
    target.epoch.term = 100U;
    target.config_id = UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U;
    ASSERT_TRUE(ucn_cluster_handover_feasibility_evaluate(
                    &target, 3U, &policy, &(ucn_cluster_handover_feasibility_t){0}) ==
                UCN_ERR_ARGUMENT);
    target.config_id = 20U;
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &winning, &old_epoch, &target, &policy, 3U, 9U, 12U,
                    UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD + 1U, 0U) == UCN_ERR_ARGUMENT);

    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_ready(&losing, &ready) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_revoke_authority(&losing, 88U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_stepdown(&losing, &output) == UCN_OK &&
                output.stepdown_nonce == 88U);
    return 0;
}

static int test_r03_target_commit_timeout_and_no_fake_durability(void)
{
    ucn_cluster_epoch_t old_epoch = make_epoch(1U, 2U, 1U);
    ucn_cluster_handover_offer_t target =
        make_offer(2U, 100U, 2U, 20U, 21U, 1U);
    ucn_cluster_handover_policy_t policy = make_policy();
    ucn_cluster_handover_receiver_context_t receiver =
        make_receiver(&target, UCN_CLUSTER_HANDOVER_ROLE_HEAD);
    ucn_cluster_handover_transaction_t losing = {0};
    ucn_cluster_handover_transaction_t winning = {0};
    ucn_cluster_handover_transaction_t before;
    ucn_cluster_handover_message_t prepare;
    ucn_cluster_handover_message_t ready;
    ucn_cluster_handover_message_t commit;

    ucn_cluster_handover_transaction_reset(&losing);
    ucn_cluster_handover_transaction_reset(&winning);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 9U, 12U, 51U, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&losing, &prepare) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 1U, &ready) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_ready(&losing, &ready) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_revoke_authority(&losing, 61U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_stepdown(&losing, &prepare) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_commit(&losing, &commit) == UCN_OK);
    before = winning;
    commit.stepdown_nonce = 71U;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_commit(&winning, &commit, 20U) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&winning, &before, sizeof(winning)) == 0);
    commit.stepdown_nonce = 0U;
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_commit(&winning, &commit, 20U) == UCN_OK &&
                winning.state == UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED);
    before = winning;
    ASSERT_TRUE(ucn_cluster_handover_transaction_mark_target_epoch_durable(
                    &winning, &target.epoch, 21U) == UCN_ERR_UNSUPPORTED);
    ASSERT_TRUE(memcmp(&winning, &before, sizeof(winning)) == 0 &&
                !ucn_cluster_handover_transaction_target_authority_ready(&winning));
    ASSERT_TRUE(ucn_cluster_handover_transaction_step(&winning, 101U) == UCN_ERR_STATE &&
                winning.state == UCN_CLUSTER_HANDOVER_STATE_ABORTED &&
                winning.recovery_observe_required);
    before = winning;
    ASSERT_TRUE(ucn_cluster_handover_transaction_mark_target_epoch_durable(
                    &winning, &target.epoch, 102U) == UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&winning, &before, sizeof(winning)) == 0);
    return 0;
}

static int test_r05_corrupt_public_transaction_is_fail_closed(void)
{
    ucn_cluster_epoch_t old_epoch = make_epoch(1U, 2U, 1U);
    ucn_cluster_handover_offer_t target =
        make_offer(2U, 100U, 2U, 20U, 21U, 1U);
    ucn_cluster_handover_policy_t policy = make_policy();
    ucn_cluster_handover_transaction_t transaction = {0};
    ucn_cluster_handover_transaction_t fenced_transaction = {0};
    ucn_cluster_handover_transaction_t before;
    ucn_cluster_handover_message_t output;
    ucn_cluster_handover_message_t output_before;

    ucn_cluster_handover_transaction_reset(&transaction);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &transaction, &old_epoch, &target, &policy, 3U, 9U, 12U, 71U, 0U) == UCN_OK);
    transaction.trace_count = (uint8_t)(UCN_CLUSTER_HANDOVER_TRACE_CAPACITY + 1U);
    before = transaction;
    (void)memset(&output, 0xA5, sizeof(output));
    output_before = output;
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&transaction, &output) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ASSERT_TRUE(memcmp(&output, &output_before, sizeof(output)) == 0);
    ASSERT_TRUE(!ucn_cluster_handover_trace_order_is_valid(&transaction));
    ASSERT_TRUE(ucn_cluster_handover_transaction_step(&transaction, 1U) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);

    ucn_cluster_handover_transaction_reset(&fenced_transaction);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &fenced_transaction, &old_epoch, &target, &policy, 3U, 9U, 12U, 711U, 0U) ==
                UCN_OK);
    fenced_transaction.authority_reentry_fence = UINT32_C(0xA5A5A5A5);
    before = fenced_transaction;
    ucn_cluster_handover_transaction_reset(&fenced_transaction);
    ASSERT_TRUE(memcmp(&fenced_transaction, &before, sizeof(fenced_transaction)) == 0);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &fenced_transaction, &old_epoch, &target, &policy, 3U, 9U, 12U,
                    712U, 1U) == UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&fenced_transaction, &before, sizeof(fenced_transaction)) == 0);
    (void)memset(&output, 0x33, sizeof(output));
    output_before = output;
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&fenced_transaction, &output) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&fenced_transaction, &before, sizeof(fenced_transaction)) == 0);
    ASSERT_TRUE(memcmp(&output, &output_before, sizeof(output)) == 0);

    ucn_cluster_handover_transaction_reset(&transaction);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &transaction, &old_epoch, &target, &policy, 3U, 9U, 12U, 72U, 0U) == UCN_OK);
    transaction.trace_count = 1U;
    transaction.trace[0] = UCN_CLUSTER_HANDOVER_TRACE_NONE;
    before = transaction;
    (void)memset(&output, 0x5A, sizeof(output));
    output_before = output;
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&transaction, &output) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    ASSERT_TRUE(memcmp(&output, &output_before, sizeof(output)) == 0);
    ASSERT_TRUE(!ucn_cluster_handover_trace_order_is_valid(&transaction));

    ucn_cluster_handover_transaction_reset(&transaction);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &transaction, &old_epoch, &target, &policy, 3U, 9U, 12U, 73U, 0U) == UCN_OK);
    transaction.state = 0xA5U;
    before = transaction;
    ASSERT_TRUE(ucn_cluster_handover_transaction_step(&transaction, 1U) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&transaction, &before, sizeof(transaction)) == 0);
    return 0;
}

static int test_r06_begin_cannot_reopen_revoked_transaction(void)
{
    ucn_cluster_epoch_t old_epoch = make_epoch(1U, 2U, 1U);
    ucn_cluster_handover_offer_t target =
        make_offer(2U, 100U, 2U, 20U, 21U, 1U);
    ucn_cluster_handover_policy_t policy = make_policy();
    ucn_cluster_handover_receiver_context_t receiver =
        make_receiver(&target, UCN_CLUSTER_HANDOVER_ROLE_HEAD);
    ucn_cluster_handover_transaction_t losing = {0};
    ucn_cluster_handover_transaction_t winning = {0};
    ucn_cluster_handover_transaction_t before;
    ucn_cluster_handover_message_t prepare;
    ucn_cluster_handover_message_t ready;
    ucn_cluster_handover_message_t scratch;

    ucn_cluster_handover_transaction_reset(&losing);
    ucn_cluster_handover_transaction_reset(&winning);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 9U, 12U, 81U, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_build_prepare(&losing, &prepare) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_prepare(
                    &winning, &receiver, &prepare, &policy, 3U, 1U, &ready) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_accept_ready(&losing, &ready) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_handover_transaction_revoke_authority(&losing, 91U) == UCN_OK);

    before = losing;
    ucn_cluster_handover_transaction_reset(&losing);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 9U, 12U, 82U, 2U) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0 &&
                !losing.local_authority_active &&
                losing.state == UCN_CLUSTER_HANDOVER_STATE_AUTHORITY_REVOKED);

    ASSERT_TRUE(ucn_cluster_handover_transaction_build_stepdown(&losing, &scratch) == UCN_OK);
    before = losing;
    ucn_cluster_handover_transaction_reset(&losing);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 9U, 12U, 83U, 3U) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0 &&
                !losing.local_authority_active &&
                losing.state == UCN_CLUSTER_HANDOVER_STATE_STEPDOWN_SENT);

    ASSERT_TRUE(ucn_cluster_handover_transaction_build_commit(&losing, &scratch) == UCN_OK);
    before = losing;
    ucn_cluster_handover_transaction_reset(&losing);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0);
    ASSERT_TRUE(ucn_cluster_handover_transaction_begin(
                    &losing, &old_epoch, &target, &policy, 3U, 9U, 12U, 84U, 4U) ==
                UCN_ERR_STATE);
    ASSERT_TRUE(memcmp(&losing, &before, sizeof(losing)) == 0 &&
                !losing.local_authority_active &&
                losing.state == UCN_CLUSTER_HANDOVER_STATE_COMMIT_SENT);
    return 0;
}

int main(void)
{
    ASSERT_TRUE(test_offer_candidate_hysteresis_and_feasibility() == 0);
    ASSERT_TRUE(test_foreign_handover_order_retry_and_member() == 0);
    ASSERT_TRUE(test_same_cluster_planned_transfer_and_timeout() == 0);
    ASSERT_TRUE(test_revoked_head_timeout_requires_observe() == 0);
    ASSERT_TRUE(test_r01_wire_identity_and_value_guards() == 0);
    ASSERT_TRUE(test_r03_target_commit_timeout_and_no_fake_durability() == 0);
    ASSERT_TRUE(test_r05_corrupt_public_transaction_is_fail_closed() == 0);
    ASSERT_TRUE(test_r06_begin_cannot_reopen_revoked_transaction() == 0);
    (void)puts("cluster handover tests passed");
    return 0;
}
