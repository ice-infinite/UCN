#ifndef UCN_INTERNAL_NATIVE_DYNAMIC_H
#define UCN_INTERNAL_NATIVE_DYNAMIC_H
#include "internal/ucn_native_capability.h"
#include "internal/ucn_native_reauth.h"

/* Sole RX Coordinator; leaves borrow this actor, never compete for ingress.
 * 唯一RX协调器组合；各子阶段不分别领取入口。 */
typedef struct ucn_i_native_dynamic {
    uint32_t magic;
    ucn_i_native_routed_t *root;
    ucn_native_t *native;
    ucn_native_borrowed_range_t facade_lifecycle;
    ucn_i_native_capability_t *current[UCN_I_NATIVE_NEIGHBORS];
    ucn_i_native_capability_t *pending[UCN_I_NATIVE_NEIGHBORS];
    ucn_i_native_reauth_t *reauth[UCN_I_NATIVE_NEIGHBORS];
    ucn_i_capability_ref_t predecessor[UCN_I_NATIVE_NEIGHBORS];
    ucn_i_route_link_ref_t peer_history[UCN_I_NATIVE_NEIGHBORS];
    uint16_t physical_mtu[UCN_I_NATIVE_NEIGHBORS];
    uint16_t cursor;
    ucn_i_capability_ref_t reference;
    ucn_i_cached_capability_t cached;
    ucn_i_profile_select_t profile;
    ucn_i_security_candidate_t candidate;
    ucn_i_route_link_ref_t retired_peer;
    ucn_i_c0_frame_t decoded;
    ucn_i_callback_claim_t claim;
    ucn_i_native_join_current_t established;
} ucn_i_native_dynamic_t;

ucn_result_t ucn_i_native_dynamic_init(ucn_i_native_dynamic_t *,
    ucn_i_native_routed_t *, ucn_native_t *,
    ucn_i_native_capability_t *const *, uint16_t count, uint64_t now);
ucn_result_t ucn_i_native_dynamic_stage(ucn_i_native_dynamic_t *, uint16_t peer,
    ucn_i_native_capability_t *successor, uint64_t now);
ucn_result_t ucn_i_native_dynamic_attach_reauth(ucn_i_native_dynamic_t *, uint16_t peer,
    ucn_i_native_reauth_t *, uint64_t now);
ucn_result_t ucn_i_native_dynamic_detach_reauth(ucn_i_native_dynamic_t *, uint16_t peer,uint64_t now);
ucn_result_t ucn_i_native_dynamic_commit(ucn_i_native_dynamic_t *, uint16_t peer, uint64_t now);
ucn_result_t ucn_i_native_dynamic_step(ucn_i_native_dynamic_t *);
ucn_result_t ucn_i_native_dynamic_receive(ucn_i_native_dynamic_t *);
ucn_result_t ucn_i_native_dynamic_neighbor(ucn_i_native_dynamic_t *, uint16_t, size_t bytes, uint64_t now);
ucn_result_t ucn_i_native_dynamic_endpoint(ucn_i_native_dynamic_t *, uint16_t, uint64_t now);
ucn_result_t ucn_i_native_dynamic_message(ucn_i_native_dynamic_t *,uint16_t,size_t,uint64_t);
bool ucn_i_native_dynamic_quiet(ucn_i_native_dynamic_t *);
bool ucn_i_native_dynamic_output_safe(const ucn_i_native_dynamic_t *, const void *, size_t);
void ucn_i_native_dynamic_stop_session(ucn_i_native_dynamic_t *,
    ucn_i_security_owner_t *, ucn_i_security_handle_t, uint64_t now);
#endif
