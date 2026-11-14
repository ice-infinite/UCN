#ifndef NATIVE_JOIN_FIXTURE_H
#define NATIVE_JOIN_FIXTURE_H

/* Host-only real-Owner fixture; test keys/media/Driver are not product crypto. */
#include "fake_persistence_provider.h"
#include "internal/ucn_identity.h"
#include "internal/ucn_security.h"
#include "internal/ucn_persistence.h"
#include "internal/ucn_native_join.h"

typedef struct native_join_fixture_lock { uint8_t held; } native_join_fixture_lock_t;
typedef struct native_join_fixture {
    ucn_persist_manifest_entry_t entries[3];
    ucn_persist_domain_binding_t bindings[3];
    ucn_persist_manifest_t manifest;
    fake_persist_provider_t fake;
    ucn_persistence_provider_t provider;
    ucn_persistence_config_t persistence_config;
    ucn_persist_gate_storage_t gate_storage;
    ucn_persistence_digest_workspace_t digest_workspace;
    ucn_persistence_storage_t storage;
    ucn_persist_callback_gate_t *gate;
    ucn_persistence_owner_t *persistence;
    ucn_i_identity_owner_t identity;
    ucn_i_native_join_t join;
    ucn_i_native_join_config_t config;
    ucn_i_adapter_t adapter;
    ucn_i_admission_owner_t admission;
    ucn_i_security_owner_t security;
    /* Optional test product Session verifier, installed in product context,
     * never by mutating the live Security Owner's provider or ACL. */
    ucn_i_security_verify_session_fn additional_verify_session;
    void *additional_verify_context;
    ucn_i_callback_gate_t callback_gate;
    ucn_i_security_domain_rule_t security_rule;
    ucn_i_security_acl_rule_t security_acl;
    ucn_i_admission_hello_t issued_hello;
    ucn_i_admission_link_t issued_link;
    uint8_t issued_cookie[16];
    uint8_t captured[UCN_ADAPTER_FRAME_BYTES];
    size_t captured_bytes;
    uint32_t submit_calls;
    ucn_result_t cancel_result;
    uint8_t driver_mode; /* 0 complete, 1 not submitted, 2 unknown */
    uint8_t sign_corrupt_event;
    ucn_i_identity_domain_rule_t identity_rules[2];
    ucn_i_identity_handle_t authority_binding;
    ucn_i_identity_binding_view_t authority_binding_view;
    ucn_i_identity_binding_view_t checked_device_binding;
    ucn_i_identity_authority_view_t authority;
    ucn_i_identity_binding_issue_t issue;
    ucn_i_identity_durability_base_t identity_base;
    ucn_i_identity_requirement_view_t requirement;
    ucn_persistence_request_t request;
    ucn_handle_t persistence_handle;
    ucn_persistence_proof_t foundation_proof;
    ucn_i_identity_durability_proof_t identity_proof;
    ucn_i_persist_record_meta_t meta;
    ucn_i_persist_codec_workspace_t codec;
    ucn_i_admission_transcript_t final_transcript;
    ucn_i_security_acl_rule_t session_acl_rules[2];
    ucn_i_admission_evidence_t final_evidence;
    ucn_i_admission_evidence_t encoding_evidence;
    ucn_i_admission_seed_workspace_t seed_workspace;
    ucn_i_admission_session_seed_t seed;
    ucn_i_sha256_workspace_t hash_workspace;
    uint8_t hash_input[64U + 1U + UCN_I_ADMISSION_TRANSCRIPT_BYTES];
    uint8_t packet_hash_input[2U * UCN_ADAPTER_FRAME_BYTES + 256U];
    uint8_t hash_inner[32];
    uint8_t logical[UCN_I_ADMISSION_LOGICAL_MAX_BYTES];
    uint8_t final_valid, device;
    uint8_t published_digest[16];
    native_join_fixture_lock_t owner_lock, gate_lock, identity_lock;
    native_join_fixture_lock_t adapter_lock, driver_lock, callback_lock, admission_lock, security_lock;
} native_join_fixture_t;

/* Foundation-only setup; full init also provisions the Authority address Binding
 * and configures real Owners/providers, but leaves JOIN init to the caller. */
ucn_result_t native_join_fixture_foundation_init(native_join_fixture_t *f);
ucn_result_t native_join_fixture_init(native_join_fixture_t *f, bool device);
ucn_result_t native_join_fixture_init_with_mtu(native_join_fixture_t *f, bool device, uint16_t mtu);
ucn_result_t native_join_fixture_provision_authority_binding(native_join_fixture_t *f);
ucn_result_t native_join_fixture_sign_event(void *context, ucn_i_admission_event_t event,
    const ucn_i_admission_transcript_t *previous, ucn_i_admission_transcript_t *next,
    ucn_i_admission_evidence_t *evidence);
ucn_result_t native_join_fixture_authorize_event(void *context, ucn_i_admission_event_t event,
    const ucn_i_admission_key_t *key, const ucn_i_admission_transcript_t *transcript,
    uint64_t now_us, const ucn_i_admission_evidence_t *evidence);
ucn_result_t native_join_fixture_verify_session(void *context,
    const ucn_i_security_candidate_t *candidate, const uint8_t *proof, size_t proof_bytes);
#endif
