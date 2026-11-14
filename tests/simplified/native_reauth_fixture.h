#ifndef NATIVE_REAUTH_FIXTURE_H
#define NATIVE_REAUTH_FIXTURE_H
#include "native_join_fixture.h"
#include "internal/ucn_native_reauth.h"
typedef struct native_reauth_fixture {
    native_join_fixture_t *prior;
    ucn_i_native_reauth_t *root;
    ucn_i_native_reauth_config_t config;
    ucn_i_security_acl_rule_t rules[8][2];
    ucn_i_identity_binding_view_t local, peer;
    ucn_i_peer_reauth_record_t final_record, decoded;
    ucn_i_sha256_workspace_t hash;
    uint8_t input[291], inner[32], message[227], digest[32];
    uint32_t sign_calls[4];
    uint8_t final_valid, corrupt_stage;
} native_reauth_fixture_t;
/* Does not init the root or mutate live Owner policy. Requires actual JOIN and
 * fenced prior Session, supplied by the caller's real raw pair fixture. */
void native_reauth_fixture_configure(native_reauth_fixture_t *, native_join_fixture_t *,
    ucn_i_native_reauth_t *, bool initiator, uint64_t nonce, uint64_t transaction);
#endif
