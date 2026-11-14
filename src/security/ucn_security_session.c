#include "internal/ucn_security.h"
#include "internal/ucn_stack.h"

#include "internal/ucn_checked.h"

#include <string.h>

#define UCN_I_SECURITY_MAGIC UINT32_C(0x5543534F)
#define UCN_I_SECURITY_PROVIDER_API UINT16_C(1)
#define UCN_I_SECURITY_CALLBACK_VERIFY UINT16_C(0x0501)
#define UCN_I_SECURITY_CALLBACK_PROTECT UINT16_C(0x0502)
#define UCN_I_SECURITY_CALLBACK_OPEN UINT16_C(0x0503)

static bool bytes_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool bytes_zero(const uint8_t *bytes, size_t length)
{
    return !bytes_nonzero(bytes, length);
}

static uint32_t address_limit(uint8_t width)
{
    if (width == 4U) {
        return UINT32_MAX;
    }
    return (UINT32_C(1) << (width * 8U)) - UINT32_C(1);
}

static bool binding_valid(const ucn_i_security_binding_t *binding,
                          uint8_t width)
{
    uint32_t limit;

    if (binding == NULL || width == 0U || width > 4U) {
        return false;
    }
    limit = address_limit(width);
    return binding->address != 0U && binding->address < limit &&
           binding->binding_generation != 0U &&
           bytes_nonzero(binding->principal,
                         UCN_I_SECURITY_PRINCIPAL_BYTES);
}

static bool selector_zero(const ucn_i_security_key_selector_t *selector)
{
    static const ucn_i_security_key_selector_t zero = {0};

    return memcmp(selector, &zero, sizeof(zero)) == 0;
}

static bool selector_equal(const ucn_i_security_key_selector_t *left,
                           const ucn_i_security_key_selector_t *right)
{
    return memcmp(left, right, sizeof(*left)) == 0;
}

static bool origin_selector_valid(
    const ucn_i_security_key_selector_t *selector,
    uint8_t origin_level)
{
    uint8_t expected_suite = origin_level == UCN_I_SECURITY_AUTHENTICATED ?
                                 1U :
                                 selector->suite_id;

    return selector->reserved_zero == 0U && selector->key_id != 0U &&
           selector->key_id != UINT16_MAX && selector->key_generation != 0U &&
           ((origin_level == UCN_I_SECURITY_AUTHENTICATED &&
             expected_suite == 1U && selector->suite_id == 1U) ||
            (origin_level == UCN_I_SECURITY_CONFIDENTIAL &&
             (selector->suite_id == 2U || selector->suite_id == 3U)));
}

static bool hop_selector_valid(
    const ucn_i_security_key_selector_t *selector)
{
    return selector->reserved_zero == 0U && selector->suite_id == 16U &&
           selector->key_id != 0U && selector->key_id != UINT16_MAX &&
           selector->key_generation != 0U;
}

static bool candidate_valid(const ucn_i_security_candidate_t *candidate,
                            uint8_t expected_width,
                            uint64_t now_us)
{
    if (candidate == NULL || candidate->reserved_zero != 0U ||
        candidate->address_width != expected_width || now_us == 0U ||
        candidate->expires_at_us == 0U || now_us >= candidate->expires_at_us ||
        !binding_valid(&candidate->local, expected_width) ||
        !binding_valid(&candidate->peer, expected_width) ||
        candidate->local.address == candidate->peer.address ||
        memcmp(candidate->local.principal, candidate->peer.principal,
               UCN_I_SECURITY_PRINCIPAL_BYTES) == 0 ||
        candidate->link_generation == 0U ||
        candidate->session_generation == 0U ||
        candidate->policy_generation == 0U ||
        !bytes_nonzero(candidate->origin_fingerprint,
                       UCN_I_SECURITY_FINGERPRINT_BYTES) ||
        !bytes_nonzero(candidate->hop_fingerprint,
                       UCN_I_SECURITY_FINGERPRINT_BYTES) ||
        !bytes_nonzero(candidate->transcript_digest,
                       UCN_I_SECURITY_TRANSCRIPT_BYTES) ||
        (candidate->origin_level != UCN_I_SECURITY_AUTHENTICATED &&
         candidate->origin_level != UCN_I_SECURITY_CONFIDENTIAL) ||
        !origin_selector_valid(&candidate->origin_tx,
                               candidate->origin_level) ||
        !origin_selector_valid(&candidate->origin_rx,
                               candidate->origin_level) ||
        selector_equal(&candidate->origin_tx, &candidate->origin_rx)) {
        return false;
    }
    if (candidate->hop_profile == UCN_I_HOP_PROFILE_H0 ||
        candidate->hop_profile == UCN_I_HOP_PROFILE_H2) {
        return selector_zero(&candidate->hop_tx) &&
               selector_zero(&candidate->hop_rx);
    }
    if (candidate->hop_profile == UCN_I_HOP_PROFILE_H1) {
        return hop_selector_valid(&candidate->hop_tx) &&
               hop_selector_valid(&candidate->hop_rx) &&
               !selector_equal(&candidate->hop_tx, &candidate->hop_rx);
    }
    return false;
}

static bool owner_valid(const ucn_i_security_owner_t *owner)
{
    return owner != NULL && owner->magic == UCN_I_SECURITY_MAGIC &&
           owner->schema == UCN_I_SECURITY_SCHEMA &&
           owner->runtime_instance != 0U && owner->owner_instance != 0U &&
           owner->persistence_business_owner_instance != 0U &&
           owner->provider_gate != NULL;
}

static ucn_result_t owner_lock(ucn_i_security_owner_t *owner)
{
    if (!owner_valid(owner)) {
        return UCN_ERR_STATE;
    }
    return owner->state_lock.enter(owner->state_lock.context);
}

static void owner_unlock(ucn_i_security_owner_t *owner)
{
    owner->state_lock.leave(owner->state_lock.context);
}

static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

static void write_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static void write_be64(uint8_t *out, uint64_t value)
{
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        out[index] = (uint8_t)(value >> ((7U - index) * 8U));
    }
}

static void write_address_be(uint8_t *out, uint32_t value, uint8_t bytes)
{
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        uint8_t shift = (uint8_t)((bytes - index - 1U) * 8U);

        out[index] = (uint8_t)(value >> shift);
    }
}

static size_t c1_header_bytes(uint8_t address_width)
{
    return 9U + (2U * (size_t)address_width);
}

static uint16_t read_be16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8U) | in[1]);
}

static uint32_t read_be32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) |
           ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
}

static uint64_t read_be64(const uint8_t *in)
{
    uint64_t value = 0U;
    uint8_t index;

    for (index = 0U; index < 8U; ++index) {
        value = (value << 8U) | in[index];
    }
    return value;
}

/* EN: Structural peer reauth validation; no owner or crypto side effects.
 * 中文：邻接重新认证结构校验，无Owner或密码副作用。 */
static bool peer_reauth_valid(const uint8_t *b)
{
    size_t i, offset;
    uint32_t limit, previous,remote,origin,hop;
    if (b == NULL || b[0] != 2U || b[1] > 3U || b[2] < 1U || b[2] > 4U ||
        b[3] != 0U || b[65] != 16U || b[70] != 0U || b[71] != 0U ||
        read_be32(b + 4U) == 0U || read_be32(b + 4U) == UINT32_MAX) return false;
    limit = address_limit(b[2]);
    for (offset = 8U; offset <= 32U; offset += 24U) {
        bool all_ff = true;
        if (read_be32(b + offset) == 0U || read_be32(b + offset) >= limit ||
            read_be32(b + offset + 4U) == 0U || !bytes_nonzero(b + offset + 8U, 16U)) return false;
        for (i = 0U; i < 16U; ++i) if (b[offset + 8U + i] != 255U) all_ff = false;
        if (all_ff) return false;
    }
    previous = read_be32(b + 56U);
    remote=read_be32(b+164U);origin=read_be32(b+156U);hop=read_be32(b+160U);
    if(read_be32(b+168U)>origin)origin=read_be32(b+168U);
    if(read_be32(b+172U)>hop)hop=read_be32(b+172U);
    if (read_be32(b + 8U) == read_be32(b + 32U) || memcmp(b + 16U,b + 40U,16U) == 0 ||
        previous == 0U || previous == UINT32_MAX || remote==UINT32_MAX ||
        read_be32(b+60U)!=(previous>remote?previous:remote)+1U || origin==UINT32_MAX || hop==UINT32_MAX ||
        read_be32(b+156U)==0U || read_be32(b+160U)==0U ||
        read_be32(b+72U)!=origin+1U || read_be32(b+76U)!=hop+1U ||
        b[64] < 1U || b[64] > 3U || read_be16(b + 66U) == 0U || read_be16(b + 66U) >= 65534U ||
        read_be16(b + 68U) == 0U || read_be16(b + 68U) >= 65534U ||
        read_be32(b + 72U) == 0U || read_be32(b + 76U) == 0U ||
        read_be16(b + 80U) == 0U || read_be32(b + 84U) == 0U ||
        read_be64(b + 92U) == 0U || read_be64(b + 100U) == 0U ||
        read_be64(b + 116U) == 0U || read_be64(b + 116U) > INT64_MAX || !bytes_nonzero(b + 124U,32U)) return false;
    if (b[1] == 0U) return read_be16(b + 82U) == 0U && read_be32(b + 88U) == 0U && read_be64(b + 108U) == 0U && !bytes_nonzero(b+164U,44U);
    if(remote==0U||read_be32(b+168U)==0U||read_be32(b+172U)==0U||!bytes_nonzero(b+176U,32U))return false;
    if(previous==remote && (read_be32(b+156U)!=read_be32(b+168U)||read_be32(b+160U)!=read_be32(b+172U)||memcmp(b+124U,b+176U,32U)!=0))return false;
    if(previous>remote && (read_be32(b+156U)<=read_be32(b+168U)||read_be32(b+160U)<=read_be32(b+172U)))return false;
    if(remote>previous && (read_be32(b+168U)<=read_be32(b+156U)||read_be32(b+172U)<=read_be32(b+160U)))return false;
    return read_be16(b + 82U) != 0U && read_be32(b + 88U) != 0U && read_be64(b + 108U) != 0U;
}

ucn_result_t ucn_i_peer_reauth_decode(const uint8_t *input, size_t length,
                                    ucn_i_peer_reauth_record_t *output)
{
    if (input == NULL || output == NULL ||
        ucn_i_ranges_overlap(input,length,output,sizeof(*output))) return UCN_ERR_ARGUMENT;
    if (length != UCN_I_PEER_REAUTH_RECORD_BYTES || !peer_reauth_valid(input)) return UCN_ERR_MALFORMED;
    memcpy(output->canonical,input,length); return UCN_OK;
}

ucn_result_t ucn_i_peer_reauth_encode(const ucn_i_peer_reauth_record_t *input,
                                    uint8_t *output, size_t capacity)
{
    if (input == NULL || output == NULL ||
        ucn_i_ranges_overlap(input,sizeof(*input),output,capacity) || !peer_reauth_valid(input->canonical)) return UCN_ERR_ARGUMENT;
    if (capacity < UCN_I_PEER_REAUTH_RECORD_BYTES) return UCN_ERR_NO_SPACE;
    memcpy(output,input->canonical,UCN_I_PEER_REAUTH_RECORD_BYTES); return UCN_OK;
}

static bool peer_reauth_fragment_valid(const ucn_i_peer_reauth_fragment_t *v)
{
    size_t offset,expected,i;
    if (v == NULL || v->stage > 3U || v->transaction == 0U || v->total_bytes < 225U || v->total_bytes > 273U) return false;
    offset=(size_t)v->index*80U;
    if(offset >= v->total_bytes)return false;
    expected=v->total_bytes-offset;if(expected>80U)expected=80U;
    if(v->data_bytes!=expected)return false;
    for(i=expected;i<80U;++i)if(v->data[i]!=0U)return false;
    return true;
}
ucn_result_t ucn_i_peer_reauth_fragment_encode(const ucn_i_peer_reauth_fragment_t *v,
    uint8_t *out,size_t capacity,size_t *written)
{
    size_t i,n,offset;uint64_t transaction;
    if(out==NULL||written==NULL||!peer_reauth_fragment_valid(v)||
        ucn_i_ranges_overlap(v,sizeof(*v),out,capacity)||ucn_i_ranges_overlap(v,sizeof(*v),written,sizeof(*written))||
        ucn_i_ranges_overlap(out,capacity,written,sizeof(*written)))return UCN_ERR_ARGUMENT;
    n=18U+v->data_bytes;if(capacity<n)return UCN_ERR_NO_SPACE;
    offset=(size_t)v->index*80U;out[0]=1U;out[1]=v->stage;out[2]=v->index;out[3]=(uint8_t)((v->total_bytes+79U)/80U);
    out[4]=(uint8_t)(v->total_bytes>>8U);out[5]=(uint8_t)v->total_bytes;
    out[6]=(uint8_t)(offset>>8U);out[7]=(uint8_t)offset;out[8]=80U;out[9]=0U;
    transaction=v->transaction;for(i=0U;i<8U;++i){out[17U-i]=(uint8_t)transaction;transaction>>=8U;}
    memcpy(out+18U,v->data,v->data_bytes);*written=n;return UCN_OK;
}
ucn_result_t ucn_i_peer_reauth_fragment_decode(const uint8_t *in,size_t n,
    ucn_i_peer_reauth_fragment_t *out)
{
    ucn_i_peer_reauth_fragment_t value={0};
    if(in==NULL||out==NULL||ucn_i_ranges_overlap(in,n,out,sizeof(*out)))return UCN_ERR_ARGUMENT;
    if(n<19U||n>98U||in[0]!=1U||in[8]!=80U||in[9]!=0U)return UCN_ERR_MALFORMED;
    value.stage=in[1];value.index=in[2];value.total_bytes=read_be16(in+4U);value.transaction=read_be64(in+10U);
    if(in[3]!=(value.total_bytes+79U)/80U||read_be16(in+6U)!=(uint16_t)value.index*80U)return UCN_ERR_MALFORMED;
    value.data_bytes=(uint8_t)(n-18U);memcpy(value.data,in+18U,value.data_bytes);
    if(!peer_reauth_fragment_valid(&value))return UCN_ERR_MALFORMED;
    *out=value;return UCN_OK;
}

static uint32_t read_address_be(const uint8_t *in, uint8_t bytes)
{
    uint32_t value = 0U;
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        value = (value << 8U) | in[index];
    }
    return value;
}

static void encode_binding(const ucn_i_security_binding_t *binding,
                           uint8_t *output)
{
    write_be32(output, binding->address);
    write_be32(output + 4U, binding->binding_generation);
    memcpy(output + 8U, binding->principal,
           UCN_I_SECURITY_PRINCIPAL_BYTES);
}

static void encode_selector(const ucn_i_security_key_selector_t *selector,
                            uint8_t *output)
{
    memset(output, 0, 8U);
    output[0] = selector->suite_id;
    write_be16(output + 2U, selector->key_id);
    write_be32(output + 4U, selector->key_generation);
}

static void decode_binding(const uint8_t *input,
                           ucn_i_security_binding_t *binding)
{
    binding->address = read_be32(input);
    binding->binding_generation = read_be32(input + 4U);
    memcpy(binding->principal, input + 8U,
           UCN_I_SECURITY_PRINCIPAL_BYTES);
}

static void decode_selector(const uint8_t *input,
                            ucn_i_security_key_selector_t *selector)
{
    selector->suite_id = input[0];
    selector->reserved_zero = input[1];
    selector->key_id = read_be16(input + 2U);
    selector->key_generation = read_be32(input + 4U);
}

static uint64_t transition_fingerprint(const uint8_t *bytes, size_t length)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    size_t index;

    for (index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(0x00000100000001B3);
    }
    return hash;
}

ucn_result_t ucn_i_security_record_encode(
    uint32_t realm_id,
    const ucn_i_security_candidate_t *candidate,
    uint8_t output[UCN_I_SECURITY_RECORD_BYTES])
{
    if (output == NULL || realm_id == 0U ||
        !candidate_valid(candidate, candidate == NULL ? 0U :
                                      candidate->address_width,
                         1U) ||
        ucn_i_ranges_overlap(candidate, sizeof(*candidate), output,
                             UCN_I_SECURITY_RECORD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    memset(output, 0, UCN_I_SECURITY_RECORD_BYTES);
    memcpy(output, "UC6S", 4U);
    write_be16(output + 4U, UCN_I_SECURITY_RECORD_SCHEMA);
    output[6] = candidate->address_width;
    output[7] = candidate->origin_level;
    output[8] = candidate->hop_profile;
    write_be32(output + 12U, realm_id);
    encode_binding(&candidate->local, output + 16U);
    encode_binding(&candidate->peer, output + 40U);
    write_be32(output + 64U, candidate->link_generation);
    write_be32(output + 68U, candidate->session_generation);
    write_be32(output + 72U, candidate->policy_generation);
    write_be64(output + 76U, candidate->expires_at_us);
    encode_selector(&candidate->origin_tx, output + 84U);
    encode_selector(&candidate->origin_rx, output + 92U);
    encode_selector(&candidate->hop_tx, output + 100U);
    encode_selector(&candidate->hop_rx, output + 108U);
    memcpy(output + 116U, candidate->origin_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    memcpy(output + 132U, candidate->hop_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    memcpy(output + 148U, candidate->transcript_digest,
           UCN_I_SECURITY_TRANSCRIPT_BYTES);
    write_be32(output + 180U, candidate->hop_tx.key_generation);
    write_be32(output + 184U, candidate->hop_rx.key_generation);
    return UCN_OK;
}

ucn_result_t ucn_i_security_record_decode(
    const uint8_t input[UCN_I_SECURITY_RECORD_BYTES],
    ucn_i_security_codec_workspace_t *workspace,
    uint32_t *realm_id_out,
    ucn_i_security_candidate_t *candidate_out)
{
    ucn_i_security_candidate_t *candidate;
    uint32_t realm_id;

    if (input == NULL || workspace == NULL || realm_id_out == NULL ||
        candidate_out == NULL ||
        ucn_i_ranges_overlap(input, UCN_I_SECURITY_RECORD_BYTES, workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(input, UCN_I_SECURITY_RECORD_BYTES, realm_id_out,
                             sizeof(*realm_id_out)) ||
        ucn_i_ranges_overlap(input, UCN_I_SECURITY_RECORD_BYTES, candidate_out,
                             sizeof(*candidate_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), realm_id_out,
                             sizeof(*realm_id_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), candidate_out,
                             sizeof(*candidate_out)) ||
        ucn_i_ranges_overlap(realm_id_out, sizeof(*realm_id_out),
                             candidate_out, sizeof(*candidate_out))) {
        return UCN_ERR_ARGUMENT;
    }
    if (memcmp(input, "UC6S", 4U) != 0 ||
        read_be16(input + 4U) != UCN_I_SECURITY_RECORD_SCHEMA ||
        !bytes_zero(input + 9U, 3U) || !bytes_zero(input + 188U, 4U)) {
        return UCN_ERR_MALFORMED;
    }
    candidate = &workspace->candidate;
    memset(candidate, 0, sizeof(*candidate));
    candidate->address_width = input[6];
    candidate->origin_level = input[7];
    candidate->hop_profile = input[8];
    realm_id = read_be32(input + 12U);
    decode_binding(input + 16U, &candidate->local);
    decode_binding(input + 40U, &candidate->peer);
    candidate->link_generation = read_be32(input + 64U);
    candidate->session_generation = read_be32(input + 68U);
    candidate->policy_generation = read_be32(input + 72U);
    candidate->expires_at_us = read_be64(input + 76U);
    decode_selector(input + 84U, &candidate->origin_tx);
    decode_selector(input + 92U, &candidate->origin_rx);
    decode_selector(input + 100U, &candidate->hop_tx);
    decode_selector(input + 108U, &candidate->hop_rx);
    memcpy(candidate->origin_fingerprint, input + 116U,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    memcpy(candidate->hop_fingerprint, input + 132U,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    memcpy(candidate->transcript_digest, input + 148U,
           UCN_I_SECURITY_TRANSCRIPT_BYTES);
    if (realm_id == 0U ||
        !candidate_valid(candidate, candidate->address_width, 1U) ||
        read_be32(input + 180U) != candidate->hop_tx.key_generation ||
        read_be32(input + 184U) != candidate->hop_rx.key_generation) {
        return UCN_ERR_MALFORMED;
    }
    *realm_id_out = realm_id;
    *candidate_out = *candidate;
    return UCN_OK;
}

static bool acl_rule_valid(const ucn_i_security_acl_rule_t *rule)
{
    return bytes_nonzero(rule->peer_principal, UCN_I_SECURITY_PRINCIPAL_BYTES) &&
        bytes_nonzero(rule->context_fingerprint, UCN_I_SECURITY_FINGERPRINT_BYTES) &&
        rule->peer_binding_generation != 0U && rule->service_id != UINT16_MAX &&
        (rule->service_id != 0U || ucn_i_protocol_opcode_valid(rule->protocol_opcode)) &&
        (rule->service_id == 0U || rule->protocol_opcode == 0U || ucn_i_protocol_opcode_valid(rule->protocol_opcode)) &&
        (rule->direction == UCN_I_SECURITY_ACCESS_INBOUND || rule->direction == UCN_I_SECURITY_ACCESS_OUTBOUND) &&
        bytes_zero(rule->reserved_zero, sizeof(rule->reserved_zero));
}
static bool rules_valid(const ucn_i_security_config_t *config)
{
    uint16_t left;
    uint16_t right;

    if (config->domain_rules == NULL || config->domain_rule_count == 0U ||
        config->domain_rule_count > UCN_I_SECURITY_SESSION_COUNT) {
        return false;
    }
    for (left = 0U; left < config->domain_rule_count; ++left) {
        if (config->domain_rules[left].domain_id == 0U ||
            !bytes_nonzero(config->domain_rules[left].peer_principal,
                           UCN_I_SECURITY_PRINCIPAL_BYTES)) {
            return false;
        }
        for (right = (uint16_t)(left + 1U);
             right < config->domain_rule_count; ++right) {
            if (config->domain_rules[left].domain_id ==
                    config->domain_rules[right].domain_id ||
                memcmp(config->domain_rules[left].peer_principal,
                       config->domain_rules[right].peer_principal,
                       UCN_I_SECURITY_PRINCIPAL_BYTES) == 0) {
                return false;
            }
        }
    }
    if (config->acl_rules == NULL || config->acl_rule_count == 0U ||
        config->replay_reservation_lifetime_us == 0U) {
        return false;
    }
    for (left = 0U; left < config->acl_rule_count; ++left) {
        const ucn_i_security_acl_rule_t *rule = &config->acl_rules[left];

        if (!acl_rule_valid(rule)) {
            return false;
        }
        for (right = (uint16_t)(left + 1U);
             right < config->acl_rule_count; ++right) {
            if (memcmp(rule, &config->acl_rules[right], sizeof(*rule)) == 0) {
                return false;
            }
        }
    }
    return true;
}

ucn_result_t ucn_i_security_owner_init(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_config_t *config)
{
    ucn_result_t lock_result;

    if (owner == NULL || config == NULL ||
        config->struct_size != sizeof(*config) ||
        config->api_version != UCN_API_VERSION ||
        config->runtime_instance == 0U || config->realm_id == 0U ||
        config->owner_instance == 0U ||
        config->persistence_business_owner_instance == 0U ||
        config->address_width == 0U || config->address_width > 4U ||
        !rules_valid(config) ||
        config->provider.struct_size != sizeof(config->provider) ||
        config->provider.api_version != UCN_I_SECURITY_PROVIDER_API ||
        config->provider.verify_session == NULL ||
        config->provider.protect_origin == NULL ||
        config->provider.open_origin == NULL ||
        config->provider.protect_hop == NULL ||
        config->provider.verify_hop == NULL ||
        config->state_lock.struct_size != sizeof(config->state_lock) ||
        config->state_lock.api_version != UCN_I_LOCK_OPS_VERSION ||
        config->state_lock.enter == NULL || config->state_lock.leave == NULL ||
        config->provider_gate == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config, sizeof(*config)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config->domain_rules,
                             sizeof(config->domain_rules[0]) *
                                 config->domain_rule_count) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config->acl_rules,
                             sizeof(config->acl_rules[0]) *
                                 config->acl_rule_count) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), config->provider_gate,
                             sizeof(*config->provider_gate)) ||
        (config->state_lock.context != NULL &&
         ucn_i_ranges_overlap(owner, sizeof(*owner),
                              config->state_lock.context, 1U)) ||
        (config->provider.context != NULL &&
         ucn_i_ranges_overlap(owner, sizeof(*owner),
                               config->provider.context, 1U))) {
        return UCN_ERR_CONFIG;
    }
    lock_result = config->state_lock.enter(config->state_lock.context);
    if (lock_result != UCN_OK) {
        return UCN_ERR_STATE;
    }
    /* The external state lock is deliberately acquired before inspecting the
     * caller-owned storage.  This makes recursive/concurrent init serialize
     * with every live Owner operation without reading an initialization flag
     * from an uninitialized object. */
    if (!bytes_zero((const uint8_t *)owner, sizeof(*owner))) {
        config->state_lock.leave(config->state_lock.context);
        return UCN_ERR_STATE;
    }
    memset(owner, 0, sizeof(*owner));
    owner->magic = UCN_I_SECURITY_MAGIC;
    owner->runtime_instance = config->runtime_instance;
    owner->realm_id = config->realm_id;
    owner->next_operation_id = 1U;
    owner->schema = UCN_I_SECURITY_SCHEMA;
    owner->owner_instance = config->owner_instance;
    owner->persistence_business_owner_instance =
        config->persistence_business_owner_instance;
    owner->address_width = config->address_width;
    owner->domain_rules = config->domain_rules;
    owner->domain_rule_count = config->domain_rule_count;
    owner->acl_rules = config->acl_rules;
    owner->acl_rule_count = config->acl_rule_count;
    owner->replay_reservation_lifetime_us =
        config->replay_reservation_lifetime_us;
    owner->provider = config->provider;
    owner->state_lock = config->state_lock;
    owner->provider_gate = config->provider_gate;
    config->state_lock.leave(config->state_lock.context);
    return UCN_OK;
}

static uint16_t find_rule(const ucn_i_security_owner_t *owner,
                          const uint8_t principal[UCN_I_SECURITY_PRINCIPAL_BYTES])
{
    uint16_t index;

    for (index = 0U; index < owner->domain_rule_count; ++index) {
        if (memcmp(owner->domain_rules[index].peer_principal, principal,
                   UCN_I_SECURITY_PRINCIPAL_BYTES) == 0) {
            return index;
        }
    }
    return UINT16_MAX;
}

static uint16_t find_free_slot(const ucn_i_security_owner_t *owner)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        if (owner->sessions[index].occupied == 0U) {
            return index;
        }
    }
    return UINT16_MAX;
}

static bool handle_matches(const ucn_i_security_owner_t *owner,
                           const ucn_i_security_handle_t *handle,
                           uint16_t *slot_out)
{
    uint16_t slot = handle->slot;

    if (slot >= UCN_I_SECURITY_SESSION_COUNT ||
        handle->runtime_instance != owner->runtime_instance ||
        handle->owner_instance != owner->owner_instance ||
        owner->sessions[slot].occupied == 0U ||
        owner->sessions[slot].slot_generation != handle->slot_generation ||
        owner->sessions[slot].candidate.session_generation !=
            handle->session_generation) {
        return false;
    }
    *slot_out = slot;
    return true;
}

static bool persistence_handle_valid(ucn_handle_t handle)
{
    return handle.runtime_instance != 0U && handle.owner_instance != 0U &&
           handle.generation != 0U &&
           handle.object_kind == UCN_OBJECT_KIND_PERSISTENCE &&
           handle.reserved_zero == 0U;
}

static void clear_callback(ucn_i_security_owner_t *owner)
{
    owner->callback_active = 0U;
    owner->callback_slot = 0U;
    owner->callback_proof_bytes = 0U;
    memset(&owner->callback_claim, 0, sizeof(owner->callback_claim));
    memset(&owner->callback_candidate, 0, sizeof(owner->callback_candidate));
    memset(owner->callback_proof, 0, sizeof(owner->callback_proof));
}

static ucn_result_t reauth_verify_identities(ucn_i_security_owner_t *owner,
    const ucn_i_security_reauth_call_t *call, ucn_i_security_reauth_workspace_t *work)
{
    ucn_result_t result;
    memcpy(work->message, "UCN6-PEER-REAUTH-V2", 19U);
    memcpy(work->message + 19U, call->record->canonical, 208U);
    work->message[20] = 1U;
    result = call->verify_identity(call->verify_context, work->message + 59U,
        work->message, owner->callback_proof, call->challenge_proof_bytes);
    if (result != UCN_OK) { return result; }
    work->message[20] = 2U;
    return call->verify_identity(call->verify_context, work->message + 35U,
        work->message, owner->callback_proof + call->challenge_proof_bytes,
        call->response_proof_bytes);
}

static ucn_result_t prepare_session(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_candidate_t *candidate,
    const uint8_t *proof,
    size_t proof_bytes,
    const ucn_i_security_durability_base_t *durability,
    uint64_t now_us,
    ucn_i_security_handle_t *handle_out,
    const ucn_i_security_reauth_call_t *reauth,
    ucn_i_security_reauth_workspace_t *reauth_work)
{
    ucn_i_security_handle_t handle;
    ucn_i_security_slot_t *slot;
    uint16_t rule_index;
    uint16_t slot_index;
    bool peer_session_found = false;
    uint32_t next_generation;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;

    if (candidate == NULL || proof == NULL || proof_bytes == 0U ||
        proof_bytes > UCN_I_SECURITY_PROOF_BYTES || durability == NULL ||
        handle_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), candidate,
                             sizeof(*candidate)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), proof, proof_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), durability,
                             sizeof(*durability)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(candidate, sizeof(*candidate), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(proof, proof_bytes, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(durability, sizeof(*durability), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !candidate_valid(candidate, owner->address_width, now_us) ||
        durability->reserved_zero != 0U || durability->domain_id == 0U ||
        durability->next_transaction_id == 0U ||
        durability->absolute_deadline_us == 0U ||
        now_us >= durability->absolute_deadline_us ||
        durability->domain_generation == 0U ||
        durability->volatile_continuation == 0U ||
        durability->prior_session_generation >= UINT32_MAX ||
        (reauth == NULL ? candidate->session_generation !=
            durability->prior_session_generation + UINT64_C(1) :
            candidate->session_generation <= durability->prior_session_generation)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    rule_index = find_rule(owner, candidate->peer.principal);
    slot_index = find_free_slot(owner);
    if (rule_index == UINT16_MAX ||
        owner->domain_rules[rule_index].domain_id != durability->domain_id) {
        owner_unlock(owner);
        return UCN_ERR_ACCESS;
    }
    if (slot_index == UINT16_MAX || owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    {
        uint16_t index;

        for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
            const ucn_i_security_slot_t *existing = &owner->sessions[index];

            if (existing->occupied != 0U &&
                memcmp(existing->candidate.peer.principal,
                       candidate->peer.principal,
                       UCN_I_SECURITY_PRINCIPAL_BYTES) == 0) {
                /* Generic prepare preserves its unique exact predecessor rule.
                 * Signed reauth may retain several fenced historical holders,
                 * but none may exceed the actual local Foundation baseline.
                 * Neither path revives slots or replaces physical draining.
                 * ZH: 普通prepare保持唯一精确前驱；双签名续接可保留多个已围栏
                 * 历史槽，但不允许任何槽高于本端真实耐久历史。 */
                if (reauth != NULL ?
                    (existing->phase != UCN_I_SECURITY_SESSION_FENCED ||
                     existing->candidate.session_generation > durability->prior_session_generation) :
                    (peer_session_found ||
                     (existing->phase != UCN_I_SECURITY_SESSION_ACTIVE &&
                      existing->phase != UCN_I_SECURITY_SESSION_FENCED) ||
                     (existing->phase == UCN_I_SECURITY_SESSION_FENCED &&
                      existing->candidate.session_generation != durability->prior_session_generation))) {
                    owner_unlock(owner);
                    return UCN_ERR_STATE;
                }
                peer_session_found = true;
                if (existing->candidate.session_generation >=
                    candidate->session_generation) {
                    owner_unlock(owner);
                    return UCN_ERR_REPLAY;
                }
            }
        }
    }
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_proof_bytes = (uint16_t)proof_bytes;
    owner->callback_candidate = *candidate;
    memcpy(owner->callback_proof, proof, proof_bytes);
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = UCN_I_SECURITY_CALLBACK_VERIFY;
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        result = reauth == NULL ? UCN_OK : reauth_verify_identities(owner, reauth, reauth_work);
        if (result == UCN_OK) {
            result = owner->provider.verify_session(
                owner->provider.context, &owner->callback_candidate,
                owner->callback_proof, owner->callback_proof_bytes);
        }
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, candidate,
               sizeof(*candidate)) != 0) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    slot = &owner->sessions[slot_index];
    next_generation = slot->slot_generation + 1U;
    if (next_generation == 0U) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    memset(slot, 0, sizeof(*slot));
    slot->candidate = owner->callback_candidate;
    slot->durability = *durability;
    slot->slot_generation = next_generation;
    result = ucn_i_security_record_encode(owner->realm_id, &slot->candidate,
                                           slot->body);
    if (result != UCN_OK) {
        memset(slot, 0, sizeof(*slot));
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    slot->transition_fingerprint = transition_fingerprint(
        slot->body, UCN_I_SECURITY_RECORD_BYTES);
    slot->occupied = 1U;
    slot->phase = UCN_I_SECURITY_SESSION_AWAITING_DURABILITY;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = owner->runtime_instance;
    handle.owner_instance = owner->owner_instance;
    handle.slot = slot_index;
    handle.slot_generation = slot->slot_generation;
    handle.session_generation = slot->candidate.session_generation;
    clear_callback(owner);
    *handle_out = handle;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_prepare_static_session(ucn_i_security_owner_t *owner,
    const ucn_i_security_candidate_t *candidate, const uint8_t *proof, size_t proof_bytes,
    const ucn_i_security_durability_base_t *durability, uint64_t now_us,
    ucn_i_security_handle_t *out)
{
    return prepare_session(owner, candidate, proof, proof_bytes, durability, now_us, out, NULL, NULL);
}

static bool borrowed_output_disjoint_locked(const ucn_i_security_owner_t *owner,
    const void *output, size_t bytes);

ucn_result_t ucn_i_security_peer_is_fenced(ucn_i_security_owner_t *owner,
    const ucn_i_security_binding_t *local, const ucn_i_security_binding_t *peer, bool *out)
{
    uint16_t i; bool fenced = true; ucn_result_t result;
    if (owner == NULL || local == NULL || peer == NULL || out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), local, sizeof(*local)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), peer, sizeof(*peer)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), out, sizeof(*out)) ||
        ucn_i_ranges_overlap(local, sizeof(*local), out, sizeof(*out)) ||
        ucn_i_ranges_overlap(peer, sizeof(*peer), out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    result = owner_lock(owner); if (result != UCN_OK) { return result; }
    if (!borrowed_output_disjoint_locked(owner, out, sizeof(*out))) {
        owner_unlock(owner); return UCN_ERR_ARGUMENT;
    }
    if (owner->callback_active || !binding_valid(local, owner->address_width) ||
        !binding_valid(peer, owner->address_width)) { owner_unlock(owner); return UCN_ERR_STATE; }
    /* Match identity, not an attacker-selected Binding generation: an active
     * slot for this identity pair cannot be hidden by a stale Binding. */
    for (i = 0U; i < UCN_I_SECURITY_SESSION_COUNT; ++i) {
        const ucn_i_security_slot_t *slot = &owner->sessions[i];
        if (slot->occupied && memcmp(slot->candidate.local.principal, local->principal, 16U) == 0 &&
            memcmp(slot->candidate.peer.principal, peer->principal, 16U) == 0 &&
            slot->phase != UCN_I_SECURITY_SESSION_FENCED) { fenced = false; }
    }
    *out = fenced; owner_unlock(owner); return UCN_OK;
}

static bool reauth_input_disjoint(const ucn_i_security_owner_t *owner,
    const ucn_i_security_reauth_workspace_t *work, const ucn_i_security_handle_t *out,
    const void *input, size_t bytes)
{
    return input != NULL && !ucn_i_ranges_overlap(input, bytes, owner, sizeof(*owner)) &&
        !ucn_i_ranges_overlap(input, bytes, work, sizeof(*work)) &&
        !ucn_i_ranges_overlap(input, bytes, out, sizeof(*out));
}

static bool reauth_ranges_valid(const ucn_i_security_owner_t *owner,
    const ucn_i_security_reauth_call_t *call, const ucn_i_security_reauth_workspace_t *work,
    const ucn_i_security_handle_t *out)
{
    if (owner == NULL || call == NULL || work == NULL || out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), work, sizeof(*work)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), out, sizeof(*out)) ||
        ucn_i_ranges_overlap(work, sizeof(*work), out, sizeof(*out)) ||
        call->verify_identity == NULL || call->initiator > 1U ||
        call->challenge_proof_bytes < 16U || call->challenge_proof_bytes > 64U ||
        call->response_proof_bytes != call->challenge_proof_bytes ||
        !bytes_zero(call->reserved_zero, sizeof(call->reserved_zero))) { return false; }
    return reauth_input_disjoint(owner, work, out, call, sizeof(*call)) &&
        reauth_input_disjoint(owner, work, out, call->candidate, sizeof(*call->candidate)) &&
        reauth_input_disjoint(owner, work, out, call->previous, sizeof(*call->previous)) &&
        reauth_input_disjoint(owner, work, out, call->durability, sizeof(*call->durability)) &&
        reauth_input_disjoint(owner, work, out, call->local_record, UCN_I_SECURITY_RECORD_BYTES) &&
        reauth_input_disjoint(owner, work, out, call->record, sizeof(*call->record)) &&
        reauth_input_disjoint(owner, work, out, call->challenge_proof, call->challenge_proof_bytes) &&
        reauth_input_disjoint(owner, work, out, call->response_proof, call->response_proof_bytes);
}

static bool reauth_binding_matches(const ucn_i_security_binding_t *binding, const uint8_t *b)
{
    return binding->address == read_be32(b) && binding->binding_generation == read_be32(b + 4U) &&
        memcmp(binding->principal, b + 8U, 16U) == 0;
}

static bool reauth_history_matches(const ucn_i_security_reauth_call_t *call)
{
    const ucn_i_security_candidate_t *p = call->previous;
    const uint8_t *b = call->record->canonical;
    size_t history = call->initiator ? 56U : 164U;
    size_t origin = call->initiator ? 156U : 168U;
    size_t digest = call->initiator ? 124U : 176U;
    return p->hop_profile == UCN_I_HOP_PROFILE_H1 &&
        call->durability->prior_session_generation == p->session_generation &&
        p->session_generation == read_be32(b + history) &&
        p->origin_tx.key_generation == read_be32(b + origin) &&
        p->origin_rx.key_generation == read_be32(b + origin) &&
        p->hop_tx.key_generation == read_be32(b + origin + 4U) &&
        p->hop_rx.key_generation == read_be32(b + origin + 4U) &&
        memcmp(p->transcript_digest, b + digest, 32U) == 0;
}

static bool reauth_selectors_match(const ucn_i_security_reauth_call_t *call)
{
    const ucn_i_security_candidate_t *c = call->candidate, *p = call->previous;
    const uint8_t *b = call->record->canonical;
    uint16_t tx = call->initiator ? 0U : 1U, rx = call->initiator ? 1U : 0U;
    return c->origin_level == (b[64] == 1U ? UCN_I_SECURITY_AUTHENTICATED : UCN_I_SECURITY_CONFIDENTIAL) &&
        c->hop_profile == UCN_I_HOP_PROFILE_H1 &&
        c->origin_tx.suite_id == b[64] && c->origin_rx.suite_id == b[64] &&
        c->origin_tx.key_id == read_be16(b + 66U) + tx && c->origin_rx.key_id == read_be16(b + 66U) + rx &&
        c->hop_tx.key_id == read_be16(b + 68U) + tx && c->hop_rx.key_id == read_be16(b + 68U) + rx &&
        c->origin_tx.key_generation == read_be32(b + 72U) && c->origin_rx.key_generation == read_be32(b + 72U) &&
        c->hop_tx.key_generation == read_be32(b + 76U) && c->hop_rx.key_generation == read_be32(b + 76U) &&
        c->origin_tx.key_id == p->origin_tx.key_id && c->origin_rx.key_id == p->origin_rx.key_id &&
        c->hop_tx.key_id == p->hop_tx.key_id && c->hop_rx.key_id == p->hop_rx.key_id &&
        c->origin_tx.suite_id == p->origin_tx.suite_id && c->origin_rx.suite_id == p->origin_rx.suite_id;
}

static bool reauth_candidate_matches(const ucn_i_security_owner_t *owner,
    const ucn_i_security_reauth_call_t *call, ucn_i_security_reauth_workspace_t *work)
{
    const ucn_i_security_candidate_t *c = call->candidate;
    const uint8_t *b = call->record->canonical;
    uint64_t lifetime = read_be64(b + 116U), expiry;
    if (!candidate_valid(c, owner->address_width, call->now_us) ||
        !peer_reauth_valid(b) || b[1] != 2U || b[2] != owner->address_width ||
        read_be32(b + 4U) != owner->realm_id || !reauth_history_matches(call) ||
        !reauth_selectors_match(call) || call->started_us == 0U || call->started_us > call->now_us ||
        UINT64_MAX - call->started_us < lifetime || call->parent_deadline_us == 0U ||
        c->session_generation != read_be32(b + 60U) ||
        c->link_generation != read_be32(b + (call->initiator ? 84U : 88U)) ||
        !reauth_binding_matches(&c->local, b + (call->initiator ? 8U : 32U)) ||
        !reauth_binding_matches(&c->peer, b + (call->initiator ? 32U : 8U)) ||
        memcmp(&c->local, &call->previous->local, sizeof(c->local)) != 0 ||
        memcmp(&c->peer, &call->previous->peer, sizeof(c->peer)) != 0) { return false; }
    expiry = call->started_us + lifetime;
    if (expiry > call->parent_deadline_us) { expiry = call->parent_deadline_us; }
    if (c->expires_at_us != expiry ||
        ucn_i_security_record_encode(owner->realm_id, call->previous, work->message) != UCN_OK ||
        memcmp(work->message, call->local_record, UCN_I_SECURITY_RECORD_BYTES) != 0) { return false; }
    memcpy(work->message, "UCN6-PEER-REAUTH-V2", 19U); memcpy(work->message + 19U, b, 208U);
    return ucn_i_sha256_256(work->message, sizeof(work->message), work->digest, &work->hash) == UCN_OK &&
        memcmp(c->transcript_digest, work->digest, 32U) == 0 &&
        memcmp(c->origin_fingerprint, work->digest, 16U) == 0 &&
        memcmp(c->hop_fingerprint, work->digest + 16U, 16U) == 0;
}

ucn_result_t ucn_i_security_prepare_reauth_session(ucn_i_security_owner_t *owner,
    const ucn_i_security_reauth_call_t *call, ucn_i_security_reauth_workspace_t *work,
    ucn_i_security_handle_t *out)
{
    ucn_result_t result; bool fenced;
    if (!reauth_ranges_valid(owner, call, work, out)) { return UCN_ERR_ARGUMENT; }
    result = owner_lock(owner); if (result != UCN_OK) { return result; }
    if (!borrowed_output_disjoint_locked(owner, work, sizeof(*work)) ||
        !borrowed_output_disjoint_locked(owner, out, sizeof(*out))) { result = UCN_ERR_ARGUMENT; }
    owner_unlock(owner); if (result != UCN_OK) { return result; }
    result = ucn_i_security_peer_is_fenced(owner, &call->candidate->local, &call->candidate->peer, &fenced);
    if (result != UCN_OK || !fenced) { return result == UCN_OK ? UCN_ERR_STATE : result; }
    if (!reauth_candidate_matches(owner, call, work)) { return UCN_ERR_SECURITY; }
    memcpy(work->proof, call->challenge_proof, call->challenge_proof_bytes);
    memcpy(work->proof + call->challenge_proof_bytes, call->response_proof, call->response_proof_bytes);
    return prepare_session(owner, call->candidate, work->proof,
        (size_t)call->challenge_proof_bytes + call->response_proof_bytes,
        call->durability, call->now_us, out, call, work);
}

ucn_result_t ucn_i_security_bind_session_acl(ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session, const ucn_i_security_acl_rule_t *rules,
    uint16_t count)
{
    uint16_t i, j, slot_index; ucn_result_t result;
    ucn_i_security_slot_t *slot;
    ucn_i_callback_claim_t claim; bool active;
    if (owner == NULL || rules == NULL || count == 0U || count > UCN_I_SECURITY_SESSION_ACL_MAX ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), rules, sizeof(*rules) * count)) { return UCN_ERR_ARGUMENT; }
    /* The shared gate may use the same non-recursive lock as this Owner.
     * Observe it before taking the Owner lock, then recheck this Owner's actor
     * latch under that lock before publishing anything. */
    if (!owner_valid(owner)) { return UCN_ERR_STATE; }
    result = ucn_i_callback_gate_view(owner->provider_gate, &active, &claim);
    if (result != UCN_OK || active) { return result == UCN_OK ? UCN_ERR_STATE : result; }
    result = owner_lock(owner); if (result != UCN_OK) { return result; }
    if (owner->callback_active != 0U || !handle_matches(owner, &session, &slot_index)) {
        owner_unlock(owner); return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_AWAITING_DURABILITY || slot->session_acl != NULL) {
        owner_unlock(owner); return UCN_ERR_STATE;
    }
    for (i = 0U; i < count; ++i) {
        const ucn_i_security_acl_rule_t *rule = &rules[i];
        if (!acl_rule_valid(rule) ||
            rule->peer_binding_generation != slot->candidate.peer.binding_generation ||
            memcmp(rule->peer_principal, slot->candidate.peer.principal, UCN_I_SECURITY_PRINCIPAL_BYTES) != 0 ||
            (memcmp(rule->context_fingerprint, slot->candidate.origin_fingerprint, UCN_I_SECURITY_FINGERPRINT_BYTES) != 0 &&
             (slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H0 ||
              memcmp(rule->context_fingerprint, slot->candidate.hop_fingerprint, UCN_I_SECURITY_FINGERPRINT_BYTES) != 0))) {
            owner_unlock(owner); return UCN_ERR_ARGUMENT;
        }
        for (j = 0U; j < i; ++j) {
            if (memcmp(rule, &rules[j], sizeof(*rule)) == 0) { owner_unlock(owner); return UCN_ERR_ARGUMENT; }
        }
    }
    slot->session_acl = rules; slot->session_acl_count = count;
    owner_unlock(owner); return UCN_OK;
}
/* Caller holds the Owner lock; shared with snapshot getters to avoid nested locks. */
static bool borrowed_output_disjoint_locked(const ucn_i_security_owner_t *owner,
    const void *output, size_t bytes)
{
    uint16_t i;
    if (ucn_i_ranges_overlap(owner->acl_rules, owner->acl_rule_count * sizeof(*owner->acl_rules), output, bytes) ||
        ucn_i_ranges_overlap(owner->domain_rules, owner->domain_rule_count * sizeof(*owner->domain_rules), output, bytes)) {
        return false;
    }
    for (i = 0U; i < UCN_I_SECURITY_SESSION_COUNT; ++i) {
        const ucn_i_security_slot_t *slot = &owner->sessions[i];
        if (slot->occupied && ucn_i_ranges_overlap(slot->session_acl,
            slot->session_acl_count * sizeof(*slot->session_acl), output, bytes)) { return false; }
    }
    return true;
}
ucn_result_t ucn_i_security_borrowed_output_check(ucn_i_security_owner_t *owner,
    const void *output, size_t bytes)
{
    ucn_result_t result;
    if (owner == NULL || output == NULL || bytes == 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output, bytes)) { return UCN_ERR_ARGUMENT; }
    result = owner_lock(owner); if (result != UCN_OK) { return result; }
    result = borrowed_output_disjoint_locked(owner, output, bytes) ? UCN_OK : UCN_ERR_ARGUMENT;
    owner_unlock(owner); return result;
}

ucn_result_t ucn_i_security_session_candidate_get(ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session, ucn_i_security_candidate_t *out)
{
    uint16_t index; ucn_result_t result;
    if (owner == NULL || out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    result = owner_lock(owner); if (result != UCN_OK) { return result; }
    if (!borrowed_output_disjoint_locked(owner, out, sizeof(*out))) {
        owner_unlock(owner); return UCN_ERR_ARGUMENT;
    }
    if (owner->callback_active || !handle_matches(owner, &session, &index) ||
        owner->sessions[index].phase != UCN_I_SECURITY_SESSION_ACTIVE) {
        owner_unlock(owner); return UCN_ERR_STATE;
    }
    *out = owner->sessions[index].candidate;
    owner_unlock(owner); return UCN_OK;
}

ucn_result_t ucn_i_security_requirement_get(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_i_security_requirement_view_t *requirement_out)
{
    ucn_i_security_requirement_view_t view;
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (requirement_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), requirement_out,
                             sizeof(*requirement_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_AWAITING_DURABILITY) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    memset(&view, 0, sizeof(view));
    view.canonical_body = slot->body;
    view.domain_id = slot->durability.domain_id;
    view.foundation_transaction_id = slot->durability.next_transaction_id;
    view.expected_record_generation =
        slot->durability.expected_record_generation;
    view.absolute_deadline_us = slot->durability.absolute_deadline_us;
    view.transition_fingerprint = slot->transition_fingerprint;
    view.runtime_instance = owner->runtime_instance;
    view.body_bytes = UCN_I_SECURITY_RECORD_BYTES;
    view.volatile_continuation = slot->durability.volatile_continuation;
    view.caller_owner_instance =
        owner->persistence_business_owner_instance;
    view.domain_generation = slot->durability.domain_generation;
    view.schema_id = UCN_I_SECURITY_RECORD_SCHEMA_ID;
    view.schema_version = UCN_I_SECURITY_RECORD_SCHEMA;
    view.operation_kind = UCN_I_SECURITY_OPERATION_KIND;
    memcpy(view.expected_body_digest,
           slot->durability.expected_body_digest,
           sizeof(view.expected_body_digest));
    *requirement_out = view;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_bind_persistence(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_handle_t persistence_handle,
    const uint8_t expected_published_digest[16])
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (!persistence_handle_valid(persistence_handle) ||
        expected_published_digest == NULL ||
        !bytes_nonzero(expected_published_digest, 16U) || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner),
                             expected_published_digest, 16U)) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_AWAITING_DURABILITY) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (slot->persistence_bound != 0U) {
        bool exact = memcmp(&slot->persistence_handle, &persistence_handle,
                            sizeof(persistence_handle)) == 0 &&
                     memcmp(slot->expected_published_digest,
                            expected_published_digest, 16U) == 0;

        owner_unlock(owner);
        return exact ? UCN_OK : UCN_ERR_STATE;
    }
    slot->persistence_handle = persistence_handle;
    memcpy(slot->expected_published_digest, expected_published_digest, 16U);
    slot->persistence_bound = 1U;
    owner_unlock(owner);
    return UCN_OK;
}

static bool current_facts_match(
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_current_facts_t *facts)
{
    uint8_t i, difference = 0U;
    if (facts == NULL || facts->now_us == 0U ||
        facts->now_us >= slot->candidate.expires_at_us ||
        facts->now_us >= slot->durability.absolute_deadline_us ||
        facts->local.address != slot->candidate.local.address ||
        facts->local.binding_generation != slot->candidate.local.binding_generation ||
        facts->peer.address != slot->candidate.peer.address ||
        facts->peer.binding_generation != slot->candidate.peer.binding_generation ||
        facts->link_generation != slot->candidate.link_generation ||
        facts->policy_generation != slot->candidate.policy_generation) { return false; }
    for (i = 0U; i < UCN_I_SECURITY_PRINCIPAL_BYTES; ++i) {
        difference |= (uint8_t)(facts->local.principal[i] ^ slot->candidate.local.principal[i]);
        difference |= (uint8_t)(facts->peer.principal[i] ^ slot->candidate.peer.principal[i]);
    }
    return difference == 0U;
}

/* EN: Fixed public ACL identifiers, not authentication tags. A bounded 16-byte
 * comparison avoids keeping libc-call spills above the deeper crypto phases.
 * ZH: 比较公开ACL标识而非认证Tag；固定16字节比较避免libc调用的寄存器栈副本。 */
static bool acl_identifier_equal(const uint8_t *left, const uint8_t *right)
{
    uint8_t i, difference = 0U;
    for (i = 0U; i < 16U; ++i) { difference |= (uint8_t)(left[i] ^ right[i]); }
    return difference == 0U;
}
static UCN_I_STACK_PHASE bool acl_rule_matches(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *slot,
    const uint8_t context_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES],
    uint16_t service_id,
    uint16_t protocol_opcode,
    uint8_t direction)
{
    uint16_t index;
    const ucn_i_security_acl_rule_t *rules = slot->session_acl != NULL ? slot->session_acl : owner->acl_rules;
    uint16_t count = slot->session_acl != NULL ? slot->session_acl_count : owner->acl_rule_count;

    for (index = 0U; index < count; ++index) {
        const ucn_i_security_acl_rule_t *rule = &rules[index];

        if (rule->peer_binding_generation ==
                slot->candidate.peer.binding_generation &&
            rule->service_id == service_id &&
            rule->protocol_opcode == protocol_opcode &&
            rule->direction == direction) {
            uint8_t i, difference = 0U;
            for (i = 0U; i < 16U; ++i) {
                difference |= (uint8_t)(rule->peer_principal[i] ^ slot->candidate.peer.principal[i]);
                difference |= (uint8_t)(rule->context_fingerprint[i] ^ context_fingerprint[i]);
            }
            if (difference == 0U) { return true; }
        }
    }
    return false;
}

static bool durability_proof_matches(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_durability_proof_t *proof)
{
    uint64_t expected_generation;

    if (proof == NULL || slot->persistence_bound == 0U ||
        slot->durability.expected_record_generation == UINT64_MAX) {
        return false;
    }
    expected_generation = slot->durability.expected_record_generation + 1U;
    return memcmp(&proof->persistence_handle, &slot->persistence_handle,
                  sizeof(proof->persistence_handle)) == 0 &&
           proof->domain_id == slot->durability.domain_id &&
           proof->record_generation == expected_generation &&
           proof->foundation_transaction_id ==
               slot->durability.next_transaction_id &&
           proof->witness_generation == proof->record_generation &&
           proof->transition_fingerprint == slot->transition_fingerprint &&
           proof->runtime_instance == owner->runtime_instance &&
           proof->body_bytes == UCN_I_SECURITY_RECORD_BYTES &&
           proof->volatile_continuation ==
               slot->durability.volatile_continuation &&
           proof->caller_owner_instance ==
               owner->persistence_business_owner_instance &&
           proof->domain_generation == slot->durability.domain_generation &&
           proof->schema_id == UCN_I_SECURITY_RECORD_SCHEMA_ID &&
           proof->schema_version == UCN_I_SECURITY_RECORD_SCHEMA &&
           proof->operation_kind == UCN_I_SECURITY_OPERATION_KIND &&
           proof->reserved_zero == 0U &&
           proof->persistence_owner_instance ==
               slot->persistence_handle.owner_instance &&
           memcmp(proof->body_digest, slot->expected_published_digest,
                  sizeof(proof->body_digest)) == 0;
}

ucn_result_t ucn_i_security_activate(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    const ucn_i_security_durability_proof_t *proof,
    const ucn_i_security_current_facts_t *facts)
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    uint16_t index;
    ucn_result_t result;

    if (proof == NULL || facts == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), proof,
                             sizeof(*proof)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_AWAITING_DURABILITY ||
        !current_facts_match(slot, facts) ||
        !durability_proof_matches(owner, slot, proof)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        const ucn_i_security_slot_t *existing = &owner->sessions[index];

        if (index != slot_index && existing->occupied != 0U &&
            existing->phase == UCN_I_SECURITY_SESSION_ACTIVE &&
            memcmp(existing->candidate.peer.principal,
                   slot->candidate.peer.principal,
                   UCN_I_SECURITY_PRINCIPAL_BYTES) == 0 &&
            existing->candidate.session_generation >=
                slot->candidate.session_generation) {
            owner_unlock(owner);
            return UCN_ERR_REPLAY;
        }
    }
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        ucn_i_security_slot_t *existing = &owner->sessions[index];

        if (index != slot_index && existing->occupied != 0U &&
            existing->phase == UCN_I_SECURITY_SESSION_ACTIVE &&
            memcmp(existing->candidate.peer.principal,
                   slot->candidate.peer.principal,
                   UCN_I_SECURITY_PRINCIPAL_BYTES) == 0) {
            existing->phase = UCN_I_SECURITY_SESSION_FENCED;
        }
    }
    slot->phase = UCN_I_SECURITY_SESSION_ACTIVE;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_control_authorize(ucn_i_security_owner_t *owner,
    const ucn_i_security_control_access_request_t *request,
    const ucn_i_security_current_facts_t *facts)
{
    uint16_t index; ucn_result_t result; const ucn_i_security_slot_t *slot;
    if (!owner || !request || !facts || request->reserved_zero ||
        !ucn_i_protocol_opcode_valid(request->protocol_opcode) ||
        (request->direction != UCN_I_SECURITY_ACCESS_INBOUND && request->direction != UCN_I_SECURITY_ACCESS_OUTBOUND) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request, sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts, sizeof(*facts))) { return UCN_ERR_ARGUMENT; }
    result = owner_lock(owner); if (result != UCN_OK) { return result; }
    if (owner->callback_active || !handle_matches(owner, &request->session, &index)) { owner_unlock(owner); return UCN_ERR_STATE; }
    slot = &owner->sessions[index];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE || !current_facts_match(slot, facts)) { result = UCN_ERR_STATE; }
    else { result = acl_rule_matches(owner, slot, request->context_fingerprint, 0U,
        request->protocol_opcode, request->direction) ? UCN_OK : UCN_ERR_ACCESS; }
    owner_unlock(owner); return result;
}

ucn_result_t ucn_i_security_authorize(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_access_request_t *request,
    const ucn_i_security_current_facts_t *facts)
{
    const ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (request == NULL || facts == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts)) ||
        request->service_id == 0U || request->service_id == UINT16_MAX ||
        (request->direction != UCN_I_SECURITY_ACCESS_INBOUND &&
         request->direction != UCN_I_SECURITY_ACCESS_OUTBOUND) ||
        !bytes_zero(request->reserved_zero, sizeof(request->reserved_zero))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &request->session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (acl_rule_matches(owner, slot, request->context_fingerprint,
                         request->service_id, request->protocol_opcode,
                         request->direction)) {
        owner_unlock(owner);
        return UCN_OK;
    }
    owner_unlock(owner);
    return UCN_ERR_ACCESS;
}

static bool c1_origin_material_matches(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_c1_origin_material_t *material)
{
    const ucn_i_security_binding_t *source;
    const ucn_i_security_binding_t *destination;
    uint8_t origin_security;

    if (material->payload_bytes > UINT32_MAX ||
        material->origin_sequence == 0U || material->service_id == 0U ||
        material->service_id == UINT16_MAX ||
        !bytes_zero(material->reserved_zero,
                    sizeof(material->reserved_zero)) ||
        material->common_header[0] != UINT8_C(0x61) ||
        (material->common_header[1] & UINT8_C(0x0F)) != 0U ||
        (material->common_header[1] & UINT8_C(0x30)) == UINT8_C(0x30) ||
        (material->common_header[2] & UINT8_C(0x3F)) == 0U) {
        return false;
    }
    origin_security = (uint8_t)(material->common_header[2] >> 6U);
    if (origin_security != slot->candidate.origin_level) {
        return false;
    }
    if (material->direction == UCN_I_SECURITY_ACCESS_OUTBOUND) {
        source = &slot->candidate.local;
        destination = &slot->candidate.peer;
    } else if (material->direction == UCN_I_SECURITY_ACCESS_INBOUND) {
        source = &slot->candidate.peer;
        destination = &slot->candidate.local;
    } else {
        return false;
    }
    return material->source_address == source->address &&
           material->destination_address == destination->address &&
           acl_rule_matches(owner, slot, slot->candidate.origin_fingerprint,
                            material->service_id,
                            material->protocol_opcode,
                            material->direction);
}

static ucn_result_t c1_origin_material_lock(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    uint16_t *slot_index_out)
{
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &material->session, slot_index_out) ||
        owner->sessions[*slot_index_out].phase !=
            UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(&owner->sessions[*slot_index_out],
                             &material->facts) ||
        !c1_origin_material_matches(owner,
                                    &owner->sessions[*slot_index_out],
                                    material)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    return UCN_OK;
}

static void c1_origin_aad_encode(
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_c1_origin_material_t *material,
    uint8_t output[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES])
{
    static const uint8_t domain[] = "UCN6-ORIGIN-V1";
    const ucn_i_security_binding_t *source;
    const ucn_i_security_binding_t *destination;
    size_t offset = 0U;

    if (material->direction == UCN_I_SECURITY_ACCESS_OUTBOUND) {
        source = &slot->candidate.local;
        destination = &slot->candidate.peer;
    } else {
        source = &slot->candidate.peer;
        destination = &slot->candidate.local;
    }
    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    write_be32(output + offset, UINT32_C(54));
    offset += 4U;
    memcpy(output + offset, material->common_header, 3U);
    output[offset + 2U] &= UINT8_C(0xC0);
    offset += 3U;
    output[offset++] = UINT8_C(1);
    memcpy(output + offset, source->principal,
           UCN_I_SECURITY_PRINCIPAL_BYTES);
    offset += UCN_I_SECURITY_PRINCIPAL_BYTES;
    write_be32(output + offset, source->binding_generation);
    offset += 4U;
    memcpy(output + offset, destination->principal,
           UCN_I_SECURITY_PRINCIPAL_BYTES);
    offset += UCN_I_SECURITY_PRINCIPAL_BYTES;
    write_be32(output + offset, destination->binding_generation);
    offset += 4U;
    write_be16(output + offset, material->service_id);
    offset += 2U;
    write_be32(output + offset, material->origin_sequence);
    offset += 4U;
    write_be32(output + offset, (uint32_t)material->payload_bytes);
}

static void sequence_nonce_input_encode(
    const ucn_i_security_slot_t *slot,
    uint32_t origin_sequence,
    uint8_t output[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES])
{
    static const uint8_t domain[] = "UCN6-NONCE-SEQ-V1";
    size_t offset = 0U;

    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    memcpy(output + offset, slot->candidate.origin_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    offset += UCN_I_SECURITY_FINGERPRINT_BYTES;
    write_be32(output + offset, origin_sequence);
}

ucn_result_t ucn_i_security_c1_origin_aad_build(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    uint8_t output[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES])
{
    uint8_t encoded[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES];
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || material == NULL || output == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), material,
                             sizeof(*material)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                             UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES) ||
        ucn_i_ranges_overlap(material, sizeof(*material), output,
                             UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = c1_origin_material_lock(owner, material, &slot_index);
    if (result != UCN_OK) {
        return result;
    }
    c1_origin_aad_encode(&owner->sessions[slot_index], material, encoded);
    memcpy(output, encoded, sizeof(encoded));
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_sequence_nonce_input_build(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    uint8_t output[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES])
{
    uint8_t encoded[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES];
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || material == NULL || output == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), material,
                             sizeof(*material)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                             UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES) ||
        ucn_i_ranges_overlap(material, sizeof(*material), output,
                             UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES)) {
        return UCN_ERR_ARGUMENT;
    }
    result = c1_origin_material_lock(owner, material, &slot_index);
    if (result != UCN_OK) {
        return result;
    }
    sequence_nonce_input_encode(&owner->sessions[slot_index],
                                material->origin_sequence, encoded);
    memcpy(output, encoded, sizeof(encoded));
    owner_unlock(owner);
    return UCN_OK;
}

static void tx_reservation_clear(
    ucn_i_security_tx_reservation_t *reservation)
{
    uint32_t generation = reservation->generation;

    memset(reservation, 0, sizeof(*reservation));
    reservation->generation = generation;
}

static bool tx_handle_matches(
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_tx_handle_t *handle)
{
    return handle->reservation_generation != 0U &&
           slot->origin_tx_reservation.valid != 0U &&
           handle->reservation_generation ==
               slot->origin_tx_reservation.generation &&
           handle->origin_sequence ==
               slot->origin_tx_reservation.origin_sequence &&
           handle->deadline_us == slot->origin_tx_reservation.deadline_us;
}

ucn_result_t ucn_i_security_tx_reserve(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_access_request_t *request,
    const ucn_i_security_current_facts_t *facts,
    ucn_i_security_tx_handle_t *handle_out)
{
    ucn_i_security_tx_handle_t handle;
    ucn_i_security_slot_t *slot;
    uint64_t deadline;
    uint16_t slot_index;
    uint32_t generation;
    uint32_t sequence;
    ucn_result_t result;

    if (owner == NULL || request == NULL || facts == NULL ||
        handle_out == NULL || request->direction !=
            UCN_I_SECURITY_ACCESS_OUTBOUND ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                             sizeof(*request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(request, sizeof(*request), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &request->session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts) ||
        !acl_rule_matches(owner, slot, request->context_fingerprint,
                          request->service_id, request->protocol_opcode,
                          request->direction)) {
        owner_unlock(owner);
        return UCN_ERR_ACCESS;
    }
    if (slot->origin_tx_reservation.valid != 0U) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (slot->origin_tx_highest_reserved == UINT32_MAX ||
        facts->now_us > UINT64_MAX - owner->replay_reservation_lifetime_us) {
        owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    generation = slot->origin_tx_reservation.generation + 1U;
    if (generation == 0U) {
        owner_unlock(owner);
        return UCN_ERR_EXHAUSTED;
    }
    sequence = slot->origin_tx_highest_reserved + 1U;
    deadline = facts->now_us + owner->replay_reservation_lifetime_us;
    memset(&slot->origin_tx_reservation, 0,
           sizeof(slot->origin_tx_reservation));
    slot->origin_tx_reservation.deadline_us = deadline;
    memcpy(slot->origin_tx_reservation.context_fingerprint,
           request->context_fingerprint,
           sizeof(slot->origin_tx_reservation.context_fingerprint));
    slot->origin_tx_reservation.origin_sequence = sequence;
    slot->origin_tx_reservation.generation = generation;
    slot->origin_tx_reservation.service_id = request->service_id;
    slot->origin_tx_reservation.protocol_opcode = request->protocol_opcode;
    slot->origin_tx_reservation.direction = request->direction;
    slot->origin_tx_reservation.valid = 1U;
    slot->origin_tx_highest_reserved = sequence;
    memset(&handle, 0, sizeof(handle));
    handle.session = request->session;
    handle.deadline_us = deadline;
    handle.origin_sequence = sequence;
    handle.reservation_generation = generation;
    *handle_out = handle;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_tx_commit(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t handle,
    const ucn_i_security_current_facts_t *facts)
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || facts == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle.session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts) ||
        !tx_handle_matches(slot, &handle) ||
        facts->now_us >= handle.deadline_us) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    tx_reservation_clear(&slot->origin_tx_reservation);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_tx_abort(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t handle)
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle.session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (!tx_handle_matches(slot, &handle)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    tx_reservation_clear(&slot->origin_tx_reservation);
    owner_unlock(owner);
    return UCN_OK;
}

static bool replay_bits_committed(uint64_t highest, uint64_t bitmap,
                                  uint64_t sequence)
{
    uint64_t distance;

    if (highest == 0U || sequence > highest) {
        return false;
    }
    distance = highest - sequence;
    return distance >= 64U ||
           (bitmap & (UINT64_C(1) << distance)) != 0U;
}

static bool replay_is_committed(const ucn_i_security_replay_window_t *window,
                                uint64_t sequence)
{
    return replay_bits_committed(window->highest_committed,
                                 window->committed_bitmap, sequence);
}

static bool origin_is_committed(const ucn_i_security_slot_t *slot,
                                uint8_t domain, uint64_t sequence)
{
    return domain == 0U ? replay_is_committed(&slot->origin_replay, sequence) :
        replay_bits_committed(slot->c0_rx_highest_committed,
                               slot->c0_rx_committed_bitmap, sequence);
}

static bool replay_is_inflight(const ucn_i_security_replay_window_t *window,
                               uint64_t sequence)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_SECURITY_REPLAY_RESERVATION_COUNT;
         ++index) {
        if (window->reservations[index].valid != 0U &&
            window->reservations[index].sequence == sequence) {
            return true;
        }
    }
    return false;
}

static uint16_t replay_free_slot(const ucn_i_security_replay_window_t *window)
{
    uint16_t index;

    for (index = 0U; index < UCN_I_SECURITY_REPLAY_RESERVATION_COUNT;
         ++index) {
        if (window->reservations[index].valid == 0U) {
            return index;
        }
    }
    return UINT16_MAX;
}

static bool replay_handle_matches(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *session_slot,
    const ucn_i_security_replay_handle_t *handle,
    uint16_t *reservation_slot_out)
{
    uint16_t index = handle->reservation_slot;

    if (!bytes_zero(handle->reserved_zero, sizeof(handle->reserved_zero)) ||
        handle->has_hop > 1U || handle->replay_domain > 1U ||
        (handle->has_hop == 0U &&
         (handle->hop_sequence != 0U ||
          handle->hop_reservation_generation != 0U ||
          handle->hop_reservation_slot != 0U)) ||
        index >= UCN_I_SECURITY_REPLAY_RESERVATION_COUNT ||
        session_slot->origin_replay.reservations[index].valid == 0U ||
        session_slot->origin_replay.reservations[index].generation !=
            handle->reservation_generation ||
        session_slot->origin_replay.reservations[index].sequence !=
            handle->sequence ||
        session_slot->origin_replay.reservations[index].replay_domain !=
            handle->replay_domain) {
        return false;
    }
    (void)owner;
    *reservation_slot_out = index;
    return true;
}

static bool hop_replay_handle_matches(
    const ucn_i_security_slot_t *session_slot,
    const ucn_i_security_replay_handle_t *handle,
    uint16_t *reservation_slot_out)
{
    uint16_t index = handle->hop_reservation_slot;

    if (handle->has_hop != 1U ||
        index >= UCN_I_SECURITY_REPLAY_RESERVATION_COUNT ||
        session_slot->hop_replay.reservations[index].valid == 0U ||
        session_slot->hop_replay.reservations[index].generation !=
            handle->hop_reservation_generation ||
        session_slot->hop_replay.reservations[index].sequence !=
            handle->hop_sequence) {
        return false;
    }
    *reservation_slot_out = index;
    return true;
}

static ucn_result_t hop_replay_reserve_locked(
    ucn_i_security_owner_t *owner,
    ucn_i_security_slot_t *slot,
    const ucn_i_security_current_facts_t *facts,
    uint32_t sequence,
    const uint8_t tag_digest[16],
    ucn_i_security_replay_handle_t *handle)
{
    ucn_i_security_replay_reservation_t *reservation;
    uint16_t reservation_slot;
    uint32_t generation;

    if (sequence == 0U ||
        replay_is_committed(&slot->hop_replay, sequence)) {
        return UCN_ERR_REPLAY;
    }
    if (replay_is_inflight(&slot->hop_replay, sequence)) {
        return UCN_ERR_STATE;
    }
    reservation_slot = replay_free_slot(&slot->hop_replay);
    if (reservation_slot == UINT16_MAX ||
        facts->now_us > UINT64_MAX - owner->replay_reservation_lifetime_us) {
        return reservation_slot == UINT16_MAX ? UCN_ERR_NO_SPACE :
                                                UCN_ERR_EXHAUSTED;
    }
    reservation = &slot->hop_replay.reservations[reservation_slot];
    generation = reservation->generation + 1U;
    if (generation == 0U) {
        return UCN_ERR_EXHAUSTED;
    }
    memset(reservation, 0, sizeof(*reservation));
    reservation->sequence = sequence;
    reservation->deadline_us = facts->now_us +
                                owner->replay_reservation_lifetime_us;
    memcpy(reservation->aad_digest, tag_digest, 16U);
    memcpy(reservation->payload_digest, tag_digest, 16U);
    reservation->generation = generation;
    reservation->valid = 1U;
    handle->has_hop = 1U;
    handle->hop_sequence = sequence;
    handle->hop_reservation_generation = generation;
    handle->hop_reservation_slot = reservation_slot;
    return UCN_OK;
}

static ucn_result_t replay_reserve_locked(
    ucn_i_security_owner_t *owner,
    ucn_i_security_slot_t *slot,
    const ucn_i_security_handle_t *session,
    const ucn_i_security_current_facts_t *facts,
    uint64_t sequence,
    uint8_t replay_domain,
    const uint8_t aad_digest[16],
    const uint8_t payload_digest[16],
    ucn_i_security_replay_handle_t *handle_out)
{
    ucn_i_security_replay_reservation_t *reservation;
    uint64_t deadline;
    uint16_t reservation_slot;
    uint32_t generation;

    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts)) {
        return UCN_ERR_STATE;
    }
    if (origin_is_committed(slot, replay_domain, sequence)) {
        return UCN_ERR_REPLAY;
    }
    for (reservation_slot = 0U;
         reservation_slot < UCN_I_SECURITY_REPLAY_RESERVATION_COUNT;
         ++reservation_slot) {
        reservation = &slot->origin_replay.reservations[reservation_slot];
        if (reservation->valid != 0U && reservation->sequence == sequence &&
            reservation->replay_domain == replay_domain) return UCN_ERR_STATE;
    }
    reservation_slot = replay_free_slot(&slot->origin_replay);
    if (reservation_slot == UINT16_MAX || facts->now_us == 0U ||
        owner->replay_reservation_lifetime_us == 0U ||
        facts->now_us > UINT64_MAX - owner->replay_reservation_lifetime_us) {
        return reservation_slot == UINT16_MAX ? UCN_ERR_NO_SPACE :
                                                UCN_ERR_EXHAUSTED;
    }
    reservation = &slot->origin_replay.reservations[reservation_slot];
    generation = reservation->generation + 1U;
    if (generation == 0U) {
        return UCN_ERR_EXHAUSTED;
    }
    deadline = facts->now_us + owner->replay_reservation_lifetime_us;
    memset(reservation, 0, sizeof(*reservation));
    reservation->sequence = sequence;
    reservation->deadline_us = deadline;
    memcpy(reservation->aad_digest, aad_digest, 16U);
    memcpy(reservation->payload_digest, payload_digest, 16U);
    reservation->generation = generation;
    reservation->valid = 1U;
    reservation->replay_domain = replay_domain;
    /* All failure checks precede this locked commit; caller alias guards
     * ensure the output is distinct from the accepted Session and inputs. */
    memset(handle_out, 0, sizeof(*handle_out));
    handle_out->session = *session;
    handle_out->sequence = sequence;
    handle_out->reservation_generation = generation;
    handle_out->reservation_slot = reservation_slot;
    handle_out->replay_domain = replay_domain;
    return UCN_OK;
}

ucn_result_t ucn_i_security_replay_reserve(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    uint64_t sequence,
    const uint8_t aad_digest[16],
    const uint8_t payload_digest[16],
    ucn_i_security_replay_handle_t *handle_out)
{
    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    ucn_result_t result;

    if (facts == NULL || sequence == 0U || aad_digest == NULL ||
        payload_digest == NULL || handle_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), aad_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), payload_digest, 16U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(aad_digest, 16U, handle_out,
                             sizeof(*handle_out)) ||
        ucn_i_ranges_overlap(payload_digest, 16U, handle_out,
                             sizeof(*handle_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    result = replay_reserve_locked(owner, slot, &session, facts, sequence,
                                   0U, aad_digest, payload_digest, handle_out);
    owner_unlock(owner);
    return result;
}

static void replay_reservation_clear(
    ucn_i_security_replay_reservation_t *reservation);

static UCN_I_STACK_PHASE bool protect_ranges_valid(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    size_t plaintext_bytes = material == NULL ? 0U : material->payload_bytes;

    return owner != NULL && material != NULL && workspace != NULL &&
           output != NULL && output_bytes != NULL &&
           (plaintext != NULL || plaintext_bytes == 0U) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), material,
                                 sizeof(*material)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext,
                                 plaintext_bytes) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(material, sizeof(*material), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(material, sizeof(*material), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(material, sizeof(*material), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                                 sizeof(*output_bytes));
}

static void hop_aad_encode(
    const uint8_t *packet_before_hop,
    size_t packet_before_hop_bytes,
    uint32_t hop_sequence,
    const uint8_t hop_fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES],
    uint8_t *output,
    size_t *output_bytes)
{
    static const uint8_t domain[] = "UCN6-HOP-V1";
    size_t offset = 0U;

    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    write_be32(output + offset,
               (uint32_t)(packet_before_hop_bytes + 4U +
                          UCN_I_SECURITY_FINGERPRINT_BYTES));
    offset += 4U;
    memcpy(output + offset, packet_before_hop, packet_before_hop_bytes);
    offset += packet_before_hop_bytes;
    write_be32(output + offset, hop_sequence);
    offset += 4U;
    memcpy(output + offset, hop_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    offset += UCN_I_SECURITY_FINGERPRINT_BYTES;
    *output_bytes = offset;
}

/* Private projection: an Origin-only result is not a complete H1 frame.
 * 中文：私有分层选择，Origin-only输出绝不代表完整H1帧。 */
ucn_result_t ucn_i_security_c1_seal_layer(
    bool origin_only,
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t tx,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    ucn_i_security_slot_t *slot;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;
    uint16_t slot_index;

    if (!protect_ranges_valid(owner, material, plaintext, workspace,
                              output, output_capacity, output_bytes) ||
        (origin_only && material->protocol_opcode != 0U) ||
        material->direction != UCN_I_SECURITY_ACCESS_OUTBOUND ||
        memcmp(&tx.session, &material->session,
               sizeof(tx.session)) != 0 ||
        tx.origin_sequence != material->origin_sequence) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &tx.session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    memset(workspace, 0, sizeof(*workspace));
    workspace->packet_state.header_bytes =
        c1_header_bytes(owner->address_width);
    workspace->packet_state.hop_trailer_bytes =
        !origin_only && slot->candidate.hop_profile ==
                            UCN_I_HOP_PROFILE_H1 ?
                            UCN_I_SECURITY_HOP_TRAILER_BYTES : 0U;
    if (ucn_i_size_add(workspace->packet_state.header_bytes,
                       material->payload_bytes,
                       &workspace->packet_state.packet_bytes) != UCN_OK ||
        ucn_i_size_add(workspace->packet_state.packet_bytes,
                       UCN_I_SECURITY_ORIGIN_TAG_BYTES,
                       &workspace->packet_state.packet_bytes) != UCN_OK ||
        ucn_i_size_add(workspace->packet_state.packet_bytes,
                       workspace->packet_state.hop_trailer_bytes,
                       &workspace->packet_state.packet_bytes) != UCN_OK ||
        workspace->packet_state.packet_bytes > UCN_ADAPTER_FRAME_BYTES ||
        workspace->packet_state.packet_bytes > output_capacity) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        (slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H0 &&
         slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1) ||
        !tx_handle_matches(slot, &tx) ||
        memcmp(slot->origin_tx_reservation.context_fingerprint,
               slot->candidate.origin_fingerprint,
               UCN_I_SECURITY_FINGERPRINT_BYTES) != 0 ||
        slot->origin_tx_reservation.service_id != material->service_id ||
        slot->origin_tx_reservation.protocol_opcode !=
            material->protocol_opcode ||
        slot->origin_tx_reservation.direction != material->direction ||
        material->payload_bytes > UCN_ADAPTER_FRAME_BYTES ||
        !current_facts_match(slot, &material->facts) ||
        !c1_origin_material_matches(owner, slot, material) ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return (slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H0 ||
                slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H1) ?
                   UCN_ERR_STATE : UCN_ERR_UNSUPPORTED;
    }
    memcpy(workspace->packet, material->common_header, 3U);
    workspace->packet_state.offset = 3U;
    write_address_be(workspace->packet + workspace->packet_state.offset,
                     material->source_address, owner->address_width);
    workspace->packet_state.offset += owner->address_width;
    write_address_be(workspace->packet + workspace->packet_state.offset,
                     material->destination_address, owner->address_width);
    workspace->packet_state.offset += owner->address_width;
    write_be16(workspace->packet + workspace->packet_state.offset,
               material->service_id);
    workspace->packet_state.offset += 2U;
    write_be32(workspace->packet + workspace->packet_state.offset,
               material->origin_sequence);
    workspace->packet_state.offset += 4U;
    if (workspace->packet_state.offset !=
        workspace->packet_state.header_bytes) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (material->payload_bytes != 0U) {
        memcpy(workspace->plaintext, plaintext, material->payload_bytes);
    }
    c1_origin_aad_encode(slot, material, workspace->origin_aad);
    if (slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL) {
        sequence_nonce_input_encode(slot, material->origin_sequence,
                                    workspace->nonce_input);
    }
    memset(&workspace->origin_protect, 0,
           sizeof(workspace->origin_protect));
    workspace->origin_protect.selector = slot->candidate.origin_tx;
    workspace->origin_protect.nonce_input =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            workspace->nonce_input : NULL;
    workspace->origin_protect.nonce_input_bytes =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES : 0U;
    workspace->origin_protect.aad = workspace->origin_aad;
    workspace->origin_protect.aad_bytes =
        UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES;
    workspace->origin_protect.plaintext = workspace->plaintext;
    workspace->origin_protect.payload_bytes = material->payload_bytes;
    workspace->origin_protect.protected_payload =
        workspace->packet + workspace->packet_state.header_bytes;
    workspace->origin_protect.origin_tag =
        workspace->packet + workspace->packet_state.header_bytes +
        material->payload_bytes;
    workspace->packet_state.packet_before_hop_bytes =
        workspace->packet_state.header_bytes + material->payload_bytes +
        UCN_I_SECURITY_ORIGIN_TAG_BYTES;
    if (workspace->packet_state.hop_trailer_bytes != 0U) {
        if (slot->hop_tx_highest_reserved == UINT32_MAX) {
            owner_unlock(owner);
            return UCN_ERR_EXHAUSTED;
        }
        workspace->packet_state.hop_sequence =
            slot->hop_tx_highest_reserved + 1U;
        slot->hop_tx_highest_reserved =
            workspace->packet_state.hop_sequence;
        write_be32(workspace->packet +
                       workspace->packet_state.packet_before_hop_bytes,
                   workspace->packet_state.hop_sequence);
    }
    workspace->candidate = slot->candidate;
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_candidate = workspace->candidate;
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = UCN_I_SECURITY_CALLBACK_PROTECT;
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        workspace->packet_state.provider_invoked = 1U;
        result = owner->provider.protect_origin(
            owner->provider.context, &workspace->origin_protect);
        if (result == UCN_OK &&
            workspace->packet_state.hop_trailer_bytes != 0U) {
            memset(&workspace->hop_protect, 0,
                   sizeof(workspace->hop_protect));
            workspace->hop_protect.selector = workspace->candidate.hop_tx;
            workspace->hop_protect.aad = workspace->hop_aad;
            hop_aad_encode(
                workspace->packet,
                workspace->packet_state.packet_before_hop_bytes,
                workspace->packet_state.hop_sequence,
                workspace->candidate.hop_fingerprint,
                workspace->hop_aad, &workspace->hop_protect.aad_bytes);
            workspace->hop_protect.hop_tag =
                workspace->packet +
                workspace->packet_state.packet_before_hop_bytes + 4U;
            result = owner->provider.protect_hop(
                owner->provider.context, &workspace->hop_protect);
        }
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        !current_facts_match(slot, &material->facts) ||
        !tx_handle_matches(slot, &tx)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (workspace->packet_state.provider_invoked != 0U) {
        tx_reservation_clear(&slot->origin_tx_reservation);
    }
    if (result == UCN_OK &&
        workspace->candidate.origin_level ==
            UCN_I_SECURITY_AUTHENTICATED &&
        material->payload_bytes != 0U &&
        memcmp(workspace->packet + workspace->packet_state.header_bytes,
               workspace->plaintext,
               material->payload_bytes) != 0) {
        result = UCN_ERR_SECURITY;
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    memcpy(output, workspace->packet,
           workspace->packet_state.packet_bytes);
    *output_bytes = workspace->packet_state.packet_bytes;
    clear_callback(owner);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_c1_protect(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t tx,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output, size_t output_capacity, size_t *output_bytes)
{
    return ucn_i_security_c1_seal_layer(false, owner, tx, material, plaintext, workspace,
                             output, output_capacity, output_bytes);
}

ucn_result_t ucn_i_security_c1_origin_protect(
    ucn_i_security_owner_t *owner,
    ucn_i_security_tx_handle_t tx,
    const ucn_i_security_c1_origin_material_t *material,
    const uint8_t *plaintext,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output, size_t output_capacity, size_t *output_bytes)
{
    return ucn_i_security_c1_seal_layer(true, owner, tx, material, plaintext, workspace,
                             output, output_capacity, output_bytes);
}

static bool open_ranges_valid(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_access_request_t *access,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_replay_handle_t *replay_out)
{
    return owner != NULL && facts != NULL && access != NULL &&
           packet != NULL && workspace != NULL && plaintext_output != NULL &&
           plaintext_bytes != NULL && replay_out != NULL &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                                 sizeof(*facts)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), access,
                                 sizeof(*access)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), packet,
                                 packet_bytes) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(access, sizeof(*access), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(access, sizeof(*access), plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(access, sizeof(*access), plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(access, sizeof(*access), replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                                 plaintext_output, plaintext_capacity) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                                 plaintext_bytes, sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), replay_out,
                                 sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                                 plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                                 replay_out, sizeof(*replay_out)) &&
           !ucn_i_ranges_overlap(plaintext_bytes, sizeof(*plaintext_bytes),
                                 replay_out, sizeof(*replay_out));
}

/* EN: Snapshot publication is disjoint from Provider/hash work. Outlining
 * avoids retaining large compiler-generated struct-copy register saves across
 * those calls; no automatic candidate copy, new state or weaker check.
 * ZH: 候选快照发布独立于Provider/摘要阶段，避免编译器结构复制寄存器副本贯穿深链。
 * 不新增候选栈副本、状态或弱化校验。Caller holds the Owner lock. */
static UCN_I_STACK_PHASE void prepare_packet_callback(ucn_i_security_owner_t *owner,
    ucn_i_security_packet_workspace_t *workspace, uint16_t slot_index, uint16_t kind)
{
    workspace->candidate = owner->sessions[slot_index].candidate;
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_candidate = workspace->candidate;
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = kind;
}

/* EN: Both C0/C1 use the same unlocked Provider transaction. A missing Hop tag
 * means Origin-only; never infer a Hop proof from the Origin Session profile.
 * 中文：C0/C1共用锁外Provider事务；仅Origin输入不得推导出逐跳认证。 */
static ucn_result_t open_provider_run(ucn_i_security_owner_t *owner,
    ucn_i_security_packet_workspace_t *workspace, const uint8_t *hop_tag,
    size_t hop_aad_bytes)
{
    ucn_result_t result = ucn_i_callback_gate_enter(owner->provider_gate,
                                                    &owner->callback_claim);
    ucn_result_t leave_result;
    if (result != UCN_OK) return result;
    if (hop_tag != NULL) {
        memset(&workspace->hop_verify, 0, sizeof(workspace->hop_verify));
        workspace->hop_verify.selector = workspace->candidate.hop_rx;
        workspace->hop_verify.aad = workspace->hop_aad;
        workspace->hop_verify.aad_bytes = hop_aad_bytes;
        workspace->hop_verify.hop_tag = hop_tag;
        result = owner->provider.verify_hop(owner->provider.context,
                                              &workspace->hop_verify);
    }
    if (result == UCN_OK)
        result = owner->provider.open_origin(owner->provider.context,
                                               &workspace->origin_open);
    leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                               &owner->callback_claim);
    return leave_result == UCN_OK ? result : UCN_ERR_STATE;
}

ucn_result_t ucn_i_security_c1_open_borrowed(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c1_open_call_t *call,
    ucn_i_security_packet_workspace_t *workspace)
{
    ucn_i_security_slot_t *slot;
    const uint8_t *origin_tag;
    const uint8_t *hop_tag = NULL;





    uint16_t slot_index;

    ucn_result_t result;
    ucn_result_t gate_result;


    if (call == NULL || call->origin_only > 1U ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->facts, sizeof(*call->facts)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->access, sizeof(*call->access)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), owner, sizeof(*owner)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->packet, call->packet_bytes) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->plaintext_output, call->plaintext_capacity) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->plaintext_bytes, sizeof(*call->plaintext_bytes)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->replay_out, sizeof(*call->replay_out))) {
        return UCN_ERR_ARGUMENT;
    }

    if (!open_ranges_valid(owner, call->facts, call->access, call->packet, call->packet_bytes,
                           workspace, call->plaintext_output, call->plaintext_capacity,
                           call->plaintext_bytes, call->replay_out) ||
        call->access->direction != UCN_I_SECURITY_ACCESS_INBOUND ||
        memcmp(&call->session, &call->access->session, sizeof(call->session)) != 0 ||
        call->packet_bytes > UCN_ADAPTER_FRAME_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &call->session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    memset(&workspace->packet_state, 0, sizeof(workspace->packet_state));
    memset(&workspace->material, 0, sizeof(workspace->material));
    if (slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H0 &&
        slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1) {
        owner_unlock(owner);
        return UCN_ERR_UNSUPPORTED;
    }
    workspace->packet_state.hop_trailer_bytes = !call->origin_only && slot->candidate.hop_profile ==
                            UCN_I_HOP_PROFILE_H1 ?
                            UCN_I_SECURITY_HOP_TRAILER_BYTES : 0U;
    workspace->packet_state.header_bytes = c1_header_bytes(owner->address_width);
    if (call->packet_bytes < workspace->packet_state.header_bytes + UCN_I_SECURITY_ORIGIN_TAG_BYTES +
                           workspace->packet_state.hop_trailer_bytes) {
        owner_unlock(owner);
        return UCN_ERR_MALFORMED;
    }
    workspace->material.payload_bytes = call->packet_bytes - workspace->packet_state.header_bytes -
                    UCN_I_SECURITY_ORIGIN_TAG_BYTES - workspace->packet_state.hop_trailer_bytes;
    if (workspace->material.payload_bytes > call->plaintext_capacity) {
        owner_unlock(owner);
        return UCN_ERR_NO_SPACE;
    }
    memcpy(workspace->packet, call->packet, call->packet_bytes);
    workspace->material.session = call->session;
    workspace->material.facts = *call->facts;
    workspace->material.common_header[0] = workspace->packet[0];
    workspace->material.common_header[1] = workspace->packet[1];
    workspace->material.common_header[2] = workspace->packet[2];
    workspace->packet_state.offset = 3U;
    workspace->material.source_address =
        read_address_be(workspace->packet + workspace->packet_state.offset, owner->address_width);
    workspace->packet_state.offset += owner->address_width;
    workspace->material.destination_address =
        read_address_be(workspace->packet + workspace->packet_state.offset, owner->address_width);
    workspace->packet_state.offset += owner->address_width;
    workspace->material.service_id = read_be16(workspace->packet + workspace->packet_state.offset);
    workspace->packet_state.offset += 2U;
    workspace->material.origin_sequence =
        read_be32(workspace->packet + workspace->packet_state.offset);
    workspace->material.protocol_opcode = call->access->protocol_opcode;
    workspace->material.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    if (workspace->packet_state.offset + 4U != workspace->packet_state.header_bytes ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        call->access->protocol_opcode != 0U ||
        call->access->service_id == 0U || call->access->service_id == UINT16_MAX ||
        call->access->service_id != workspace->material.service_id ||
        !bytes_zero(call->access->reserved_zero,
                    sizeof(call->access->reserved_zero)) ||
        memcmp(call->access->context_fingerprint,
               slot->candidate.origin_fingerprint,
               UCN_I_SECURITY_FINGERPRINT_BYTES) != 0 ||
        !current_facts_match(slot, call->facts) ||
        !c1_origin_material_matches(owner, slot, &workspace->material) ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    c1_origin_aad_encode(slot, &workspace->material,
                         workspace->origin_aad);
    if (slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL) {
        sequence_nonce_input_encode(slot,
                                    workspace->material.origin_sequence,
                                    workspace->nonce_input);
    }
    origin_tag = workspace->packet + workspace->packet_state.header_bytes + workspace->material.payload_bytes;
    workspace->packet_state.packet_before_hop_bytes = workspace->packet_state.header_bytes + workspace->material.payload_bytes +
                              UCN_I_SECURITY_ORIGIN_TAG_BYTES;
    if (workspace->packet_state.hop_trailer_bytes != 0U) {
        workspace->packet_state.hop_sequence = read_be32(workspace->packet +
                                 workspace->packet_state.packet_before_hop_bytes);
        if (workspace->packet_state.hop_sequence == 0U) {
            owner_unlock(owner);
            return UCN_ERR_MALFORMED;
        }
        hop_tag = workspace->packet + workspace->packet_state.packet_before_hop_bytes + 4U;
        hop_aad_encode(workspace->packet, workspace->packet_state.packet_before_hop_bytes,
                       workspace->packet_state.hop_sequence, slot->candidate.hop_fingerprint,
                       workspace->hop_aad, &workspace->packet_state.hop_aad_bytes);
    }
    memset(&workspace->origin_open, 0, sizeof(workspace->origin_open));
    workspace->origin_open.selector = slot->candidate.origin_rx;
    workspace->origin_open.nonce_input =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            workspace->nonce_input : NULL;
    workspace->origin_open.nonce_input_bytes =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES : 0U;
    workspace->origin_open.aad = workspace->origin_aad;
    workspace->origin_open.aad_bytes =
        UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES;
    workspace->origin_open.protected_payload =
        workspace->packet + workspace->packet_state.header_bytes;
    workspace->origin_open.origin_tag = origin_tag;
    workspace->origin_open.payload_bytes = workspace->material.payload_bytes;
    workspace->origin_open.plaintext = workspace->plaintext;
    prepare_packet_callback(owner, workspace, slot_index, UCN_I_SECURITY_CALLBACK_OPEN);
    owner_unlock(owner);

    result = open_provider_run(owner, workspace, hop_tag,
                                   workspace->packet_state.hop_aad_bytes);

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        !current_facts_match(slot, call->facts) ||
        !c1_origin_material_matches(owner, slot, &workspace->material)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    if (workspace->candidate.origin_level ==
            UCN_I_SECURITY_AUTHENTICATED &&
        workspace->material.payload_bytes != 0U &&
        memcmp(workspace->plaintext, workspace->packet + workspace->packet_state.header_bytes,
               workspace->material.payload_bytes) != 0) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    memcpy(workspace->digest, origin_tag,
           UCN_I_SECURITY_ORIGIN_TAG_BYTES);
    result = replay_reserve_locked(owner, slot, &call->session, call->facts,
                                   workspace->material.origin_sequence,
                                   0U,
                                   workspace->digest, workspace->digest,
                                   &workspace->replay);
    if (result == UCN_OK && workspace->packet_state.hop_trailer_bytes != 0U) {
        memset(workspace->digest + UCN_I_SECURITY_ORIGIN_TAG_BYTES,
               0, UCN_I_SECURITY_ORIGIN_TAG_BYTES);
        memcpy(workspace->digest + UCN_I_SECURITY_ORIGIN_TAG_BYTES,
               hop_tag, UCN_I_SECURITY_HOP_TAG_BYTES);
        result = hop_replay_reserve_locked(owner, slot, call->facts,
            workspace->packet_state.hop_sequence,
            workspace->digest + UCN_I_SECURITY_ORIGIN_TAG_BYTES,
            &workspace->replay);
        if (result != UCN_OK) {
            replay_reservation_clear(
                &slot->origin_replay.reservations[
                    workspace->replay.reservation_slot]);
        }
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    if (workspace->material.payload_bytes != 0U) {
        memcpy(call->plaintext_output, workspace->plaintext, workspace->material.payload_bytes);
    }
    *call->plaintext_bytes = workspace->material.payload_bytes;
    *call->replay_out = workspace->replay;
    clear_callback(owner);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_c1_open(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_access_request_t *access,
    const uint8_t *packet, size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output, size_t plaintext_capacity,
    size_t *plaintext_bytes, ucn_i_security_replay_handle_t *replay_out)
{
    const ucn_i_security_c1_open_call_t call = {session, facts, access,
        packet, packet_bytes, plaintext_output, plaintext_capacity,
        plaintext_bytes, replay_out, 0U};
    return ucn_i_security_c1_open_borrowed(owner, &call, workspace);
}

ucn_result_t ucn_i_security_c1_origin_open(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_access_request_t *access,
    const uint8_t *packet, size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output, size_t plaintext_capacity,
    size_t *plaintext_bytes, ucn_i_security_replay_handle_t *replay_out)
{
    const ucn_i_security_c1_open_call_t call = {session, facts, access,
        packet, packet_bytes, plaintext_output, plaintext_capacity,
        plaintext_bytes, replay_out, 1U};
    return ucn_i_security_c1_open_borrowed(owner, &call, workspace);
}

ucn_result_t ucn_i_security_c1_context_check(ucn_i_security_owner_t *o,
    const ucn_i_security_handle_t *session, const ucn_i_security_current_facts_t *f,
    const uint8_t *origin_fingerprint, const uint8_t *hop_fingerprint)
{
    uint16_t index; ucn_result_t r; const ucn_i_security_slot_t *s;
    if (!o || !session || !f || (!origin_fingerprint && !hop_fingerprint) ||
        ucn_i_ranges_overlap(o, sizeof(*o), session, sizeof(*session)) ||
        ucn_i_ranges_overlap(o, sizeof(*o), f, sizeof(*f)) ||
        (origin_fingerprint && ucn_i_ranges_overlap(o, sizeof(*o), origin_fingerprint, 16U)) ||
        (hop_fingerprint && ucn_i_ranges_overlap(o, sizeof(*o), hop_fingerprint, 16U))) { return UCN_ERR_ARGUMENT; }
    r = owner_lock(o); if (r != UCN_OK) { return r; }
    if (o->callback_active || !handle_matches(o, session, &index)) { owner_unlock(o); return UCN_ERR_STATE; }
    s = &o->sessions[index];
    r = s->phase == UCN_I_SECURITY_SESSION_ACTIVE && current_facts_match(s, f) &&
        (!origin_fingerprint || acl_identifier_equal(origin_fingerprint, s->candidate.origin_fingerprint)) &&
        (!hop_fingerprint || (s->candidate.hop_profile == UCN_I_HOP_PROFILE_H1 &&
            acl_identifier_equal(hop_fingerprint, s->candidate.hop_fingerprint))) ? UCN_OK : UCN_ERR_SECURITY;
    owner_unlock(o); return r;
}

static bool c1_hop_ranges(ucn_i_security_owner_t *o,
    const ucn_i_security_current_facts_t *f, const uint8_t *p, size_t n,
    ucn_i_security_packet_workspace_t *w, const void *out, size_t cap)
{
    return o != NULL && f != NULL && p != NULL && w != NULL && out != NULL &&
        n != 0U && n <= UCN_ADAPTER_FRAME_BYTES && cap != 0U &&
        !ucn_i_ranges_overlap(o, sizeof(*o), f, sizeof(*f)) &&
        !ucn_i_ranges_overlap(o, sizeof(*o), p, n) &&
        !ucn_i_ranges_overlap(o, sizeof(*o), w, sizeof(*w)) &&
        !ucn_i_ranges_overlap(o, sizeof(*o), out, cap) &&
        !ucn_i_ranges_overlap(f, sizeof(*f), p, n) &&
        !ucn_i_ranges_overlap(f, sizeof(*f), w, sizeof(*w)) &&
        !ucn_i_ranges_overlap(f, sizeof(*f), out, cap) &&
        !ucn_i_ranges_overlap(p, n, w, sizeof(*w)) &&
        !ucn_i_ranges_overlap(p, n, out, cap) &&
        !ucn_i_ranges_overlap(w, sizeof(*w), out, cap);
}

/* Starts the existing Owner callback transaction; all large storage is caller
 * owned. Every counter is the original Session counter, never a backend copy. */
static ucn_result_t c1_hop_begin(ucn_i_security_owner_t *o,
    const ucn_i_security_handle_t *session, const ucn_i_security_current_facts_t *f,
    const uint8_t *p, size_t n, ucn_i_security_packet_workspace_t *w, bool inbound)
{
    uint16_t slot_index, service;
    ucn_i_security_slot_t *slot;
    ucn_result_t r = owner_lock(o);
    if (r != UCN_OK) { return r; }
    if (o->callback_active || !handle_matches(o, session, &slot_index)) {
        owner_unlock(o); return UCN_ERR_STATE;
    }
    slot = &o->sessions[slot_index];
    if (n < c1_header_bytes(o->address_width) + 16U + (inbound ? 16U : 0U) ||
        p[0] != 0x61U || (p[1] & 0x3FU) != 0U || (p[2] & 0x3FU) == 0U ||
        (p[2] >> 6U) == 0U || (p[2] >> 6U) > 2U) {
        owner_unlock(o); return UCN_ERR_MALFORMED;
    }
    service = read_be16(p + 3U + 2U * o->address_width);
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1 ||
        !current_facts_match(slot, f) || !service || service == UINT16_MAX ||
        !acl_rule_matches(o, slot, slot->candidate.hop_fingerprint, service, 0U,
            inbound ? UCN_I_SECURITY_ACCESS_INBOUND : UCN_I_SECURITY_ACCESS_OUTBOUND) ||
        o->next_operation_id == UINT32_MAX ||
        (!inbound && slot->hop_tx_highest_reserved == UINT32_MAX)) {
        owner_unlock(o); return UCN_ERR_STATE;
    }
    memset(w, 0, sizeof(*w));
    memcpy(w->packet, p, n);
    w->packet_state.packet_before_hop_bytes = inbound ? n - 16U : n;
    w->packet_state.hop_sequence = inbound ? read_be32(p + n - 16U) : slot->hop_tx_highest_reserved + 1U;
    if (!w->packet_state.hop_sequence) { owner_unlock(o); return UCN_ERR_MALFORMED; }
    if (!inbound) {
        slot->hop_tx_highest_reserved = w->packet_state.hop_sequence;
        write_be32(w->packet + n, w->packet_state.hop_sequence);
    }
    w->candidate = slot->candidate;
    w->replay.session = *session;
    hop_aad_encode(w->packet, w->packet_state.packet_before_hop_bytes,
        w->packet_state.hop_sequence, slot->candidate.hop_fingerprint,
        w->hop_aad, &w->packet_state.hop_aad_bytes);
    w->hop_protect.selector = slot->candidate.hop_tx;
    w->hop_protect.aad = w->hop_aad;
    w->hop_protect.aad_bytes = w->packet_state.hop_aad_bytes;
    w->hop_protect.hop_tag = w->packet + w->packet_state.packet_before_hop_bytes + 4U;
    w->hop_verify.selector = slot->candidate.hop_rx;
    w->hop_verify.aad = w->hop_aad;
    w->hop_verify.aad_bytes = w->packet_state.hop_aad_bytes;
    w->hop_verify.hop_tag = w->hop_protect.hop_tag;
    o->callback_active = 1U; o->callback_slot = slot_index;
    o->callback_candidate = w->candidate;
    o->callback_claim.owner_instance = o->owner_instance;
    o->callback_claim.operation_id = o->next_operation_id++;
    o->callback_claim.operation_generation = 1U;
    o->callback_claim.operation_kind = inbound ? UCN_I_SECURITY_CALLBACK_OPEN : UCN_I_SECURITY_CALLBACK_PROTECT;
    owner_unlock(o); return UCN_OK;
}

static ucn_result_t c1_hop_finish(ucn_i_security_owner_t *o,
    const ucn_i_security_current_facts_t *f, ucn_i_security_packet_workspace_t *w,
    bool inbound)
{
    ucn_result_t r, g;
    ucn_i_security_slot_t *slot;
    r = ucn_i_callback_gate_enter(o->provider_gate, &o->callback_claim);
    if (r == UCN_OK) {
        r = inbound ? o->provider.verify_hop(o->provider.context, &w->hop_verify) :
            o->provider.protect_hop(o->provider.context, &w->hop_protect);
        g = ucn_i_callback_gate_leave(o->provider_gate, &o->callback_claim);
        if (g != UCN_OK) { r = g; }
    }
    g = owner_lock(o);
    if (g != UCN_OK) { return g; }
    slot = &o->sessions[o->callback_slot];
    if (!o->callback_active || slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &w->candidate, sizeof(w->candidate)) != 0 ||
        memcmp(&o->callback_candidate, &w->candidate, sizeof(w->candidate)) != 0 ||
        !current_facts_match(slot, f) ||
        !acl_rule_matches(o, slot, slot->candidate.hop_fingerprint,
            read_be16(w->packet + 3U + 2U * o->address_width), 0U,
            inbound ? UCN_I_SECURITY_ACCESS_INBOUND : UCN_I_SECURITY_ACCESS_OUTBOUND)) { r = UCN_ERR_STATE; }
    if (r == UCN_OK && inbound) {
        memset(w->digest, 0, 16U); memcpy(w->digest, w->hop_verify.hop_tag, 12U);
        r = hop_replay_reserve_locked(o, slot, f, w->packet_state.hop_sequence, w->digest, &w->replay);
    }
    clear_callback(o); owner_unlock(o); return r;
}

ucn_result_t ucn_i_security_c1_hop_wrap(ucn_i_security_owner_t *o,
    ucn_i_security_handle_t session, const ucn_i_security_current_facts_t *f,
    const uint8_t *p, size_t n, ucn_i_security_packet_workspace_t *w,
    uint8_t *out, size_t cap, size_t *written)
{
    ucn_result_t r;
    if (!c1_hop_ranges(o, f, p, n, w, out, cap) || written == NULL ||
        !c1_hop_ranges(o, f, p, n, w, written, sizeof(*written)) ||
        ucn_i_ranges_overlap(out, cap, written, sizeof(*written))) { return UCN_ERR_ARGUMENT; }
    if (n > UCN_ADAPTER_FRAME_BYTES - 16U || cap < n + 16U) { return UCN_ERR_NO_SPACE; }
    r = c1_hop_begin(o, &session, f, p, n, w, false);
    if (r != UCN_OK) { return r; }
    r = c1_hop_finish(o, f, w, false);
    if (r == UCN_OK) { memcpy(out, w->packet, n + 16U); *written = n + 16U; }
    return r;
}

ucn_result_t ucn_i_security_c1_hop_open(ucn_i_security_owner_t *o,
    ucn_i_security_handle_t session, const ucn_i_security_current_facts_t *f,
    const uint8_t *p, size_t n, ucn_i_security_packet_workspace_t *w,
    ucn_i_security_hop_replay_handle_t *out)
{
    ucn_result_t r;
    if (!c1_hop_ranges(o, f, p, n, w, out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    r = c1_hop_begin(o, &session, f, p, n, w, true);
    if (r != UCN_OK) { return r; }
    r = c1_hop_finish(o, f, w, true);
    if (r == UCN_OK) {
        memset(out, 0, sizeof(*out)); out->session = session;
        out->hop_sequence = w->replay.hop_sequence;
        out->reservation_slot = w->replay.hop_reservation_slot;
        out->reservation_generation = w->replay.hop_reservation_generation;
    }
    return r;
}

static void c0_nonce_input_encode(
    const ucn_i_security_slot_t *slot,
    const ucn_i_c0_frame_t *frame,
    uint8_t direction,
    uint8_t output[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES])
{
    static const uint8_t domain[] = "UCN6-NONCE-C0-V1";
    size_t offset = 0U;

    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    memcpy(output + offset, slot->candidate.origin_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    offset += UCN_I_SECURITY_FINGERPRINT_BYTES;
    write_be64(output + offset, frame->transaction_id);
    offset += 8U;
    write_be16(output + offset, frame->opcode);
    offset += 2U;
    /* Canonical sender direction is shared by both endpoints. Local IN/OUT is not. */
    output[offset++] = (uint8_t)((direction == UCN_I_SECURITY_ACCESS_OUTBOUND ?
        memcmp(slot->candidate.local.principal, slot->candidate.peer.principal, 16U) :
        memcmp(slot->candidate.peer.principal, slot->candidate.local.principal, 16U)) < 0 ? 0U : 1U);
    output[offset] = frame->interaction_role;
}

static bool c0_bindings_match(
    const ucn_i_security_owner_t *owner,
    const ucn_i_security_slot_t *slot,
    const ucn_i_c0_frame_t *frame,
    uint8_t direction)
{
    const ucn_i_security_binding_t *source;
    const ucn_i_security_binding_t *destination;

    if (direction == UCN_I_SECURITY_ACCESS_INBOUND) {
        source = &slot->candidate.peer;
        destination = &slot->candidate.local;
    } else if (direction == UCN_I_SECURITY_ACCESS_OUTBOUND) {
        source = &slot->candidate.local;
        destination = &slot->candidate.peer;
    } else {
        return false;
    }
    return frame->realm_id == owner->realm_id &&
           frame->source_address == source->address &&
           frame->destination_address == destination->address &&
           frame->source_binding_generation == source->binding_generation &&
           frame->destination_binding_generation ==
               destination->binding_generation;
}

static UCN_I_STACK_PHASE bool c0_open_ranges_valid(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_control_access_request_t *access,
    const ucn_i_security_current_facts_t *facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_authenticated_control_t *fact_out)
{
    return owner != NULL && access != NULL && facts != NULL &&
           packet != NULL && workspace != NULL && plaintext_output != NULL &&
           plaintext_bytes != NULL && fact_out != NULL &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), access,
                                 sizeof(*access)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                                 sizeof(*facts)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), packet,
                                 packet_bytes) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), fact_out,
                                 sizeof(*fact_out)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, plaintext_output,
                                 plaintext_capacity) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, plaintext_bytes,
                                 sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, fact_out,
                                 sizeof(*fact_out)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                                 plaintext_output, plaintext_capacity) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                                 plaintext_bytes, sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), fact_out,
                                 sizeof(*fact_out)) &&
           !ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                                 plaintext_bytes, sizeof(*plaintext_bytes)) &&
           !ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                                 fact_out, sizeof(*fact_out)) &&
           !ucn_i_ranges_overlap(plaintext_bytes, sizeof(*plaintext_bytes),
                                 fact_out, sizeof(*fact_out));
}

static UCN_I_STACK_PHASE bool c0_protect_ranges_valid(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c0_protect_request_t *request,
    const uint8_t *plaintext,
    size_t plaintext_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    return owner != NULL && request != NULL && workspace != NULL &&
           output != NULL && output_bytes != NULL &&
           (plaintext != NULL || plaintext_bytes == 0U) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), request,
                                 sizeof(*request)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext,
                                 plaintext_bytes) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(request, sizeof(*request), plaintext,
                                 plaintext_bytes) &&
           !ucn_i_ranges_overlap(request, sizeof(*request), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(request, sizeof(*request), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(request, sizeof(*request), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(plaintext, plaintext_bytes, output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                                 sizeof(*output_bytes));
}

ucn_result_t ucn_i_security_c0_protect(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c0_protect_request_t *request,
    const uint8_t *plaintext,
    size_t plaintext_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    ucn_i_security_slot_t *slot;
    size_t encoded_bytes = 0U;
    size_t preliminary_bytes = 0U;
    size_t hop_aad_bytes = 0U;
    uint16_t slot_index;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;

    if (!c0_protect_ranges_valid(owner, request, plaintext, plaintext_bytes,
                                  workspace, output, output_capacity,
                                  output_bytes) ||
        plaintext_bytes > UCN_ADAPTER_FRAME_BYTES ||
        request->direction != UCN_I_SECURITY_ACCESS_OUTBOUND ||
        request->transaction_id == 0U ||
        request->protocol_opcode == 0U ||
        !bytes_zero(request->reserved_zero,
                    sizeof(request->reserved_zero))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &request->session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    memset(workspace, 0, sizeof(*workspace));
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, &request->facts) ||
        (slot->candidate.origin_level != UCN_I_SECURITY_AUTHENTICATED &&
         slot->candidate.origin_level != UCN_I_SECURITY_CONFIDENTIAL) ||
        (slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H0 &&
         slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1) ||
        memcmp(request->context_fingerprint,
               slot->candidate.origin_fingerprint,
               UCN_I_SECURITY_FINGERPRINT_BYTES) != 0 ||
        !acl_rule_matches(owner, slot, request->context_fingerprint, 0U,
                           request->protocol_opcode, request->direction) ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (request->transaction_id <= slot->c0_tx_highest_consumed) {
        owner_unlock(owner);
        return UCN_ERR_REPLAY;
    }
    workspace->control_frame.payload = workspace->packet;
    workspace->control_frame.payload_bytes = plaintext_bytes;
    workspace->control_frame.origin_tag = workspace->digest;
    workspace->control_frame.hop_tag =
        slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H1 ?
            workspace->digest + UCN_I_SECURITY_ORIGIN_TAG_BYTES : NULL;
    workspace->control_frame.transaction_id = request->transaction_id;
    workspace->control_frame.realm_id = owner->realm_id;
    workspace->control_frame.source_address = slot->candidate.local.address;
    workspace->control_frame.destination_address = slot->candidate.peer.address;
    workspace->control_frame.source_binding_generation =
        slot->candidate.local.binding_generation;
    workspace->control_frame.destination_binding_generation =
        slot->candidate.peer.binding_generation;
    workspace->control_frame.opcode = request->protocol_opcode;
    workspace->control_frame.traffic_class = request->traffic_class;
    workspace->control_frame.delivery_guarantee =
        request->delivery_guarantee;
    workspace->control_frame.interaction_role = request->interaction_role;
    workspace->control_frame.payload_kind = request->payload_kind;
    workspace->control_frame.origin_security = slot->candidate.origin_level;
    workspace->control_frame.hop_limit = request->hop_limit;
    if (slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H1) {
        if (slot->hop_tx_highest_reserved == UINT32_MAX) {
            owner_unlock(owner);
            return UCN_ERR_EXHAUSTED;
        }
        workspace->control_frame.hop_sequence =
            slot->hop_tx_highest_reserved + 1U;
    }
    result = ucn_i_c0_encoded_size(
        owner->address_width, workspace->control_frame.origin_security,
        slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H1 ?
            UCN_I_CONTROL_H1 : UCN_I_CONTROL_H0,
        plaintext_bytes, &encoded_bytes);
    if (result != UCN_OK || encoded_bytes > UCN_ADAPTER_FRAME_BYTES ||
        encoded_bytes > output_capacity) {
        owner_unlock(owner);
        return result == UCN_OK ? UCN_ERR_NO_SPACE : result;
    }
    if (plaintext_bytes != 0U) {
        memcpy(workspace->plaintext, plaintext, plaintext_bytes);
    }
    if (slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL) {
        c0_nonce_input_encode(slot, &workspace->control_frame,
                              request->direction, workspace->nonce_input);
    }
    memset(&workspace->origin_protect, 0,
           sizeof(workspace->origin_protect));
    workspace->origin_protect.selector = slot->candidate.origin_tx;
    workspace->origin_protect.nonce_input =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            workspace->nonce_input : NULL;
    workspace->origin_protect.nonce_input_bytes =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES : 0U;
    workspace->origin_protect.aad = workspace->origin_aad;
    result = ucn_i_c0_origin_aad_build(
        &workspace->control_frame, owner->address_width,
        workspace->origin_aad, &workspace->origin_protect.aad_bytes);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return result;
    }
    if (slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H1) {
        slot->hop_tx_highest_reserved =
            workspace->control_frame.hop_sequence;
    }
    /* EN: Burn the transaction before the Provider can observe the nonce.
     * Even a Provider or gate failure cannot make this ID reusable.  Exact
     * physical retries are handled by Composition using the sealed bytes.
     * 中文：在 Provider 能观察 nonce 之前烧毁事务号。即使 Provider 或 Gate
     * 失败也不能复用；精确物理重试由 Composition 复用 sealed bytes。 */
    slot->c0_tx_highest_consumed = request->transaction_id;
    workspace->origin_protect.plaintext = workspace->plaintext;
    workspace->origin_protect.payload_bytes = plaintext_bytes;
    workspace->origin_protect.protected_payload = workspace->packet;
    workspace->origin_protect.origin_tag = workspace->digest;
    prepare_packet_callback(owner, workspace, slot_index, UCN_I_SECURITY_CALLBACK_PROTECT);
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        result = owner->provider.protect_origin(
            owner->provider.context, &workspace->origin_protect);
        if (result == UCN_OK &&
            workspace->candidate.origin_level ==
                UCN_I_SECURITY_AUTHENTICATED &&
            plaintext_bytes != 0U &&
            memcmp(workspace->packet, workspace->plaintext,
                   plaintext_bytes) != 0) {
            result = UCN_ERR_SECURITY;
        }
        if (result == UCN_OK &&
            workspace->candidate.hop_profile == UCN_I_HOP_PROFILE_H1) {
            result = ucn_i_c0_encode(
                &workspace->control_frame, workspace->candidate.address_width,
                UCN_I_CONTROL_H1, workspace->plaintext,
                sizeof(workspace->plaintext), &preliminary_bytes);
            if (result == UCN_OK) {
                workspace->packet_state.packet_before_hop_bytes =
                    preliminary_bytes - UCN_I_SECURITY_HOP_TRAILER_BYTES;
                hop_aad_encode(
                    workspace->plaintext,
                    workspace->packet_state.packet_before_hop_bytes,
                    workspace->control_frame.hop_sequence,
                    workspace->candidate.hop_fingerprint,
                    workspace->hop_aad, &hop_aad_bytes);
                memset(&workspace->hop_protect, 0,
                       sizeof(workspace->hop_protect));
                workspace->hop_protect.selector =
                    workspace->candidate.hop_tx;
                workspace->hop_protect.aad = workspace->hop_aad;
                workspace->hop_protect.aad_bytes = hop_aad_bytes;
                workspace->hop_protect.hop_tag =
                    workspace->digest + UCN_I_SECURITY_ORIGIN_TAG_BYTES;
                result = owner->provider.protect_hop(
                    owner->provider.context, &workspace->hop_protect);
            }
        }
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        !current_facts_match(slot, &request->facts)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_c0_encode(
            &workspace->control_frame, workspace->candidate.address_width,
            workspace->candidate.hop_profile == UCN_I_HOP_PROFILE_H1 ?
                UCN_I_CONTROL_H1 : UCN_I_CONTROL_H0,
            output, output_capacity, output_bytes);
    }
    clear_callback(owner);
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_security_c0_receive_context_resolve(
    ucn_i_security_owner_t *owner,
    const uint8_t *packet,
    size_t packet_bytes,
    uint64_t now_us,
    uint32_t link_generation,
    ucn_i_security_control_access_request_t *access_out,
    ucn_i_security_current_facts_t *facts_out)
{
    ucn_i_c0_frame_t frame;
    uint16_t match_slot = UINT16_MAX;
    uint16_t match_opcode = 0U;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || packet == NULL || packet_bytes == 0U ||
        packet_bytes > UCN_ADAPTER_FRAME_BYTES || now_us == 0U ||
        link_generation == 0U || access_out == NULL || facts_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), packet, packet_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), access_out,
                             sizeof(*access_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts_out,
                             sizeof(*facts_out)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, access_out,
                             sizeof(*access_out)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, facts_out,
                             sizeof(*facts_out)) ||
        ucn_i_ranges_overlap(access_out, sizeof(*access_out), facts_out,
                             sizeof(*facts_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (slot_index = 0U; slot_index < UCN_I_SECURITY_SESSION_COUNT;
         ++slot_index) {
        ucn_i_security_slot_t *slot = &owner->sessions[slot_index];

        if (!slot->occupied ||
            slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
            slot->candidate.link_generation != link_generation ||
            slot->candidate.expires_at_us == 0U ||
            slot->durability.absolute_deadline_us == 0U ||
            ucn_i_deadline_expired_us(now_us,
                                      slot->candidate.expires_at_us) ||
            ucn_i_deadline_expired_us(
                now_us, slot->durability.absolute_deadline_us) ||
            ucn_i_c0_decode(packet, packet_bytes, owner->address_width,
                            slot->candidate.hop_profile, &frame) != UCN_OK ||
            frame.origin_security != slot->candidate.origin_level ||
            frame.origin_security == UCN_I_ORIGIN_O0 ||
            !c0_bindings_match(owner, slot, &frame,
                               UCN_I_SECURITY_ACCESS_INBOUND) ||
            !acl_rule_matches(
                owner, slot, slot->candidate.origin_fingerprint, 0U,
                frame.opcode, UCN_I_SECURITY_ACCESS_INBOUND)) {
            continue;
        }
        if (match_slot != UINT16_MAX) {
            owner_unlock(owner);
            return UCN_ERR_STATE;
        }
        match_slot = slot_index;
        match_opcode = frame.opcode;
    }
    if (match_slot == UINT16_MAX) {
        owner_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    {
        const ucn_i_security_slot_t *slot = &owner->sessions[match_slot];

        memset(access_out, 0, sizeof(*access_out));
        access_out->session.runtime_instance = owner->runtime_instance;
        access_out->session.owner_instance = owner->owner_instance;
        access_out->session.slot = match_slot;
        access_out->session.slot_generation = slot->slot_generation;
        access_out->session.session_generation =
            slot->candidate.session_generation;
        memcpy(access_out->context_fingerprint,
               slot->candidate.origin_fingerprint,
               sizeof(access_out->context_fingerprint));
        access_out->protocol_opcode = match_opcode;
        access_out->direction = UCN_I_SECURITY_ACCESS_INBOUND;
        memset(facts_out, 0, sizeof(*facts_out));
        facts_out->local = slot->candidate.local;
        facts_out->peer = slot->candidate.peer;
        facts_out->now_us = now_us;
        facts_out->link_generation = link_generation;
        facts_out->policy_generation = slot->candidate.policy_generation;
    }
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_c0_hop_receive_context_resolve(
    ucn_i_security_owner_t *owner,
    const uint8_t *packet,
    size_t packet_bytes,
    uint64_t now_us,
    uint32_t link_generation,
    ucn_i_security_handle_t *session_out,
    ucn_i_security_current_facts_t *facts_out)
{
    ucn_i_c0_frame_t frame;
    uint16_t match_slot = UINT16_MAX;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || packet == NULL || packet_bytes == 0U ||
        packet_bytes > UCN_ADAPTER_FRAME_BYTES || now_us == 0U ||
        link_generation == 0U || session_out == NULL || facts_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), packet, packet_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), session_out,
                             sizeof(*session_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts_out,
                             sizeof(*facts_out)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, session_out,
                             sizeof(*session_out)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, facts_out,
                             sizeof(*facts_out)) ||
        ucn_i_ranges_overlap(session_out, sizeof(*session_out), facts_out,
                             sizeof(*facts_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = ucn_i_c0_decode(packet, packet_bytes, owner->address_width,
                             UCN_I_CONTROL_H1, &frame);
    if (result != UCN_OK || frame.origin_security == UCN_I_ORIGIN_O0) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (slot_index = 0U; slot_index < UCN_I_SECURITY_SESSION_COUNT;
         ++slot_index) {
        const ucn_i_security_slot_t *slot = &owner->sessions[slot_index];

        if (!slot->occupied ||
            slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
            slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1 ||
            slot->candidate.link_generation != link_generation ||
            slot->candidate.expires_at_us == 0U ||
            slot->durability.absolute_deadline_us == 0U ||
            ucn_i_deadline_expired_us(now_us,
                                      slot->candidate.expires_at_us) ||
            ucn_i_deadline_expired_us(
                now_us, slot->durability.absolute_deadline_us) ||
            !acl_rule_matches(owner, slot,
                              slot->candidate.hop_fingerprint, 0U,
                              frame.opcode,
                              UCN_I_SECURITY_ACCESS_INBOUND)) {
            continue;
        }
        if (match_slot != UINT16_MAX) {
            owner_unlock(owner);
            return UCN_ERR_STATE;
        }
        match_slot = slot_index;
    }
    if (match_slot == UINT16_MAX) {
        owner_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    memset(session_out, 0, sizeof(*session_out));
    session_out->runtime_instance = owner->runtime_instance;
    session_out->owner_instance = owner->owner_instance;
    session_out->slot = match_slot;
    session_out->slot_generation =
        owner->sessions[match_slot].slot_generation;
    session_out->session_generation =
        owner->sessions[match_slot].candidate.session_generation;
    memset(facts_out, 0, sizeof(*facts_out));
    facts_out->local = owner->sessions[match_slot].candidate.local;
    facts_out->peer = owner->sessions[match_slot].candidate.peer;
    facts_out->now_us = now_us;
    facts_out->link_generation = link_generation;
    facts_out->policy_generation =
        owner->sessions[match_slot].candidate.policy_generation;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_c0_hop_send_context_resolve(
    ucn_i_security_owner_t *owner,
    uint16_t protocol_opcode,
    uint64_t now_us,
    uint32_t link_generation,
    ucn_i_security_handle_t *session_out,
    ucn_i_security_current_facts_t *facts_out)
{
    ucn_i_security_handle_t candidate_session;
    ucn_i_security_current_facts_t candidate_facts;
    uint16_t match_slot = UINT16_MAX;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || protocol_opcode == 0U ||
        !ucn_i_protocol_opcode_valid(protocol_opcode) || now_us == 0U ||
        link_generation == 0U || session_out == NULL || facts_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), session_out,
                             sizeof(*session_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts_out,
                             sizeof(*facts_out)) ||
        ucn_i_ranges_overlap(session_out, sizeof(*session_out), facts_out,
                             sizeof(*facts_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (slot_index = 0U; slot_index < UCN_I_SECURITY_SESSION_COUNT;
         ++slot_index) {
        const ucn_i_security_slot_t *slot = &owner->sessions[slot_index];

        if (!slot->occupied ||
            slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
            slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1 ||
            slot->candidate.link_generation != link_generation ||
            slot->candidate.expires_at_us == 0U ||
            slot->durability.absolute_deadline_us == 0U ||
            ucn_i_deadline_expired_us(now_us,
                                      slot->candidate.expires_at_us) ||
            ucn_i_deadline_expired_us(
                now_us, slot->durability.absolute_deadline_us) ||
            !acl_rule_matches(owner, slot,
                              slot->candidate.hop_fingerprint, 0U,
                              protocol_opcode,
                              UCN_I_SECURITY_ACCESS_OUTBOUND)) {
            continue;
        }
        if (match_slot != UINT16_MAX) {
            owner_unlock(owner);
            return UCN_ERR_STATE;
        }
        match_slot = slot_index;
    }
    if (match_slot == UINT16_MAX) {
        owner_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    memset(&candidate_session, 0, sizeof(candidate_session));
    candidate_session.runtime_instance = owner->runtime_instance;
    candidate_session.owner_instance = owner->owner_instance;
    candidate_session.slot = match_slot;
    candidate_session.slot_generation =
        owner->sessions[match_slot].slot_generation;
    candidate_session.session_generation =
        owner->sessions[match_slot].candidate.session_generation;
    memset(&candidate_facts, 0, sizeof(candidate_facts));
    candidate_facts.local = owner->sessions[match_slot].candidate.local;
    candidate_facts.peer = owner->sessions[match_slot].candidate.peer;
    candidate_facts.now_us = now_us;
    candidate_facts.link_generation = link_generation;
    candidate_facts.policy_generation =
        owner->sessions[match_slot].candidate.policy_generation;
    owner_unlock(owner);
    *session_out = candidate_session;
    *facts_out = candidate_facts;
    return UCN_OK;
}

ucn_result_t ucn_i_security_c0_origin_receive_context_resolve(
    ucn_i_security_owner_t *owner,
    const uint8_t *packet,
    size_t packet_bytes,
    uint64_t now_us,
    ucn_i_security_c0_origin_resolve_workspace_t *workspace,
    ucn_i_security_control_access_request_t *access_out,
    ucn_i_security_current_facts_t *facts_out)
{
    uint16_t match_slot = UINT16_MAX;
    uint16_t slot_index;
    ucn_result_t result;

    if (owner == NULL || packet == NULL || packet_bytes == 0U ||
        packet_bytes > UCN_ADAPTER_FRAME_BYTES || now_us == 0U ||
        workspace == NULL || access_out == NULL || facts_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), packet, packet_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), access_out,
                             sizeof(*access_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts_out,
                             sizeof(*facts_out)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, access_out,
                             sizeof(*access_out)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, facts_out,
                             sizeof(*facts_out)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), access_out,
                             sizeof(*access_out)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), facts_out,
                             sizeof(*facts_out)) ||
        ucn_i_ranges_overlap(access_out, sizeof(*access_out), facts_out,
                             sizeof(*facts_out))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_c0_decode(packet, packet_bytes, owner->address_width,
                             UCN_I_CONTROL_H1, &workspace->frame);
    if (result != UCN_OK) {
        result = ucn_i_c0_decode(packet, packet_bytes, owner->address_width,
                                 UCN_I_CONTROL_H0, &workspace->frame);
    }
    if (result != UCN_OK ||
        workspace->frame.origin_security == UCN_I_ORIGIN_O0) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (slot_index = 0U; slot_index < UCN_I_SECURITY_SESSION_COUNT;
         ++slot_index) {
        const ucn_i_security_slot_t *slot = &owner->sessions[slot_index];

        if (!slot->occupied ||
            slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
            slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H0 ||
            slot->candidate.origin_level !=
                workspace->frame.origin_security ||
            slot->candidate.expires_at_us == 0U ||
            slot->durability.absolute_deadline_us == 0U ||
            ucn_i_deadline_expired_us(now_us,
                                      slot->candidate.expires_at_us) ||
            ucn_i_deadline_expired_us(
                now_us, slot->durability.absolute_deadline_us) ||
            !c0_bindings_match(owner, slot, &workspace->frame,
                               UCN_I_SECURITY_ACCESS_INBOUND) ||
            !acl_rule_matches(owner, slot,
                              slot->candidate.origin_fingerprint, 0U,
                              workspace->frame.opcode,
                              UCN_I_SECURITY_ACCESS_INBOUND)) {
            continue;
        }
        if (match_slot != UINT16_MAX) {
            owner_unlock(owner);
            return UCN_ERR_STATE;
        }
        match_slot = slot_index;
    }
    if (match_slot == UINT16_MAX) {
        owner_unlock(owner);
        return UCN_ERR_NOT_FOUND;
    }
    workspace->access.session.runtime_instance = owner->runtime_instance;
    workspace->access.session.owner_instance = owner->owner_instance;
    workspace->access.session.slot = match_slot;
    workspace->access.session.slot_generation =
        owner->sessions[match_slot].slot_generation;
    workspace->access.session.session_generation =
        owner->sessions[match_slot].candidate.session_generation;
    memcpy(workspace->access.context_fingerprint,
           owner->sessions[match_slot].candidate.origin_fingerprint,
           sizeof(workspace->access.context_fingerprint));
    workspace->access.protocol_opcode = workspace->frame.opcode;
    workspace->access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    workspace->facts.local = owner->sessions[match_slot].candidate.local;
    workspace->facts.peer = owner->sessions[match_slot].candidate.peer;
    workspace->facts.now_us = now_us;
    workspace->facts.link_generation =
        owner->sessions[match_slot].candidate.link_generation;
    workspace->facts.policy_generation =
        owner->sessions[match_slot].candidate.policy_generation;
    owner_unlock(owner);
    *access_out = workspace->access;
    *facts_out = workspace->facts;
    return UCN_OK;
}

static UCN_I_STACK_PHASE bool c0_borrowed_ranges(
    ucn_i_security_owner_t *owner, const ucn_i_security_c0_open_call_t *call,
    ucn_i_security_packet_workspace_t *workspace)
{
    if (call == NULL ||
        ucn_i_ranges_overlap(call, sizeof(*call), owner, sizeof(*owner)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->access, sizeof(*call->access)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->facts, sizeof(*call->facts)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->packet, call->packet_bytes) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->plaintext_output, call->plaintext_capacity) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->plaintext_bytes, sizeof(*call->plaintext_bytes)) ||
        ucn_i_ranges_overlap(call, sizeof(*call), call->fact_out, sizeof(*call->fact_out))) { return false; }

    if (!c0_open_ranges_valid(owner, call->access, call->facts, call->packet, call->packet_bytes,
                              workspace, call->plaintext_output,
                              call->plaintext_capacity, call->plaintext_bytes, call->fact_out) ||
        call->packet_bytes > UCN_ADAPTER_FRAME_BYTES ||
        call->access->direction != UCN_I_SECURITY_ACCESS_INBOUND ||
        call->access->protocol_opcode == 0U || call->access->reserved_zero != 0U) {
        return false;
    }    return true;
}

static UCN_I_STACK_PHASE ucn_result_t c0_reserve_origin_replay_locked(
    ucn_i_security_owner_t *owner, ucn_i_security_slot_t *slot,
    const ucn_i_security_c0_open_call_t *call, ucn_i_security_packet_workspace_t *workspace)
{
    /* Isolate the reservation commit's outgoing arguments from the preceding
     * digest/provider phases; the same Owner lock remains held by the caller. */
    return replay_reserve_locked(owner, slot, &call->access->session, call->facts,
        workspace->control_frame.transaction_id, 1U, workspace->digest,
        workspace->digest + UCN_I_DIGEST128_BYTES, &workspace->replay);
}
UCN_I_STACK_PHASE ucn_result_t ucn_i_security_c0_open_borrowed(
    ucn_i_security_owner_t *owner, const ucn_i_security_c0_open_call_t *call,
    ucn_i_security_packet_workspace_t *workspace)
{
    ucn_i_security_slot_t *slot;
    uint16_t slot_index;

    ucn_result_t result;
    ucn_result_t gate_result;

    if (!c0_borrowed_ranges(owner, call, workspace)) { return UCN_ERR_ARGUMENT; }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &call->access->session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_c0_decode(call->packet, call->packet_bytes, owner->address_width,
                             slot->candidate.hop_profile,
                             &workspace->control_frame);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return result;
    }
    if (workspace->control_frame.payload_bytes > call->plaintext_capacity ||
        workspace->control_frame.origin_security !=
            slot->candidate.origin_level ||
        workspace->control_frame.origin_security == UCN_I_ORIGIN_O0 ||
        call->access->protocol_opcode != workspace->control_frame.opcode ||
        memcmp(call->access->context_fingerprint,
               slot->candidate.origin_fingerprint,
               UCN_I_SECURITY_FINGERPRINT_BYTES) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, call->facts) ||
        !c0_bindings_match(owner, slot, &workspace->control_frame,
                           call->access->direction) ||
        !acl_rule_matches(owner, slot, call->access->context_fingerprint, 0U,
                          call->access->protocol_opcode, call->access->direction) ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return workspace->control_frame.payload_bytes > call->plaintext_capacity ?
                   UCN_ERR_NO_SPACE : UCN_ERR_STATE;
    }
    result = ucn_i_c0_header_bytes(owner->address_width, &workspace->packet_state.header_bytes);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL) {
        c0_nonce_input_encode(slot, &workspace->control_frame,
                              call->access->direction, workspace->nonce_input);
    }
    workspace->packet_state.packet_before_hop_bytes = workspace->packet_state.header_bytes +
        workspace->control_frame.payload_bytes +
        UCN_I_SECURITY_ORIGIN_TAG_BYTES;
    if (slot->candidate.hop_profile == UCN_I_HOP_PROFILE_H1) {
        workspace->packet_state.hop_sequence = workspace->control_frame.hop_sequence;
        hop_aad_encode(call->packet, workspace->packet_state.packet_before_hop_bytes, workspace->packet_state.hop_sequence,
                       slot->candidate.hop_fingerprint,
                       workspace->hop_aad, &workspace->packet_state.hop_aad_bytes);
    }
    memset(&workspace->origin_open, 0, sizeof(workspace->origin_open));
    workspace->origin_open.selector = slot->candidate.origin_rx;
    workspace->origin_open.nonce_input =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ?
            workspace->nonce_input : NULL;
    workspace->origin_open.nonce_input_bytes =
        slot->candidate.origin_level == UCN_I_SECURITY_CONFIDENTIAL ? 44U : 0U;
    workspace->origin_open.aad = workspace->origin_aad;
    result = ucn_i_c0_origin_aad_build(&workspace->control_frame,
                                        owner->address_width,
                                        workspace->origin_aad,
                                        &workspace->origin_open.aad_bytes);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return result;
    }
    workspace->origin_open.protected_payload =
        workspace->control_frame.payload;
    workspace->origin_open.origin_tag = workspace->control_frame.origin_tag;
    workspace->origin_open.payload_bytes =
        workspace->control_frame.payload_bytes;
    workspace->origin_open.plaintext = workspace->plaintext;
    prepare_packet_callback(owner, workspace, slot_index, UCN_I_SECURITY_CALLBACK_OPEN);
    owner_unlock(owner);

    result = open_provider_run(owner, workspace,
        workspace->candidate.hop_profile == UCN_I_HOP_PROFILE_H1 ? workspace->control_frame.hop_tag : NULL,
                                   workspace->packet_state.hop_aad_bytes);

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        !current_facts_match(slot, call->facts) ||
        !c0_bindings_match(owner, slot, &workspace->control_frame,
                           call->access->direction)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    if (workspace->candidate.origin_level ==
            UCN_I_SECURITY_AUTHENTICATED &&
        workspace->control_frame.payload_bytes != 0U &&
        memcmp(workspace->plaintext, workspace->control_frame.payload,
               workspace->control_frame.payload_bytes) != 0) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_SECURITY;
    }
    result = UCN_I_DIGEST_CANONICAL("UCN6-AAD-V1",
        workspace->origin_aad, workspace->origin_open.aad_bytes,
        workspace->digest, &workspace->digest_workspace);
    if (result == UCN_OK) {
        result = UCN_I_DIGEST_CANONICAL("UCN6-PAYLOAD-V1",
            workspace->plaintext, workspace->control_frame.payload_bytes,
            workspace->digest + UCN_I_DIGEST128_BYTES,
            &workspace->digest_workspace);
    }
    if (result == UCN_OK) {
        result = c0_reserve_origin_replay_locked(owner, slot, call, workspace);
    }
    if (result == UCN_OK &&
        workspace->candidate.hop_profile == UCN_I_HOP_PROFILE_H1) {
        result = UCN_I_DIGEST_CANONICAL("UCN6-AAD-V1",
            workspace->hop_aad, workspace->packet_state.hop_aad_bytes, workspace->digest,
            &workspace->digest_workspace);
        if (result == UCN_OK) {
            result = hop_replay_reserve_locked(
                owner, slot, call->facts, workspace->packet_state.hop_sequence, workspace->digest,
                &workspace->replay);
        }
        if (result != UCN_OK) {
            replay_reservation_clear(
                &slot->origin_replay.reservations[
                    workspace->replay.reservation_slot]);
        }
    }
    if (result != UCN_OK) {
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    result = UCN_I_DIGEST_CANONICAL("UCN6-PAYLOAD-V1", workspace->plaintext,
                              workspace->control_frame.payload_bytes,
                              workspace->digest,
                              &workspace->digest_workspace);
    if (result != UCN_OK) {
        replay_reservation_clear(
            &slot->origin_replay.reservations[
                workspace->replay.reservation_slot]);
        if (workspace->replay.has_hop != 0U) {
            replay_reservation_clear(
                &slot->hop_replay.reservations[
                    workspace->replay.hop_reservation_slot]);
        }
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    clear_callback(owner);
    owner_unlock(owner);
    if (workspace->control_frame.payload_bytes != 0U) {
        memcpy(call->plaintext_output, workspace->plaintext,
               workspace->control_frame.payload_bytes);
    }
    memset(call->fact_out, 0, sizeof(*call->fact_out));
    call->fact_out->session = call->access->session;
    call->fact_out->replay = workspace->replay;
    call->fact_out->body = call->plaintext_output;
    call->fact_out->body_bytes = workspace->control_frame.payload_bytes;
    call->fact_out->transaction_id = workspace->control_frame.transaction_id;
    call->fact_out->runtime_instance = owner->runtime_instance;
    call->fact_out->realm_id = workspace->control_frame.realm_id;
    call->fact_out->source_address = workspace->control_frame.source_address;
    call->fact_out->destination_address =
        workspace->control_frame.destination_address;
    call->fact_out->source_binding_generation =
        workspace->control_frame.source_binding_generation;
    call->fact_out->destination_binding_generation =
        workspace->control_frame.destination_binding_generation;
    call->fact_out->security_owner_instance = owner->owner_instance;
    call->fact_out->protocol_opcode = workspace->control_frame.opcode;
    memcpy(call->fact_out->body_digest, workspace->digest,
           sizeof(call->fact_out->body_digest));
    call->fact_out->traffic_class = workspace->control_frame.traffic_class;
    call->fact_out->delivery_guarantee =
        workspace->control_frame.delivery_guarantee;
    call->fact_out->interaction_role = workspace->control_frame.interaction_role;
    call->fact_out->payload_kind = workspace->control_frame.payload_kind;
    call->fact_out->origin_security = workspace->control_frame.origin_security;
    call->fact_out->hop_limit = workspace->control_frame.hop_limit;
    call->fact_out->direction = call->access->direction;
    *call->plaintext_bytes = workspace->control_frame.payload_bytes;
    return UCN_OK;
}


ucn_result_t ucn_i_security_c0_open(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_control_access_request_t *access,
    const ucn_i_security_current_facts_t *facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_authenticated_control_t *fact_out)
{
    const ucn_i_security_c0_open_call_t call = {
        access, facts, packet, packet_bytes, plaintext_output, plaintext_capacity,
        plaintext_bytes, fact_out
    };
    return ucn_i_security_c0_open_borrowed(owner, &call, workspace);
}

static bool c0_hop_open_ranges_valid(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_current_facts_t *facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_c0_path_workspace_t *workspace)
{
    return owner != NULL && facts != NULL && packet != NULL &&
           packet_bytes != 0U && workspace != NULL &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                                 sizeof(*facts)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), packet,
                                 packet_bytes) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), packet,
                                 packet_bytes) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(packet, packet_bytes, workspace,
                                 sizeof(*workspace));
}

ucn_result_t ucn_i_security_c0_hop_open(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t inbound_session,
    const ucn_i_security_current_facts_t *facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_c0_path_workspace_t *workspace)
{
    ucn_i_security_packet_workspace_t *packet_workspace;
    ucn_i_security_slot_t *slot;
    size_t header_bytes = 0U;
    size_t hop_aad_bytes = 0U;
    size_t packet_before_hop_bytes;
    uint16_t slot_index;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;

    if (!c0_hop_open_ranges_valid(owner, facts, packet, packet_bytes,
                                   workspace) ||
        packet_bytes > UCN_ADAPTER_FRAME_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    memset(workspace, 0, sizeof(*workspace));
    packet_workspace = &workspace->packet;
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &inbound_session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1 ||
        !current_facts_match(slot, facts) ||
        ucn_i_c0_decode(packet, packet_bytes, owner->address_width,
                        UCN_I_CONTROL_H1,
                        &packet_workspace->control_frame) != UCN_OK ||
        packet_workspace->control_frame.realm_id != owner->realm_id ||
        packet_workspace->control_frame.origin_security == UCN_I_ORIGIN_O0 ||
        packet_workspace->control_frame.hop_limit == 0U ||
        !acl_rule_matches(owner, slot, slot->candidate.hop_fingerprint, 0U,
                          packet_workspace->control_frame.opcode,
                          UCN_I_SECURITY_ACCESS_INBOUND) ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    result = ucn_i_c0_header_bytes(owner->address_width, &header_bytes);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    packet_before_hop_bytes = header_bytes +
        packet_workspace->control_frame.payload_bytes +
        UCN_I_SECURITY_ORIGIN_TAG_BYTES;
    hop_aad_encode(packet, packet_before_hop_bytes,
                    packet_workspace->control_frame.hop_sequence,
                   slot->candidate.hop_fingerprint,
                    packet_workspace->hop_aad, &hop_aad_bytes);
    memset(&packet_workspace->hop_verify, 0,
           sizeof(packet_workspace->hop_verify));
    packet_workspace->hop_verify.selector = slot->candidate.hop_rx;
    packet_workspace->hop_verify.aad = packet_workspace->hop_aad;
    packet_workspace->hop_verify.aad_bytes = hop_aad_bytes;
    packet_workspace->hop_verify.hop_tag =
        packet_workspace->control_frame.hop_tag;
    packet_workspace->candidate = slot->candidate;
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_candidate = packet_workspace->candidate;
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = UCN_I_SECURITY_CALLBACK_OPEN;
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        result = owner->provider.verify_hop(owner->provider.context,
                                            &packet_workspace->hop_verify);
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &packet_workspace->candidate,
               sizeof(packet_workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &packet_workspace->candidate,
               sizeof(packet_workspace->candidate)) != 0 ||
        !current_facts_match(slot, facts)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_sha256_128(packet_workspace->hop_aad, hop_aad_bytes,
                                  packet_workspace->digest,
                                  &packet_workspace->digest_workspace);
    }
    memset(&packet_workspace->replay, 0, sizeof(packet_workspace->replay));
    packet_workspace->replay.session = inbound_session;
    if (result == UCN_OK) {
        result = hop_replay_reserve_locked(
            owner, slot, facts,
            packet_workspace->control_frame.hop_sequence,
            packet_workspace->digest, &packet_workspace->replay);
    }
    if (result == UCN_OK) {
        result = ucn_i_sha256_128(packet, packet_bytes,
                                  packet_workspace->digest +
                                      UCN_I_DIGEST128_BYTES,
                                  &packet_workspace->digest_workspace);
    }
    if (result != UCN_OK) {
        if (packet_workspace->replay.has_hop != 0U) {
            replay_reservation_clear(
                &slot->hop_replay.reservations[
                    packet_workspace->replay.hop_reservation_slot]);
        }
        clear_callback(owner);
        owner_unlock(owner);
        return result;
    }
    workspace->relay.replay.session = inbound_session;
    workspace->relay.replay.hop_sequence =
        packet_workspace->replay.hop_sequence;
    workspace->relay.replay.reservation_generation =
        packet_workspace->replay.hop_reservation_generation;
    workspace->relay.replay.reservation_slot =
        packet_workspace->replay.hop_reservation_slot;
    workspace->relay.packet = packet;
    workspace->relay.packet_bytes = packet_bytes;
    workspace->relay.transaction_id =
        packet_workspace->control_frame.transaction_id;
    workspace->relay.runtime_instance = owner->runtime_instance;
    workspace->relay.realm_id = packet_workspace->control_frame.realm_id;
    workspace->relay.source_address =
        packet_workspace->control_frame.source_address;
    workspace->relay.destination_address =
        packet_workspace->control_frame.destination_address;
    workspace->relay.source_binding_generation =
        packet_workspace->control_frame.source_binding_generation;
    workspace->relay.destination_binding_generation =
        packet_workspace->control_frame.destination_binding_generation;
    workspace->relay.protocol_opcode = packet_workspace->control_frame.opcode;
    memcpy(workspace->relay.packet_digest,
           packet_workspace->digest + UCN_I_DIGEST128_BYTES,
           sizeof(workspace->relay.packet_digest));
    workspace->relay.traffic_class =
        packet_workspace->control_frame.traffic_class;
    workspace->relay.delivery_guarantee =
        packet_workspace->control_frame.delivery_guarantee;
    workspace->relay.interaction_role =
        packet_workspace->control_frame.interaction_role;
    workspace->relay.payload_kind =
        packet_workspace->control_frame.payload_kind;
    workspace->relay.origin_security =
        packet_workspace->control_frame.origin_security;
    workspace->relay.hop_limit = packet_workspace->control_frame.hop_limit;
    clear_callback(owner);
    owner_unlock(owner);
    return UCN_OK;
}

static bool c0_hop_seal_ranges_valid(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_c0_relay_fact_t *relay,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    return owner != NULL && facts != NULL && relay != NULL &&
           workspace != NULL && output != NULL && output_capacity != 0U &&
           output_bytes != NULL && relay->packet != NULL &&
           relay->packet_bytes != 0U &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                                 sizeof(*facts)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), relay,
                                 sizeof(*relay)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(owner, sizeof(*owner), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(relay->packet, relay->packet_bytes, facts,
                                 sizeof(*facts)) &&
           !ucn_i_ranges_overlap(relay->packet, relay->packet_bytes, relay,
                                 sizeof(*relay)) &&
           !ucn_i_ranges_overlap(relay->packet, relay->packet_bytes,
                                 workspace, sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(relay->packet, relay->packet_bytes, output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(relay->packet, relay->packet_bytes,
                                 output_bytes, sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), relay,
                                 sizeof(*relay)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(facts, sizeof(*facts), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(relay, sizeof(*relay), workspace,
                                 sizeof(*workspace)) &&
           !ucn_i_ranges_overlap(relay, sizeof(*relay), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(relay, sizeof(*relay), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), output,
                                 output_capacity) &&
           !ucn_i_ranges_overlap(workspace, sizeof(*workspace), output_bytes,
                                 sizeof(*output_bytes)) &&
           !ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                                 sizeof(*output_bytes));
}

ucn_result_t ucn_i_security_c0_hop_seal(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t outbound_session,
    const ucn_i_security_current_facts_t *facts,
    const ucn_i_security_c0_relay_fact_t *relay,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    ucn_i_security_slot_t *slot;
    size_t encoded_bytes = 0U;
    size_t preliminary_bytes = 0U;
    size_t hop_aad_bytes = 0U;
    uint16_t slot_index;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;

    if (!c0_hop_seal_ranges_valid(owner, facts, relay, workspace, output,
                                   output_capacity, output_bytes) ||
        relay->runtime_instance != owner->runtime_instance ||
        relay->realm_id != owner->realm_id ||
        relay->hop_limit <= 1U || relay->origin_security == UCN_I_ORIGIN_O0 ||
        relay->reserved_zero[0] != 0U || relay->reserved_zero[1] != 0U ||
        relay->packet_bytes > UCN_ADAPTER_FRAME_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &outbound_session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_sha256_128(relay->packet, relay->packet_bytes,
                              workspace->digest,
                              &workspace->digest_workspace);
    if (result != UCN_OK ||
        memcmp(workspace->digest, relay->packet_digest,
               UCN_I_DIGEST128_BYTES) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1 ||
        !current_facts_match(slot, facts) ||
        !acl_rule_matches(owner, slot, slot->candidate.hop_fingerprint, 0U,
                          relay->protocol_opcode,
                          UCN_I_SECURITY_ACCESS_OUTBOUND) ||
        ucn_i_c0_decode(relay->packet, relay->packet_bytes,
                        owner->address_width, UCN_I_CONTROL_H1,
                        &workspace->control_frame) != UCN_OK ||
        workspace->control_frame.transaction_id != relay->transaction_id ||
        workspace->control_frame.realm_id != relay->realm_id ||
        workspace->control_frame.source_address != relay->source_address ||
        workspace->control_frame.destination_address !=
            relay->destination_address ||
        workspace->control_frame.source_binding_generation !=
            relay->source_binding_generation ||
        workspace->control_frame.destination_binding_generation !=
            relay->destination_binding_generation ||
        workspace->control_frame.opcode != relay->protocol_opcode ||
        workspace->control_frame.traffic_class != relay->traffic_class ||
        workspace->control_frame.delivery_guarantee !=
            relay->delivery_guarantee ||
        workspace->control_frame.interaction_role !=
            relay->interaction_role ||
        workspace->control_frame.payload_kind != relay->payload_kind ||
        workspace->control_frame.origin_security != relay->origin_security ||
        workspace->control_frame.hop_limit != relay->hop_limit ||
        slot->hop_tx_highest_reserved == UINT32_MAX ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return result == UCN_OK ? UCN_ERR_STATE : result;
    }
    workspace->control_frame.hop_limit =
        (uint8_t)(workspace->control_frame.hop_limit - 1U);
    workspace->control_frame.hop_sequence =
        slot->hop_tx_highest_reserved + 1U;
    workspace->control_frame.hop_tag =
        workspace->digest + UCN_I_DIGEST128_BYTES;
    result = ucn_i_c0_encoded_size(
        owner->address_width, workspace->control_frame.origin_security,
        UCN_I_CONTROL_H1, workspace->control_frame.payload_bytes,
        &encoded_bytes);
    if (result != UCN_OK || encoded_bytes > output_capacity ||
        encoded_bytes > UCN_ADAPTER_FRAME_BYTES) {
        owner_unlock(owner);
        return result == UCN_OK ? UCN_ERR_NO_SPACE : result;
    }
    slot->hop_tx_highest_reserved =
        workspace->control_frame.hop_sequence;
    workspace->candidate = slot->candidate;
    result = ucn_i_c0_encode(
        &workspace->control_frame, owner->address_width, UCN_I_CONTROL_H1,
        workspace->plaintext, sizeof(workspace->plaintext),
        &preliminary_bytes);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return result;
    }
    workspace->packet_state.packet_before_hop_bytes =
        preliminary_bytes - UCN_I_SECURITY_HOP_TRAILER_BYTES;
    hop_aad_encode(
        workspace->plaintext,
        workspace->packet_state.packet_before_hop_bytes,
        workspace->control_frame.hop_sequence,
        workspace->candidate.hop_fingerprint,
        workspace->hop_aad, &hop_aad_bytes);
    memset(&workspace->hop_protect, 0, sizeof(workspace->hop_protect));
    workspace->hop_protect.selector = workspace->candidate.hop_tx;
    workspace->hop_protect.aad = workspace->hop_aad;
    workspace->hop_protect.aad_bytes = hop_aad_bytes;
    workspace->hop_protect.hop_tag =
        workspace->digest + UCN_I_DIGEST128_BYTES;
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_candidate = workspace->candidate;
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = UCN_I_SECURITY_CALLBACK_PROTECT;
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        result = owner->provider.protect_hop(owner->provider.context,
                                             &workspace->hop_protect);
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        !current_facts_match(slot, facts)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_c0_encode(
            &workspace->control_frame, owner->address_width,
            UCN_I_CONTROL_H1, output, output_capacity, output_bytes);
    }
    clear_callback(owner);
    owner_unlock(owner);
    return result;
}

static ucn_result_t c0_hop_wrap_protected(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t outbound_session,
    const ucn_i_security_current_facts_t *facts,
    const uint8_t *protected_packet,
    size_t protected_packet_bytes,
    ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes,
    uint8_t input_hop_profile)
{
    ucn_i_security_slot_t *slot;
    size_t encoded_bytes = 0U;
    size_t preliminary_bytes = 0U;
    size_t hop_aad_bytes = 0U;
    uint16_t slot_index;
    ucn_result_t result;
    ucn_result_t gate_result;
    ucn_result_t leave_result;

    if (owner == NULL || facts == NULL || protected_packet == NULL ||
        protected_packet_bytes == 0U ||
        protected_packet_bytes > UCN_ADAPTER_FRAME_BYTES ||
        workspace == NULL || output == NULL || output_capacity == 0U ||
        output_bytes == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), protected_packet,
                             protected_packet_bytes) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                             output_capacity) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), protected_packet,
                             protected_packet_bytes) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), output,
                             output_capacity) ||
        ucn_i_ranges_overlap(facts, sizeof(*facts), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(protected_packet, protected_packet_bytes,
                             workspace, sizeof(*workspace)) ||
        ucn_i_ranges_overlap(protected_packet, protected_packet_bytes,
                             output, output_capacity) ||
        ucn_i_ranges_overlap(protected_packet, protected_packet_bytes,
                             output_bytes, sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), output,
                             output_capacity) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), output_bytes,
                             sizeof(*output_bytes)) ||
        ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                             sizeof(*output_bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &outbound_session, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (!borrowed_output_disjoint_locked(owner, workspace, sizeof(*workspace)) ||
        !borrowed_output_disjoint_locked(owner, output, output_capacity) ||
        !borrowed_output_disjoint_locked(owner, output_bytes, sizeof(*output_bytes))) {
        owner_unlock(owner); return UCN_ERR_ARGUMENT;
    }
    slot = &owner->sessions[slot_index];
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_c0_decode(protected_packet, protected_packet_bytes,
                             owner->address_width, input_hop_profile,
                             &workspace->control_frame);
    if (result != UCN_OK ||
        workspace->control_frame.realm_id != owner->realm_id ||
        workspace->control_frame.origin_security == UCN_I_ORIGIN_O0 ||
        workspace->control_frame.hop_limit == 0U ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        slot->candidate.hop_profile != UCN_I_HOP_PROFILE_H1 ||
        !current_facts_match(slot, facts) ||
        !acl_rule_matches(owner, slot, slot->candidate.hop_fingerprint, 0U,
                          workspace->control_frame.opcode,
                          UCN_I_SECURITY_ACCESS_OUTBOUND) ||
        slot->hop_tx_highest_reserved == UINT32_MAX ||
        owner->next_operation_id == UINT32_MAX) {
        owner_unlock(owner);
        return result == UCN_OK ? UCN_ERR_STATE : result;
    }
    if (input_hop_profile == UCN_I_CONTROL_H1 &&
        (workspace->control_frame.origin_security != slot->candidate.origin_level ||
         workspace->control_frame.source_address != slot->candidate.local.address ||
         workspace->control_frame.destination_address != slot->candidate.peer.address ||
         workspace->control_frame.source_binding_generation != slot->candidate.local.binding_generation ||
         workspace->control_frame.destination_binding_generation != slot->candidate.peer.binding_generation ||
         workspace->control_frame.transaction_id > slot->c0_tx_highest_consumed ||
         workspace->control_frame.hop_sequence > slot->hop_tx_highest_reserved ||
         !acl_rule_matches(owner, slot, slot->candidate.origin_fingerprint, 0U,
             workspace->control_frame.opcode, UCN_I_SECURITY_ACCESS_OUTBOUND))) {
        owner_unlock(owner); return UCN_ERR_ACCESS;
    }
    workspace->control_frame.hop_sequence =
        slot->hop_tx_highest_reserved + 1U;
    workspace->control_frame.hop_tag =
        workspace->digest + UCN_I_DIGEST128_BYTES;
    result = ucn_i_c0_encoded_size(
        owner->address_width, workspace->control_frame.origin_security,
        UCN_I_CONTROL_H1, workspace->control_frame.payload_bytes,
        &encoded_bytes);
    if (result != UCN_OK || encoded_bytes > output_capacity ||
        encoded_bytes > UCN_ADAPTER_FRAME_BYTES) {
        owner_unlock(owner);
        return result == UCN_OK ? UCN_ERR_NO_SPACE : result;
    }
    slot->hop_tx_highest_reserved =
        workspace->control_frame.hop_sequence;
    workspace->candidate = slot->candidate;
    result = ucn_i_c0_encode(
        &workspace->control_frame, owner->address_width, UCN_I_CONTROL_H1,
        workspace->plaintext, sizeof(workspace->plaintext),
        &preliminary_bytes);
    if (result != UCN_OK) {
        owner_unlock(owner);
        return result;
    }
    workspace->packet_state.packet_before_hop_bytes =
        preliminary_bytes - UCN_I_SECURITY_HOP_TRAILER_BYTES;
    hop_aad_encode(
        workspace->plaintext,
        workspace->packet_state.packet_before_hop_bytes,
        workspace->control_frame.hop_sequence,
        workspace->candidate.hop_fingerprint,
        workspace->hop_aad, &hop_aad_bytes);
    memset(&workspace->hop_protect, 0, sizeof(workspace->hop_protect));
    workspace->hop_protect.selector = workspace->candidate.hop_tx;
    workspace->hop_protect.aad = workspace->hop_aad;
    workspace->hop_protect.aad_bytes = hop_aad_bytes;
    workspace->hop_protect.hop_tag =
        workspace->digest + UCN_I_DIGEST128_BYTES;
    owner->callback_active = 1U;
    owner->callback_slot = slot_index;
    owner->callback_candidate = workspace->candidate;
    owner->callback_claim.owner_instance = owner->owner_instance;
    owner->callback_claim.operation_id = owner->next_operation_id++;
    owner->callback_claim.operation_generation = 1U;
    owner->callback_claim.operation_kind = UCN_I_SECURITY_CALLBACK_PROTECT;
    owner_unlock(owner);

    gate_result = ucn_i_callback_gate_enter(owner->provider_gate,
                                             &owner->callback_claim);
    if (gate_result == UCN_OK) {
        result = owner->provider.protect_hop(owner->provider.context,
                                             &workspace->hop_protect);
        leave_result = ucn_i_callback_gate_leave(owner->provider_gate,
                                                  &owner->callback_claim);
        if (leave_result != UCN_OK) {
            result = UCN_ERR_STATE;
        }
    } else {
        result = gate_result;
    }

    gate_result = owner_lock(owner);
    if (gate_result != UCN_OK) {
        return gate_result;
    }
    slot = &owner->sessions[slot_index];
    if (owner->callback_active == 0U ||
        owner->callback_slot != slot_index ||
        memcmp(&owner->callback_candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        memcmp(&slot->candidate, &workspace->candidate,
               sizeof(workspace->candidate)) != 0 ||
        !current_facts_match(slot, facts)) {
        clear_callback(owner);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (result == UCN_OK) {
        result = ucn_i_c0_encode(
            &workspace->control_frame, owner->address_width,
            UCN_I_CONTROL_H1, output, output_capacity, output_bytes);
    }
    clear_callback(owner);
    owner_unlock(owner);
    return result;
}

ucn_result_t ucn_i_security_c0_refresh_hop(ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session, const ucn_i_security_current_facts_t *facts,
    const uint8_t *packet, size_t packet_bytes, ucn_i_security_packet_workspace_t *workspace,
    uint8_t *output, size_t capacity, size_t *written)
{
    return c0_hop_wrap_protected(owner, session, facts, packet, packet_bytes,
        workspace, output, capacity, written, UCN_I_CONTROL_H1);
}

/* A split path owns an H0 end-to-end Origin and an independent H1 next Hop.
 * Direct H1 Sessions already emit/consume one Hop trailer in c0_protect/open;
 * interpreting that frame as H0 would turn its old trailer into payload. */
static ucn_result_t c0_path_origin_check(ucn_i_security_owner_t *owner,
    const ucn_i_security_handle_t *session, const ucn_i_security_current_facts_t *facts)
{
    uint16_t slot; ucn_result_t result = owner_lock(owner);
    if (result != UCN_OK) { return result; }
    if (owner->callback_active || !handle_matches(owner, session, &slot) ||
        owner->sessions[slot].phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        owner->sessions[slot].candidate.hop_profile != UCN_I_HOP_PROFILE_H0 ||
        !current_facts_match(&owner->sessions[slot], facts)) { result = UCN_ERR_STATE; }
    owner_unlock(owner); return result;
}

ucn_result_t ucn_i_security_c0_path_protect(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_c0_protect_request_t *origin_request,
    ucn_i_security_handle_t hop_session,
    const ucn_i_security_current_facts_t *hop_facts,
    const uint8_t *plaintext,
    size_t plaintext_bytes,
    ucn_i_security_c0_path_workspace_t *workspace,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bytes)
{
    size_t origin_packet_bytes = 0U;
    ucn_result_t result;

    if (owner == NULL || origin_request == NULL || hop_facts == NULL ||
        workspace == NULL || output == NULL || output_capacity == 0U ||
        output_bytes == NULL || (plaintext == NULL && plaintext_bytes != 0U) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), origin_request,
                             sizeof(*origin_request)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), hop_facts,
                             sizeof(*hop_facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), output,
                             output_capacity) ||
        (plaintext_bytes != 0U &&
         (ucn_i_ranges_overlap(owner, sizeof(*owner), plaintext,
                               plaintext_bytes) ||
          ucn_i_ranges_overlap(plaintext, plaintext_bytes, workspace,
                               sizeof(*workspace)) ||
          ucn_i_ranges_overlap(plaintext, plaintext_bytes, output,
                               output_capacity))) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace), output,
                             output_capacity) ||
        ucn_i_ranges_overlap(output, output_capacity, output_bytes,
                             sizeof(*output_bytes))) {
        return UCN_ERR_ARGUMENT;
    }
    result = c0_path_origin_check(owner, &origin_request->session, &origin_request->facts);
    if (result != UCN_OK) { return result; }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_security_c0_protect(
        owner, origin_request, plaintext, plaintext_bytes,
        &workspace->packet, workspace->origin_packet,
        sizeof(workspace->origin_packet), &origin_packet_bytes);
    if (result != UCN_OK) {
        return result;
    }
    return c0_hop_wrap_protected(
        owner, hop_session, hop_facts, workspace->origin_packet,
        origin_packet_bytes, &workspace->packet, output, output_capacity,
        output_bytes, UCN_I_CONTROL_H0);
}

ucn_result_t ucn_i_security_c0_path_open(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_control_access_request_t *origin_access,
    const ucn_i_security_current_facts_t *origin_facts,
    ucn_i_security_handle_t hop_session,
    const ucn_i_security_current_facts_t *hop_facts,
    const uint8_t *packet,
    size_t packet_bytes,
    ucn_i_security_c0_path_workspace_t *workspace,
    uint8_t *plaintext_output,
    size_t plaintext_capacity,
    size_t *plaintext_bytes,
    ucn_i_security_authenticated_control_t *fact_out,
    ucn_i_security_hop_replay_handle_t *hop_replay_out)
{
    size_t origin_packet_bytes = 0U;
    ucn_result_t result;
    ucn_result_t abort_result;

    if (owner == NULL || origin_access == NULL || origin_facts == NULL ||
        hop_facts == NULL || packet == NULL || packet_bytes == 0U ||
        workspace == NULL || plaintext_output == NULL ||
        plaintext_capacity == 0U || plaintext_bytes == NULL ||
        fact_out == NULL || hop_replay_out == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), packet, packet_bytes) ||
        ucn_i_ranges_overlap(packet, packet_bytes, workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(packet, packet_bytes, plaintext_output,
                             plaintext_capacity) ||
        ucn_i_ranges_overlap(workspace, sizeof(*workspace),
                             plaintext_output, plaintext_capacity) ||
        ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                             plaintext_bytes, sizeof(*plaintext_bytes)) ||
        ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                             fact_out, sizeof(*fact_out)) ||
        ucn_i_ranges_overlap(plaintext_output, plaintext_capacity,
                             hop_replay_out, sizeof(*hop_replay_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = c0_path_origin_check(owner, &origin_access->session, origin_facts);
    if (result != UCN_OK) { return result; }
    memset(workspace, 0, sizeof(*workspace));
    result = ucn_i_security_c0_hop_open(
        owner, hop_session, hop_facts, packet, packet_bytes, workspace);
    if (result != UCN_OK) {
        return result;
    }
    workspace->packet.control_frame.hop_sequence = 0U;
    workspace->packet.control_frame.hop_tag = NULL;
    result = ucn_i_c0_encode(
        &workspace->packet.control_frame, owner->address_width,
        UCN_I_CONTROL_H0, workspace->origin_packet,
        sizeof(workspace->origin_packet), &origin_packet_bytes);
    if (result == UCN_OK) {
        result = ucn_i_security_c0_open(
            owner, origin_access, origin_facts, workspace->origin_packet,
            origin_packet_bytes, &workspace->packet, plaintext_output,
            plaintext_capacity, plaintext_bytes, fact_out);
    }
    if (result != UCN_OK) {
        abort_result = ucn_i_security_hop_replay_abort(
            owner, &(workspace->relay.replay));
        return abort_result == UCN_OK ? result : UCN_ERR_IN_DOUBT;
    }
    *hop_replay_out = workspace->relay.replay;
    return UCN_OK;
}

ucn_result_t ucn_i_security_control_fact_validate(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_authenticated_control_t *fact,
    const ucn_i_security_current_facts_t *current_facts,
    ucn_i_security_control_validate_workspace_t *workspace)
{
    ucn_i_security_slot_t *slot;
    size_t aad_bytes = 0U;
    uint16_t session_slot;
    uint16_t reservation_slot;
    ucn_result_t result;

    if (owner == NULL || fact == NULL || current_facts == NULL ||
        workspace == NULL ||
        (fact->body == NULL && fact->body_bytes != 0U) ||
        fact->body_bytes > UCN_ADAPTER_FRAME_BYTES ||
        fact->runtime_instance != owner->runtime_instance ||
        fact->security_owner_instance != owner->owner_instance ||
        fact->transaction_id == 0U || fact->protocol_opcode == 0U ||
        fact->traffic_class >= UCN_TRAFFIC_CLASS_COUNT ||
        fact->delivery_guarantee > UCN_DELIVERY_RELIABLE ||
        fact->interaction_role > UCN_INTERACTION_ERROR ||
        fact->payload_kind > UCN_I_PAYLOAD_DIAGNOSTIC ||
        fact->origin_security == UCN_I_ORIGIN_O0 ||
        fact->origin_security > UCN_I_ORIGIN_O2 ||
        fact->hop_limit == 0U || fact->hop_limit > 63U ||
        fact->direction != UCN_I_SECURITY_ACCESS_INBOUND ||
        fact->reserved_zero != 0U ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), fact, sizeof(*fact)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), current_facts,
                             sizeof(*current_facts)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(fact, sizeof(*fact), workspace,
                             sizeof(*workspace)) ||
        ucn_i_ranges_overlap(current_facts, sizeof(*current_facts),
                             workspace, sizeof(*workspace)) ||
        (fact->body_bytes != 0U &&
         (ucn_i_ranges_overlap(owner, sizeof(*owner), fact->body,
                               fact->body_bytes) ||
          ucn_i_ranges_overlap(fact, sizeof(*fact), fact->body,
                               fact->body_bytes) ||
          ucn_i_ranges_overlap(current_facts, sizeof(*current_facts),
                               fact->body, fact->body_bytes) ||
          ucn_i_ranges_overlap(workspace, sizeof(*workspace), fact->body,
                               fact->body_bytes)))) {
        return UCN_ERR_ARGUMENT;
    }
    memset(workspace, 0, sizeof(*workspace));
    result = UCN_I_DIGEST_CANONICAL("UCN6-PAYLOAD-V1", fact->body, fact->body_bytes,
                              workspace->digest,
                              &workspace->digest_workspace);
    if (result != UCN_OK ||
        memcmp(workspace->digest, fact->body_digest,
               sizeof(workspace->digest)) != 0) {
        return result == UCN_OK ? UCN_ERR_SECURITY : result;
    }
    memset(&workspace->canonical, 0, sizeof(workspace->canonical));
    workspace->canonical.transaction_id = fact->transaction_id;
    workspace->canonical.realm_id = fact->realm_id;
    workspace->canonical.source_address = fact->source_address;
    workspace->canonical.destination_address = fact->destination_address;
    workspace->canonical.source_binding_generation =
        fact->source_binding_generation;
    workspace->canonical.destination_binding_generation =
        fact->destination_binding_generation;
    workspace->canonical.opcode = fact->protocol_opcode;
    workspace->canonical.traffic_class = fact->traffic_class;
    workspace->canonical.delivery_guarantee = fact->delivery_guarantee;
    workspace->canonical.interaction_role = fact->interaction_role;
    workspace->canonical.payload_kind = fact->payload_kind;
    workspace->canonical.origin_security = fact->origin_security;
    workspace->canonical.hop_limit = fact->hop_limit;
    workspace->canonical.payload = fact->body;
    workspace->canonical.payload_bytes = fact->body_bytes;
    workspace->canonical.origin_tag = workspace->digest;
    result = ucn_i_c0_origin_aad_build(
        &workspace->canonical, owner->address_width, workspace->aad,
        &aad_bytes);
    if (result != UCN_OK) {
        return result;
    }
    result = UCN_I_DIGEST_CANONICAL("UCN6-AAD-V1", workspace->aad, aad_bytes, workspace->digest,
                              &workspace->digest_workspace);
    if (result != UCN_OK) {
        return result;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &fact->session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, current_facts) ||
        fact->realm_id != owner->realm_id ||
        fact->source_address != slot->candidate.peer.address ||
        fact->destination_address != slot->candidate.local.address ||
        fact->source_binding_generation !=
            slot->candidate.peer.binding_generation ||
        fact->destination_binding_generation !=
            slot->candidate.local.binding_generation ||
        fact->origin_security != slot->candidate.origin_level ||
        !ucn_i_protocol_opcode_valid(fact->protocol_opcode) ||
        ((fact->payload_kind == UCN_I_PAYLOAD_DIAGNOSTIC) !=
         ((fact->protocol_opcode >> 8U) == UCN_I_OPCODE_DIAGNOSTICS)) ||
        fact->transaction_id != fact->replay.sequence ||
        fact->replay.replay_domain != 1U ||
        !replay_handle_matches(owner, slot, &(fact->replay),
                               &reservation_slot) ||
        memcmp(workspace->digest,
               slot->origin_replay.reservations[reservation_slot].aad_digest,
               sizeof(workspace->digest)) != 0 ||
        !acl_rule_matches(owner, slot,
                          slot->candidate.origin_fingerprint, 0U,
                          fact->protocol_opcode, fact->direction)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner_unlock(owner);
    return UCN_OK;
}

static void replay_reservation_clear(
    ucn_i_security_replay_reservation_t *reservation)
{
    uint32_t generation = reservation->generation;

    memset(reservation, 0, sizeof(*reservation));
    reservation->generation = generation;
}

static void replay_bits_commit(uint64_t *highest, uint64_t *bitmap,
                               uint64_t sequence)
{
    uint64_t distance;

    if (*highest == 0U || sequence > *highest) {
        distance = *highest == 0U ? 64U : sequence - *highest;
        *bitmap = distance >= 64U ? 0U : *bitmap << distance;
        *highest = sequence;
        *bitmap |= UINT64_C(1);
    } else {
        distance = *highest - sequence;
        *bitmap |= UINT64_C(1) << distance;
    }
}

static void replay_window_commit(ucn_i_security_replay_window_t *window,
                                 uint64_t sequence)
{
    replay_bits_commit(&window->highest_committed,
                        &window->committed_bitmap, sequence);
}

static ucn_result_t replay_commit_internal(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts,
    bool require_business_completion)
{
    ucn_i_security_replay_reservation_t *reservation;
    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    uint16_t reservation_slot;
    uint16_t hop_reservation_slot = 0U;
    ucn_result_t result;

    if (facts == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle->session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    if (!replay_handle_matches(owner, slot, handle, &reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    reservation = &slot->origin_replay.reservations[reservation_slot];
    if (handle->has_hop != 0U &&
        !hop_replay_handle_matches(slot, handle, &hop_reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts) ||
        facts->now_us >= reservation->deadline_us ||
        (require_business_completion &&
         reservation->business_completed == 0U) ||
        (handle->has_hop != 0U &&
         facts->now_us >= slot->hop_replay
                              .reservations[hop_reservation_slot]
                              .deadline_us) ||
        origin_is_committed(slot, handle->replay_domain, handle->sequence)) {
        replay_reservation_clear(reservation);
        if (handle->has_hop != 0U) {
            replay_reservation_clear(
                &slot->hop_replay.reservations[hop_reservation_slot]);
        }
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (handle->has_hop != 0U &&
        replay_is_committed(&slot->hop_replay, handle->hop_sequence)) {
        replay_reservation_clear(reservation);
        replay_reservation_clear(
            &slot->hop_replay.reservations[hop_reservation_slot]);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (handle->replay_domain == 0U) {
        replay_window_commit(&slot->origin_replay, handle->sequence);
    } else {
        replay_bits_commit(&slot->c0_rx_highest_committed,
                            &slot->c0_rx_committed_bitmap, handle->sequence);
    }
    if (handle->has_hop != 0U) {
        replay_window_commit(&slot->hop_replay, handle->hop_sequence);
        replay_reservation_clear(
            &slot->hop_replay.reservations[hop_reservation_slot]);
    }
    replay_reservation_clear(reservation);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_replay_commit(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts)
{
    if(owner == NULL || handle == NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),handle,sizeof(*handle)))
        return UCN_ERR_ARGUMENT;

    return replay_commit_internal(owner, handle, facts, false);
}

ucn_result_t ucn_i_security_control_replay_complete(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts)
{
    if(owner == NULL || handle == NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),handle,sizeof(*handle)))
        return UCN_ERR_ARGUMENT;

    ucn_i_security_replay_reservation_t *reservation;
    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    uint16_t reservation_slot;
    uint16_t hop_reservation_slot = 0U;
    ucn_result_t result;

    if (facts == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle->session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    if (!replay_handle_matches(owner, slot, handle, &reservation_slot) ||
        (handle->has_hop != 0U &&
         !hop_replay_handle_matches(slot, handle,
                                    &hop_reservation_slot))) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    reservation = &slot->origin_replay.reservations[reservation_slot];
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts) ||
        facts->now_us >= reservation->deadline_us ||
        origin_is_committed(slot, handle->replay_domain, handle->sequence) ||
        (handle->has_hop != 0U &&
         (facts->now_us >= slot->hop_replay
                               .reservations[hop_reservation_slot]
                               .deadline_us ||
          replay_is_committed(&slot->hop_replay,
                              handle->hop_sequence)))) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    reservation->business_completed = 1U;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_control_replay_commit(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts)
{
    if(owner == NULL || handle == NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),handle,sizeof(*handle)))
        return UCN_ERR_ARGUMENT;

    return replay_commit_internal(owner, handle, facts, true);
}

ucn_result_t ucn_i_security_replay_abort(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_replay_handle_t *handle)
{
    if(owner == NULL || handle == NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),handle,sizeof(*handle)))
        return UCN_ERR_ARGUMENT;

    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    uint16_t reservation_slot;
    uint16_t hop_reservation_slot = 0U;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle->session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    if (!replay_handle_matches(owner, slot, handle, &reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (handle->has_hop != 0U &&
        !hop_replay_handle_matches(slot, handle, &hop_reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    replay_reservation_clear(
        &slot->origin_replay.reservations[reservation_slot]);
    if (handle->has_hop != 0U) {
        replay_reservation_clear(
            &slot->hop_replay.reservations[hop_reservation_slot]);
    }
    owner_unlock(owner);
    return UCN_OK;
}

static bool hop_only_handle_matches(
    const ucn_i_security_slot_t *slot,
    const ucn_i_security_hop_replay_handle_t *handle,
    uint16_t *reservation_slot_out)
{
    const uint16_t index = handle->reservation_slot;

    if (handle->reserved_zero != 0U || handle->hop_sequence == 0U ||
        handle->reservation_generation == 0U ||
        index >= UCN_I_SECURITY_REPLAY_RESERVATION_COUNT ||
        slot->hop_replay.reservations[index].valid == 0U ||
        slot->hop_replay.reservations[index].generation !=
            handle->reservation_generation ||
        slot->hop_replay.reservations[index].sequence !=
            handle->hop_sequence) {
        return false;
    }
    *reservation_slot_out = index;
    return true;
}

ucn_result_t ucn_i_security_hop_replay_commit(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_hop_replay_handle_t *handle,
    const ucn_i_security_current_facts_t *facts)
{
    if(owner == NULL || handle == NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),handle,sizeof(*handle)))
        return UCN_ERR_ARGUMENT;

    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    uint16_t reservation_slot;
    ucn_result_t result;

    if (owner == NULL || facts == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), facts,
                             sizeof(*facts))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle->session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    if (!hop_only_handle_matches(slot, handle, &reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    if (slot->phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        !current_facts_match(slot, facts) ||
        facts->now_us >=
            slot->hop_replay.reservations[reservation_slot].deadline_us ||
        replay_is_committed(&slot->hop_replay, handle->hop_sequence)) {
        replay_reservation_clear(
            &slot->hop_replay.reservations[reservation_slot]);
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    replay_window_commit(&slot->hop_replay, handle->hop_sequence);
    replay_reservation_clear(
        &slot->hop_replay.reservations[reservation_slot]);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_hop_replay_abort(
    ucn_i_security_owner_t *owner,
    const ucn_i_security_hop_replay_handle_t *handle)
{
    if(owner == NULL || handle == NULL ||
       ucn_i_ranges_overlap(owner,sizeof(*owner),handle,sizeof(*handle)))
        return UCN_ERR_ARGUMENT;

    ucn_i_security_slot_t *slot;
    uint16_t session_slot;
    uint16_t reservation_slot;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle->session, &session_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[session_slot];
    if (!hop_only_handle_matches(slot, handle, &reservation_slot)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    replay_reservation_clear(
        &slot->hop_replay.reservations[reservation_slot]);
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_replay_maintain(
    ucn_i_security_owner_t *owner,
    uint64_t now_us,
    uint16_t budget,
    uint16_t *inspected_out,
    uint16_t *expired_out)
{
    uint16_t inspected = 0U;
    uint16_t expired = 0U;
    uint16_t per_session = (uint16_t)(
        (2U * UCN_I_SECURITY_REPLAY_RESERVATION_COUNT) + 1U);
    uint16_t total = (uint16_t)(UCN_I_SECURITY_SESSION_COUNT * per_session);
    ucn_result_t result;

    if (now_us == 0U || budget == 0U || inspected_out == NULL ||
        expired_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), inspected_out,
                             sizeof(*inspected_out)) ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), expired_out,
                             sizeof(*expired_out)) ||
        ucn_i_ranges_overlap(inspected_out, sizeof(*inspected_out),
                             expired_out, sizeof(*expired_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    while (inspected < budget && inspected < total) {
        uint16_t flat = owner->replay_maintenance_cursor;
        uint16_t session_index = (uint16_t)(flat / per_session);
        uint16_t reservation_index = (uint16_t)(flat % per_session);

        if (reservation_index == 0U) {
            ucn_i_security_tx_reservation_t *tx =
                &owner->sessions[session_index].origin_tx_reservation;

            if (tx->valid != 0U && now_us >= tx->deadline_us) {
                tx_reservation_clear(tx);
                expired++;
            }
        } else if (reservation_index <=
                   UCN_I_SECURITY_REPLAY_RESERVATION_COUNT) {
            ucn_i_security_replay_reservation_t *reservation =
                &owner->sessions[session_index]
                     .origin_replay.reservations[reservation_index - 1U];

            if (reservation->valid != 0U &&
                now_us >= reservation->deadline_us) {
                replay_reservation_clear(reservation);
                expired++;
            }
        } else {
            uint16_t hop_index = (uint16_t)(
                reservation_index -
                UCN_I_SECURITY_REPLAY_RESERVATION_COUNT - 1U);
            ucn_i_security_replay_reservation_t *reservation =
                &owner->sessions[session_index]
                     .hop_replay.reservations[hop_index];

            if (reservation->valid != 0U &&
                now_us >= reservation->deadline_us) {
                replay_reservation_clear(reservation);
                expired++;
            }
        }
        owner->replay_maintenance_cursor = (uint16_t)((flat + 1U) % total);
        inspected++;
    }
    *inspected_out = inspected;
    *expired_out = expired;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_session_get(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle,
    ucn_i_security_session_view_t *view_out)
{
    const ucn_i_security_slot_t *slot;
    uint16_t slot_index;
    ucn_result_t result;

    if (view_out == NULL || owner == NULL ||
        ucn_i_ranges_overlap(owner, sizeof(*owner), view_out,
                             sizeof(*view_out))) {
        return UCN_ERR_ARGUMENT;
    }
    result = owner_lock(owner);
    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot = &owner->sessions[slot_index];
    /* All fallible checks precede publication under the same Owner lock.
     * 同锁内先完成全部可失败检查，再直接写只读快照，避免完整自动副本。 */
    memset(view_out, 0, sizeof(*view_out));
    view_out->peer = slot->candidate.peer;
    view_out->expires_at_us = slot->candidate.expires_at_us;
    view_out->link_generation = slot->candidate.link_generation;
    view_out->session_generation = slot->candidate.session_generation;
    view_out->policy_generation = slot->candidate.policy_generation;
    view_out->origin_tx_key_generation =
        slot->candidate.origin_tx.key_generation;
    view_out->origin_rx_key_generation =
        slot->candidate.origin_rx.key_generation;
    view_out->origin_level = slot->candidate.origin_level;
    view_out->hop_profile = slot->candidate.hop_profile;
    view_out->phase = slot->phase;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_fence(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle)
{
    uint16_t slot_index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle, &slot_index)) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    owner->sessions[slot_index].phase = UCN_I_SECURITY_SESSION_FENCED;
    memset(&owner->sessions[slot_index].origin_tx_reservation, 0,
           sizeof(owner->sessions[slot_index].origin_tx_reservation));
    memset(&owner->sessions[slot_index].origin_replay, 0,
           sizeof(owner->sessions[slot_index].origin_replay));
    owner->sessions[slot_index].c0_rx_highest_committed = 0U;
    owner->sessions[slot_index].c0_rx_committed_bitmap = 0U;
    memset(&owner->sessions[slot_index].hop_replay, 0,
           sizeof(owner->sessions[slot_index].hop_replay));
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_retire_fenced(
    ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t handle)
{
    uint32_t slot_generation;
    uint16_t slot_index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U ||
        !handle_matches(owner, &handle, &slot_index) ||
        owner->sessions[slot_index].phase !=
            UCN_I_SECURITY_SESSION_FENCED) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    slot_generation = owner->sessions[slot_index].slot_generation;
    memset(&owner->sessions[slot_index], 0,
           sizeof(owner->sessions[slot_index]));
    owner->sessions[slot_index].slot_generation = slot_generation;
    owner_unlock(owner);
    return UCN_OK;
}

ucn_result_t ucn_i_security_owner_destroy(ucn_i_security_owner_t *owner)
{
    uint16_t index;
    ucn_result_t result = owner_lock(owner);

    if (result != UCN_OK) {
        return result;
    }
    if (owner->callback_active != 0U) {
        owner_unlock(owner);
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        if (owner->sessions[index].occupied != 0U) {
            owner_unlock(owner);
            return UCN_ERR_STATE;
        }
    }
    owner_unlock(owner);
    memset(owner, 0, sizeof(*owner));
    return UCN_OK;
}
