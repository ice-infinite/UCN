#![no_std]
#![no_main]
#![forbid(unsafe_code)]
#![deny(clippy::mem_forget)]

use core::mem::size_of;

use esp_backtrace as _;
use esp_hal::clock::CpuClock;
use esp_hal::main;
use esp_hal::time::{Duration, Instant};
use log::{error, info};
use ucn_adapter::AdapterEventIngress;
use ucn_core::NanoCoreNode;
use ucn_flow::{
    LABEL_SETUP_BYTES, LabelSetup, NanoFlowOwner, decode_label_setup, encode_label_setup,
};
use ucn_routing::{
    NanoRouteOwner, RREQ_PAYLOAD_BYTES, RreqPayload, decode_rreq_payload, encode_rreq_payload,
};
use ucn_types::{
    AddressWidth, CandidateId, DeliveryGuarantee, Error, ForwardingLabel, HeaderContract, HopLimit,
    HopProfile, InteractionRole, NodeAddress, OriginSecurity, OriginSequence, PayloadKind,
    RouteGeneration, ServiceId, TrafficClass,
};
use ucn_wire::{C1Frame, CommonHeader, decode_c1_o0_h0, encode_c1_o0_h0};

esp_bootloader_esp_idf::esp_app_desc!();

const C1_GOLDEN: [u8; 17] = [
    0x61, 0x40, 0x03, 0x12, 0x34, 0x56, 0x78, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04, 0xDE, 0xAD, 0xBE,
    0xEF,
];
const RREQ_GOLDEN: [u8; RREQ_PAYLOAD_BYTES] =
    [0x01, 0x02, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44, 0x03];
const FLOW_SETUP_GOLDEN: [u8; LABEL_SETUP_BYTES] = [
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x01,
];

fn wire_self_test() -> bool {
    let header = CommonHeader {
        contract: HeaderContract::C1,
        traffic_class: TrafficClass::Q1,
        delivery: DeliveryGuarantee::BestEffort,
        interaction: InteractionRole::OneWay,
        payload_kind: PayloadKind::Data,
        origin_security: OriginSecurity::O0,
        hop_limit: match HopLimit::new(3) {
            Ok(value) => value,
            Err(_) => return false,
        },
    };
    let source = match NodeAddress::new(0x1234, AddressWidth::A1) {
        Ok(value) => value,
        Err(_) => return false,
    };
    let destination = match NodeAddress::new(0x5678, AddressWidth::A1) {
        Ok(value) => value,
        Err(_) => return false,
    };
    let service_id = match ServiceId::new(0x0102) {
        Ok(value) => value,
        Err(_) => return false,
    };
    let origin_sequence = match OriginSequence::new(0x0102_0304) {
        Ok(value) => value,
        Err(_) => return false,
    };
    let frame = C1Frame {
        common: header,
        source,
        destination,
        service_id,
        origin_sequence,
        payload: &[0xDE, 0xAD, 0xBE, 0xEF],
    };
    let mut encoded = [0xA5; 32];
    let written = match encode_c1_o0_h0(&frame, AddressWidth::A1, HopProfile::H0, &mut encoded) {
        Ok(value) => value,
        Err(_) => return false,
    };
    if encoded[..written] != C1_GOLDEN {
        return false;
    }
    if decode_c1_o0_h0(&C1_GOLDEN, AddressWidth::A1, HopProfile::H0) != Ok(frame) {
        return false;
    }
    let mut malformed = C1_GOLDEN;
    malformed[0] = 0x51;
    decode_c1_o0_h0(&malformed, AddressWidth::A1, HopProfile::H0) == Err(Error::Malformed)
}

fn routing_self_test() -> bool {
    let value = RreqPayload {
        accumulated_cost: 0x0102_0304,
        minimum_payload_budget: 0x1122,
        required_capability_bits: 0x3344,
        flags: 0x03,
    };
    let mut encoded = [0xA5; RREQ_PAYLOAD_BYTES];
    if encode_rreq_payload(value, &mut encoded).is_err()
        || encoded != RREQ_GOLDEN
        || decode_rreq_payload(&encoded) != Ok(value)
    {
        return false;
    }
    encoded[8] = 0x80;
    decode_rreq_payload(&encoded) == Err(Error::Malformed)
}

fn flow_self_test() -> bool {
    let value = LabelSetup {
        candidate_id: match CandidateId::new(0x1122) {
            Ok(value) => value,
            Err(_) => return false,
        },
        route_generation: match RouteGeneration::new(0x3344_5566) {
            Ok(value) => value,
            Err(_) => return false,
        },
        reverse_label: match ForwardingLabel::new(0x7788) {
            Ok(value) => value,
            Err(_) => return false,
        },
        forward_label: match ForwardingLabel::new(0x99AA) {
            Ok(value) => value,
            Err(_) => return false,
        },
        path_profile_id: 0xBBCC,
        context_digest: 0xDDEE_F001,
    };
    let mut encoded = [0xA5; LABEL_SETUP_BYTES];
    if encode_label_setup(value, &mut encoded).is_err()
        || encoded != FLOW_SETUP_GOLDEN
        || decode_label_setup(&encoded) != Ok(value)
    {
        return false;
    }
    encoded[0..2].fill(0);
    decode_label_setup(&encoded) == Err(Error::Malformed)
}

#[main]
fn main() -> ! {
    esp_println::logger::init_logger_from_env();
    let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
    let _peripherals = esp_hal::init(config);

    info!(
        "UCN_RUST_HW BOOT protocol={} target=ESP32-S3",
        ucn_wire::protocol_major()
    );
    info!(
        "UCN_RUST_HW SIZE core_nano={} route_nano={} flow_nano={} adapter_1_2_2_64={}",
        size_of::<NanoCoreNode<'static>>(),
        size_of::<NanoRouteOwner>(),
        size_of::<NanoFlowOwner>(),
        size_of::<AdapterEventIngress<1, 2, 2, 64>>()
    );
    info!(
        "UCN_RUST_HW LINKED admission={} capability={} identity={} owner={} persistence={} security={}",
        size_of::<ucn_admission::NanoAdmissionOwner<'static>>(),
        size_of::<ucn_capability::NanoCapabilityOwner>(),
        size_of::<ucn_identity::NanoIdentityOwner<'static>>(),
        size_of::<ucn_owner::TypedCoordinator<u8, u8, 4>>(),
        size_of::<ucn_persistence::PersistenceOwner<'static, 1, 64, 256>>(),
        size_of::<ucn_security::NanoSecurityOwner<'static>>()
    );

    let wire = wire_self_test();
    let routing = routing_self_test();
    let flow = flow_self_test();
    if wire && routing && flow {
        info!("UCN_RUST_HW SELFTEST wire=PASS routing=PASS flow=PASS overall=PASS");
    } else {
        error!(
            "UCN_RUST_HW SELFTEST wire={} routing={} flow={} overall=FAIL",
            wire, routing, flow
        );
    }

    let mut heartbeat = 0_u32;
    loop {
        heartbeat = heartbeat.wrapping_add(1);
        info!(
            "UCN_RUST_HW HEARTBEAT count={} overall={}",
            heartbeat,
            if wire && routing && flow {
                "PASS"
            } else {
                "FAIL"
            }
        );
        let start = Instant::now();
        while start.elapsed() < Duration::from_secs(1) {}
    }
}
