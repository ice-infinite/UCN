#include "internal/ucn_digest.h"
#include "internal/ucn_realtime.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression_) do { if (!(expression_)) return __LINE__; } while (0)
typedef struct test_lock { uint8_t held; } test_lock_t;
static test_lock_t lock_state;
static ucn_i_realtime_owner_t owner;

static ucn_result_t enter(void *context) { test_lock_t *l=context; if(l==NULL||l->held)return UCN_ERR_STATE; l->held=1U; return UCN_OK; }
static void leave(void *context) { ((test_lock_t *)context)->held=0U; }
static ucn_i_lock_ops_t lock_ops(void) { ucn_i_lock_ops_t v; memset(&v,0,sizeof(v)); v.struct_size=sizeof(v); v.api_version=UCN_I_LOCK_OPS_VERSION; v.context=&lock_state; v.enter=enter; v.leave=leave; return v; }

static ucn_i_realtime_domain_config_t domain_config(uint32_t generation)
{
    ucn_i_realtime_domain_config_t c; memset(&c,0,sizeof(c));
    c.clock_domain_id=7U; c.domain_generation=generation; c.lock_sample_count=2U;
    c.sync_timeout_us=10U; c.max_holdover_us=30U; c.max_offset_jump_us=1000U;
    c.oscillator_uncertainty_ppb=1000000U;
    c.base_uncertainty.known_mask=UCN_I_REALTIME_KNOWN_ALL;
    c.base_uncertainty.timer_resolution_bound_us=1U;
    c.base_uncertainty.link_timestamp_capture_bound_us=1U;
    c.base_uncertainty.filter_residual_bound_us=1U;
    c.base_uncertainty.arithmetic_rounding_bound_us=1U;
    c.base_uncertainty.sample_capture_bound_us=1U;
    c.base_uncertainty.path_asymmetry_bound_us=5U;
    c.path.route_causal_id=10U; c.path.route_generation=11U;
    c.path.session_generation=12U; c.path.capability_generation=13U;
    c.path.forward_link_generation=14U; c.path.reverse_link_generation=15U;
    c.path.forward_link_id=1U; c.path.reverse_link_id=2U;
    memset(c.path.capability_digest,0xA5,16U);
    c.path.authenticated=1U; c.path.frozen=1U; c.path.directional=1U;
    c.path.asymmetry_known=1U; c.path.max_asymmetry_us=5U;
    return c;
}

static ucn_i_realtime_durability_t durability(uint32_t generation)
{
    ucn_i_realtime_durability_t d; memset(&d,0,sizeof(d));
    d.domain_id=800U; d.foundation_transaction_id=generation;
    d.expected_record_generation=generation-1U; d.absolute_deadline_us=10000U;
    d.volatile_continuation.runtime_instance=1U;
    d.volatile_continuation.owner_instance=8U;
    d.volatile_continuation.slot=1U; d.volatile_continuation.generation=(uint16_t)generation;
    d.volatile_continuation.object_kind=UCN_OBJECT_KIND_TIME_DOMAIN;
    d.persistence_domain_generation=4U;
    d.schema_id=UCN_I_REALTIME_DOMAIN_SCHEMA_ID;
    d.schema_version=UCN_I_REALTIME_DOMAIN_RECORD_SCHEMA;
    return d;
}

static int configure(void)
{
    ucn_i_realtime_config_t c; memset(&owner,0,sizeof(owner)); memset(&lock_state,0,sizeof(lock_state)); memset(&c,0,sizeof(c));
    c.runtime_instance=1U; c.owner_instance=8U; c.state_lock=lock_ops();
    return ucn_i_realtime_owner_init(&owner,&c)==UCN_OK?0:__LINE__;
}

static int activate(uint32_t generation, ucn_handle_t *handle_out)
{
    ucn_i_realtime_domain_config_t c=domain_config(generation);
    ucn_i_realtime_durability_t d=durability(generation);
    ucn_i_realtime_requirement_t r; ucn_i_realtime_proof_t p;
    ucn_i_sha256_workspace_t w; ucn_handle_t ph;
    CHECK(ucn_i_realtime_domain_prepare(&owner,&c,&d,handle_out,&r)==UCN_OK);
    memset(&w,0,sizeof(w)); memset(&p,0,sizeof(p)); memset(&ph,0,sizeof(ph));
    ph.runtime_instance=1U; ph.owner_instance=9U; ph.slot=1U; ph.generation=(uint16_t)generation; ph.object_kind=UCN_OBJECT_KIND_PERSISTENCE;
    CHECK(ucn_i_sha256_128(r.body,r.body_bytes,p.body_digest,&w)==UCN_OK);
    CHECK(ucn_i_realtime_domain_bind_persistence(&owner,*handle_out,ph,p.body_digest)==UCN_OK);
    p.persistence_handle=ph; p.domain_id=d.domain_id; p.foundation_transaction_id=d.foundation_transaction_id;
    p.record_generation=d.expected_record_generation+1U; p.witness_generation=p.record_generation; p.runtime_instance=1U;
    p.body_bytes=r.body_bytes; p.persistence_domain_generation=4U;
    p.persistence_owner_instance=9U; p.caller_owner_instance=8U;
    p.schema_id=d.schema_id; p.schema_version=d.schema_version; p.operation_kind=UCN_I_REALTIME_DOMAIN_PERSIST_KIND;
    {
        ucn_i_realtime_proof_t wrong=p;
        wrong.persistence_owner_instance=10U;
        CHECK(ucn_i_realtime_domain_accept_proof(&owner,*handle_out,&wrong,10U)==UCN_ERR_STATE);
    }
    CHECK(ucn_i_realtime_domain_accept_proof(
              &owner,*handle_out,&p,d.absolute_deadline_us)==UCN_ERR_STATE);
    CHECK(ucn_i_realtime_domain_accept_proof(&owner,*handle_out,&p,10U)==UCN_OK);
    return 0;
}

static int test_prepare_preflight(void)
{
    ucn_i_realtime_domain_config_t c=domain_config(1U);
    ucn_i_realtime_durability_t d=durability(1U);
    ucn_i_realtime_requirement_t requirement, requirement_before;
    ucn_handle_t handle, handle_before;
    memset(&handle,0xA5,sizeof(handle));
    memset(&requirement,0xA5,sizeof(requirement));
    handle_before=handle;
    requirement_before=requirement;
    d.volatile_continuation.owner_instance=99U;
    CHECK(ucn_i_realtime_domain_prepare(
              &owner,&c,&d,&handle,&requirement)==UCN_ERR_ARGUMENT);
    CHECK(memcmp(&handle,&handle_before,sizeof(handle))==0);
    CHECK(memcmp(&requirement,&requirement_before,sizeof(requirement))==0);
    d=durability(1U);
    CHECK(ucn_i_realtime_domain_prepare(
              &owner,&c,&d,(ucn_handle_t *)&owner,
              &requirement)==UCN_ERR_ARGUMENT);
    return 0;
}

static int test_durable_then_lock_holdover(void)
{
    ucn_handle_t handle; ucn_i_realtime_sync_sample_t s; ucn_i_realtime_clock_view_t v;
    int result=activate(1U,&handle); if(result!=0)return result;
    memset(&s,0,sizeof(s)); s.path=domain_config(1U).path; s.clock_domain_id=7U; s.valid_sync_sample=1U; s.uncertainty_us=20U; s.offset_us=100;
    s.sample_local_us=1000U; CHECK(ucn_i_realtime_domain_accept_sample(&owner,&s)==UCN_OK);
    CHECK(ucn_i_realtime_domain_get_clock(&owner,7U,1000U,&v)==UCN_ERR_STATE);
    s.sample_local_us=1001U; CHECK(ucn_i_realtime_domain_accept_sample(&owner,&s)==UCN_OK);
    CHECK(ucn_i_realtime_domain_get_clock(&owner,7U,1002U,&v)==UCN_OK && v.domain_time_us==1102U && v.phase==UCN_I_REALTIME_LOCKED);
    CHECK(ucn_i_realtime_step(&owner,1011U)==UCN_OK);
    CHECK(ucn_i_realtime_domain_get_clock(&owner,7U,1011U,&v)==UCN_OK && v.holdover==1U && v.uncertainty_us>=20U);
    CHECK(ucn_i_realtime_step(&owner,1031U)==UCN_OK);
    CHECK(ucn_i_realtime_domain_get_clock(&owner,7U,1031U,&v)==UCN_ERR_STATE);
    return 0;
}

static int test_same_generation_regression_fault(void)
{
    ucn_i_realtime_sync_sample_t s; ucn_i_realtime_clock_view_t v;
    memset(&s,0,sizeof(s)); s.path=domain_config(1U).path; s.clock_domain_id=7U; s.valid_sync_sample=1U; s.uncertainty_us=20U; s.offset_us=-1000;
    s.sample_local_us=2000U; CHECK(ucn_i_realtime_domain_accept_sample(&owner,&s)==UCN_ERR_STATE);
    CHECK(ucn_i_realtime_domain_get_clock(&owner,7U,2000U,&v)==UCN_ERR_STATE);
    return 0;
}

static int test_record_reserved_and_diagnostic(void)
{
    ucn_i_realtime_domain_config_t c=domain_config(2U); uint8_t body[128]; ucn_i_realtime_sync_sample_t s;
    CHECK(ucn_i_realtime_domain_record_encode(&c,body)==UCN_OK);
    CHECK(body[0]==0U && body[1]==1U && body[2]==0U && body[3]==7U);
    memset(&s,0,sizeof(s)); s.path=c.path; s.path.asymmetry_known=0U; s.path.max_asymmetry_us=0U; s.clock_domain_id=7U; s.sample_local_us=3000U; s.uncertainty_us=1U;
    CHECK(ucn_i_realtime_domain_accept_sample(&owner,&s)==UCN_ERR_STATE);
    return 0;
}

int main(void)
{
    int result=configure();
    if(result==0)result=test_prepare_preflight();
    if(result==0)result=test_durable_then_lock_holdover();
    if(result==0)result=test_same_generation_regression_fault();
    if(result==0)result=test_record_reserved_and_diagnostic();
    if(result!=0)fprintf(stderr,"realtime domain failed: %d\n",result);
    return result;
}
