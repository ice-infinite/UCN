#![no_std]
#![no_main]
#![forbid(unsafe_code)]
#![deny(clippy::mem_forget)]

use esp_backtrace as _;
use esp_hal::Blocking;
use esp_hal::clock::CpuClock;
use esp_hal::efuse;
use esp_hal::gpio::interconnect::InputSignal;
use esp_hal::main;
use esp_hal::time::{Duration, Instant};
use esp_hal::uart::{Config as UartConfig, Uart};
use log::{error, info, warn};
use ucn_routing::{
    DiscoveryHandle, DiscoveryKey, LinkRef, NanoRouteOwner, ReplyAction, RequestAction, RerrReason,
    ResolvedRoute, RouteConfig, RouteDomain, RouteUseFacts, RrepMessage, RreqMessage,
    decode_rerr_payload, decode_rrep_payload, decode_rreq_payload, encode_rerr_payload,
    encode_rrep_payload, encode_rreq_payload,
};
use ucn_security::Binding;
use ucn_types::{
    AddressWidth, BindingGeneration, C0TransactionId, DeliveryGuarantee, Error, HeaderContract,
    HopLimit, HopProfile, InteractionRole, LinkInstanceGeneration, NodeAddress, OriginSecurity,
    OriginSequence, PayloadKind, PeerSessionGeneration, RealmId, Result, ServiceId, TrafficClass,
};
use ucn_wire::{C1Frame, CommonHeader, decode_c1_o0_h0, encode_c1_o0_h0};

esp_bootloader_esp_idf::esp_app_desc!();

const MAC_B: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x2A, 0x92, 0x44];
const MAC_C: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x29, 0x9C, 0xD8];
const MAC_D: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x2A, 0x8F, 0x98];

const ADDRESS_B: u32 = 2;
const ADDRESS_C: u32 = 3;
const ADDRESS_D: u32 = 4;
const LINK_BC: u16 = 1;
const LINK_CD: u16 = 2;
const LINK_GENERATION: u32 = 1;
const REALM: u32 = 7;
const SESSION_GENERATION: u32 = 1;
const BINDING_GENERATION: u32 = 1;
const LINK_BAUD: u32 = 691_200;
const LINK_FRAME_MTU: u16 = 128;
const SERVICE_ID: u16 = 0x7002;
const DISCOVERY_RETRY_US: u64 = 800_000;
const ROUTE_LIFETIME_US: u64 = 60_000_000;
const PING_INTERVAL_US: u64 = 1_000_000;
const PING_TIMEOUT_US: u64 = 800_000;
const SUMMARY_INTERVAL_US: u64 = 5_000_000;
const INVALIDATE_AFTER_PINGS: u32 = 8;

const PACKET_RREQ: u8 = 1;
const PACKET_RREP: u8 = 2;
const PACKET_DATA: u8 = 3;
const DATA_PING: u8 = 0x50;
const DATA_ACK: u8 = 0x41;
const RREQ_MESSAGE_BYTES: usize = 21;
const RREP_MESSAGE_BYTES: usize = 40;
const C1_MAX_BYTES: usize = 64;
const PACKET_MAX_BYTES: usize = 128;
const CARRIER_MAGIC_0: u8 = 0xD6;
const CARRIER_MAGIC_1: u8 = 0x53;
const CARRIER_OVERHEAD: usize = 5;
const CARRIER_MAX_BYTES: usize = PACKET_MAX_BYTES + CARRIER_OVERHEAD;

type Routes = NanoRouteOwner;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum BoardRole {
    B,
    C,
    D,
}

impl BoardRole {
    const fn label(self) -> &'static str {
        match self {
            Self::B => "B",
            Self::C => "C",
            Self::D => "D",
        }
    }

    const fn address(self) -> u32 {
        match self {
            Self::B => ADDRESS_B,
            Self::C => ADDRESS_C,
            Self::D => ADDRESS_D,
        }
    }

    const fn principal_byte(self) -> u8 {
        match self {
            Self::B => 0xB2,
            Self::C => 0xC3,
            Self::D => 0xD4,
        }
    }

    const fn peer_endpoint(self) -> Self {
        match self {
            Self::B => Self::D,
            Self::D => Self::B,
            Self::C => Self::C,
        }
    }

    const fn discovery_start_us(self) -> u64 {
        match self {
            Self::B => 2_000_000,
            Self::D => 3_000_000,
            Self::C => u64::MAX,
        }
    }

    const fn first_ping_delay_us(self) -> u64 {
        match self {
            // B usually finishes its own discovery first. Delay its first
            // business frame until D->B has also converged at relay C.
            Self::B => 3_500_000,
            Self::D => 2_000_000,
            Self::C => u64::MAX,
        }
    }
}

#[derive(Clone, Copy)]
enum CarrierEvent {
    None,
    Frame(usize),
    Fault,
}

struct CarrierParser {
    state: u8,
    frame_len: usize,
    frame_pos: usize,
    crc: u16,
    crc_high: u8,
    frame: [u8; PACKET_MAX_BYTES],
}

impl CarrierParser {
    const fn new() -> Self {
        Self {
            state: 0,
            frame_len: 0,
            frame_pos: 0,
            crc: 0xFFFF,
            crc_high: 0,
            frame: [0; PACKET_MAX_BYTES],
        }
    }

    fn reset(&mut self) {
        self.state = 0;
        self.frame_len = 0;
        self.frame_pos = 0;
        self.crc = 0xFFFF;
        self.crc_high = 0;
    }

    fn push(&mut self, byte: u8) -> CarrierEvent {
        match self.state {
            0 => {
                if byte == CARRIER_MAGIC_0 {
                    self.state = 1;
                }
            }
            1 => {
                if byte == CARRIER_MAGIC_1 {
                    self.state = 2;
                } else if byte != CARRIER_MAGIC_0 {
                    self.reset();
                }
            }
            2 => {
                let length = usize::from(byte);
                if length == 0 || length > PACKET_MAX_BYTES {
                    self.reset();
                    return CarrierEvent::Fault;
                }
                self.frame_len = length;
                self.frame_pos = 0;
                self.crc = crc16_update(0xFFFF, byte);
                self.state = 3;
            }
            3 => {
                self.frame[self.frame_pos] = byte;
                self.frame_pos += 1;
                self.crc = crc16_update(self.crc, byte);
                if self.frame_pos == self.frame_len {
                    self.state = 4;
                }
            }
            4 => {
                self.crc_high = byte;
                self.state = 5;
            }
            5 => {
                let received = u16::from_be_bytes([self.crc_high, byte]);
                let length = self.frame_len;
                let valid = received == self.crc;
                self.reset();
                return if valid {
                    CarrierEvent::Frame(length)
                } else {
                    CarrierEvent::Fault
                };
            }
            _ => {
                self.reset();
                return CarrierEvent::Fault;
            }
        }
        CarrierEvent::None
    }

    fn copy_frame(&self, length: usize, output: &mut [u8; PACKET_MAX_BYTES]) {
        output[..length].copy_from_slice(&self.frame[..length]);
    }
}

#[derive(Default)]
struct Stats {
    tx: u32,
    rx: u32,
    forwarded_rreq: u32,
    forwarded_rrep: u32,
    forwarded_data: u32,
    ping_ok: u32,
    timeout: u32,
    carrier_errors: u32,
    wire_errors: u32,
    route_errors: u32,
    tx_errors: u32,
    rx_errors: u32,
    rtt_sum_us: u64,
    rtt_min_us: u64,
    rtt_max_us: u64,
}

impl Stats {
    const fn errors_are_zero(&self) -> bool {
        self.timeout == 0
            && self.carrier_errors == 0
            && self.wire_errors == 0
            && self.route_errors == 0
            && self.tx_errors == 0
            && self.rx_errors == 0
    }

    fn record_rtt(&mut self, value: u64) {
        self.ping_ok = self.ping_ok.saturating_add(1);
        self.rtt_sum_us = self.rtt_sum_us.saturating_add(value);
        if self.rtt_min_us == 0 || value < self.rtt_min_us {
            self.rtt_min_us = value;
        }
        if value > self.rtt_max_us {
            self.rtt_max_us = value;
        }
    }

    fn rtt_average_us(&self) -> u64 {
        if self.ping_ok == 0 {
            0
        } else {
            self.rtt_sum_us / u64::from(self.ping_ok)
        }
    }
}

#[derive(Clone, Copy)]
struct PendingPing {
    active: bool,
    sequence: u32,
    sent_at_us: u64,
}

impl PendingPing {
    const EMPTY: Self = Self {
        active: false,
        sequence: 0,
        sent_at_us: 0,
    };
}

fn crc16_update(mut crc: u16, byte: u8) -> u16 {
    crc ^= u16::from(byte) << 8;
    for _ in 0..8 {
        crc = if crc & 0x8000 != 0 {
            (crc << 1) ^ 0x1021
        } else {
            crc << 1
        };
    }
    crc
}

fn now_us() -> u64 {
    Instant::now().duration_since_epoch().as_micros()
}

fn binding(role: BoardRole) -> Result<Binding> {
    Ok(Binding {
        address: NodeAddress::new(role.address(), AddressWidth::A0)?,
        generation: BindingGeneration::active(BINDING_GENERATION)?,
        principal: [role.principal_byte(); 16],
    })
}

fn role_for_address(address: u32) -> Result<BoardRole> {
    match address {
        ADDRESS_B => Ok(BoardRole::B),
        ADDRESS_C => Ok(BoardRole::C),
        ADDRESS_D => Ok(BoardRole::D),
        _ => Err(Error::Malformed),
    }
}

fn route_domain(origin: BoardRole, destination: BoardRole) -> Result<RouteDomain> {
    Ok(RouteDomain {
        realm: RealmId::new(REALM)?,
        origin: binding(origin)?,
        origin_session_generation: PeerSessionGeneration::new(SESSION_GENERATION)?,
        destination: binding(destination)?,
    })
}

fn new_routes(role: BoardRole) -> Result<Routes> {
    NanoRouteOwner::new(
        RouteConfig {
            owner_instance: 0x5254_0000 | role.address(),
            realm: RealmId::new(REALM)?,
            address_width: AddressWidth::A0,
            local: binding(role)?,
            local_session_generation: PeerSessionGeneration::new(SESSION_GENERATION)?,
            discovery_lifetime_us: 12_000_000,
            discovery_retry_us: DISCOVERY_RETRY_US,
            discovery_max_attempts: 10,
            reverse_lifetime_us: 10_000_000,
            route_lifetime_us: ROUTE_LIFETIME_US,
            maximum_hops: HopLimit::new(4)?,
        },
        match role {
            BoardRole::B => 100,
            BoardRole::D => 200,
            BoardRole::C => 300,
        },
    )
}

fn link_for(role: BoardRole, link_id: u16) -> Result<LinkRef> {
    let (peer, cost) = match (role, link_id) {
        (BoardRole::B, LINK_BC) | (BoardRole::C, LINK_BC) => (
            if role == BoardRole::B {
                BoardRole::C
            } else {
                BoardRole::B
            },
            10,
        ),
        (BoardRole::C, LINK_CD) | (BoardRole::D, LINK_CD) => (
            if role == BoardRole::C {
                BoardRole::D
            } else {
                BoardRole::C
            },
            20,
        ),
        _ => return Err(Error::Argument),
    };
    LinkRef::new(
        link_id,
        LinkInstanceGeneration::new(LINK_GENERATION)?,
        binding(peer)?,
        LINK_FRAME_MTU,
        cost,
        0,
    )
}

fn send_carrier(uart: &mut Uart<'_, Blocking>, packet: &[u8], stats: &mut Stats) -> bool {
    if packet.is_empty() || packet.len() > PACKET_MAX_BYTES {
        stats.tx_errors = stats.tx_errors.saturating_add(1);
        return false;
    }
    let mut carrier = [0_u8; CARRIER_MAX_BYTES];
    carrier[0] = CARRIER_MAGIC_0;
    carrier[1] = CARRIER_MAGIC_1;
    carrier[2] = packet.len() as u8;
    carrier[3..3 + packet.len()].copy_from_slice(packet);
    let mut crc = crc16_update(0xFFFF, carrier[2]);
    for byte in packet {
        crc = crc16_update(crc, *byte);
    }
    let crc_bytes = crc.to_be_bytes();
    carrier[3 + packet.len()] = crc_bytes[0];
    carrier[4 + packet.len()] = crc_bytes[1];
    let carrier_len = packet.len() + CARRIER_OVERHEAD;
    let mut written = 0;
    while written < carrier_len {
        match uart.write(&carrier[written..carrier_len]) {
            Ok(0) | Err(_) => {
                stats.tx_errors = stats.tx_errors.saturating_add(1);
                return false;
            }
            Ok(count) => written += count,
        }
    }
    if uart.flush().is_err() {
        stats.tx_errors = stats.tx_errors.saturating_add(1);
        return false;
    }
    stats.tx = stats.tx.saturating_add(1);
    true
}

fn encode_rreq_message(message: RreqMessage, output: &mut [u8; PACKET_MAX_BYTES]) -> Result<usize> {
    let mut encoded = [0_u8; RREQ_MESSAGE_BYTES];
    encoded[0] = PACKET_RREQ;
    encoded[1] = u8::try_from(message.key.origin.address.get()).map_err(|_| Error::Argument)?;
    encoded[2] = u8::try_from(message.key.target_address.get()).map_err(|_| Error::Argument)?;
    encoded[3..11].copy_from_slice(&message.key.transaction_id.get().to_be_bytes());
    encoded[11] = message.remaining_hops.get();
    encode_rreq_payload(message.payload, &mut encoded[12..21])?;
    output[..RREQ_MESSAGE_BYTES].copy_from_slice(&encoded);
    Ok(RREQ_MESSAGE_BYTES)
}

fn decode_rreq_message(input: &[u8]) -> Result<RreqMessage> {
    if input.len() != RREQ_MESSAGE_BYTES || input[0] != PACKET_RREQ {
        return Err(Error::Malformed);
    }
    let origin_role = role_for_address(u32::from(input[1]))?;
    let target = NodeAddress::new(u32::from(input[2]), AddressWidth::A0)?;
    let transaction_id = C0TransactionId::new(u64::from_be_bytes(
        input[3..11].try_into().map_err(|_| Error::Malformed)?,
    ))?;
    Ok(RreqMessage {
        key: DiscoveryKey {
            realm: RealmId::new(REALM)?,
            origin: binding(origin_role)?,
            origin_session_generation: PeerSessionGeneration::new(SESSION_GENERATION)?,
            target_address: target,
            transaction_id,
        },
        remaining_hops: HopLimit::new(input[11])?,
        payload: decode_rreq_payload(&input[12..21])?,
    })
}

fn encode_rrep_message(message: RrepMessage, output: &mut [u8; PACKET_MAX_BYTES]) -> Result<usize> {
    let mut encoded = [0_u8; RREP_MESSAGE_BYTES];
    encoded[0] = PACKET_RREP;
    encoded[1] = u8::try_from(message.key.origin.address.get()).map_err(|_| Error::Argument)?;
    encoded[2] = u8::try_from(message.key.target_address.get()).map_err(|_| Error::Argument)?;
    encoded[3..11].copy_from_slice(&message.key.transaction_id.get().to_be_bytes());
    encode_rrep_payload(message.payload, &mut encoded[11..40])?;
    output[..RREP_MESSAGE_BYTES].copy_from_slice(&encoded);
    Ok(RREP_MESSAGE_BYTES)
}

fn decode_rrep_message(input: &[u8]) -> Result<RrepMessage> {
    if input.len() != RREP_MESSAGE_BYTES || input[0] != PACKET_RREP {
        return Err(Error::Malformed);
    }
    let origin_role = role_for_address(u32::from(input[1]))?;
    let target = NodeAddress::new(u32::from(input[2]), AddressWidth::A0)?;
    Ok(RrepMessage {
        key: DiscoveryKey {
            realm: RealmId::new(REALM)?,
            origin: binding(origin_role)?,
            origin_session_generation: PeerSessionGeneration::new(SESSION_GENERATION)?,
            target_address: target,
            transaction_id: C0TransactionId::new(u64::from_be_bytes(
                input[3..11].try_into().map_err(|_| Error::Malformed)?,
            ))?,
        },
        payload: decode_rrep_payload(&input[11..40])?,
    })
}

fn encode_data_message(
    source: BoardRole,
    destination: BoardRole,
    hop_limit: u8,
    sequence: u32,
    kind: u8,
    echo: u32,
    output: &mut [u8; PACKET_MAX_BYTES],
) -> Result<usize> {
    let mut payload = [0_u8; 5];
    payload[0] = kind;
    payload[1..5].copy_from_slice(&echo.to_be_bytes());
    let frame = C1Frame {
        common: CommonHeader {
            contract: HeaderContract::C1,
            traffic_class: TrafficClass::Q1,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::OneWay,
            payload_kind: PayloadKind::Data,
            origin_security: OriginSecurity::O0,
            hop_limit: HopLimit::new(hop_limit)?,
        },
        source: NodeAddress::new(source.address(), AddressWidth::A0)?,
        destination: NodeAddress::new(destination.address(), AddressWidth::A0)?,
        service_id: ServiceId::new(SERVICE_ID)?,
        origin_sequence: OriginSequence::new(sequence)?,
        payload: &payload,
    };
    let mut wire = [0_u8; C1_MAX_BYTES];
    let length = encode_c1_o0_h0(&frame, AddressWidth::A0, HopProfile::H0, &mut wire)?;
    output[0] = PACKET_DATA;
    output[1..1 + length].copy_from_slice(&wire[..length]);
    Ok(length + 1)
}

fn forward_data_message(input: &[u8], output: &mut [u8; PACKET_MAX_BYTES]) -> Result<usize> {
    if input.first().copied() != Some(PACKET_DATA) {
        return Err(Error::Malformed);
    }
    let frame = decode_c1_o0_h0(&input[1..], AddressWidth::A0, HopProfile::H0)?;
    let current_hops = frame.common.hop_limit.get();
    if current_hops <= 1 {
        return Err(Error::Exhausted);
    }
    let forwarded = C1Frame {
        common: CommonHeader {
            contract: frame.common.contract,
            traffic_class: frame.common.traffic_class,
            delivery: frame.common.delivery,
            interaction: frame.common.interaction,
            payload_kind: frame.common.payload_kind,
            origin_security: frame.common.origin_security,
            hop_limit: HopLimit::new(current_hops - 1)?,
        },
        source: frame.source,
        destination: frame.destination,
        service_id: frame.service_id,
        origin_sequence: frame.origin_sequence,
        payload: frame.payload,
    };
    let mut wire = [0_u8; C1_MAX_BYTES];
    let length = encode_c1_o0_h0(&forwarded, AddressWidth::A0, HopProfile::H0, &mut wire)?;
    output[0] = PACKET_DATA;
    output[1..1 + length].copy_from_slice(&wire[..length]);
    Ok(length + 1)
}

fn next_sequence(value: &mut u32) -> Result<u32> {
    let current = *value;
    *value = value.checked_add(1).ok_or(Error::Exhausted)?;
    Ok(current)
}

struct EndpointState {
    role: BoardRole,
    link: LinkRef,
    routes: Routes,
    parser: CarrierParser,
    route: Option<ucn_routing::SoftRouteView>,
    discovery: Option<DiscoveryHandle>,
    next_discovery_at_us: u64,
    next_ping_at_us: u64,
    next_origin_sequence: u32,
    last_peer_sequence: u32,
    pending: PendingPing,
    invalidated: bool,
    recovered: bool,
    rerr_guard_done: bool,
    stats: Stats,
}

impl EndpointState {
    fn new(role: BoardRole) -> Result<Self> {
        let link_id = if role == BoardRole::B {
            LINK_BC
        } else {
            LINK_CD
        };
        Ok(Self {
            role,
            link: link_for(role, link_id)?,
            routes: new_routes(role)?,
            parser: CarrierParser::new(),
            route: None,
            discovery: None,
            next_discovery_at_us: role.discovery_start_us(),
            next_ping_at_us: u64::MAX,
            next_origin_sequence: if role == BoardRole::B { 1 } else { 10_001 },
            last_peer_sequence: 0,
            pending: PendingPing::EMPTY,
            invalidated: false,
            recovered: false,
            rerr_guard_done: false,
            stats: Stats::default(),
        })
    }

    fn send_rreq(&mut self, uart: &mut Uart<'_, Blocking>, request: RreqMessage) -> bool {
        let mut packet = [0_u8; PACKET_MAX_BYTES];
        match encode_rreq_message(request, &mut packet) {
            Ok(length) => send_carrier(uart, &packet[..length], &mut self.stats),
            Err(problem) => {
                self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                error!(
                    "UCN_RUST_ROUTE RREQ_ENCODE_FAIL role={} error={problem:?}",
                    self.role.label()
                );
                false
            }
        }
    }

    fn send_rrep(&mut self, uart: &mut Uart<'_, Blocking>, reply: RrepMessage) -> bool {
        let mut packet = [0_u8; PACKET_MAX_BYTES];
        match encode_rrep_message(reply, &mut packet) {
            Ok(length) => send_carrier(uart, &packet[..length], &mut self.stats),
            Err(problem) => {
                self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                error!(
                    "UCN_RUST_ROUTE RREP_ENCODE_FAIL role={} error={problem:?}",
                    self.role.label()
                );
                false
            }
        }
    }

    fn resolved_route(&self, current_us: u64) -> Result<ucn_routing::SoftRouteView> {
        let domain = route_domain(self.role, self.role.peer_endpoint())?;
        match self.routes.resolve(
            domain,
            RouteUseFacts {
                now_us: current_us,
                origin_session_generation: PeerSessionGeneration::new(SESSION_GENERATION)?,
                destination_binding_generation: BINDING_GENERATION,
                link_generation: LinkInstanceGeneration::new(LINK_GENERATION)?,
            },
        )? {
            ResolvedRoute::Dynamic(route) => Ok(route),
            ResolvedRoute::Static(_) => Err(Error::State),
        }
    }

    fn send_business(&mut self, uart: &mut Uart<'_, Blocking>, kind: u8, echo: u32) -> Option<u32> {
        if self.resolved_route(now_us()).is_err() {
            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
            return None;
        }
        let sequence = match next_sequence(&mut self.next_origin_sequence) {
            Ok(value) => value,
            Err(_) => {
                self.stats.tx_errors = self.stats.tx_errors.saturating_add(1);
                return None;
            }
        };
        let actual_echo = if kind == DATA_PING { sequence } else { echo };
        let mut packet = [0_u8; PACKET_MAX_BYTES];
        match encode_data_message(
            self.role,
            self.role.peer_endpoint(),
            2,
            sequence,
            kind,
            actual_echo,
            &mut packet,
        ) {
            Ok(length) if send_carrier(uart, &packet[..length], &mut self.stats) => Some(sequence),
            Ok(_) => None,
            Err(problem) => {
                self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                error!(
                    "UCN_RUST_ROUTE DATA_ENCODE_FAIL role={} error={problem:?}",
                    self.role.label()
                );
                None
            }
        }
    }

    fn process_frame(&mut self, uart: &mut Uart<'_, Blocking>, packet: &[u8]) {
        self.stats.rx = self.stats.rx.saturating_add(1);
        match packet.first().copied() {
            Some(PACKET_RREQ) => {
                let request = match decode_rreq_message(packet) {
                    Ok(value) => value,
                    Err(problem) => {
                        self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                        warn!(
                            "UCN_RUST_ROUTE RREQ_REJECT role={} error={problem:?}",
                            self.role.label()
                        );
                        return;
                    }
                };
                match self.routes.on_rreq(request, self.link, now_us()) {
                    Ok(RequestAction::LocalTarget) => {
                        match self.routes.make_rrep(request, 0, LINK_FRAME_MTU, now_us()) {
                            Ok(reply) => {
                                let _ = self.send_rrep(uart, reply);
                            }
                            Err(problem) => {
                                self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                                error!(
                                    "UCN_RUST_ROUTE MAKE_RREP_FAIL role={} error={problem:?}",
                                    self.role.label()
                                );
                            }
                        }
                    }
                    Ok(RequestAction::Duplicate) => {}
                    Ok(RequestAction::Forward(_)) | Err(_) => {
                        self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                    }
                }
            }
            Some(PACKET_RREP) => {
                let reply = match decode_rrep_message(packet) {
                    Ok(value) => value,
                    Err(problem) => {
                        self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                        warn!(
                            "UCN_RUST_ROUTE RREP_REJECT role={} error={problem:?}",
                            self.role.label()
                        );
                        return;
                    }
                };
                match self.routes.on_rrep(reply, self.link, now_us()) {
                    Ok(ReplyAction::ReachedOrigin(route)) => {
                        let Ok(expected_origin) = binding(self.role) else {
                            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                            return;
                        };
                        let Ok(expected_destination) = binding(self.role.peer_endpoint()) else {
                            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                            return;
                        };
                        if route.domain().origin != expected_origin
                            || route.domain().destination != expected_destination
                        {
                            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                            return;
                        }
                        self.route = Some(route);
                        self.discovery = None;
                        self.next_ping_at_us =
                            now_us().saturating_add(self.role.first_ping_delay_us());
                        let phase = if self.invalidated {
                            "RECOVERED"
                        } else {
                            "INITIAL"
                        };
                        if self.invalidated {
                            self.recovered = true;
                        }
                        info!(
                            "UCN_RUST_ROUTE ROUTE_ACTIVE role={} phase={} destination={} hops={} cost={} mtu={} generation={} causal={}",
                            self.role.label(),
                            phase,
                            route.domain().destination.address.get(),
                            route.hop_count(),
                            route.cost(),
                            route.path_frame_mtu(),
                            route.route_generation().get(),
                            route.route_causal_id()
                        );
                        info!("V6R ROUTE {} {phase}", self.role.label());
                    }
                    Ok(ReplyAction::Forward { .. }) | Err(_) => {
                        self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                    }
                }
            }
            Some(PACKET_DATA) => self.process_data(uart, packet),
            _ => self.stats.wire_errors = self.stats.wire_errors.saturating_add(1),
        }
    }

    fn process_data(&mut self, uart: &mut Uart<'_, Blocking>, packet: &[u8]) {
        let frame = match decode_c1_o0_h0(&packet[1..], AddressWidth::A0, HopProfile::H0) {
            Ok(value) => value,
            Err(problem) => {
                self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                warn!(
                    "UCN_RUST_ROUTE DATA_REJECT role={} error={problem:?}",
                    self.role.label()
                );
                return;
            }
        };
        if frame.destination.get() != self.role.address()
            || frame.source.get() != self.role.peer_endpoint().address()
            || frame.service_id.get() != SERVICE_ID
            || frame.payload.len() != 5
            || frame.common.hop_limit.get() != 1
        {
            self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
            return;
        }
        let sequence = frame.origin_sequence.get();
        if sequence <= self.last_peer_sequence {
            self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
            return;
        }
        self.last_peer_sequence = sequence;
        let echo = u32::from_be_bytes([
            frame.payload[1],
            frame.payload[2],
            frame.payload[3],
            frame.payload[4],
        ]);
        match frame.payload[0] {
            DATA_PING if echo == sequence => {
                let _ = self.send_business(uart, DATA_ACK, sequence);
            }
            DATA_ACK if self.pending.active && echo == self.pending.sequence => {
                let rtt = now_us().saturating_sub(self.pending.sent_at_us);
                self.pending.active = false;
                self.stats.record_rtt(rtt);
                info!(
                    "UCN_RUST_ROUTE PING_OK role={} sequence={} rtt_us={}",
                    self.role.label(),
                    echo,
                    rtt
                );
            }
            _ => self.stats.wire_errors = self.stats.wire_errors.saturating_add(1),
        }
    }

    fn run_discovery(&mut self, uart: &mut Uart<'_, Blocking>, current_us: u64) {
        if self.route.is_some() || current_us < self.next_discovery_at_us {
            return;
        }
        let request = if let Some(handle) = self.discovery {
            match self.routes.retry_discovery(handle, current_us) {
                Ok(value) => value,
                Err(Error::State) => return,
                Err(problem) => {
                    self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                    error!(
                        "UCN_RUST_ROUTE DISCOVERY_RETRY_FAIL role={} error={problem:?}",
                        self.role.label()
                    );
                    self.next_discovery_at_us = current_us.saturating_add(DISCOVERY_RETRY_US);
                    return;
                }
            }
        } else {
            let target = match binding(self.role.peer_endpoint()) {
                Ok(value) => value.address,
                Err(problem) => {
                    self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                    error!(
                        "UCN_RUST_ROUTE DISCOVERY_TARGET_FAIL role={} error={problem:?}",
                        self.role.label()
                    );
                    self.next_discovery_at_us = current_us.saturating_add(DISCOVERY_RETRY_US);
                    return;
                }
            };
            match self.routes.ensure_discovery(target, 0, 32, 0, current_us) {
                Ok(start) => {
                    self.discovery = Some(start.handle);
                    start.request
                }
                Err(problem) => {
                    self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                    error!(
                        "UCN_RUST_ROUTE DISCOVERY_START_FAIL role={} error={problem:?}",
                        self.role.label()
                    );
                    return;
                }
            }
        };
        let _ = self.send_rreq(uart, request);
        self.next_discovery_at_us = current_us.saturating_add(DISCOVERY_RETRY_US);
    }

    fn run_rerr_guard(&mut self) {
        if self.role != BoardRole::B || self.rerr_guard_done {
            return;
        }
        let Some(route) = self.route else {
            return;
        };
        let mut wrong = match self.routes.make_rerr(route, RerrReason::LinkInvalid) {
            Ok(value) => value,
            Err(_) => {
                self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                return;
            }
        };
        wrong.route_causal_id = wrong.route_causal_id.saturating_add(1);
        let mut encoded = [0_u8; ucn_routing::RERR_PAYLOAD_BYTES];
        let preserved = encode_rerr_payload(wrong, &mut encoded).is_ok()
            && decode_rerr_payload(&encoded).and_then(|decoded| self.routes.on_rerr(decoded))
                == Ok(false)
            && self.resolved_route(now_us()).is_ok();
        self.rerr_guard_done = preserved;
        if preserved {
            info!("UCN_RUST_ROUTE RERR_GUARD role=B preserved=1");
            info!("V6R RERR B");
        } else {
            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
        }
    }

    fn maybe_invalidate(&mut self, current_us: u64) {
        if self.role != BoardRole::B
            || self.invalidated
            || self.pending.active
            || self.stats.ping_ok < INVALIDATE_AFTER_PINGS
        {
            return;
        }
        let Some(route) = self.route else {
            return;
        };
        let removed = self.routes.invalidate_link(
            route.next_hop().link_id(),
            route.next_hop().link_generation(),
        );
        let rejected = self.resolved_route(current_us).is_err();
        if removed == 1 && rejected {
            self.route = None;
            self.discovery = None;
            self.invalidated = true;
            self.next_discovery_at_us = current_us.saturating_add(100_000);
            info!("UCN_RUST_ROUTE ROUTE_INVALIDATED role=B removed=1 resolve_rejected=1");
            info!("V6R INVALID B");
        } else {
            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
        }
    }

    fn maintenance(&mut self, uart: &mut Uart<'_, Blocking>, next_summary: &mut u64) {
        let current_us = now_us();
        if self.pending.active
            && current_us.saturating_sub(self.pending.sent_at_us) >= PING_TIMEOUT_US
        {
            self.pending.active = false;
            self.stats.timeout = self.stats.timeout.saturating_add(1);
            warn!("UCN_RUST_ROUTE PING_TIMEOUT role={}", self.role.label());
        }
        self.run_discovery(uart, current_us);
        self.run_rerr_guard();
        self.maybe_invalidate(current_us);
        if self.route.is_some()
            && !self.pending.active
            && current_us >= self.next_ping_at_us
            && let Some(sequence) = self.send_business(uart, DATA_PING, 0)
        {
            self.pending = PendingPing {
                active: true,
                sequence,
                sent_at_us: now_us(),
            };
            self.next_ping_at_us = current_us.saturating_add(PING_INTERVAL_US);
        }
        if current_us >= *next_summary {
            if self.stats.errors_are_zero() {
                info!("V6R ZERO {}", self.role.label());
            }
            info!(
                "UCN_RUST_ROUTE SUMMARY role={} tx={} rx={} ping_ok={} rtt_min={} rtt_avg={} rtt_max={} route={} recovered={} rerr_guard={} carrier_err={} wire_err={} route_err={} tx_err={} rx_err={} timeout={}",
                self.role.label(),
                self.stats.tx,
                self.stats.rx,
                self.stats.ping_ok,
                self.stats.rtt_min_us,
                self.stats.rtt_average_us(),
                self.stats.rtt_max_us,
                u8::from(self.route.is_some()),
                u8::from(self.recovered),
                u8::from(self.rerr_guard_done),
                self.stats.carrier_errors,
                self.stats.wire_errors,
                self.stats.route_errors,
                self.stats.tx_errors,
                self.stats.rx_errors,
                self.stats.timeout
            );
            *next_summary = current_us.saturating_add(SUMMARY_INTERVAL_US);
        }
    }
}

fn endpoint_loop(role: BoardRole, mut uart: Uart<'_, Blocking>) -> ! {
    let mut state = EndpointState::new(role).unwrap_or_else(|problem| panic!("state: {problem:?}"));
    let mut read_buffer = [0_u8; 64];
    let mut packet = [0_u8; PACKET_MAX_BYTES];
    let mut next_summary = SUMMARY_INTERVAL_US;
    info!(
        "UCN_RUST_ROUTE READY role={} topology=B-C-D baud={} link={} peer={} rx={} tx={}",
        role.label(),
        LINK_BAUD,
        state.link.link_id(),
        state.link.peer().address.get(),
        if role == BoardRole::B {
            "GPIO19"
        } else {
            "GPIO15"
        },
        if role == BoardRole::B {
            "GPIO20"
        } else {
            "GPIO16"
        }
    );
    info!("V6R READY {}", role.label());
    loop {
        if uart.read_ready() {
            match uart.read(&mut read_buffer) {
                Ok(read) => {
                    for byte in &read_buffer[..read] {
                        match state.parser.push(*byte) {
                            CarrierEvent::None => {}
                            CarrierEvent::Fault => {
                                state.stats.carrier_errors =
                                    state.stats.carrier_errors.saturating_add(1);
                            }
                            CarrierEvent::Frame(length) => {
                                state.parser.copy_frame(length, &mut packet);
                                state.process_frame(&mut uart, &packet[..length]);
                            }
                        }
                    }
                }
                Err(_) => state.stats.rx_errors = state.stats.rx_errors.saturating_add(1),
            }
        }
        state.maintenance(&mut uart, &mut next_summary);
    }
}

struct RelayState {
    routes: Routes,
    west: LinkRef,
    east: LinkRef,
    west_parser: CarrierParser,
    east_parser: CarrierParser,
    ready_logged: bool,
    stats: Stats,
}

impl RelayState {
    fn new() -> Result<Self> {
        Ok(Self {
            routes: new_routes(BoardRole::C)?,
            west: link_for(BoardRole::C, LINK_BC)?,
            east: link_for(BoardRole::C, LINK_CD)?,
            west_parser: CarrierParser::new(),
            east_parser: CarrierParser::new(),
            ready_logged: false,
            stats: Stats::default(),
        })
    }

    fn uart_for_link<'a, 'd>(
        link_id: u16,
        west: &'a mut Uart<'d, Blocking>,
        east: &'a mut Uart<'d, Blocking>,
    ) -> Result<&'a mut Uart<'d, Blocking>> {
        match link_id {
            LINK_BC => Ok(west),
            LINK_CD => Ok(east),
            _ => Err(Error::Argument),
        }
    }

    fn process_frame<'d>(
        &mut self,
        ingress_id: u16,
        packet: &[u8],
        west_uart: &mut Uart<'d, Blocking>,
        east_uart: &mut Uart<'d, Blocking>,
    ) {
        self.stats.rx = self.stats.rx.saturating_add(1);
        let ingress = if ingress_id == LINK_BC {
            self.west
        } else {
            self.east
        };
        match packet.first().copied() {
            Some(PACKET_RREQ) => {
                let request = match decode_rreq_message(packet) {
                    Ok(value) => value,
                    Err(_) => {
                        self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                        return;
                    }
                };
                match self.routes.on_rreq(request, ingress, now_us()) {
                    Ok(RequestAction::Forward(forwarded)) => {
                        let output_id = if ingress_id == LINK_BC {
                            LINK_CD
                        } else {
                            LINK_BC
                        };
                        let mut encoded = [0_u8; PACKET_MAX_BYTES];
                        let sent = encode_rreq_message(forwarded, &mut encoded)
                            .ok()
                            .is_some_and(|length| {
                                Self::uart_for_link(output_id, west_uart, east_uart)
                                    .ok()
                                    .is_some_and(|uart| {
                                        send_carrier(uart, &encoded[..length], &mut self.stats)
                                    })
                            });
                        if sent {
                            self.stats.forwarded_rreq = self.stats.forwarded_rreq.saturating_add(1);
                        } else {
                            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                        }
                    }
                    Ok(RequestAction::Duplicate) => {}
                    Ok(RequestAction::LocalTarget) | Err(_) => {
                        self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                    }
                }
            }
            Some(PACKET_RREP) => {
                let reply = match decode_rrep_message(packet) {
                    Ok(value) => value,
                    Err(_) => {
                        self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                        return;
                    }
                };
                match self.routes.on_rrep(reply, ingress, now_us()) {
                    Ok(ReplyAction::Forward {
                        message,
                        upstream,
                        completion,
                    }) => {
                        let mut encoded = [0_u8; PACKET_MAX_BYTES];
                        let sent =
                            encode_rrep_message(message, &mut encoded)
                                .ok()
                                .is_some_and(|length| {
                                    Self::uart_for_link(upstream.link_id(), west_uart, east_uart)
                                        .ok()
                                        .is_some_and(|uart| {
                                            send_carrier(uart, &encoded[..length], &mut self.stats)
                                        })
                                });
                        if self.routes.complete_rrep_forward(completion, sent).is_err() {
                            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                        } else if sent {
                            self.stats.forwarded_rrep = self.stats.forwarded_rrep.saturating_add(1);
                        }
                    }
                    Ok(ReplyAction::ReachedOrigin(_)) | Err(_) => {
                        self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                    }
                }
            }
            Some(PACKET_DATA) => self.forward_data(packet, west_uart, east_uart),
            _ => self.stats.wire_errors = self.stats.wire_errors.saturating_add(1),
        }
    }

    fn forward_data<'d>(
        &mut self,
        packet: &[u8],
        west_uart: &mut Uart<'d, Blocking>,
        east_uart: &mut Uart<'d, Blocking>,
    ) {
        let frame = match decode_c1_o0_h0(&packet[1..], AddressWidth::A0, HopProfile::H0) {
            Ok(value) => value,
            Err(_) => {
                self.stats.wire_errors = self.stats.wire_errors.saturating_add(1);
                return;
            }
        };
        let origin = match role_for_address(frame.source.get()) {
            Ok(value @ (BoardRole::B | BoardRole::D)) => value,
            _ => {
                self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                return;
            }
        };
        let destination = match role_for_address(frame.destination.get()) {
            Ok(value @ (BoardRole::B | BoardRole::D)) if value != origin => value,
            _ => {
                self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                return;
            }
        };
        let output_id = if destination == BoardRole::B {
            LINK_BC
        } else {
            LINK_CD
        };
        let resolved = self.routes.resolve(
            match route_domain(origin, destination) {
                Ok(value) => value,
                Err(_) => {
                    self.stats.route_errors = self.stats.route_errors.saturating_add(1);
                    return;
                }
            },
            RouteUseFacts {
                now_us: now_us(),
                origin_session_generation: PeerSessionGeneration::new(SESSION_GENERATION).unwrap(),
                destination_binding_generation: BINDING_GENERATION,
                link_generation: LinkInstanceGeneration::new(LINK_GENERATION).unwrap(),
            },
        );
        let Ok(ResolvedRoute::Dynamic(route)) = resolved else {
            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
            return;
        };
        if route.next_hop().link_id() != output_id {
            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
            return;
        }
        let mut forwarded = [0_u8; PACKET_MAX_BYTES];
        let sent = forward_data_message(packet, &mut forwarded)
            .ok()
            .is_some_and(|length| {
                Self::uart_for_link(output_id, west_uart, east_uart)
                    .ok()
                    .is_some_and(|uart| send_carrier(uart, &forwarded[..length], &mut self.stats))
            });
        if sent {
            self.stats.forwarded_data = self.stats.forwarded_data.saturating_add(1);
            if self.stats.forwarded_data == 1 {
                info!(
                    "UCN_RUST_ROUTE RELAY_DATA_ACTIVE role=C origin={} destination={}",
                    origin.label(),
                    destination.label()
                );
                info!("V6R C_DATA");
            }
        } else {
            self.stats.route_errors = self.stats.route_errors.saturating_add(1);
        }
    }

    fn maybe_log_ready(&mut self) {
        if !self.ready_logged && self.routes.counts().2 == 4 {
            self.ready_logged = true;
            info!("UCN_RUST_ROUTE RELAY_PATHS_READY role=C routes=4");
            info!("V6R C_READY");
        }
    }

    fn summary(&self) {
        if self.stats.errors_are_zero() {
            info!("V6R ZERO C");
        }
        info!(
            "UCN_RUST_ROUTE SUMMARY role=C tx={} rx={} fwd_rreq={} fwd_rrep={} fwd_data={} routes={} carrier_err={} wire_err={} route_err={} tx_err={} rx_err={} timeout={}",
            self.stats.tx,
            self.stats.rx,
            self.stats.forwarded_rreq,
            self.stats.forwarded_rrep,
            self.stats.forwarded_data,
            self.routes.counts().2,
            self.stats.carrier_errors,
            self.stats.wire_errors,
            self.stats.route_errors,
            self.stats.tx_errors,
            self.stats.rx_errors,
            self.stats.timeout
        );
    }
}

fn poll_relay_link<'d>(
    link_id: u16,
    state: &mut RelayState,
    west_uart: &mut Uart<'d, Blocking>,
    east_uart: &mut Uart<'d, Blocking>,
    read_buffer: &mut [u8; 64],
    packet: &mut [u8; PACKET_MAX_BYTES],
) {
    let uart = if link_id == LINK_BC {
        &mut *west_uart
    } else {
        &mut *east_uart
    };
    if !uart.read_ready() {
        return;
    }
    let read = match uart.read(read_buffer) {
        Ok(value) => value,
        Err(_) => {
            state.stats.rx_errors = state.stats.rx_errors.saturating_add(1);
            return;
        }
    };
    for byte in &read_buffer[..read] {
        let event = if link_id == LINK_BC {
            state.west_parser.push(*byte)
        } else {
            state.east_parser.push(*byte)
        };
        match event {
            CarrierEvent::None => {}
            CarrierEvent::Fault => {
                state.stats.carrier_errors = state.stats.carrier_errors.saturating_add(1);
            }
            CarrierEvent::Frame(length) => {
                if link_id == LINK_BC {
                    state.west_parser.copy_frame(length, packet);
                } else {
                    state.east_parser.copy_frame(length, packet);
                }
                state.process_frame(link_id, &packet[..length], west_uart, east_uart);
            }
        }
    }
}

fn relay_loop<'d>(mut west_uart: Uart<'d, Blocking>, mut east_uart: Uart<'d, Blocking>) -> ! {
    let mut state = RelayState::new().unwrap_or_else(|problem| panic!("relay: {problem:?}"));
    let mut read_buffer = [0_u8; 64];
    let mut packet = [0_u8; PACKET_MAX_BYTES];
    let mut next_summary = SUMMARY_INTERVAL_US;
    info!(
        "UCN_RUST_ROUTE READY role=C topology=B-C-D baud={} west=GPIO19/20 east=GPIO15/16",
        LINK_BAUD
    );
    info!("V6R READY C");
    loop {
        poll_relay_link(
            LINK_BC,
            &mut state,
            &mut west_uart,
            &mut east_uart,
            &mut read_buffer,
            &mut packet,
        );
        poll_relay_link(
            LINK_CD,
            &mut state,
            &mut west_uart,
            &mut east_uart,
            &mut read_buffer,
            &mut packet,
        );
        state.maybe_log_ready();
        let current_us = now_us();
        if current_us >= next_summary {
            state.summary();
            next_summary = current_us.saturating_add(SUMMARY_INTERVAL_US);
        }
    }
}

#[main]
fn main() -> ! {
    esp_println::logger::init_logger_from_env();
    let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
    let peripherals = esp_hal::init(config);
    let mac = efuse::base_mac_address();
    let role = if mac.as_bytes() == MAC_B {
        BoardRole::B
    } else if mac.as_bytes() == MAC_C {
        BoardRole::C
    } else if mac.as_bytes() == MAC_D {
        BoardRole::D
    } else {
        error!("UCN_RUST_ROUTE UNSUPPORTED_BOARD mac={mac}");
        loop {
            let start = Instant::now();
            while start.elapsed() < Duration::from_secs(1) {}
        }
    };
    info!(
        "UCN_RUST_ROUTE BOOT protocol={} target=ESP32-S3 mac={} role={}",
        ucn_wire::protocol_major(),
        mac,
        role.label()
    );
    info!("V6R BOOT {}", role.label());
    let uart_config = UartConfig::default().with_baudrate(LINK_BAUD);
    match role {
        BoardRole::B => {
            let rx = InputSignal::from(peripherals.GPIO19).with_gpio_matrix_forced(true);
            let uart = Uart::new(peripherals.UART2, uart_config)
                .unwrap_or_else(|problem| panic!("UART2: {problem:?}"))
                .with_rx(rx)
                .with_tx(peripherals.GPIO20);
            endpoint_loop(role, uart)
        }
        BoardRole::D => {
            let rx = InputSignal::from(peripherals.GPIO15).with_gpio_matrix_forced(true);
            let uart = Uart::new(peripherals.UART1, uart_config)
                .unwrap_or_else(|problem| panic!("UART1: {problem:?}"))
                .with_rx(rx)
                .with_tx(peripherals.GPIO16);
            endpoint_loop(role, uart)
        }
        BoardRole::C => {
            let west_rx = InputSignal::from(peripherals.GPIO19).with_gpio_matrix_forced(true);
            let west = Uart::new(peripherals.UART2, uart_config)
                .unwrap_or_else(|problem| panic!("UART2: {problem:?}"))
                .with_rx(west_rx)
                .with_tx(peripherals.GPIO20);
            let east_rx = InputSignal::from(peripherals.GPIO15).with_gpio_matrix_forced(true);
            let east = Uart::new(peripherals.UART1, uart_config)
                .unwrap_or_else(|problem| panic!("UART1: {problem:?}"))
                .with_rx(east_rx)
                .with_tx(peripherals.GPIO16);
            relay_loop(west, east)
        }
    }
}
