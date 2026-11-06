#include "internal/ucn_admission.h"
#include "internal/ucn_checked.h"
#include "internal/ucn_control_wire.h"
#include <string.h>

/* EN: Scalar BE helpers; all caller ranges are checked before first write.
 * 中文：大端标量辅助；首次写入前已验证所有调用方范围。 */
static void put(uint8_t *out, uint64_t value, uint8_t n)
{
    uint8_t i;
    for (i = 0U; i < n; ++i) out[i] = (uint8_t)(value >> ((n - i - 1U) * 8U));
}
static uint64_t get(const uint8_t *in, uint8_t n)
{
    uint64_t value = 0U;
    uint8_t i;
    for (i = 0U; i < n; ++i) value = (value << 8U) | in[i];
    return value;
}
static bool nonzero(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0U; i < n; ++i) if (p[i] != 0U) return true;
    return false;
}
/* EN: Only registered JOIN stages; local Abort has no Wire alias.
 * 中文：仅登记JOIN阶段；本地Abort没有线上别名。 */
static bool event_phase(uint16_t opcode, uint8_t phase)
{
    return (opcode == UCN_I_OPCODE_IDENTITY_CHALLENGE && phase == 2U) ||
           (opcode == UCN_I_OPCODE_IDENTITY_RESPONSE && phase == 3U) ||
           (opcode == UCN_I_OPCODE_ADDRESS_OFFER && phase == 4U) ||
           (opcode == UCN_I_OPCODE_DEVICE_COMMIT && phase == 5U) ||
           (opcode == UCN_I_OPCODE_FINAL_COMMIT && phase == 6U);
}
static void transcript_write(const ucn_i_admission_transcript_t *v, uint8_t *out)
{
    put(out + 0U, v->protocol_version, 1U);
    put(out + 1U, v->bootstrap_header_contract, 2U);
    out[3] = 1U;
    out[4] = 1U;
    memcpy(out + 5U, v->joining_device_principal, 16U);
    memcpy(out + 21U, v->joining_device_identity_digest, 16U);
    memcpy(out + 37U, v->authority_principal, 16U);
    put(out + 53U, v->authority_generation, 4U);
    put(out + 57U, v->device_nonce, 8U);
    put(out + 65U, v->authority_nonce, 8U);
    put(out + 73U, v->transaction_id, 8U);
    put(out + 81U, v->lease_freshness_challenge_nonce, 8U);
    put(out + 89U, v->realm_id, 4U);
    put(out + 93U, v->proposed_address, 4U);
    put(out + 97U, v->address_binding_generation, 4U);
    put(out + 101U, v->authority_address, 4U);
    put(out + 105U, v->authority_binding_generation, 4U);
    put(out + 109U, v->selected_link_id, 2U);
    memcpy(out + 111U, v->binding_lease_id, 16U);
    put(out + 127U, v->binding_lease_duration_us, 8U);
    put(out + 135U, v->authority_lease_sequence, 8U);
    put(out + 143U, v->authority_lease_duration_us, 8U);
    put(out + 151U, v->freshness_max_remaining_lease_us, 8U);
    memcpy(out + 159U, v->durable_fence_token, 16U);
    memcpy(out + 175U, v->allocation_high_water_digest, 16U);
    memcpy(out + 191U, v->quorum_config_digest, 32U);
    memcpy(out + 223U, v->signer_set_digest, 32U);
    memcpy(out + 255U, v->threshold_proof_digest, 32U);
    memcpy(out + 287U, v->freshness_proof_transcript_hash, 32U);
    put(out + 319U, v->authority_signer_count, 2U);
    put(out + 321U, v->authority_quorum_threshold, 2U);
    put(out + 323U, v->binding_mode, 1U);
    put(out + 324U, v->selected_hop_suite, 1U);
    put(out + 325U, v->selected_hop_key_id, 2U);
    put(out + 327U, v->selected_hop_key_generation, 4U);
    put(out + 331U, v->selected_e2e_mode, 1U);
    put(out + 332U, v->selected_e2e_suite, 1U);
    put(out + 333U, v->selected_e2e_key_id, 2U);
    put(out + 335U, v->selected_e2e_key_generation, 4U);
    put(out + 339U, v->selected_session_generation, 4U);
    put(out + 343U, v->selected_link_generation, 4U);
    memcpy(out + 347U, v->prior_messages_hash, 32U);
}
static void transcript_read(const uint8_t *in, ucn_i_admission_transcript_t *v)
{
    memset(v, 0, sizeof(*v));
    v->protocol_version = (uint8_t)get(in + 0U, 1U);
    v->bootstrap_header_contract = (uint16_t)get(in + 1U, 2U);
    memcpy(v->joining_device_principal, in + 5U, 16U);
    memcpy(v->joining_device_identity_digest, in + 21U, 16U);
    memcpy(v->authority_principal, in + 37U, 16U);
    v->authority_generation = (uint32_t)get(in + 53U, 4U);
    v->device_nonce = (uint64_t)get(in + 57U, 8U);
    v->authority_nonce = (uint64_t)get(in + 65U, 8U);
    v->transaction_id = (uint64_t)get(in + 73U, 8U);
    v->lease_freshness_challenge_nonce = (uint64_t)get(in + 81U, 8U);
    v->realm_id = (uint32_t)get(in + 89U, 4U);
    v->proposed_address = (uint32_t)get(in + 93U, 4U);
    v->address_binding_generation = (uint32_t)get(in + 97U, 4U);
    v->authority_address = (uint32_t)get(in + 101U, 4U);
    v->authority_binding_generation = (uint32_t)get(in + 105U, 4U);
    v->selected_link_id = (uint16_t)get(in + 109U, 2U);
    memcpy(v->binding_lease_id, in + 111U, 16U);
    v->binding_lease_duration_us = (uint64_t)get(in + 127U, 8U);
    v->authority_lease_sequence = (uint64_t)get(in + 135U, 8U);
    v->authority_lease_duration_us = (uint64_t)get(in + 143U, 8U);
    v->freshness_max_remaining_lease_us = (uint64_t)get(in + 151U, 8U);
    memcpy(v->durable_fence_token, in + 159U, 16U);
    memcpy(v->allocation_high_water_digest, in + 175U, 16U);
    memcpy(v->quorum_config_digest, in + 191U, 32U);
    memcpy(v->signer_set_digest, in + 223U, 32U);
    memcpy(v->threshold_proof_digest, in + 255U, 32U);
    memcpy(v->freshness_proof_transcript_hash, in + 287U, 32U);
    v->authority_signer_count = (uint16_t)get(in + 319U, 2U);
    v->authority_quorum_threshold = (uint16_t)get(in + 321U, 2U);
    v->binding_mode = (uint8_t)get(in + 323U, 1U);
    v->selected_hop_suite = (uint8_t)get(in + 324U, 1U);
    v->selected_hop_key_id = (uint16_t)get(in + 325U, 2U);
    v->selected_hop_key_generation = (uint32_t)get(in + 327U, 4U);
    v->selected_e2e_mode = (uint8_t)get(in + 331U, 1U);
    v->selected_e2e_suite = (uint8_t)get(in + 332U, 1U);
    v->selected_e2e_key_id = (uint16_t)get(in + 333U, 2U);
    v->selected_e2e_key_generation = (uint32_t)get(in + 335U, 4U);
    v->selected_session_generation = (uint32_t)get(in + 339U, 4U);
    v->selected_link_generation = (uint32_t)get(in + 343U, 4U);
    memcpy(v->prior_messages_hash, in + 347U, 32U);
}
ucn_result_t ucn_i_admission_logical_encode(
    uint16_t opcode, ucn_i_admission_phase_t phase,
    const ucn_i_admission_transcript_t *v, const ucn_i_admission_evidence_t *e,
    uint8_t *out, size_t capacity, size_t *written)
{
    size_t n;
    if (out == NULL || written == NULL || v == NULL || e == NULL ||
        !event_phase(opcode, phase) || !ucn_i_admission_transcript_valid(v, phase) ||
        e->length == 0U || e->length > UCN_I_ADMISSION_EVIDENCE_BYTES ||
        !nonzero(e->bytes, e->length) ||
        nonzero(e->reserved_zero, sizeof(e->reserved_zero))) return UCN_ERR_ARGUMENT;
    n = UCN_I_ADMISSION_TRANSCRIPT_BYTES + 1U + e->length;
    if (ucn_i_ranges_overlap(v, sizeof(*v), out, capacity) ||
        ucn_i_ranges_overlap(e, sizeof(*e), out, capacity) ||
        ucn_i_ranges_overlap(v, sizeof(*v), written, sizeof(*written)) ||
        ucn_i_ranges_overlap(e, sizeof(*e), written, sizeof(*written)) ||
        ucn_i_ranges_overlap(out, capacity, written, sizeof(*written))) return UCN_ERR_ARGUMENT;
    if (capacity < n) return UCN_ERR_NO_SPACE;
    transcript_write(v, out);
    out[379] = e->length;
    memcpy(out + 380U, e->bytes, e->length);
    *written = n;
    return UCN_OK;
}
ucn_result_t ucn_i_admission_logical_decode(
    uint16_t opcode, ucn_i_admission_phase_t phase, const uint8_t *in, size_t n,
    ucn_i_admission_transcript_t *out, ucn_i_admission_evidence_t *eout,
    ucn_i_admission_wire_workspace_t *work)
{
    if (in == NULL || out == NULL || eout == NULL || work == NULL ||
        ucn_i_ranges_overlap(in, n, out, sizeof(*out)) ||
        ucn_i_ranges_overlap(in, n, eout, sizeof(*eout)) ||
        ucn_i_ranges_overlap(out, sizeof(*out), eout, sizeof(*eout)) ||
        ucn_i_ranges_overlap(work, sizeof(*work), in, n) ||
        ucn_i_ranges_overlap(work, sizeof(*work), out, sizeof(*out)) ||
        ucn_i_ranges_overlap(work, sizeof(*work), eout, sizeof(*eout))) return UCN_ERR_ARGUMENT;
    if (!event_phase(opcode, phase) || n < 381U || n > 508U ||
        in[3] != 1U || in[4] != 1U || in[379] == 0U ||
        in[379] > 128U || n != 380U + in[379] ||
        !nonzero(in + 380U, in[379])) return UCN_ERR_MALFORMED;
    transcript_read(in, &work->transcript);
    if (!ucn_i_admission_transcript_valid(&work->transcript, phase)) return UCN_ERR_MALFORMED;
    memset(&work->evidence, 0, sizeof(work->evidence));
    work->evidence.length = in[379];
    memcpy(work->evidence.bytes, in + 380U, work->evidence.length);
    *out = work->transcript;
    *eout = work->evidence;
    return UCN_OK;
}
typedef struct fragment_shape {
    uint64_t transaction_id;
    const uint8_t *identity;
    uint16_t total_length, fragment_offset, fragment_length;
    uint8_t phase, fragment_index, fragment_count, chunk_bytes;
} fragment_shape_t;

static bool fragment_shape_valid(const fragment_shape_t *v)
{
    size_t count, offset, n, i;
    bool not_all_ff = false;
    if (v->phase < 2U || v->phase > 6U ||
        v->chunk_bytes < 32U || v->chunk_bytes > 128U ||
        v->total_length < 381U || v->total_length > 508U ||
        v->transaction_id == 0U || !nonzero(v->identity,16U)) return false;
    for (i=0U;i<16U;++i) not_all_ff = not_all_ff || v->identity[i] != 255U;
    if (!not_all_ff) return false;
    count=((size_t)v->total_length+v->chunk_bytes-1U)/v->chunk_bytes;
    if (count>16U || v->fragment_count!=count || v->fragment_index>=count) return false;
    offset=(size_t)v->fragment_index*v->chunk_bytes;
    n=v->total_length-offset;
    if(n>v->chunk_bytes)n=v->chunk_bytes;
    return v->fragment_offset==offset && v->fragment_length==n;
}

static bool fragment_valid(const ucn_i_admission_fragment_t *v)
{
    fragment_shape_t shape;
    if(v==NULL)return false;
    shape.transaction_id=v->transaction_id;shape.identity=v->identity_digest;
    shape.total_length=v->total_length;shape.fragment_offset=v->fragment_offset;
    shape.fragment_length=v->fragment_length;shape.phase=v->phase;
    shape.fragment_index=v->fragment_index;shape.fragment_count=v->fragment_count;
    shape.chunk_bytes=v->chunk_bytes;
    return fragment_shape_valid(&shape) &&
        !nonzero(v->data+v->fragment_length,128U-v->fragment_length);
}
ucn_result_t ucn_i_admission_fragment_encode(const ucn_i_admission_fragment_t *v,
    uint8_t *out, size_t capacity, size_t *written)
{
    size_t n;
    if (out == NULL || written == NULL || !fragment_valid(v) ||
        ucn_i_ranges_overlap(v, sizeof(*v), out, capacity) ||
        ucn_i_ranges_overlap(v, sizeof(*v), written, sizeof(*written)) ||
        ucn_i_ranges_overlap(out, capacity, written, sizeof(*written))) return UCN_ERR_ARGUMENT;
    n = 36U + v->fragment_length;
    if (capacity < n) return UCN_ERR_NO_SPACE;
    out[0] = 1U; out[1] = 1U; out[2] = v->phase;
    out[3] = v->fragment_index; out[4] = v->fragment_count; out[5] = v->chunk_bytes;
    put(out + 6U, v->total_length, 2U); put(out + 8U, v->fragment_offset, 2U);
    put(out + 10U, v->fragment_length, 2U); put(out + 12U, v->transaction_id, 8U);
    memcpy(out + 20U, v->identity_digest, 16U);
    memcpy(out + 36U, v->data, v->fragment_length);
    *written = n;
    return UCN_OK;
}
ucn_result_t ucn_i_admission_fragment_decode(const uint8_t *in, size_t n,
                                             ucn_i_admission_fragment_t *out)
{
    fragment_shape_t shape;
    if(in==NULL || out==NULL || ucn_i_ranges_overlap(in,n,out,sizeof(*out)))
        return UCN_ERR_ARGUMENT;
    if(n<37U || n>164U || in[0]!=1U || in[1]!=1U) return UCN_ERR_MALFORMED;
    shape.phase=in[2];shape.fragment_index=in[3];shape.fragment_count=in[4];shape.chunk_bytes=in[5];
    shape.total_length=(uint16_t)get(in+6U,2U);
    shape.fragment_offset=(uint16_t)get(in+8U,2U);
    shape.fragment_length=(uint16_t)get(in+10U,2U);
    shape.transaction_id=get(in+12U,8U);shape.identity=in+20U;
    if(n!=36U+shape.fragment_length || !fragment_shape_valid(&shape))
        return UCN_ERR_MALFORMED;
    /* EN: Validate the small header before writing the final bounded DTO.
     * 中文：小型头部预检完成后才发布正文，失败时不触碰输出。 */
    memset(out,0,sizeof(*out));
    out->phase=shape.phase;out->fragment_index=shape.fragment_index;
    out->fragment_count=shape.fragment_count;out->chunk_bytes=shape.chunk_bytes;
    out->total_length=shape.total_length;out->fragment_offset=shape.fragment_offset;
    out->fragment_length=shape.fragment_length;out->transaction_id=shape.transaction_id;
    memcpy(out->identity_digest,shape.identity,16U);
    memcpy(out->data,in+36U,shape.fragment_length);
    return UCN_OK;
}

ucn_result_t ucn_i_admission_session_seed(
    const ucn_i_admission_transcript_t *t, uint64_t start, uint64_t lifetime,
    uint64_t deadline, ucn_i_admission_session_seed_t *out,
    ucn_i_admission_seed_workspace_t *w)
{
    static const uint8_t domain[] = "UCN6-JOIN-SESSION-V1";
    size_t written;
    ucn_result_t result;
    if (t == NULL || out == NULL || w == NULL || start == 0U || lifetime == 0U ||
        deadline <= start || lifetime > UINT64_MAX - start ||
        ucn_i_ranges_overlap(t, sizeof(*t), out, sizeof(*out)) ||
        ucn_i_ranges_overlap(t, sizeof(*t), w, sizeof(*w)) ||
        ucn_i_ranges_overlap(out, sizeof(*out), w, sizeof(*w))) return UCN_ERR_ARGUMENT;
    if (!ucn_i_admission_transcript_valid(t, UCN_I_ADMISSION_FINAL_DURABLE) ||
        !((t->selected_e2e_mode == 1U && t->selected_e2e_suite == 1U) ||
          (t->selected_e2e_mode == 2U && (t->selected_e2e_suite == 2U || t->selected_e2e_suite == 3U))) ||
        t->selected_hop_suite != 16U ||
        t->selected_e2e_key_id >= UINT16_MAX - 1U ||
        t->selected_hop_key_id >= UINT16_MAX - 1U) return UCN_ERR_ARGUMENT;
    memset(w, 0, sizeof(*w));
    w->evidence.length = 1U;
    w->evidence.bytes[0] = 1U;
    result = ucn_i_admission_logical_encode(UCN_I_OPCODE_FINAL_COMMIT,
        UCN_I_ADMISSION_FINAL_DURABLE, t, &w->evidence, w->logical,
        sizeof(w->logical), &written);
    if (result != UCN_OK) return result;
    memcpy(w->digest_input, domain, sizeof(domain) - 1U);
    put(w->digest_input + sizeof(domain) - 1U, UCN_I_ADMISSION_TRANSCRIPT_BYTES, 4U);
    memcpy(w->digest_input + sizeof(domain) + 3U, w->logical,
           UCN_I_ADMISSION_TRANSCRIPT_BYTES);
    result = ucn_i_sha256_256(w->digest_input, sizeof(w->digest_input),
                             w->seed.transcript_digest, &w->digest);
    if (result != UCN_OK) return result;
    w->seed.expires_at_us = start + lifetime;
    if (deadline < w->seed.expires_at_us) w->seed.expires_at_us = deadline;
    w->seed.origin_forward_key_id = t->selected_e2e_key_id;
    w->seed.origin_reverse_key_id = (uint16_t)(t->selected_e2e_key_id + 1U);
    w->seed.hop_forward_key_id = t->selected_hop_key_id;
    w->seed.hop_reverse_key_id = (uint16_t)(t->selected_hop_key_id + 1U);
    *out = w->seed;
    return UCN_OK;
}
