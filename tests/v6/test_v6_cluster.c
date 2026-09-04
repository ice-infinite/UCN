#include "ucn/v6/ucn_v6_cluster.h"

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
    bool valid;
    bool fail_submit;
    uint8_t bytes[UCN_V6_CLUSTER_RECORD_BYTES];
    unsigned loads;
    unsigned submits;
    ucn_v6_cluster_owner_t *reenter_owner;
    ucn_v6_result_t reenter_result;
} fake_store_t;

static void gate_lock(void *context)
{
    (void)context;
}

static void gate_unlock(void *context)
{
    (void)context;
}

static ucn_v6_result_t store_load(void *context, uint8_t *record,
                                  size_t capacity, size_t *length)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->loads;
    if (!store->valid) return UCN_V6_ERR_NOT_FOUND;
    if (capacity < sizeof(store->bytes)) return UCN_V6_ERR_NO_SPACE;
    memcpy(record, store->bytes, sizeof(store->bytes));
    *length = sizeof(store->bytes);
    return UCN_V6_OK;
}

static ucn_v6_result_t store_submit(void *context, const uint8_t *record,
                                    size_t length)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->submits;
    if (store->reenter_owner != NULL) {
        store->reenter_result =
            ucn_v6_cluster_step(store->reenter_owner, 0U);
    }
    if (store->fail_submit) return UCN_V6_ERR_STATE;
    if (length != sizeof(store->bytes)) return UCN_V6_ERR_MALFORMED;
    memcpy(store->bytes, record, sizeof(store->bytes));
    store->valid = true;
    return UCN_V6_OK;
}

static ucn_v6_principal_t principal(uint8_t seed)
{
    ucn_v6_principal_t value;
    size_t index;
    for (index = 0U; index < sizeof(value.bytes); ++index) {
        value.bytes[index] = (uint8_t)(seed + index);
    }
    return value;
}

static ucn_v6_binding_key_t binding(uint32_t address)
{
    ucn_v6_binding_key_t value = { 1U, address, 1U };
    return value;
}

static ucn_v6_cluster_voter_t voter(uint8_t seed, uint32_t address)
{
    ucn_v6_cluster_voter_t value;
    memset(&value, 0, sizeof(value));
    value.principal = principal(seed);
    value.binding = binding(address);
    return value;
}

static ucn_v6_cluster_config_t config3(void)
{
    ucn_v6_cluster_config_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.config_id = 1U;
    value.generation = 1U;
    value.voter_count = 3U;
    value.voters[0] = voter(0x10U, 1U);
    value.voters[1] = voter(0x20U, 2U);
    value.voters[2] = voter(0x30U, 3U);
    return value;
}

static ucn_v6_security_open_result_t opened(uint8_t seed, uint32_t address)
{
    ucn_v6_security_open_result_t value;
    memset(&value, 0, sizeof(value));
    value.authenticated_principal = principal(seed);
    value.ingress_peer_session.principal = value.authenticated_principal;
    value.ingress_peer_session.binding = binding(address);
    value.ingress_peer_session.session_generation = 1U;
    value.frame.flags = UCN_V6_FLAG_E2E_CONTEXT |
                        UCN_V6_FLAG_PEER_HOP_CONTEXT;
    value.hop_authenticated = true;
    value.endpoint_authorized = true;
    return value;
}

static ucn_v6_cached_peer_capability_t capability(uint8_t seed,
                                                   uint32_t address)
{
    ucn_v6_cached_peer_capability_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.principal = principal(seed);
    value.binding = binding(address);
    value.session_generation = 1U;
    value.record.capability_generation = 1U;
    value.record.peer.feature_bits = UCN_V6_FEATURE_CLUSTER;
    return value;
}

static ucn_v6_cluster_control_t transition_control(
    const ucn_v6_cluster_snapshot_t *snapshot,
    ucn_v6_cluster_control_kind_t kind)
{
    ucn_v6_cluster_control_t value;
    memset(&value, 0, sizeof(value));
    value.kind = kind;
    value.transaction_id = snapshot->transition.transaction_id;
    value.old_epoch = snapshot->transition.old_epoch;
    value.target_epoch = snapshot->transition.target_epoch;
    value.config_id = snapshot->transition.target_config_id;
    value.config_generation = snapshot->transition.target_config_generation;
    if (snapshot->last_vote.valid) {
        value.backup_generation = snapshot->last_vote.backup_generation;
    }
    return value;
}

static ucn_v6_result_t init_owner(
    ucn_v6_cluster_owner_storage_t *storage, fake_store_t *fake,
    ucn_v6_callback_gate_t *gate, uint8_t seed, uint32_t address,
    ucn_v6_cluster_owner_t **owner)
{
    ucn_v6_cluster_store_ops_t ops;
    ucn_v6_principal_t local = principal(seed);
    ucn_v6_binding_key_t local_binding = binding(address);
    memset(&ops, 0, sizeof(ops));
    ops.context = fake;
    ops.load = store_load;
    ops.submit = store_submit;
    return ucn_v6_cluster_owner_init_in_place(
        storage, sizeof(*storage), ucn_v6_compiled_manifest(), &local,
        &local_binding, 1U, &ops, gate, owner);
}

static int test_record_and_control_codec(void)
{
    ucn_v6_cluster_snapshot_t snapshot;
    ucn_v6_cluster_snapshot_t decoded;
    ucn_v6_cluster_snapshot_t scratch;
    ucn_v6_cluster_control_t control;
    ucn_v6_cluster_control_t control_decoded;
    uint8_t bytes[UCN_V6_CLUSTER_RECORD_BYTES];
    uint8_t control_bytes[UCN_V6_CLUSTER_CONTROL_BYTES];
    uint8_t before[UCN_V6_CLUSTER_RECORD_BYTES];
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.record_generation = 1U;
    snapshot.boot_incarnation = 1U;
    snapshot.role = UCN_V6_CLUSTER_OBSERVER;
    snapshot.phase = UCN_V6_CLUSTER_PHASE_STABLE;
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_snapshot_decode(bytes, sizeof(bytes), &scratch,
                                         &decoded) ==
          UCN_V6_OK);
    CHECK(decoded.record_generation == 1U && decoded.boot_incarnation == 1U);
    snapshot = decoded;
    CHECK(ucn_v6_cluster_snapshot_decode(bytes, sizeof(bytes), &snapshot,
                                         &snapshot) ==
          UCN_V6_ERR_MALFORMED);
    CHECK(memcmp(&snapshot, &decoded, sizeof(snapshot)) == 0);
    bytes[17] ^= 1U;
    CHECK(ucn_v6_cluster_snapshot_decode(bytes, sizeof(bytes), &scratch,
                                         &decoded) ==
          UCN_V6_ERR_MALFORMED);
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) == UCN_V6_OK);
    bytes[4] = 0U;
    bytes[5] = 0U;
    {
        uint32_t crc = ucn_v6_crc32c(bytes,
            UCN_V6_CLUSTER_RECORD_BYTES - 4U);
        bytes[UCN_V6_CLUSTER_RECORD_BYTES - 4U] = (uint8_t)(crc >> 24U);
        bytes[UCN_V6_CLUSTER_RECORD_BYTES - 3U] = (uint8_t)(crc >> 16U);
        bytes[UCN_V6_CLUSTER_RECORD_BYTES - 2U] = (uint8_t)(crc >> 8U);
        bytes[UCN_V6_CLUSTER_RECORD_BYTES - 1U] = (uint8_t)crc;
    }
    memset(&decoded, 0xA5, sizeof(decoded));
    CHECK(ucn_v6_cluster_snapshot_decode(bytes, sizeof(bytes), &scratch,
                                         &decoded) ==
          UCN_V6_ERR_MALFORMED);
    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    snapshot.record_generation = 0U;
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);

    memset(&control, 0, sizeof(control));
    control.kind = UCN_V6_CLUSTER_CTL_HANDOVER_COMMIT;
    control.transaction_id = 7U;
    control.old_epoch.cluster_id = 1U;
    control.old_epoch.term = 2U;
    control.old_epoch.head_principal = principal(0x10U);
    control.old_epoch.head_binding = binding(1U);
    control.target_epoch.cluster_id = 1U;
    control.target_epoch.term = 3U;
    control.target_epoch.head_principal = principal(0x20U);
    control.target_epoch.head_binding = binding(2U);
    control.config_id = 1U;
    control.config_generation = 1U;
    CHECK(ucn_v6_cluster_control_encode(&control, control_bytes) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_control_decode(control_bytes, sizeof(control_bytes),
                                        &control_decoded) == UCN_V6_OK);
    CHECK(control_decoded.kind == control.kind &&
          control_decoded.transaction_id == control.transaction_id &&
          control_decoded.target_epoch.term == 3U);
    control_bytes[80] ^= 1U;
    CHECK(ucn_v6_cluster_control_decode(control_bytes, sizeof(control_bytes),
                                        &control_decoded) ==
          UCN_V6_ERR_MALFORMED);
    return 0;
}

static int test_joint_authority_directory_and_tunnel(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate;
    fake_store_t fake;
    ucn_v6_cluster_config_t old_config = config3();
    ucn_v6_cluster_config_t new_config = old_config;
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_security_open_result_t open3 = opened(0x30U, 3U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap3 = capability(0x30U, 3U);
    ucn_v6_cluster_view_t view;
    ucn_v6_cluster_directory_entry_t directory;
    ucn_v6_cluster_tunnel_t tunnel;
    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(fake.valid && fake.submits == 1U);
    CHECK(ucn_v6_cluster_create(owner, 1U, &old_config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 0U, &view) == UCN_V6_OK);
    CHECK(!view.authority_active && !view.quorum_met);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 1U, &view) == UCN_V6_OK);
    CHECK(view.authority_active && view.quorum_met);
    CHECK(ucn_v6_cluster_admit_member(owner, &open3, &cap3, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_assign_backup(owner, &old_config.voters[1], 1U, 1U) ==
          UCN_V6_OK);
    new_config.config_id = 2U;
    new_config.generation = 2U;
    CHECK(ucn_v6_cluster_prepare_joint(owner, 100U, &new_config, 2U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_joint(owner, 100U, 2U) == UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_backup_ack_config(owner, &open2, 100U, 2U, 2U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_joint(owner, 100U, 2U) == UCN_V6_OK);
    new_config.config_id = 3U;
    new_config.generation = 3U;
    CHECK(ucn_v6_cluster_prepare_joint(owner, 101U, &new_config, 3U) ==
          UCN_V6_OK);
    {
        ucn_v6_cluster_config_t wrong = new_config;
        wrong.config_id = 4U;
        CHECK(ucn_v6_cluster_abort_joint(owner, 101U, &wrong, 3U) ==
              UCN_V6_ERR_STATE);
    }
    CHECK(ucn_v6_cluster_abort_joint(owner, 101U, &new_config, 3U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_prepare_joint(owner, 101U, &new_config, 3U) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_cluster_assign_backup(owner, &old_config.voters[1], 2U,
                                       101U) == UCN_V6_ERR_STATE);

    memset(&directory, 0, sizeof(directory));
    directory.occupied = true;
    directory.remote_cluster_id = 9U;
    directory.remote_epoch.cluster_id = 9U;
    directory.remote_epoch.term = 1U;
    directory.remote_epoch.head_principal = open2.authenticated_principal;
    directory.remote_epoch.head_binding = open2.ingress_peer_session.binding;
    directory.next_hop = open2.ingress_peer_session;
    directory.route_generation = 1U;
    directory.path_id = 1U;
    directory.path_generation = 1U;
    directory.deadline_us = 50U;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &directory, 2U) ==
          UCN_V6_OK);
    memset(&tunnel, 0, sizeof(tunnel));
    tunnel.occupied = true;
    tunnel.tunnel_id = 1U;
    tunnel.source_cluster_id = 1U;
    tunnel.destination_cluster_id = 9U;
    tunnel.route_domain.origin_principal = principal(0x10U);
    tunnel.route_domain.origin_binding = binding(1U);
    tunnel.route_domain.origin_session_generation = 1U;
    tunnel.route_domain.destination_principal = principal(0x20U);
    tunnel.route_domain.destination_binding = binding(2U);
    tunnel.path.valid = true;
    tunnel.path.destination_principal = principal(0x20U);
    tunnel.path.destination_binding = binding(2U);
    tunnel.path.session_generation = 1U;
    tunnel.path.route_generation = 1U;
    tunnel.path.path_id = 1U;
    tunnel.path.path_generation = 1U;
    tunnel.path.payload_budget = 256U;
    tunnel.path.fragment_data_budget = 128U;
    tunnel.path.deadline_us = 50U;
    tunnel.deadline_us = 50U;
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, 2U) == UCN_V6_OK);
    {
        ucn_v6_cluster_tunnel_t copied;
        memset(&copied, 0, sizeof(copied));
        CHECK(ucn_v6_cluster_copy_tunnel(owner, 1U, 2U, &copied) == UCN_V6_OK);
        CHECK(copied.path.fragment_data_budget == 128U &&
              copied.route_domain.destination_binding.node_address == 2U);
    }
    CHECK(ucn_v6_cluster_copy_view(owner, 2U, &view) == UCN_V6_OK);
    CHECK(view.directory_entries == 1U && view.tunnels == 1U);
    CHECK(ucn_v6_cluster_step(owner, 101U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 101U, &view) == UCN_V6_OK);
    CHECK(!view.authority_active && view.members == 0U &&
          view.directory_entries == 0U && view.tunnels == 0U);
    return 0;
}

static int test_takeover_recovery_rekey_and_handover(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_storage_t reload_storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_cluster_owner_t *reloaded = NULL;
    ucn_v6_callback_gate_t gate;
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open1 = opened(0x10U, 1U);
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_security_open_result_t open3 = opened(0x30U, 3U);
    ucn_v6_cached_peer_capability_t cap1 = capability(0x10U, 1U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap3 = capability(0x30U, 3U);
    ucn_v6_cluster_snapshot_t snapshot;
    ucn_v6_cluster_epoch_t target;
    ucn_v6_cluster_control_t message;
    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x20U, 2U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open1, &cap1, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open3, &cap3, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_assign_backup(owner, &config.voters[0], 1U, 1U) ==
          UCN_V6_OK);
    /* Install a durable self-Backup state exactly as a remote Head would. */
    CHECK(ucn_v6_cluster_copy_snapshot(owner, &snapshot) == UCN_V6_OK);
    snapshot.role = UCN_V6_CLUSTER_BACKUP;
    snapshot.active_epoch.head_principal = principal(0x10U);
    snapshot.active_epoch.head_binding = binding(1U);
    snapshot.max_epoch = snapshot.active_epoch;
    snapshot.backup.principal = principal(0x20U);
    snapshot.backup.binding = binding(2U);
    snapshot.backup.generation = 2U;
    snapshot.record_generation++;
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, fake.bytes) == UCN_V6_OK);
    memset(&gate, 0, sizeof(gate));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&reload_storage, &fake, &gate, 0x20U, 2U, &reloaded) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(reloaded, &open1, &cap1, 1U, 10U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_begin_takeover(reloaded, 200U, 2U, 2U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_begin_takeover(reloaded, 200U, 2U, 200U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    message = transition_control(&snapshot, UCN_V6_CLUSTER_CTL_TAKEOVER_VOTE);
    {
        ucn_v6_cluster_snapshot_t before_vote = snapshot;
        ucn_v6_cluster_snapshot_t after_vote;
        ++message.target_epoch.term;
        CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open1,
                                                    &message) ==
              UCN_V6_ERR_ACCESS);
        CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &after_vote) == UCN_V6_OK);
        CHECK(memcmp(&before_vote, &after_vote, sizeof(before_vote)) == 0);
        --message.target_epoch.term;
    }
    CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open1, &message) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_takeover(reloaded, 200U, 200U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.role == UCN_V6_CLUSTER_HEAD &&
          snapshot.active_epoch.term == 2U);

    CHECK(ucn_v6_cluster_admit_member(reloaded, &open1, &cap1, 201U, 10U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_rekey(reloaded, 201U, 2U, 201U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.active_epoch.cluster_id == 2U &&
          snapshot.tombstone_count == 1U &&
          ucn_v6_cluster_rekey(reloaded, 202U, 1U, 201U) == UCN_V6_ERR_REPLAY);

    target = snapshot.active_epoch;
    target.term = 2U;
    target.head_principal = principal(0x10U);
    target.head_binding = binding(1U);
    CHECK(ucn_v6_cluster_begin_handover(
              reloaded, 300U, &target, &snapshot.stable_config, 201U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    message = transition_control(&snapshot, UCN_V6_CLUSTER_CTL_HANDOVER_READY);
    ++message.old_epoch.term;
    CHECK(ucn_v6_cluster_handover_ready(reloaded, &open1, &message) ==
          UCN_V6_ERR_ACCESS);
    --message.old_epoch.term;
    CHECK(ucn_v6_cluster_handover_ready(reloaded, &open1, &message) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_handover(reloaded, 300U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.role == UCN_V6_CLUSTER_FENCED &&
          snapshot.authority_fenced);

    target.cluster_id = 3U;
    target.term = 1U;
    target.head_principal = principal(0x20U);
    target.head_binding = binding(2U);
    CHECK(ucn_v6_cluster_begin_recovery(reloaded, 400U, &target, 400U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    message = transition_control(&snapshot, UCN_V6_CLUSTER_CTL_RECOVERY_VOTE);
    CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open1, &message) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open3, &message) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_recovery(reloaded, 400U, 400U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.active_epoch.cluster_id == 3U &&
          snapshot.role == UCN_V6_CLUSTER_HEAD &&
          !snapshot.authority_fenced && snapshot.tombstone_count == 2U);
    (void)open2;
    (void)cap2;
    return 0;
}

static int test_callback_reentry_and_failure_close(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate;
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cluster_snapshot_t before;
    ucn_v6_cluster_snapshot_t after;
    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    fake.reenter_owner = owner;
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(fake.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 0U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(owner, &before) == UCN_V6_OK);
    fake.fail_submit = true;
    CHECK(ucn_v6_cluster_rekey(owner, 9U, 2U, 0U) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_cluster_copy_snapshot(owner, &after) == UCN_V6_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    return 0;
}

int main(void)
{
    CHECK(test_record_and_control_codec() == 0);
    CHECK(test_joint_authority_directory_and_tunnel() == 0);
    CHECK(test_takeover_recovery_rekey_and_handover() == 0);
    CHECK(test_callback_reentry_and_failure_close() == 0);
    puts("ucn v6 cluster tests passed");
    return 0;
}
