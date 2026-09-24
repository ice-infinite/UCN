#include "internal/ucn_realtime.h"

#include "internal/ucn_checked.h"

#include <limits.h>
#include <string.h>

#define SYNC_HANDLE_KIND UINT8_C(0x81)

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

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;
    for (index = 0U; index < length; ++index) if (bytes[index] != 0U) return true;
    return false;
}

static bool path_valid(const ucn_i_realtime_path_facts_t *p)
{
    return p != NULL && p->route_causal_id != 0U && p->route_generation != 0U &&
           p->session_generation != 0U && p->capability_generation != 0U &&
           p->forward_link_generation != 0U && p->reverse_link_generation != 0U &&
           p->forward_link_id != 0U && p->reverse_link_id != 0U &&
           bytes_nonzero(p->capability_digest, 16U) && p->authenticated == 1U &&
           p->frozen == 1U && p->directional == 1U &&
           p->asymmetry_known <= 1U &&
           (p->asymmetry_known == 0U || p->max_asymmetry_us != 0U);
}

static bool event_valid(const ucn_i_realtime_event_key_t *key,
                        uint8_t direction)
{
    return key != NULL && key->link_generation != 0U &&
           key->link_generation != UINT32_MAX && key->token != 0U &&
           key->token != UINT32_MAX && key->link_id != 0U &&
           key->direction == direction && key->reserved_zero == 0U;
}

static bool path_equal(const ucn_i_realtime_path_facts_t *left,
                       const ucn_i_realtime_path_facts_t *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static ucn_i_realtime_domain_slot_t *domain_slot(
    ucn_i_realtime_owner_t *owner, uint16_t domain_id,
    uint32_t domain_generation)
{
    uint16_t index;
    for (index = 0U; index < UCN_I_REALTIME_DOMAIN_COUNT; ++index) {
        ucn_i_realtime_domain_slot_t *slot = &owner->domains[index];
        if (slot->occupied != 0U && slot->config.clock_domain_id == domain_id &&
            slot->config.domain_generation == domain_generation) return slot;
    }
    return NULL;
}

static ucn_handle_t sync_handle(const ucn_i_realtime_owner_t *owner,
                                uint16_t slot, uint16_t generation)
{
    ucn_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = slot;
    handle.generation = generation;
    handle.object_kind = SYNC_HANDLE_KIND;
    return handle;
}

static ucn_i_realtime_sync_slot_t *sync_slot(
    ucn_i_realtime_owner_t *owner, ucn_handle_t handle)
{
    ucn_i_realtime_sync_slot_t *slot;
    if (handle.runtime_instance != owner->runtime_instance ||
        handle.owner_instance != owner->owner_instance ||
        handle.slot >= UCN_I_REALTIME_SYNC_COUNT || handle.generation == 0U ||
        handle.object_kind != SYNC_HANDLE_KIND || handle.reserved_zero != 0U) return NULL;
    slot = &owner->sync[handle.slot];
    return slot->occupied != 0U && slot->handle_generation == handle.generation ?
               slot : NULL;
}

static uint16_t free_releases(const ucn_i_realtime_owner_t *owner)
{
    uint16_t index, count = 0U;
    for (index = 0U; index < UCN_I_REALTIME_RELEASE_COUNT; ++index)
        if (owner->releases[index].occupied == 0U &&
            owner->releases[index].generation != UINT16_MAX) ++count;
    return count;
}

static ucn_result_t enqueue_release(ucn_i_realtime_owner_t *owner,
                                    const ucn_i_realtime_event_key_t *key)
{
    uint16_t index;
    if (key->token == 0U) return UCN_OK;
    for (index = 0U; index < UCN_I_REALTIME_RELEASE_COUNT; ++index) {
        ucn_i_realtime_release_slot_t *slot = &owner->releases[index];
        if (slot->occupied == 0U && slot->generation != UINT16_MAX) {
            ++slot->generation;
            slot->key = *key; slot->occupied = 1U; return UCN_OK;
        }
    }
    return UCN_ERR_NO_SPACE;
}

static ucn_result_t clear_sync_with_releases(
    ucn_i_realtime_owner_t *owner, ucn_i_realtime_sync_slot_t *slot)
{
    uint16_t generation = slot->handle_generation;
    ucn_result_t result = enqueue_release(owner, &slot->t2);
    if (result != UCN_OK) return result;
    result = enqueue_release(owner, &slot->t3);
    if (result != UCN_OK) return result;
    memset(slot, 0, sizeof(*slot));
    slot->handle_generation = generation;
    return UCN_OK;
}

ucn_result_t ucn_i_realtime_sync_begin(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_sync_announce_t *announce,
    ucn_handle_t *sync_out)
{
    ucn_i_realtime_domain_slot_t *domain;
    ucn_i_realtime_sync_slot_t *slot = NULL;
    uint16_t index;
    ucn_result_t result;
    if (owner == NULL || announce == NULL || sync_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), announce,
                             sizeof(*announce)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), sync_out,
                             sizeof(*sync_out)) ||
        ucn_i_ranges_overlap(announce, sizeof(*announce), sync_out,
                             sizeof(*sync_out)) ||
        announce->clock_domain_id == 0U ||
        announce->domain_generation == 0U || announce->sync_sequence == 0U ||
        announce->t2_member_rx_us == 0U || announce->t2_uncertainty_us == 0U ||
        announce->absolute_deadline_us <= announce->t2_member_rx_us ||
        announce->reserved_zero != 0U || !path_valid(&announce->path) ||
        !event_valid(&announce->t2_member_rx, UCN_I_REALTIME_EVENT_RX) ||
        announce->t2_member_rx.link_id != announce->path.forward_link_id ||
        announce->t2_member_rx.link_generation !=
            announce->path.forward_link_generation) return UCN_ERR_ARGUMENT;
    result = lock_owner(owner); if (result != UCN_OK) return result;
    if (owner->next_sync_generation == UINT16_MAX) {
        unlock_owner(owner); return UCN_ERR_EXHAUSTED;
    }
    domain = domain_slot(owner, announce->clock_domain_id,
                         announce->domain_generation);
    if (domain == NULL || domain->phase == UCN_I_REALTIME_FAULT ||
        domain->phase == UCN_I_REALTIME_GENERATION_PENDING ||
        !path_equal(&domain->config.path, &announce->path)) {
        unlock_owner(owner); return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_REALTIME_SYNC_COUNT; ++index) {
        if (owner->sync[index].occupied != 0U &&
            owner->sync[index].clock_domain_id == announce->clock_domain_id) {
            if (announce->sync_sequence <= owner->sync[index].sync_sequence) {
                unlock_owner(owner); return UCN_ERR_REPLAY;
            }
            if (free_releases(owner) <
                (uint16_t)(1U + (owner->sync[index].t3_bound != 0U ? 1U : 0U))) {
                unlock_owner(owner); return UCN_ERR_NO_SPACE;
            }
            result = clear_sync_with_releases(owner, &owner->sync[index]);
            if (result != UCN_OK) { unlock_owner(owner); return result; }
            slot = &owner->sync[index]; break;
        }
    }
    if (slot == NULL) {
        for (index = 0U; index < UCN_I_REALTIME_SYNC_COUNT; ++index)
            if (owner->sync[index].occupied == 0U) { slot=&owner->sync[index]; break; }
    }
    if (slot == NULL) {
        unlock_owner(owner); return UCN_ERR_NO_SPACE;
    }
    ++owner->next_sync_generation;
    if (owner->next_sync_generation == 0U) owner->next_sync_generation = 1U;
    memset(slot, 0, sizeof(*slot));
    slot->path=announce->path; slot->t2=announce->t2_member_rx;
    slot->t2_us=announce->t2_member_rx_us; slot->deadline_us=announce->absolute_deadline_us;
    slot->domain_generation=announce->domain_generation;
    slot->sync_sequence=announce->sync_sequence;
    slot->t2_uncertainty_us=announce->t2_uncertainty_us;
    slot->clock_domain_id=announce->clock_domain_id;
    slot->handle_generation=owner->next_sync_generation; slot->occupied=1U;
    *sync_out=sync_handle(owner,index,slot->handle_generation);
    unlock_owner(owner); return UCN_OK;
}

ucn_result_t ucn_i_realtime_sync_bind_t3(
    ucn_i_realtime_owner_t *owner, ucn_handle_t sync,
    const ucn_i_realtime_event_key_t *t3_event,
    uint64_t t3_member_tx_us, uint32_t t3_uncertainty_us)
{
    ucn_i_realtime_sync_slot_t *slot;
    ucn_result_t result;
    if (owner == NULL || !event_valid(t3_event, UCN_I_REALTIME_EVENT_TX) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), t3_event,
                             sizeof(*t3_event)) ||
        t3_member_tx_us == 0U || t3_uncertainty_us == 0U) return UCN_ERR_ARGUMENT;
    result=lock_owner(owner); if(result!=UCN_OK)return result;
    slot=sync_slot(owner,sync);
    if(slot==NULL || slot->t3_bound!=0U || t3_member_tx_us<slot->t2_us ||
       t3_event->link_id!=slot->path.reverse_link_id ||
       t3_event->link_generation!=slot->path.reverse_link_generation) {
        unlock_owner(owner); return UCN_ERR_STATE;
    }
    slot->t3=*t3_event; slot->t3_us=t3_member_tx_us;
    slot->t3_uncertainty_us=t3_uncertainty_us; slot->t3_bound=1U;
    unlock_owner(owner); return UCN_OK;
}

static bool offset_compute(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4,
                           int64_t *offset_out)
{
    int64_t a, b;
    if (t1 > INT64_MAX || t2 > INT64_MAX || t3 > INT64_MAX || t4 > INT64_MAX ||
        t4 < t1 || t3 < t2 || (t4-t1) < (t3-t2)) return false;
    a=(int64_t)t1-(int64_t)t2; b=(int64_t)t4-(int64_t)t3;
    if ((b>0 && a>INT64_MAX-b) || (b<0 && a<INT64_MIN-b)) return false;
    *offset_out=(a+b)/2;
    return true;
}

ucn_result_t ucn_i_realtime_sync_complete(
    ucn_i_realtime_owner_t *owner, ucn_handle_t sync,
    const ucn_i_realtime_sync_response_t *response,
    uint64_t member_receive_local_us)
{
    ucn_i_realtime_sync_slot_t *slot;
    ucn_i_realtime_sync_sample_t sample;
    ucn_i_realtime_domain_slot_t *domain;
    uint64_t uncertainty;
    uint32_t base_uncertainty;
    int64_t offset;
    ucn_result_t result, sample_result;
    if(owner==NULL || response==NULL || member_receive_local_us==0U ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),response,sizeof(*response)) ||
       response->reserved_zero!=0U ||
       response->t1_uncertainty_us==0U || response->t4_uncertainty_us==0U ||
       !path_valid(&response->path))return UCN_ERR_ARGUMENT;
    result=lock_owner(owner); if(result!=UCN_OK)return result;
    slot=sync_slot(owner,sync);
    if(slot==NULL || slot->t3_bound==0U ||
       member_receive_local_us<slot->t3_us ||
       member_receive_local_us>=slot->deadline_us ||
       response->clock_domain_id!=slot->clock_domain_id ||
       response->domain_generation!=slot->domain_generation ||
       response->sync_sequence!=slot->sync_sequence ||
       !path_equal(&response->path,&slot->path) || free_releases(owner)<2U ||
       !offset_compute(response->t1_master_tx_us,slot->t2_us,slot->t3_us,
                       response->t4_master_rx_us,&offset)) {
        unlock_owner(owner); return UCN_ERR_STATE;
    }
    domain=domain_slot(owner,slot->clock_domain_id,slot->domain_generation);
    if(domain==NULL){unlock_owner(owner);return UCN_ERR_STATE;}
    if (ucn_i_realtime_uncertainty_aggregate(
            &domain->config.base_uncertainty, &base_uncertainty) != UCN_OK) {
        unlock_owner(owner); return UCN_ERR_STATE;
    }
    uncertainty=(uint64_t)base_uncertainty+slot->t2_uncertainty_us+
                slot->t3_uncertainty_us+response->t1_uncertainty_us+
                response->t4_uncertainty_us;
    if(uncertainty>UINT32_MAX){
        unlock_owner(owner);return UCN_ERR_EXHAUSTED;
    }
    memset(&sample,0,sizeof(sample)); sample.path=slot->path;
    sample.offset_us=offset; sample.sample_local_us=slot->t3_us;
    sample.uncertainty_us=(uint32_t)uncertainty;
    sample.clock_domain_id=slot->clock_domain_id;
    sample.valid_sync_sample=slot->path.asymmetry_known;
    sample_result=ucn_i_realtime_p_accept_sample_locked(owner,&sample);
    result = clear_sync_with_releases(owner,slot);
    if (result != UCN_OK) { unlock_owner(owner); return result; }
    unlock_owner(owner); return sample_result;
}

ucn_result_t ucn_i_realtime_release_peek(
    ucn_i_realtime_owner_t *owner,
    ucn_i_realtime_release_view_t *release_out)
{
    uint16_t scanned;
    ucn_result_t result;
    if(owner==NULL || release_out==NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),release_out,
                            sizeof(*release_out)))return UCN_ERR_ARGUMENT;
    result=lock_owner(owner); if(result!=UCN_OK)return result;
    for(scanned=0U;scanned<UCN_I_REALTIME_RELEASE_COUNT;++scanned){
        uint16_t index=(uint16_t)((owner->release_cursor+scanned)%UCN_I_REALTIME_RELEASE_COUNT);
        if(owner->releases[index].occupied!=0U){
            ucn_i_realtime_release_view_t view; memset(&view,0,sizeof(view));
            view.key=owner->releases[index].key; view.slot=index;
            view.generation=owner->releases[index].generation;
            *release_out=view; owner->release_cursor=(uint16_t)((index+1U)%UCN_I_REALTIME_RELEASE_COUNT);
            unlock_owner(owner);return UCN_OK;
        }
    }
    unlock_owner(owner);return UCN_ERR_NOT_FOUND;
}

ucn_result_t ucn_i_realtime_release_ack(
    ucn_i_realtime_owner_t *owner,
    const ucn_i_realtime_release_view_t *release)
{
    ucn_i_realtime_release_slot_t *slot; ucn_result_t result;
    if(owner==NULL || release==NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),release,sizeof(*release)) ||
       release->slot>=UCN_I_REALTIME_RELEASE_COUNT ||
       release->generation==0U)return UCN_ERR_ARGUMENT;
    result=lock_owner(owner);if(result!=UCN_OK)return result;
    slot=&owner->releases[release->slot];
    if(slot->occupied==0U || slot->generation!=release->generation ||
       memcmp(&slot->key,&release->key,sizeof(slot->key))!=0){
        unlock_owner(owner);return UCN_ERR_STATE;
    }
    memset(&slot->key,0,sizeof(slot->key));slot->occupied=0U;
    unlock_owner(owner);return UCN_OK;
}

ucn_result_t ucn_i_realtime_p_sync_step_locked(
    ucn_i_realtime_owner_t *owner, uint64_t now_us)
{
    uint16_t index;
    for(index=0U;index<UCN_I_REALTIME_SYNC_COUNT;++index){
        ucn_i_realtime_sync_slot_t *slot=&owner->sync[index];
        uint16_t needed;
        if(slot->occupied==0U || now_us<slot->deadline_us)continue;
        needed=(uint16_t)(1U+(slot->t3_bound!=0U?1U:0U));
        if(free_releases(owner)<needed)return UCN_ERR_NO_SPACE;
        if (clear_sync_with_releases(owner,slot) != UCN_OK)
            return UCN_ERR_NO_SPACE;
    }
    return UCN_OK;
}
