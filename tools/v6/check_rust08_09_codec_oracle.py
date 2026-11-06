#!/usr/bin/env python3
"""Three independent producers: native C, Rust, Python; no C FFI in Rust."""
from __future__ import annotations
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
def run(command:list[str],cwd:Path)->str:
    result=subprocess.run(command,cwd=cwd,text=True,capture_output=True,encoding="utf-8",errors="replace")
    if result.returncode: raise RuntimeError(f"exit={result.returncode} {command}\n{result.stdout}\n{result.stderr}")
    return result.stdout
def records(text:str)->dict[str,bytes]:
    return {name:bytes.fromhex(value) for line in text.splitlines() if "=" in line for name,value in [line.split("=",1)]}
def main()->None:
    parser=argparse.ArgumentParser();parser.add_argument("--cargo",default=shutil.which("cargo") or str(Path.home()/".cargo/bin/cargo.exe"));parser.add_argument("--cc",default=shutil.which("gcc") or "gcc");args=parser.parse_args()
    contract=json.loads((ROOT/"protocol/contracts/v6-shared-v1.json").read_text(encoding="utf-8"))
    assert contract["byte_order"]=="big-endian" and contract["wire_fragment_max"]==32767 and contract["duration_ms_max"]==2147483647
    assert contract["operation_flags"]=={"mode_mask":3,"repeatable":0,"volatile_dedup":1,"durable_at_most_once":2,"reserved_mode":3,"final":4,"reserved_mask":248,"request_final":False,"result_error_final":True}
    assert contract["durable_data_rpc"]["request_flags"]==2 and contract["durable_data_rpc"]["result_error_flags"]==6
    assert contract["durable_data_rpc"]["error_no_effect"]==65533 and contract["durable_data_rpc"]["error_in_doubt"]==65534
    assert contract["group_context"]=={"schema":0x0802,"version":2,"header_bytes":64,"member_bytes":28,"runtime_handles":False,"restore_phase":"FENCED"}
    rust=records(run([args.cargo,"run","--quiet","-p","ucn-runtime","--example","wire_vectors"],ROOT/"rust"))
    route=contract["route_control_runtime"]
    assert route["opcodes"]==[513,514,515,518] and route["roles"]==["Request","Result","Error","OneWay"]
    assert (route["discovery_key_bytes"],route["request_bytes"],route["reply_bytes"],route["error_bytes"],route["fragment_prefix_bytes"],route["ordinary_data_header_added_bytes"])==(44,54,77,99,14,0)
    key=struct.pack(">HHII",1,0,2,3)
    setup=struct.pack(">BHIIHI",1,0,2,3,0x1234,5)+bytes([0xa5])*16+struct.pack(">BBHHI",2,0,2,3,1000)
    expected={"ack":struct.pack(">BHI BB",1,0x1234,0x01020304,0,7),"setup":setup,"request_setup":setup[:34]+bytes([1])+setup[35:]+struct.pack(">QB",0x0102030405060708,1),"c1_fragment":struct.pack(">IIHH",3,2,1,2),"flow_fragment":struct.pack(">IHH",3,1,2),"c1_sack":struct.pack(">IIHHHIH",3,2,0x8000,0x1234,0,3,4),"flow_sack":struct.pack(">IHHIH",3,0x8000,0,3,4),"time_key":key,"time_stamp":key+struct.pack(">QI",0x0102030405060708,7),"envelope":bytes([0x13,0x5e])+struct.pack(">HIQ",1,2,0x0102030405060708)}
    route_key=struct.pack(">III",7,1,2)+bytes([0xa1])*16+struct.pack(">IIQ",3,9,0x0102030405060708)
    expected["route_request_full"]=route_key+struct.pack(">BIHHB",8,0x01020304,100,3,0)
    expected["route_reply_full"]=route_key+bytes([0xc3])*16+struct.pack(">IBIHHI",4,1,12,127,3,0x11223344)
    for total,count in ((77,2),(99,2),(99,3)):
        chunk=(total+count-1)//count;op=0x0202 if total==77 else 0x0203
        for index in range(count):expected[f"route_fragment_{total}_{count}_{index}"]=struct.pack(">HHBBQ",op,total,index,count,0x0102030405060708)+bytes(range(index*chunk,min((index+1)*chunk,total)))
    for stage in range(4):
        raw=(bytes([2,stage,2,0])+struct.pack(">I",1)+struct.pack(">II",1,1)+bytes([0x11])*16+
            struct.pack(">II",2,1)+bytes([0x22])*16+struct.pack(">IIBBHHHIIHHIIQQQQ",1,2,1,16,1,3,0,2,2,7,0 if stage==0 else 8,9,0 if stage==0 else 10,3,4,0 if stage==0 else 5,1000)+bytes([0x77])*32+
            struct.pack(">II",1,1)+(bytes(44) if stage==0 else struct.pack(">III",1,1,1)+bytes([0x77])*32))
        assert len(raw)==208
        expected[f"peer_reauth_{stage}"]=raw
    reauth=contract["peer_reauth"]
    asym=bytearray(expected["peer_reauth_2"])
    for offset in (60,72,76):asym[offset:offset+4]=struct.pack(">I",3)
    asym[164:176]=struct.pack(">III",2,2,2);asym[176:208]=bytes([0x88])*32
    expected["peer_reauth_asymmetric"]=bytes(asym)
    assert reauth["opcodes"]==[16,17,18,20] and reauth["roles"]==["Request","Result","Request","Result"]
    assert reauth["record_bytes"]==208 and reauth["version"]==2 and reauth["fragment_prefix_bytes"]==18 and reauth["signature_bytes_min_max"]==[16,64]
    assert reauth["signature_domain"]=="UCN6-PEER-REAUTH-V2" and reauth["ordinary_data_header_added_bytes"]==0
    logical=expected["peer_reauth_2"]+bytes([32])+bytes([0x44])*32
    for index in range(4):
        expected[f"peer_reauth_fragment_{index}"]=struct.pack(">BBBBHHBBQ",1,2,index,4,241,index*80,80,0,3)+logical[index*80:min((index+1)*80,241)]
    transfer_key=struct.pack(">BHIIH",1,0,0x01020304,0x05060708,0x1234)+bytes(range(16))
    operation_key=struct.pack(">QHH",0x0102030405060708,0x1234,0x2345)+bytes(range(16))
    expected["operation_data_key"]=struct.pack(">QHH",0x0102030405060708,0x1234,0)+bytes(range(16))
    assert contract["parent_publication"]["body_bytes"]==32 and contract["parent_publication"]["opcodes"]==[1040,1041,1042,1043]
    assert contract["time_sync_runtime"]["opcodes"]==[1537,1538,1539,1540]
    assert contract["time_sync_runtime"]["roles"]==["OneWay","OneWay","Request","Result"] and contract["time_sync_runtime"]["body_bytes"]==[12,24,12,24]
    assert contract["time_sync_runtime"]["event_keys_on_wire"] is False and contract["time_sync_runtime"]["ordinary_data_header_added_bytes"]==0
    for name,disposition in (("parent_prepare",0),("parent_prepared",1),("parent_committed",2),("parent_commit",0),("parent_abort",7)):
        expected[name]=struct.pack(">QI",0x0102030405060708,0x11223344)+bytes(range(16))+bytes([disposition,0,0,0])
    phases=bytearray()
    for source in (False,True):
        for phase in range(6):
            for op in (0x0410,0x0411,0x0412,0x0413,0x0901):
                for disposition in range(4):
                    for sent in range(4):
                        next_phase=reply=255
                        valid=phase<5 and sent<3 and phase not in ((0,2) if source else (1,3))
                        if valid:
                            if op==0x0413 and disposition>0:next_phase,reply=5,0
                            elif not source and op==0x0410 and disposition==0:
                                next_phase,reply=(4,2) if phase==4 else (2,1)
                            elif not source and phase in (2,4) and op==0x0412 and disposition==0:next_phase,reply=4,2
                            elif source and phase==1 and op==0x0411 and disposition==1 and sent==1:next_phase,reply=3,0
                            elif source and phase==3 and op==0x0411 and disposition==2 and sent==2:next_phase,reply=4,0
                        phases.extend((next_phase,reply))
    expected["parent_phase_matrix"]=bytes(phases)
    expected["durable_result_codes"]=struct.pack(">HH",65533,65534)
    rpc=contract["data_rpc"]
    assert rpc["opcode"]==0 and rpc["oneway_prefix_bytes"]==0 and rpc["request_prefix_bytes"]==9 and rpc["result_error_prefix_bytes"]==11
    expected.update({"rpc_request":struct.pack(">QB",0x0102030405060708,1)+b"abc", "rpc_result":struct.pack(">QBH",0x0102030405060708,5,7)+b"abc", "rpc_error":struct.pack(">QBH",0x0102030405060708,5,65535)+b"abc"})
    expected.update({"setup_ack":transfer_key+struct.pack(">HH",0x0102,0x0304),"transfer_abort":transfer_key+b"\x07\x00\x00\x00","transfer_terminal":transfer_key+struct.pack(">BH",2,0x5678),"operation_key":operation_key,"operation_receipt":operation_key+bytes([0xa5])*16+b"\x03\x00\x00\x00","operation_cancel":operation_key+b"\x01\x00\x00\x00"})
    recovery=contract["operation_recovery"]
    assert (recovery["resume_opcode"],recovery["result_opcode"],recovery["ack_opcode"])==(1285,1286,1284)
    assert (recovery["resume_bytes"],recovery["result_prefix_bytes"],recovery["ack_bytes"],recovery["result_max_bytes"])==(28,54,48,146)
    assert recovery["ack_phases"]==[3,4] and recovery["result_phases"]==[3,4,5]
    expected.update({"operation_result_ack":expected["operation_receipt"],"operation_resume":operation_key,"operation_recovered_result":expected["operation_receipt"]+struct.pack(">iH",-12,2)+b"ok"})
    def digest(domain:bytes,payload:bytes)->bytes:
        return hashlib.sha256(domain+struct.pack(">I",len(payload))+payload).digest()[:16]
    expected["setup_digest"]=digest(b"UCN6-TRANSFER-SETUP-V1",expected["request_setup"])
    expected["parent_record"]=(b"UCPT"+struct.pack(">HBBIII",1,1,0,1,1,1)+bytes([1])*16+
        struct.pack(">II",2,1)+bytes([2])*16+struct.pack(">IIIIIQ",1,1,1,7,0x05060708,0x0102030405060708))
    expected["parent_context_digest"]=digest(b"UCN6-C1-PARENT-V1",expected["parent_record"][:76])
    capability=contract["capability_negotiation"]
    fragments=contract["capability_fragmentation"]
    assert fragments["opcode"]==264 and fragments["metadata_bytes"]==6 and fragments["count"]==2
    for total,op in ((68,0x0103),(75,0x0105)):
        half=(total+1)//2
        for index in range(2):
            expected[f"cap_fragment_{total}_{index}"]=struct.pack(">HHBB",op,total,index,2)+bytes(range(index*half,min((index+1)*half,total)))
    assert capability["opcodes"]==list(range(257,264)) and capability["body_bytes"]==[24,24,68,59,75,20,20]
    assert capability["transcript_bytes"]==152 and capability["ordinary_data_header_added_bytes"]==0
    record=struct.pack(">IIIIIHHHHHHIIIIIHHBBHHHI",1,1,140,128,128,4,0,4,0,1,8,1000000,0,0x010b,2,14,4,2,2,1,0,0,0,0)
    assert len(record)==68
    peer_record=bytearray(record);struct.pack_into(">I",peer_record,0,7);struct.pack_into(">H",peer_record,52,2)
    ld=hashlib.sha256(record).digest()[:16];pd=hashlib.sha256(peer_record).digest()[:16]
    selected=struct.pack(">IIIIIHHHB",1,7,0x010b,2,14,2,2,0,2)+ld+pd
    context=struct.pack(">III",1,1,1)+bytes([1])*16+struct.pack(">II",2,1)+bytes([2])*16+struct.pack(">IIIB",1,1,1,1)+bytes([5])*16+struct.pack(">QHH",0x0102030405060708,0x40,0x0104)+selected
    assert len(context)==152
    transcript=digest(b"UCN6-CAPABILITY-SELECT-V1",context)
    expected.update({"capability_record":record,"capability_digest":ld,"capability_summary":struct.pack(">II",1,1)+ld,
        "capability_query":struct.pack(">I",1)+ld,"capability_select":selected,"capability_select_transcript":transcript,"capability_ack":selected+transcript})
    bootstrap=contract["bootstrap_pending"]
    assert bootstrap["body_bytes"]==[40,40,98] and bootstrap["challenge_issuer_link"]=={"id_offset":32,"generation_offset":34,"reserved_zero_offset":38}
    assert bootstrap["ordinary_data_header_added_bytes"]==0
    tx=0x1112131415161718;nonce=0x0102030405060708
    def boot_header(op:int,role:int)->bytes:
        return bytes([0x60,(role<<2)|1,1])+struct.pack(">IHHIIQH",1,0,65535,0,0,tx,op)
    hello=bytes([1,1,0,0])+bytes([0x11])*16+struct.pack(">QQI",nonce,tx,0)
    challenge=bytes([1,1,4,0])+struct.pack(">QI",tx,0x01020304)+bytes([1,2,3,4])+bytes(12)+struct.pack(">HIH",7,9,0)
    expected["bootstrap_hello"]=boot_header(1,1)+hello
    expected["bootstrap_challenge"]=boot_header(2,2)+challenge
    previous=expected["bootstrap_hello"]+expected["bootstrap_challenge"]
    prior=hashlib.sha256(b"UCN6-BOOTSTRAP-COOKIE-V1"+struct.pack(">I",len(previous))+previous).digest()
    echo=bytes([1,1,4,0])+bytes([0x11])*16+struct.pack(">QQQHI",nonce,tx,0x2122232425262728,7,9)+prior+bytes([1,2,3,4])
    expected["bootstrap_echo"]=boot_header(3,1)+echo;expected["bootstrap_prior_hash"]=prior
    auth_contract=contract["bootstrap_auth_events"]
    assert auth_contract["opcodes"]==[4,5,6,7,8] and auth_contract["fragment_header_bytes"]==36
    assert auth_contract["chunk_min_max"]==[32,128] and auth_contract["max_fragments"]==16
    assert auth_contract["bootstrap_root_chunk_bytes"]==62
    for phase in range(2,7):
        device=phase>=3;address=phase>=4
        auth=(struct.pack(">BHBB",6,1,1,1)+bytes([0x44 if device else 0])*16+bytes([0x11])*16+bytes([0x22])*16
            +struct.pack(">IQQQQIIIIIH",3,nonce,0x3132333435363738,tx,0x2122232425262728,42,9 if address else 0,
                11 if address else 0,2,5,7)
            +bytes([0x55 if address else 0])*16+struct.pack(">QQQQ",2000 if address else 0,6,10000,9000)
            +bytes([1])*16+bytes([2])*16+bytes([3])*32+bytes([4])*32+bytes([5])*32+bytes([6])*32
            +struct.pack(">HHBBHIBBHIII",3,2,1 if address else 0,16 if device else 0,1 if device else 0,
                2 if device else 0,1 if device else 0,1 if device else 0,3 if device else 0,
                4 if device else 0,5 if device else 0,9)+bytes([0x31])*32)
        assert len(auth)==379
        logical=auth+bytes([128])+bytes([0x77])*128
        expected[f"bootstrap_event_{phase}"]=logical
        if phase==6:
            expected["bootstrap_session_seed"]=struct.pack(">QHHHH",2000,3,4,1,2)+hashlib.sha256(b"UCN6-JOIN-SESSION-V1"+struct.pack(">I",379)+auth).digest()
        if phase==2:
            for index in range(9):
                fragment=logical[index*63:(index+1)*63]
                raw=bytes([1,1,2,index,9,63])+struct.pack(">HHHQ",508,index*63,len(fragment),tx)+bytes([0x11])*16+fragment
                expected[f"bootstrap_auth_fragment_{index}"]=raw
                if index in (0,8): expected[f"bootstrap_auth_frame_{index}"]=bytes([0x60,5,1])+struct.pack(">IHHIIQH",42,0,65535,0,0,tx,4)+raw
    assert contract["operation_result_digest"]=={"domain":"UCN6-OPERATION-RESULT-V1","body":"OperationWireKey28 || phase:u8 || status:i32be || payload_bytes:u16be || exact_payload","terminal_phases":[3,4,5,6],"payload_only_phase":3,"record_digest":False}
    expected["operation_result_digest"]=digest(b"UCN6-OPERATION-RESULT-V1",operation_key+struct.pack(">BiH",3,-12,2)+b"ok")
    expected["operation_flags_matrix"]=bytes(int((flags==0) if role==0 else (flags&248==0 and flags&3!=3 and bool(flags&4)==(role!=1))) for role in range(4) for flags in range(256))
    group=bytearray(512);group[:4]=bytes([0,2,2,2])
    for offset,value in ((4,1),(8,1),(12,1),(16,1),(20,1),(36,1),(40,0),(44,1)):
        struct.pack_into(">I",group,offset,value)
    struct.pack_into(">HH",group,24,0x40,9);group[28:36]=bytes([1,1,1,31,1,0,1,0]);group[48:64]=bytes([5])*16
    group[64:92]=struct.pack(">II",2,1)+bytes([2])*16+struct.pack(">HBB",1,1,0)
    expected["group_context"]=bytes(group)
    expected["group_allocator"]=b"GID1"+struct.pack(">III",1,10,3)
    prefix=bytes.fromhex("61A04800010002004000000001")
    identity=bytes([1])*16+struct.pack(">I",1)+bytes([2])*16+struct.pack(">IHI",1,0x40,1)
    aad=b"UCN6-ORIGIN-V1"+struct.pack(">I",54)+prefix[:2]+b"\x40\x01"+identity+struct.pack(">I",3)
    expected["protected_c1"]=prefix+b"abc"+hmac.new(bytes([1])*64,aad+b"abc",hashlib.sha256).digest()[:16]
    for name,domain in (("aad_digest",b"UCN6-AAD-V1"),("payload_digest",b"UCN6-PAYLOAD-V1"),("message_digest",b"UCN6-TRANSFER-MESSAGE-V1")):
        expected[name]=digest(domain,b"abc")
    with tempfile.TemporaryDirectory(prefix="ucn-rust-oracle-") as directory:
        scratch=Path(directory);executable=scratch/("oracle.exe" if os.name=="nt" else "oracle")
        codec_sources=("rust/tests/conformance/rust08_09_oracle.c","src/routing/ucn_route.c","src/transport/ucn_transport_codec.c","src/transport/ucn_transport_parent.c","src/transport/ucn_transport.c","src/core/ucn_owner.c","src/wire/ucn_control_wire.c","src/service/ucn_service_control.c","src/group/ucn_group.c","src/realtime/ucn_realtime_codec.c","src/capability/ucn_capability.c","src/admission/ucn_admission.c","src/admission/ucn_admission_wire.c","src/wire/ucn_wire.c","src/core/ucn_checked.c","src/core/ucn_digest.c")
        run([args.cc,"-std=c99","-Wall","-Wextra","-Werror","-I",str(ROOT/"include"),"-I",str(ROOT/"src"),*[str(ROOT/s) for s in codec_sources],"-o",str(executable)],ROOT)
        # The capability codec is independently compiled, not a Rust FFI wrapper.
        native=records(run([str(executable)],ROOT))
        secure=scratch/("secure.exe" if os.name=="nt" else "secure")
        sources=("rust/tests/conformance/secure_c1_oracle.c","src/security/ucn_security_session.c","src/wire/ucn_control_wire.c","src/wire/ucn_wire.c","src/core/ucn_owner.c","src/core/ucn_checked.c","src/core/ucn_digest.c")
        run([args.cc,"-std=c99","-Wall","-Wextra","-Werror","-I",str(ROOT/"include"),"-I",str(ROOT/"src"),*[str(ROOT/s) for s in sources],"-o",str(secure)],ROOT)
        native.update(records(run([str(secure)],ROOT)))
    if set(rust)!=set(expected) or set(native)!=set(expected): raise AssertionError("fixture membership mismatch")
    for name,value in expected.items():
        if rust[name]!=value or native[name]!=value: raise AssertionError(f"{name}: python={value.hex()} rust={rust[name].hex()} c={native[name].hex()}")
    print(f"RUST08_09_THREE_WAY_CODEC_OK vectors={len(expected)}")
if __name__=="__main__": main()
