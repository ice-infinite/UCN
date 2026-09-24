#include "internal/ucn_realtime.h"

#include "internal/ucn_checked.h"

#include <string.h>

static ucn_result_t lock_owner(ucn_i_realtime_owner_t *owner)
{
    if (owner == NULL || owner->magic != UCN_I_REALTIME_MAGIC ||
        owner->schema != UCN_I_REALTIME_SCHEMA) return UCN_ERR_STATE;
    return owner->state_lock.enter(owner->state_lock.context);
}

static void unlock_owner(ucn_i_realtime_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static bool policy_valid(const ucn_i_realtime_policy_t *policy)
{
    if (policy == NULL || policy->endpoint == 0U ||
        policy->mode > UCN_I_REALTIME_DEADLINE ||
        policy->requirement > UCN_I_REALTIME_REQUIRED ||
        policy->require_hardware_capture > 1U ||
        policy->allow_remote_holdover > 1U) return false;
    if (policy->mode == UCN_I_REALTIME_NONE) {
        return policy->requirement == UCN_I_REALTIME_DISABLED &&
               policy->clock_domain_id == 0U && policy->max_age_us == 0U &&
               policy->max_uncertainty_us == 0U &&
               policy->require_hardware_capture == 0U &&
               policy->allow_remote_holdover == 0U;
    }
    if (policy->requirement == UCN_I_REALTIME_DISABLED) return false;
    if (policy->mode == UCN_I_REALTIME_LOCAL_STAMP) {
        return policy->clock_domain_id == 0U && policy->max_age_us == 0U &&
               policy->max_uncertainty_us == 0U &&
               policy->allow_remote_holdover == 0U;
    }
    if (policy->clock_domain_id == 0U ||
        policy->clock_domain_id > UCN_I_REALTIME_DOMAIN_ID_MAX ||
        policy->max_uncertainty_us == 0U) return false;
    if (policy->mode == UCN_I_REALTIME_SYNCED_STAMP)
        return policy->max_age_us == 0U;
    return policy->max_age_us != 0U;
}

static bool policy_copy(ucn_i_realtime_owner_t *owner, uint16_t endpoint,
                        ucn_i_realtime_policy_t *policy_out)
{
    uint16_t index;
    for (index = 0U; index < UCN_I_REALTIME_POLICY_COUNT; ++index) {
        if (owner->policies[index].occupied != 0U &&
            owner->policies[index].policy.endpoint == endpoint) {
            *policy_out = owner->policies[index].policy; return true;
        }
    }
    return false;
}

ucn_result_t ucn_i_realtime_policy_set(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_policy_t *policy)
{
    uint16_t index, free_index = UCN_I_REALTIME_POLICY_COUNT;
    ucn_result_t result;
    if (owner == NULL || !policy_valid(policy) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), policy, sizeof(*policy))) {
        return UCN_ERR_ARGUMENT;
    }
    result=lock_owner(owner);if(result!=UCN_OK)return result;
    for(index=0U;index<UCN_I_REALTIME_POLICY_COUNT;++index){
        if(owner->policies[index].occupied!=0U &&
           owner->policies[index].policy.endpoint==policy->endpoint){
            owner->policies[index].policy=*policy;unlock_owner(owner);return UCN_OK;
        }
        if(free_index==UCN_I_REALTIME_POLICY_COUNT &&
           owner->policies[index].occupied==0U)free_index=index;
    }
    if(free_index==UCN_I_REALTIME_POLICY_COUNT){unlock_owner(owner);return UCN_ERR_NO_SPACE;}
    owner->policies[free_index].policy=*policy;
    owner->policies[free_index].occupied=1U;
    unlock_owner(owner);return UCN_OK;
}

ucn_result_t ucn_i_realtime_prepare(
    ucn_i_realtime_owner_t *owner, uint16_t endpoint,
    uint64_t local_capture_us, uint32_t sample_capture_bound_us,
    bool hardware_capture, ucn_i_realtime_prepared_t *prepared_out)
{
    ucn_i_realtime_prepared_t prepared;
    ucn_i_realtime_policy_t policy;
    ucn_i_realtime_clock_view_t clock;
    uint64_t source_uncertainty;
    uint8_t uncertainty_class;
    ucn_result_t result;
    if(owner==NULL || prepared_out==NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),prepared_out,
                            sizeof(*prepared_out)))return UCN_ERR_ARGUMENT;
    result=lock_owner(owner);
    if(result!=UCN_OK)return result;
    if(endpoint==0U ||
       !policy_copy(owner,endpoint,&policy)){unlock_owner(owner);return UCN_ERR_NOT_FOUND;}
    unlock_owner(owner);
    memset(&prepared,0,sizeof(prepared));
    if(policy.mode==UCN_I_REALTIME_NONE){*prepared_out=prepared;return UCN_OK;}
    if(policy.require_hardware_capture!=0U && !hardware_capture)return UCN_ERR_POLICY;
    if(policy.mode==UCN_I_REALTIME_LOCAL_STAMP){
        result=ucn_i_realtime_local_stamp(local_capture_us,hardware_capture,
                                           &prepared.envelope);
        if(result==UCN_OK)prepared.metadata_present=1U;
        if(result==UCN_OK)*prepared_out=prepared;
        return result;
    }
    result=ucn_i_realtime_domain_get_clock(owner,policy.clock_domain_id,
                                            local_capture_us,&clock);
    if(result!=UCN_OK){
        if(policy.requirement==UCN_I_REALTIME_REQUIRED)return result;
        result=ucn_i_realtime_local_stamp(local_capture_us,hardware_capture,
                                           &prepared.envelope);
        if(result==UCN_OK){prepared.metadata_present=1U;prepared.fell_back=1U;*prepared_out=prepared;}
        return result;
    }
    if(sample_capture_bound_us==0U)return UCN_ERR_STATE;
    source_uncertainty=(uint64_t)clock.uncertainty_us+sample_capture_bound_us;
    if(source_uncertainty>policy.max_uncertainty_us)return UCN_ERR_POLICY;
    result=ucn_i_realtime_uncertainty_class_encode(true,source_uncertainty,
                                                    &uncertainty_class);
    if(result!=UCN_OK || uncertainty_class==UCN_I_REALTIME_UNCERTAINTY_UNKNOWN)
        return UCN_ERR_STATE;
    prepared.envelope.capture_time_us=clock.domain_time_us;
    prepared.envelope.domain_generation=clock.domain_generation;
    prepared.envelope.clock_domain_id=clock.clock_domain_id;
    prepared.envelope.mode=policy.mode;
    prepared.envelope.uncertainty_class=uncertainty_class;
    prepared.envelope.sample_capture_hardware=hardware_capture?1U:0U;
    prepared.envelope.domain_time_valid=1U;
    prepared.envelope.source_holdover=clock.holdover;
    prepared.metadata_present=1U;
    if(!ucn_i_realtime_envelope_valid(&prepared.envelope))return UCN_ERR_STATE;
    *prepared_out=prepared;return UCN_OK;
}

static ucn_result_t reject(ucn_i_realtime_reject_reason_t reason,
                           ucn_i_realtime_receive_view_t *view_out)
{
    ucn_i_realtime_receive_view_t view;
    memset(&view,0,sizeof(view));view.reason=reason;*view_out=view;
    return UCN_ERR_POLICY;
}

ucn_result_t ucn_i_realtime_admit(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_receive_facts_t *facts,
    ucn_i_realtime_receive_view_t *view_out)
{
    ucn_i_realtime_policy_t policy;
    ucn_i_realtime_receive_view_t view;
    ucn_i_realtime_clock_view_t clock;
    uint32_t source_uncertainty;
    uint64_t combined,age;
    bool known;
    ucn_result_t result;
    if(owner==NULL || facts==NULL || view_out==NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),facts,sizeof(*facts)) ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),view_out,sizeof(*view_out)) ||
       ucn_i_ranges_overlap(facts,sizeof(*facts),view_out,sizeof(*view_out)))
        return UCN_ERR_ARGUMENT;
    result=lock_owner(owner);
    if(result!=UCN_OK)return result;
    if(facts->endpoint==0U ||
       facts->metadata_present>1U || facts->e2e_authenticated>1U ||
       facts->source_acl_authorized>1U || facts->reserved_zero!=0U ||
       !policy_copy(owner,facts->endpoint,&policy)){
        unlock_owner(owner);return UCN_ERR_ARGUMENT;
    }
    unlock_owner(owner);
    if(facts->metadata_present==0U){
        if(policy.mode!=UCN_I_REALTIME_NONE)return reject(UCN_I_REALTIME_REJECT_DOMAIN,view_out);
        memset(&view,0,sizeof(view));view.accepted=1U;*view_out=view;return UCN_OK;
    }
    if(!ucn_i_realtime_envelope_valid(&facts->envelope))
        return reject(UCN_I_REALTIME_REJECT_MALFORMED,view_out);
    if(policy.mode==UCN_I_REALTIME_NONE)
        return reject(UCN_I_REALTIME_REJECT_DOMAIN,view_out);
    if(facts->e2e_authenticated==0U || facts->source_acl_authorized==0U)
        return reject(UCN_I_REALTIME_REJECT_SECURITY,view_out);
    if(facts->envelope.mode==UCN_I_REALTIME_LOCAL_STAMP){
        if(policy.mode!=UCN_I_REALTIME_LOCAL_STAMP &&
           policy.requirement==UCN_I_REALTIME_REQUIRED)
            return reject(UCN_I_REALTIME_REJECT_DOMAIN,view_out);
        memset(&view,0,sizeof(view));view.accepted=1U;*view_out=view;return UCN_OK;
    }
    if(facts->envelope.mode!=policy.mode ||
       facts->envelope.clock_domain_id!=policy.clock_domain_id)
        return reject(UCN_I_REALTIME_REJECT_DOMAIN,view_out);
    if(facts->envelope.source_holdover!=0U &&
       (policy.requirement==UCN_I_REALTIME_REQUIRED ||
        policy.allow_remote_holdover==0U))
        return reject(UCN_I_REALTIME_REJECT_HOLDOVER,view_out);
    result=ucn_i_realtime_domain_get_clock(owner,policy.clock_domain_id,
                                            facts->local_now_us,&clock);
    if(result!=UCN_OK || clock.domain_generation!=facts->envelope.domain_generation)
        return reject(UCN_I_REALTIME_REJECT_DOMAIN,view_out);
    result=ucn_i_realtime_uncertainty_class_decode(
        facts->envelope.uncertainty_class,&known,&source_uncertainty);
    if(result!=UCN_OK || !known)
        return reject(UCN_I_REALTIME_REJECT_UNCERTAINTY,view_out);
    combined=(uint64_t)source_uncertainty+clock.uncertainty_us;
    if(combined>policy.max_uncertainty_us)
        return reject(UCN_I_REALTIME_REJECT_UNCERTAINTY,view_out);
    if(facts->envelope.capture_time_us>clock.domain_time_us){
        if(facts->envelope.capture_time_us-clock.domain_time_us>combined)
            return reject(UCN_I_REALTIME_REJECT_FUTURE,view_out);
        age=combined;
    }else{
        uint64_t elapsed=clock.domain_time_us-facts->envelope.capture_time_us;
        if(UINT64_MAX-elapsed<combined)return reject(UCN_I_REALTIME_REJECT_UNCERTAINTY,view_out);
        age=elapsed+combined;
    }
    if(policy.mode==UCN_I_REALTIME_DEADLINE && age>=policy.max_age_us)
        return reject(UCN_I_REALTIME_REJECT_EXPIRED,view_out);
    memset(&view,0,sizeof(view));view.accepted=1U;view.age_upper_us=age;
    view.combined_uncertainty_us=(uint32_t)combined;*view_out=view;return UCN_OK;
}
