#include "internal/ucn_control_wire.h"

#include "rust/tests/conformance/v6s_wire_core_v1.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

/* EN: Keep every assertion active in Release so MSVC /W4 /WX also compiles
 * and executes all Golden/zero-write checks. 中文：Release 下也保持全部断言
 * 生效，使 MSVC /W4 /WX 真正编译并执行 Golden 与零写检查。 */
#undef assert
#define assert(condition_) CHECK(condition_)

static const uint8_t c0_golden[] = {
    UCN_V6S_WIRE_C0_A1_O0_H0_BOOTSTRAP_BYTES
};

static ucn_i_c0_frame_t bootstrap_frame(void)
{
    static const uint8_t payload[] = {0xAAU, 0x55U};
    ucn_i_c0_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.payload = payload;
    frame.payload_bytes = sizeof(payload);
    frame.transaction_id = UINT64_C(0x0102030405060708);
    frame.realm_id = UINT32_C(0x01020304);
    frame.destination_address = UINT32_C(0xFFFF);
    frame.opcode = UCN_I_OPCODE_BOOTSTRAP_HELLO;
    frame.traffic_class = UCN_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    frame.interaction_role = UCN_INTERACTION_ONE_WAY;
    frame.payload_kind = UCN_I_PAYLOAD_CONTROL;
    frame.origin_security = UCN_I_ORIGIN_O0;
    frame.hop_limit = 1U;
    return frame;
}

static void test_typed_to_raw_uses_shared_golden(void)
{
    ucn_i_c0_frame_t frame = bootstrap_frame();
    uint8_t output[sizeof(c0_golden)];
    size_t output_bytes = 0U;

    memset(output, 0xA5, sizeof(output));
    assert(ucn_i_c0_encode(&frame, 2U, UCN_I_CONTROL_H0, output,
                           sizeof(output), &output_bytes) == UCN_OK);
    assert(output_bytes == sizeof(c0_golden));
    assert(memcmp(output, c0_golden, sizeof(output)) == 0);
}

static void test_raw_to_typed_uses_shared_golden(void)
{
    ucn_i_c0_frame_t decoded;

    memset(&decoded, 0xA5, sizeof(decoded));
    assert(ucn_i_c0_decode(c0_golden, sizeof(c0_golden), 2U,
                           UCN_I_CONTROL_H0, &decoded) == UCN_OK);
    assert(decoded.realm_id == UINT32_C(0x01020304));
    assert(decoded.source_address == 0U);
    assert(decoded.destination_address == UINT32_C(0xFFFF));
    assert(decoded.source_binding_generation == 0U);
    assert(decoded.destination_binding_generation == 0U);
    assert(decoded.transaction_id == UINT64_C(0x0102030405060708));
    assert(decoded.opcode == UCN_I_OPCODE_BOOTSTRAP_HELLO);
    assert(decoded.payload_bytes == 2U);
    assert(decoded.payload[0] == 0xAAU && decoded.payload[1] == 0x55U);
}

static void test_negative_is_zero_write(void)
{
    ucn_i_c0_frame_t frame = bootstrap_frame();
    ucn_i_c0_frame_t decoded;
    ucn_i_c0_frame_t decoded_before;
    uint8_t raw[sizeof(c0_golden)];
    uint8_t output[sizeof(c0_golden)];
    uint8_t output_before[sizeof(output)];
    size_t output_bytes = 0xA5A5U;

    memcpy(raw, c0_golden, sizeof(raw));
    memset(&decoded, 0xA5, sizeof(decoded));
    decoded_before = decoded;
    raw[27] = 0U;
    raw[28] = 0U;
    assert(ucn_i_c0_decode(raw, sizeof(raw), 2U, UCN_I_CONTROL_H0,
                           &decoded) == UCN_ERR_MALFORMED);
    assert(memcmp(&decoded, &decoded_before, sizeof(decoded)) == 0);

    memcpy(raw, c0_golden, sizeof(raw));
    raw[27] = 0x0AU;
    raw[28] = 0xA0U;
    assert(ucn_i_c0_decode(raw, sizeof(raw), 2U, UCN_I_CONTROL_H0,
                           &decoded) == UCN_ERR_MALFORMED);
    assert(memcmp(&decoded, &decoded_before, sizeof(decoded)) == 0);

    memcpy(raw, c0_golden, sizeof(raw));
    raw[1] = (uint8_t)((raw[1] & 0xFCU) | UCN_I_PAYLOAD_DIAGNOSTIC);
    assert(ucn_i_c0_decode(raw, sizeof(raw), 2U, UCN_I_CONTROL_H0,
                           &decoded) == UCN_ERR_MALFORMED);
    assert(memcmp(&decoded, &decoded_before, sizeof(decoded)) == 0);

    memset(output, 0x5A, sizeof(output));
    memcpy(output_before, output, sizeof(output));
    frame.destination_binding_generation = 1U;
    assert(ucn_i_c0_encode(&frame, 2U, UCN_I_CONTROL_H0, output,
                           sizeof(output), &output_bytes) ==
           UCN_ERR_ARGUMENT);
    assert(output_bytes == 0xA5A5U);
    assert(memcmp(output, output_before, sizeof(output)) == 0);
    {
        static const uint16_t invalid[] = {0U,0x000BU,0x00FFU,0x01FFU,0x09FFU,0x0AA0U,UINT16_MAX};
        size_t i;
        for(i=0U;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
            frame=bootstrap_frame(); frame.source_address=1U;frame.destination_address=2U;
            frame.source_binding_generation=10U;frame.destination_binding_generation=20U;
            frame.opcode=invalid[i];
            frame.payload_kind=(invalid[i]>>8U)==UCN_I_OPCODE_DIAGNOSTICS ?
                UCN_I_PAYLOAD_DIAGNOSTIC : UCN_I_PAYLOAD_CONTROL;
            memset(output,0x5A,sizeof(output));output_bytes=0xA5A5U;
            assert(ucn_i_c0_encode(&frame,1U,UCN_I_CONTROL_H0,output,
                sizeof(output),&output_bytes)==UCN_ERR_ARGUMENT);
            assert(output_bytes==0xA5A5U);
            assert(memcmp(output,output_before,sizeof(output))==0);
        }
    }
}

static void test_registry_and_aad(void)
{
    static const uint8_t body[] = {0xCAU, 0xFEU};
    static const uint8_t tag[16] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
        8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U
    };
    ucn_i_c0_frame_t frame;
    uint8_t aad[UCN_I_C0_ORIGIN_AAD_MAX_BYTES];
    uint8_t domain = 0xA5U;
    size_t aad_bytes = 0U;

    assert(ucn_i_protocol_opcode_valid(UCN_I_OPCODE_CLUSTER_TUNNEL));
    assert(!ucn_i_protocol_opcode_valid(0U));
    assert(!ucn_i_protocol_opcode_valid(0x0A01U));
    assert(ucn_i_protocol_opcode_domain(UCN_I_OPCODE_TIME_SYNC,
                                        &domain) == UCN_OK);
    assert(domain == UCN_I_OPCODE_TIME);

    memset(&frame, 0, sizeof(frame));
    frame.payload = body;
    frame.payload_bytes = sizeof(body);
    frame.origin_tag = tag;
    frame.transaction_id = UINT64_C(0x0102030405060708);
    frame.realm_id = UINT32_C(0x11223344);
    frame.source_address = 0x0102U;
    frame.destination_address = 0x0304U;
    frame.source_binding_generation = 5U;
    frame.destination_binding_generation = 6U;
    frame.opcode = UCN_I_OPCODE_TIME_SYNC;
    frame.traffic_class = UCN_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_DELIVERY_RELIABLE;
    frame.interaction_role = UCN_INTERACTION_REQUEST;
    frame.payload_kind = UCN_I_PAYLOAD_CONTROL;
    frame.origin_security = UCN_I_ORIGIN_O1;
    frame.hop_limit = 9U;
    memset(aad, 0xA5, sizeof(aad));
    assert(ucn_i_c0_origin_aad_build(&frame, 2U, aad,
                                     &aad_bytes) == UCN_OK);
    assert(aad_bytes == 52U);
    assert(memcmp(aad, "UCN6-ORIGIN-V1", 14U) == 0);
    assert(aad[14] == 0U && aad[15] == 0U && aad[16] == 0U &&
           aad[17] == 34U);
    assert(aad[18] == 0x60U);
    assert(aad[19] == 0x25U);
    assert(aad[20] == 0x40U);
    assert(aad[21] == 0U);
    assert(aad[22] == 0x11U && aad[25] == 0x44U);
    assert(aad[46] == 0x06U && aad[47] == 0x01U);
    assert(aad[48] == 0U && aad[49] == 0U && aad[50] == 0U &&
           aad[51] == 2U);
}

static void test_tag_aliases_are_zero_write(void)
{
    static const uint8_t body[] = {0x11U};
    ucn_i_c0_frame_t frame;
    uint8_t output[96];
    uint8_t before[sizeof(output)];
    size_t output_bytes = SIZE_MAX;

    memset(&frame, 0, sizeof(frame));
    memset(output, 0xA5, sizeof(output));
    memcpy(before, output, sizeof(output));
    frame.payload = body;
    frame.payload_bytes = sizeof(body);
    frame.origin_tag = output + 2U;
    frame.transaction_id = 1U;
    frame.realm_id = 1U;
    frame.source_address = 1U;
    frame.destination_address = 2U;
    frame.source_binding_generation = 1U;
    frame.destination_binding_generation = 1U;
    frame.opcode = UCN_I_OPCODE_TIME_SYNC;
    frame.traffic_class = UCN_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    frame.interaction_role = UCN_INTERACTION_REQUEST;
    frame.payload_kind = UCN_I_PAYLOAD_CONTROL;
    frame.origin_security = UCN_I_ORIGIN_O1;
    frame.hop_limit = 1U;
    assert(ucn_i_c0_encode(&frame, 1U, UCN_I_CONTROL_H0,
                           output, sizeof(output), &output_bytes) ==
           UCN_ERR_ARGUMENT);
    assert(output_bytes == SIZE_MAX);
    assert(memcmp(output, before, sizeof(output)) == 0);

    frame.origin_tag = before;
    frame.hop_sequence = 1U;
    frame.hop_tag = output + 3U;
    assert(ucn_i_c0_encode(&frame, 1U, UCN_I_CONTROL_H1,
                           output, sizeof(output), &output_bytes) ==
           UCN_ERR_ARGUMENT);
    assert(output_bytes == SIZE_MAX);
    assert(memcmp(output, before, sizeof(output)) == 0);
}

static void test_opcode_support_and_diagnostic_echo(void)
{
    static const uint8_t echo_golden[UCN_I_DIAGNOSTIC_ECHO_BYTES] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U
    };
    ucn_i_protocol_opcode_support_t support = UINT8_C(0xA5);
    ucn_i_diagnostic_echo_t echo;
    ucn_i_diagnostic_echo_t decoded;
    ucn_i_diagnostic_echo_t before;
    uint8_t output[UCN_I_DIAGNOSTIC_ECHO_BYTES];
    uint8_t output_before[UCN_I_DIAGNOSTIC_ECHO_BYTES];

    assert(ucn_i_protocol_opcode_support(
               UCN_I_OPCODE_BOOTSTRAP_HELLO, &support) == UCN_OK);
    assert((support & UCN_I_OPCODE_SUPPORT_BOOTSTRAP_TERMINAL) != 0U);
    assert((support & UCN_I_OPCODE_SUPPORT_RELAY) == 0U);
    assert(ucn_i_protocol_opcode_support(
               UCN_I_OPCODE_CAPABILITY_ADVERTISE, &support) == UCN_OK);
    assert((support & UCN_I_OPCODE_SUPPORT_RELAY) != 0U);
    assert((support & UCN_I_OPCODE_SUPPORT_AUTH_TERMINAL) != 0U);
    assert(ucn_i_protocol_opcode_support(
               UCN_I_OPCODE_TIME_SYNC, &support) == UCN_OK);
    assert((support & UCN_I_OPCODE_SUPPORT_RELAY) != 0U);
    assert((support & UCN_I_OPCODE_SUPPORT_AUTH_TERMINAL) == 0U);
    assert(ucn_i_protocol_opcode_support(
               UCN_I_OPCODE_ROUTE_FRAGMENT, &support) == UCN_OK);
    assert(support == UCN_I_OPCODE_SUPPORT_REGISTERED);
    support = UINT8_C(0xA5);
    assert(ucn_i_protocol_opcode_support(UINT16_C(0x0A01), &support) ==
           UCN_ERR_MALFORMED);
    assert(support == UINT8_C(0xA5));

    echo.nonce = UINT64_C(0x0102030405060708);
    memset(output, 0xA5, sizeof(output));
    assert(ucn_i_diagnostic_echo_encode(&echo, output) == UCN_OK);
    assert(memcmp(output, echo_golden, sizeof(output)) == 0);
    memset(&decoded, 0xA5, sizeof(decoded));
    assert(ucn_i_diagnostic_echo_decode(echo_golden, &decoded) == UCN_OK);
    assert(decoded.nonce == echo.nonce);

    echo.nonce = 0U;
    memset(output, 0x5A, sizeof(output));
    memcpy(output_before, output, sizeof(output));
    assert(ucn_i_diagnostic_echo_encode(&echo, output) ==
           UCN_ERR_ARGUMENT);
    assert(memcmp(output, output_before, sizeof(output)) == 0);
    memset(&decoded, 0x3C, sizeof(decoded));
    before = decoded;
    memset(output, 0, sizeof(output));
    assert(ucn_i_diagnostic_echo_decode(output, &decoded) ==
           UCN_ERR_MALFORMED);
    assert(memcmp(&decoded, &before, sizeof(decoded)) == 0);
}

static void test_parent_publication_exact_body(void)
{
    static const uint8_t golden[32] = {
        1U,2U,3U,4U,5U,6U,7U,8U,0x11U,0x22U,0x33U,0x44U,
        0U,1U,2U,3U,4U,5U,6U,7U,8U,9U,10U,11U,12U,13U,14U,15U,0U,0U,0U,0U
    };
    const uint16_t opcodes[5] = {0x0410U,0x0411U,0x0411U,0x0412U,0x0413U};
    const uint8_t dispositions[5] = {0U,1U,2U,0U,7U};
    uint8_t bytes[33], expected[32], before[33];
    ucn_i_parent_publication_t value, decoded, saved;
    size_t index, offset;
    memset(&value, 0, sizeof(value));
    value.proposal_transaction = UINT64_C(0x0102030405060708);
    value.parent_generation = UINT32_C(0x11223344);
    for (index=0U;index<16U;++index) { value.context_digest[index]=(uint8_t)index; }
    for (index=0U;index<5U;++index) {
        value.disposition=dispositions[index];
        memcpy(expected,golden,32U);expected[28]=value.disposition;
        memset(bytes,0xA5,sizeof(bytes));
        assert(ucn_i_parent_publication_encode(opcodes[index],&value,bytes)==UCN_OK);
        assert(memcmp(bytes,expected,32U)==0 && bytes[32]==0xA5U);
        assert(ucn_i_parent_publication_decode(opcodes[index],expected,32U,&decoded)==UCN_OK);
        assert(decoded.proposal_transaction==value.proposal_transaction);
        assert(decoded.parent_generation==value.parent_generation);
        assert(memcmp(decoded.context_digest,value.context_digest,16U)==0);
        memset(&decoded,0x5A,sizeof(decoded));saved=decoded;
        assert(ucn_i_parent_publication_decode(opcodes[index],bytes,31U,&decoded)==UCN_ERR_MALFORMED);
        assert(ucn_i_parent_publication_decode(opcodes[index],bytes,33U,&decoded)==UCN_ERR_MALFORMED);
        for(offset=29U;offset<32U;++offset) {
            memcpy(bytes,expected,32U);bytes[offset]=1U;
            assert(ucn_i_parent_publication_decode(opcodes[index],bytes,32U,&decoded)==UCN_ERR_MALFORMED);
            assert(memcmp(&decoded,&saved,sizeof(decoded))==0);
        }
        memcpy(before,bytes,sizeof(bytes));value.proposal_transaction=0U;
        assert(ucn_i_parent_publication_encode(opcodes[index],&value,bytes)==UCN_ERR_ARGUMENT);
        assert(memcmp(before,bytes,sizeof(bytes))==0);
        value.proposal_transaction=UINT64_C(0x0102030405060708);
        /* EN: Partial alias must not corrupt the input. 中文：部分别名失败不改输入。 */
        assert(ucn_i_parent_publication_decode(opcodes[index],bytes,32U,
            (ucn_i_parent_publication_t *)(void *)bytes)==UCN_ERR_ARGUMENT);
        assert(memcmp(before,bytes,sizeof(bytes))==0);
    }
}

int main(void)
{
    test_typed_to_raw_uses_shared_golden();
    test_raw_to_typed_uses_shared_golden();
    test_negative_is_zero_write();
    test_registry_and_aad();
    test_tag_aliases_are_zero_write();
    test_opcode_support_and_diagnostic_echo();
    test_parent_publication_exact_body();
    puts("ucn control wire tests passed");
    return failures == 0 ? 0 : 1;
}
