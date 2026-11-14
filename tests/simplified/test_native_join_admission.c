/* Tests the real Admission owner, not a simulated JOIN success. */
#define main admission_existing_regression_main
#include "test_admission.c"
#undef main

/* Baseline adapter allows the pre-extension implementation to execute and
 * expose its late-start behavior. Only used by the recorded RED command. */
#ifdef UCN_JOIN_EARLY_START_RED
#define open_started(o,c,t,s,n,h) \
    ((void)(s), ucn_i_admission_open_after_cookie(o,c,t,n,h))
#else
#define open_started ucn_i_admission_open_after_cookie_started
#endif

int main(void)
{
    static ucn_i_admission_owner_t owner;
    static ucn_i_callback_gate_t gate;
    ucn_i_admission_config_t cfg = {0};
    test_lock_t state = {0}, gate_state = {0};
    test_provider_t provider = {0};
    ucn_i_lock_ops_t gate_ops = lock_ops(&gate_state);
    ucn_i_admission_hello_cookie_t cookie = make_hello_cookie();
    ucn_i_admission_transcript_t t = make_cookie_transcript();
    ucn_i_admission_handle_t h, duplicate, sentinel;
    ucn_i_admission_pending_view_t view;
    CHECK(ucn_i_callback_gate_init(&gate, 55U, &gate_ops) == UCN_OK);
    cfg.struct_size = sizeof(cfg); cfg.api_version = UCN_API_VERSION;
    cfg.runtime_instance = 10U; cfg.realm_id = 42U; cfg.owner_instance = 20U;
    cfg.identity_owner_instance = 21U; cfg.persistence_owner_instance = 22U;
    cfg.max_pending_per_link = 1U; cfg.token_burst = 4U;
    cfg.tokens_per_second = 2U; cfg.pending_timeout_us = 5000U;
    cfg.provider.struct_size = sizeof(cfg.provider); cfg.provider.api_version = 1U;
    cfg.provider.context = &provider; cfg.provider.issue_cookie = issue_cookie;
    cfg.provider.verify_cookie = verify_cookie; cfg.provider.authorize_event = authorize_event;
    cfg.state_lock = lock_ops(&state); cfg.provider_gate = &gate;
    CHECK(ucn_i_admission_owner_init(&owner, &cfg) == UCN_OK);
    CHECK(open_started(&owner, &cookie, &t, 100U, 500U, &h) == UCN_OK);
    CHECK(ucn_i_admission_pending_get(&owner, h, &view) == UCN_OK);
    CHECK(view.challenge_started_local_us == 100U);
    CHECK(view.deadline_us == 5100U);
    CHECK(open_started(&owner, &cookie, &t, 100U, 600U, &duplicate) == UCN_OK);
    CHECK(h.runtime_instance == duplicate.runtime_instance &&
          h.slot_generation == duplicate.slot_generation &&
          h.transaction_id == duplicate.transaction_id &&
          h.owner_instance == duplicate.owner_instance && h.slot == duplicate.slot);
    memset(&sentinel, 0xA5, sizeof(sentinel));
    memcpy(&duplicate, &sentinel, sizeof(duplicate));
    CHECK(open_started(&owner, &cookie, &t, 101U, 600U, &duplicate) == UCN_ERR_REPLAY);
    CHECK(memcmp(&duplicate, &sentinel, sizeof(sentinel)) == 0);
    CHECK(open_started(&owner, &cookie, &t, 700U, 600U, &duplicate) == UCN_ERR_ARGUMENT);
    CHECK(open_started(&owner, &cookie, &t, 100U, 5100U, &duplicate) == UCN_ERR_TIMEOUT);
    CHECK(memcmp(&duplicate, &sentinel, sizeof(sentinel)) == 0);
    CHECK(ucn_i_admission_pending_get(&owner, h, &view) == UCN_OK);
    CHECK(view.challenge_started_local_us == 100U && view.deadline_us == 5100U);
    puts("native JOIN Admission early start and no-refresh passed");
    return 0;
}
