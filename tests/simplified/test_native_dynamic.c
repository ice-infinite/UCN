/* Real JOIN -> durable Session -> authenticated Capability -> Native Pub/Sub.
 * Host Driver/provider only; no fabricated authenticated event or hardware. */
#define UCN_CAPABILITY_FIXTURE_ONLY
#define UCN_NATIVE_DYNAMIC_CONTROL_FIXTURE
#include <stdlib.h>
#include "test_native_capability.c"
#include "internal/ucn_native_dynamic.h"
#include "native_reauth_fixture.h"
#include "internal/ucn_native_route_control.h"

#if UCN_PERSIST_DOMAIN_COUNT >= 3U
static ucn_i_native_routed_t backend[2];
static ucn_i_native_dynamic_t dynamic[2];
static ucn_i_route_owner_t route_owner[2];
static ucn_i_native_route_control_t route_control[2];
static native_join_fixture_lock_t route_lock[2], native_lock[2];
static ucn_native_storage_t native_storage[2];
static ucn_native_lifecycle_storage_t lifecycle_storage[2];
static ucn_native_t *application[2];
static ucn_native_handle_t publisher[2], subscription[2];
static uint64_t clock_now;
static unsigned callbacks[2];
static size_t business_bytes;
static unsigned route_packets;
typedef struct hot_bank {
    ucn_i_native_reauth_t roots[2];
    native_reauth_fixture_t products[2];
    ucn_i_native_capability_t capabilities[2];
    ucn_i_native_capability_config_t configs[2];
    ucn_i_security_acl_rule_t rules[2][64];
} hot_bank_t;
static hot_bank_t hot_banks[2],*hot;
#define renewal (hot->roots)
#define renewal_product (hot->products)
#define successor_cap (hot->capabilities)
#define successor_config (hot->configs)
#define fresh_rules (hot->rules)
static ucn_i_native_join_current_t renewed[2];

static ucn_result_t renewed_policy(void *context,const ucn_i_security_candidate_t *candidate,
    const ucn_i_security_acl_rule_t **out,uint16_t *count)
{
    native_reauth_fixture_t *f=context;
    unsigned index=f->prior==&device?0U:1U;
    return fill_capability_policy(fresh_rules[index],candidate,out,count);
}

static void route_binding_copy(ucn_i_route_binding_t *out, const ucn_i_security_binding_t *in)
{
    out->address = in->address; out->generation = in->binding_generation;
    memcpy(out->principal, in->principal, 16U);
}
static void sample_received(void *ctx, const ucn_native_sample_t *sample)
{
    if(sample->payload_bytes != 4U || sample->payload[0] != 0x55U) abort();
    ++callbacks[(size_t)(uintptr_t)ctx];
}
static ucn_driver_submit_result_t network_submit(void *ctx, ucn_link_handle_t ignored,
    const uint8_t *bytes, size_t count, ucn_driver_token_t token)
{
    native_join_fixture_t *from = ctx;
    native_join_fixture_t *to = from == &device ? &authority : &device;
    ucn_rx_meta_t meta = {0}; ucn_link_handle_t link;
    (void)ignored; (void)token;
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    meta.timestamp_us = clock_now; meta.sender_discriminator = 77U;
    if (bytes[0] == 0x61U) business_bytes = count;
    if(bytes[0]==0x60U) {
        ucn_i_c0_frame_t frame;
        if(ucn_i_c0_decode(bytes,count,2U,UCN_I_CONTROL_H1,&frame)==UCN_OK && frame.opcode>=0x201U) ++route_packets;
    }
    if (ucn_i_adapter_link_handle(&to->adapter, 0U, &link) != UCN_OK ||
        ucn_i_adapter_rx_publish(&to->adapter, link, bytes, count, &meta) != UCN_OK)
        return UCN_DRIVER_NOT_SUBMITTED;
    return UCN_DRIVER_COMPLETE;
}
static int assemble(unsigned index, native_join_fixture_t *fixture,
    const ucn_i_native_join_current_t *current, ucn_i_native_capability_t *capability)
{
    ucn_i_route_config_t rc = {0}; ucn_i_lock_ops_t lock = {0};
    ucn_i_native_endpoint_t endpoint = {0}; ucn_i_native_neighbor_t neighbor = {0};
    ucn_i_native_routed_config_t cfg = {0}; ucn_i_route_static_entry_t entry = {0};
    ucn_native_channel_t channel = {0}; ucn_native_config_t config = {0};
    const ucn_i_security_candidate_t *candidate = &current->candidate;
    REQUIRE(ucn_i_adapter_set_rx_enabled(&fixture->adapter, false) == UCN_OK);
    lock.struct_size = sizeof(lock); lock.api_version = UCN_I_LOCK_OPS_VERSION;
    lock.context = &route_lock[index]; lock.enter = cap_lock_enter; lock.leave = cap_lock_leave;
    rc.runtime_instance = fixture->adapter.runtime_instance; rc.owner_instance = 41U;
    rc.realm = fixture->config.realm_id; rc.local_session_generation = 1U; rc.address_width = 2U;
    route_binding_copy(&rc.local, &candidate->local);
    rc.discovery_lifetime_us = 10000U; rc.discovery_retry_us = 100U;
    rc.reverse_lifetime_us = 10000U; rc.route_lifetime_us = 100000U;
    rc.first_transaction_id = 1U; rc.discovery_max_attempts = 3U; rc.maximum_hops = 4U;
    REQUIRE(ucn_i_route_owner_init(&route_owner[index], &rc, &lock) == UCN_OK);
    endpoint.security = &fixture->security; endpoint.session = current->session;
    endpoint.facts.now_us = clock_now; endpoint.facts.local = candidate->local;
    endpoint.facts.peer = candidate->peer;
    endpoint.facts.link_generation = candidate->link_generation; endpoint.facts.policy_generation = candidate->policy_generation;
    endpoint.source.address = candidate->peer.address; endpoint.source.binding_generation = candidate->peer.binding_generation;
    endpoint.source.principal_digest = index ? 1U : 2U; endpoint.service = 1U;
    endpoint.minimum = UCN_NATIVE_ORIGIN_AUTH; memcpy(endpoint.fingerprint, candidate->origin_fingerprint, 16U);
    neighbor.security = &fixture->security; neighbor.session = current->session; neighbor.facts = endpoint.facts;
    neighbor.sender_discriminator = 77U; neighbor.capability_bits = 3U;
    memcpy(neighbor.fingerprint, candidate->hop_fingerprint, 16U);
    memcpy(neighbor.control_fingerprint, candidate->origin_fingerprint, 16U);
    cfg.adapter = &fixture->adapter; cfg.routes = &route_owner[index]; cfg.gate = &fixture->callback_gate;
    cfg.local.address = candidate->local.address; cfg.local.binding_generation = candidate->local.binding_generation;
    cfg.local.principal_digest = index + 1U; cfg.now_us = clock_now; cfg.expires_at_us = 900000U;
    cfg.relay_lifetime_us = 100000U; cfg.endpoints = &endpoint; cfg.endpoint_count = 1U;
    cfg.neighbors = &neighbor; cfg.neighbor_count = 1U; cfg.hop_limit = 4U;
    cfg.control=&route_control[index]; cfg.reserve_c0=reserve_c0; cfg.counter_context=index?&peer_counter:&next_c0;
    REQUIRE(ucn_i_native_routed_init(&backend[index], &cfg) == UCN_OK);
    route_binding_copy(&entry.destination, &candidate->peer);
    entry.next_hop.peer = entry.destination; entry.next_hop.link_id = 1U;
    entry.next_hop.link_generation = candidate->link_generation; entry.next_hop.cost = 1U;
    entry.next_hop.frame_mtu = UCN_ADAPTER_FRAME_BYTES; entry.next_hop.capability_bits = 3U;
    entry.path_frame_mtu = UCN_ADAPTER_FRAME_BYTES;
    REQUIRE(ucn_i_route_install_static(&route_owner[index], &entry) == UCN_OK);
    channel.id = 1U; channel.service_id = 1U; channel.schema_id = 1U; channel.schema_revision = 1U;
    channel.payload_bytes = 4U; channel.traffic_class = UCN_TRAFFIC_Q1;
    channel.queue_policy = UCN_NATIVE_KEEP_LATEST; channel.security_minimum = UCN_NATIVE_ORIGIN_AUTH;
    channel.max_local_age_us = 100000U; channel.source_count = 2U;
    channel.sources[0] = cfg.local; channel.sources[1] = endpoint.source; channel.target_count = 1U;
    channel.targets[0].address = candidate->peer.address;
    channel.targets[0].binding_generation = candidate->peer.binding_generation;
    config.api_version = UCN_NATIVE_API_VERSION; config.storage_layout = UCN_NATIVE_LAYOUT;
    config.manifest_hash = UCN_NATIVE_MANIFEST_HASH; config.instance = 100U + index;
    config.channels = &channel; config.channel_count = 1U;
    config.lock.struct_size = sizeof(config.lock); config.lock.api_version = UCN_API_VERSION;
    config.lock.context = &native_lock[index]; config.lock.enter = cap_lock_enter; config.lock.leave = cap_lock_leave;
    REQUIRE(ucn_native_init_backend(&native_storage[index], sizeof(native_storage[index]), &backend[index].port,
        &config, &application[index]) == UCN_OK);
    REQUIRE(ucn_native_backend_enable_lifecycle(application[index], &lifecycle_storage[index], sizeof(lifecycle_storage[index])) == UCN_OK);
    REQUIRE(ucn_i_native_dynamic_init(&dynamic[index], &backend[index], application[index], &capability, 1U, clock_now) == UCN_OK);
    REQUIRE(backend[index].port.local_view(&backend[index],(ucn_node_view_t *)lifecycle_storage[index].bytes)==UCN_ERR_ARGUMENT);
    REQUIRE(ucn_publisher_create(application[index], 1U, &publisher[index]) == UCN_OK);
    REQUIRE(ucn_subscription_create(application[index], 1U, sample_received, (void *)(uintptr_t)index, &subscription[index]) == UCN_OK);
    REQUIRE(ucn_native_start(application[index], clock_now) == UCN_OK);
    fixture->adapter.links[0].tx.submit = network_submit;
    return 0;
}
static int dynamic_roundtrip(void)
{
    const uint8_t payload[4] = {0x55, 2, 3, 4}; uint16_t delivered; unsigned i;
    REQUIRE(capability_ready_pair() == 0); clock_now = capability_ready_now;
    REQUIRE(assemble(0U, &device, &current_device, &cap) == 0);
    REQUIRE(assemble(1U, &authority, &current_authority, &peer_cap) == 0);
    REQUIRE(backend[0].port.local_view(&backend[0],(ucn_node_view_t *)device.identity_rules)==UCN_ERR_ARGUMENT);
    REQUIRE(ucn_publish(application[0], publisher[0], payload, sizeof(payload)) == UCN_OK);
    for (i = 0U; i < 10U; ++i) {
        ++clock_now;
        REQUIRE(ucn_native_step(application[0], clock_now, 4U) == UCN_OK);
        REQUIRE(ucn_native_step(application[1], clock_now, 4U) == UCN_OK);
        REQUIRE(ucn_native_dispatch(application[1], clock_now, 4U, &delivered) == UCN_OK);
    }
    REQUIRE(callbacks[1] == 1U && business_bytes == 49U);
    /* Queue an actual old-Scope sample, then retire it before app dispatch. */
    REQUIRE(ucn_publish(application[0],publisher[0],payload,sizeof(payload))==UCN_OK);
    for(i=0U;i<4U;++i) {
        ++clock_now;
        REQUIRE(ucn_native_step(application[0],clock_now,4U)==UCN_OK);
        REQUIRE(ucn_native_step(application[1],clock_now,4U)==UCN_OK);
    }
    return 0;
}
static int hot_cycle(unsigned cycle)
{
    const uint8_t payload[4]={0x55,2,3,4}; uint16_t delivered; unsigned i;
    ucn_i_security_handle_t old_session[2]; ucn_i_security_session_view_t retired_view;
    hot=&hot_banks[cycle];
    old_session[0]=backend[0].neighbors[0].session; old_session[1]=backend[1].neighbors[0].session;
    /* Actual authenticated RREQ/RREP via the sole RX actor creates the old
     * reverse history; a direct-only test could not detect MTU-key mismatch. */
    REQUIRE(ucn_i_native_route_control_discover(&backend[1],backend[0].local.address,49U,clock_now+10000U)==UCN_ERR_NO_SPACE);
    for(i=0U;i<400U;++i) {
        ++clock_now;
        REQUIRE(ucn_native_step(application[0],clock_now,4U)==UCN_OK);
        REQUIRE(ucn_native_step(application[1],clock_now,4U)==UCN_OK);
    }
    { uint16_t discoveries,reverse,routes,statics;
      REQUIRE(ucn_i_route_counts(&route_owner[0],&discoveries,&reverse,&routes,&statics)==UCN_OK);
      REQUIRE(reverse>0U && route_packets>=2U);
    }
    REQUIRE(ucn_i_native_routed_fence_session(&backend[0],application[0],&device.security,backend[0].neighbors[0].session,clock_now)==UCN_OK);
    REQUIRE(ucn_i_native_routed_fence_session(&backend[1],application[1],&authority.security,backend[1].neighbors[0].session,clock_now)==UCN_OK);
    for(i=0U;i<12U;++i) {
        ++clock_now;
        REQUIRE(ucn_native_step(application[0],clock_now,4U)==UCN_OK);
        REQUIRE(ucn_native_step(application[1],clock_now,4U)==UCN_OK);
    }
    native_reauth_fixture_configure(&renewal_product[0],&device,&renewal[0],true,0x1234U+cycle,101U+cycle);
    native_reauth_fixture_configure(&renewal_product[1],&authority,&renewal[1],false,0x5678U+cycle,0U);
    for(i=0U;i<2U;++i) {
        renewal_product[i].config.configure_session_policy=renewed_policy;
        REQUIRE(ucn_i_native_reauth_init(&renewal[i],&renewal_product[i].config,clock_now)==UCN_OK);
        REQUIRE(ucn_i_native_dynamic_attach_reauth(&dynamic[i],0U,&renewal[i],clock_now)==UCN_OK);
        REQUIRE(backend[i].port.local_view(&backend[i],(ucn_node_view_t *)&renewal[i])==UCN_ERR_ARGUMENT);
    }
    /* Sole Native Coordinator owns every RX, including four raw Reauth stages. */
    for(i=0U;i<10000U;++i) {
        ucn_persistence_step_result_t progress;
        ++clock_now;
        REQUIRE(ucn_i_persistence_step(device.persistence,clock_now,1U,&progress)==UCN_OK);
        REQUIRE(ucn_i_persistence_step(authority.persistence,clock_now,1U,&progress)==UCN_OK);
        REQUIRE(ucn_native_step(application[0],clock_now,4U)==UCN_OK);
        REQUIRE(ucn_native_step(application[1],clock_now,4U)==UCN_OK);
        if(ucn_i_native_reauth_session_current(&renewal[0],clock_now,&renewed[0])==UCN_OK &&
            ucn_i_native_reauth_session_current(&renewal[1],clock_now,&renewed[1])==UCN_OK) break;
    }
    REQUIRE(i<10000U && renewed[0].candidate.session_generation==2U+cycle && renewed[1].candidate.session_generation==2U+cycle);
    for(i=0U;i<2U;++i) {
        const ucn_i_native_capability_t *prior=dynamic[i].current[0];
        successor_config[i]=prior->config; successor_config[i].session=renewed[i].session;
        successor_config[i].candidate=renewed[i].candidate; successor_config[i].has_predecessor=1U;
        successor_config[i].predecessor=dynamic[i].predecessor[0];
        successor_config[i].local_record.capability_generation=2U+cycle;
        successor_config[i].deadline_us=clock_now+100000U;
        REQUIRE(ucn_i_native_capability_init(&successor_cap[i],&successor_config[i],clock_now)==UCN_OK);
        REQUIRE(ucn_i_native_dynamic_stage(&dynamic[i],0U,&successor_cap[i],clock_now)==UCN_OK);
        REQUIRE(backend[i].endpoint_generation[0]==1U+cycle && backend[i].endpoint_fenced[0]);
        REQUIRE(ucn_i_native_dynamic_commit(&dynamic[i],0U,clock_now)!=UCN_OK);
    }
    for(i=0U;i<10000U;++i) {
        ++clock_now;
        REQUIRE(ucn_native_step(application[0],clock_now,4U)==UCN_OK);
        REQUIRE(ucn_native_step(application[1],clock_now,4U)==UCN_OK);
        if(successor_cap[0].selected_ready && successor_cap[1].selected_ready && !successor_cap[0].tx_live && !successor_cap[1].tx_live) break;
    }
    REQUIRE(i<10000U);
    for(i=0U;i<2U;++i) {
        REQUIRE(ucn_i_native_dynamic_commit(&dynamic[i],0U,clock_now)==UCN_OK);
        REQUIRE(backend[i].endpoint_generation[0]==2U+cycle && !backend[i].endpoint_fenced[0]);
    }
    { uint16_t discoveries,reverse,routes,statics;
      REQUIRE(ucn_i_route_counts(&route_owner[0],&discoveries,&reverse,&routes,&statics)==UCN_OK);
      REQUIRE(reverse==0U);
    }
    REQUIRE(ucn_publish(application[0],publisher[0],payload,sizeof(payload))==UCN_OK);
    for(i=0U;i<16U;++i) {
        ++clock_now;
        REQUIRE(ucn_native_step(application[0],clock_now,4U)==UCN_OK);
        REQUIRE(ucn_native_step(application[1],clock_now,4U)==UCN_OK);
        REQUIRE(ucn_native_dispatch(application[1],clock_now,4U,&delivered)==UCN_OK);
    }
    if(callbacks[1]!=2U+cycle) fprintf(stderr,"hot delivery=%u bytes=%lu tx=%u/%u rx=%u gen=%lu/%lu cap=%u/%u\n",
        callbacks[1],(unsigned long)business_bytes,backend[0].tx_pending,backend[0].tx_prepared,backend[1].rx_ready,
        (unsigned long)backend[0].endpoint_generation[0],(unsigned long)backend[1].endpoint_generation[0],
        successor_cap[0].selected_ready,successor_cap[1].selected_ready);
    REQUIRE(callbacks[1]==2U+cycle && business_bytes==49U);
    REQUIRE(ucn_i_native_dynamic_detach_reauth(&dynamic[0],0U,clock_now)==UCN_OK);
    REQUIRE(ucn_i_native_dynamic_detach_reauth(&dynamic[1],0U,clock_now)==UCN_OK);
    REQUIRE(backend[0].port.local_view(&backend[0],(ucn_node_view_t *)fresh_rules[0])==UCN_ERR_ARGUMENT);
    /* Product Coordinator explicitly releases the proven-drained old slots;
     * the new ACTIVE Session never inherits those slots' policy pointers. */
    REQUIRE(ucn_i_security_retire_fenced(&device.security,old_session[0])==UCN_OK);
    REQUIRE(ucn_i_security_retire_fenced(&authority.security,old_session[1])==UCN_OK);
    REQUIRE(ucn_i_security_session_get(&device.security,old_session[0],&retired_view)!=UCN_OK);
    return 0;
}
#endif
int main(void)
{
#if UCN_PERSIST_DOMAIN_COUNT >= 3U
    if (dynamic_roundtrip()) return 1;
    if(hot_cycle(0U) || hot_cycle(1U)) return 1;
    REQUIRE(ucn_native_stop(application[0])==UCN_OK);
    REQUIRE(ucn_i_native_dynamic_stage(&dynamic[0],0U,&cap,clock_now)==UCN_ERR_STATE);
    REQUIRE(ucn_i_native_dynamic_commit(&dynamic[0],0U,clock_now)==UCN_ERR_STATE);
    REQUIRE(ucn_i_native_dynamic_attach_reauth(&dynamic[0],0U,&renewal[0],clock_now)==UCN_ERR_STATE);
    REQUIRE(backend[0].endpoint_generation[0]==3U && !backend[0].endpoint_fenced[0]);
#else
    puts("Dynamic first JOIN requires three Foundation domains: default Nano unsupported");
#endif
    return 0;
}
