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
use ucn_adapter::{
    AdapterEventIngress, AdapterOwner, DriverCancel, DriverSubmit, LinkHandle, RxMeta,
    TerminalOutcome, TxDriver, TxState, TxToken,
};
use ucn_types::{
    AddressWidth, DeliveryGuarantee, Error, HeaderContract, HopLimit, HopProfile, InteractionRole,
    NodeAddress, OriginSecurity, OriginSequence, PayloadKind, ServiceId, TrafficClass,
};
use ucn_wire::{C1Frame, CommonHeader, decode_c1_o0_h0, encode_c1_o0_h0};

esp_bootloader_esp_idf::esp_app_desc!();

const MAC_B: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x2A, 0x92, 0x44];
const MAC_C: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x29, 0x9C, 0xD8];
const LINK_BAUD: u32 = 691_200;
const LINK_MTU: usize = 64;
const ADAPTER_INSTANCE: u32 = 0x5255_4152;
const SERVICE_ID: u16 = 0x7001;
const PING_INTERVAL_US: u64 = 1_000_000;
const PING_TIMEOUT_US: u64 = 800_000;
const SUMMARY_INTERVAL_US: u64 = 10_000_000;
const MSG_PING: u8 = 0x50;
const MSG_ACK: u8 = 0x41;

const CARRIER_MAGIC_0: u8 = 0xD6;
const CARRIER_MAGIC_1: u8 = 0x5A;
const CARRIER_OVERHEAD: usize = 5;
const CARRIER_MAX_BYTES: usize = LINK_MTU + CARRIER_OVERHEAD;

type Ingress = AdapterEventIngress<1, 2, 2, LINK_MTU>;
type Owner<'a> = AdapterOwner<'a, 1, 2, 2, LINK_MTU>;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum BoardRole {
    B,
    C,
}

impl BoardRole {
    const fn label(self) -> &'static str {
        match self {
            Self::B => "B",
            Self::C => "C",
        }
    }

    const fn local_address(self) -> u32 {
        match self {
            Self::B => 2,
            Self::C => 3,
        }
    }

    const fn peer_address(self) -> u32 {
        match self {
            Self::B => 3,
            Self::C => 2,
        }
    }

    const fn first_ping_us(self) -> u64 {
        match self {
            Self::B => 1_000_000,
            Self::C => 1_500_000,
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
    frame: [u8; LINK_MTU],
}

impl CarrierParser {
    const fn new() -> Self {
        Self {
            state: 0,
            frame_len: 0,
            frame_pos: 0,
            crc: 0xFFFF,
            crc_high: 0,
            frame: [0; LINK_MTU],
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
                if length == 0 || length > LINK_MTU {
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

    fn frame(&self, length: usize) -> &[u8] {
        &self.frame[..length]
    }
}

#[derive(Default)]
struct Stats {
    tx_frames: u32,
    rx_frames: u32,
    uart_rx_bytes: u32,
    ping_ok: u32,
    ping_timeout: u32,
    tx_errors: u32,
    rx_errors: u32,
    wire_errors: u32,
    sequence_errors: u32,
    carrier_errors: u32,
    backpressure_retries: u32,
    rtt_sum_us: u64,
    rtt_min_us: u64,
    rtt_max_us: u64,
}

impl Stats {
    fn record_rtt(&mut self, rtt_us: u64) {
        self.ping_ok += 1;
        self.rtt_sum_us = self.rtt_sum_us.saturating_add(rtt_us);
        if self.rtt_min_us == 0 || rtt_us < self.rtt_min_us {
            self.rtt_min_us = rtt_us;
        }
        if rtt_us > self.rtt_max_us {
            self.rtt_max_us = rtt_us;
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

struct PendingPing {
    active: bool,
    sequence: u32,
    sent_at_us: u64,
}

impl PendingPing {
    const fn empty() -> Self {
        Self {
            active: false,
            sequence: 0,
            sent_at_us: 0,
        }
    }
}

struct UartTxDriver<'a, 'd> {
    uart: &'a mut Uart<'d, Blocking>,
    forced_backpressure: &'a mut u8,
}

impl TxDriver for UartTxDriver<'_, '_> {
    fn submit(&mut self, _link: LinkHandle, frame: &[u8], _token: TxToken) -> DriverSubmit {
        if *self.forced_backpressure != 0 {
            *self.forced_backpressure -= 1;
            return DriverSubmit::NotSubmitted;
        }
        if frame.is_empty() || frame.len() > LINK_MTU {
            return DriverSubmit::Complete(TerminalOutcome::Failure(Error::Argument));
        }
        if !self.uart.write_ready() {
            return DriverSubmit::NotSubmitted;
        }

        let mut carrier = [0_u8; CARRIER_MAX_BYTES];
        carrier[0] = CARRIER_MAGIC_0;
        carrier[1] = CARRIER_MAGIC_1;
        carrier[2] = frame.len() as u8;
        carrier[3..3 + frame.len()].copy_from_slice(frame);
        let mut crc = crc16_update(0xFFFF, carrier[2]);
        for byte in frame {
            crc = crc16_update(crc, *byte);
        }
        let crc_bytes = crc.to_be_bytes();
        let carrier_len = frame.len() + CARRIER_OVERHEAD;
        carrier[3 + frame.len()] = crc_bytes[0];
        carrier[4 + frame.len()] = crc_bytes[1];

        match self.uart.write(&carrier[..carrier_len]) {
            Ok(written) if written == carrier_len => match self.uart.flush() {
                Ok(()) => DriverSubmit::Complete(TerminalOutcome::Success),
                Err(_) => DriverSubmit::Unknown,
            },
            Ok(_) | Err(_) => DriverSubmit::Unknown,
        }
    }

    fn cancel(&mut self, _token: TxToken) -> DriverCancel {
        DriverCancel::NotCancelled
    }
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

fn delay_us(duration_us: u64) {
    let start = Instant::now();
    while start.elapsed() < Duration::from_micros(duration_us) {}
}

fn next_sequence(sequence: &mut u32) -> Option<u32> {
    let current = *sequence;
    let next = current.checked_add(1)?;
    *sequence = next;
    Some(current)
}

fn encode_message(
    role: BoardRole,
    origin_sequence: u32,
    kind: u8,
    echoed_sequence: u32,
    output: &mut [u8; LINK_MTU],
) -> Result<usize, Error> {
    let mut payload = [0_u8; 5];
    payload[0] = kind;
    payload[1..].copy_from_slice(&echoed_sequence.to_be_bytes());
    let frame = C1Frame {
        common: CommonHeader {
            contract: HeaderContract::C1,
            traffic_class: TrafficClass::Q1,
            delivery: DeliveryGuarantee::BestEffort,
            interaction: InteractionRole::OneWay,
            payload_kind: PayloadKind::Data,
            origin_security: OriginSecurity::O0,
            hop_limit: HopLimit::new(1)?,
        },
        source: NodeAddress::new(role.local_address(), AddressWidth::A0)?,
        destination: NodeAddress::new(role.peer_address(), AddressWidth::A0)?,
        service_id: ServiceId::new(SERVICE_ID)?,
        origin_sequence: OriginSequence::new(origin_sequence)?,
        payload: &payload,
    };
    encode_c1_o0_h0(&frame, AddressWidth::A0, HopProfile::H0, output)
}

fn submit_message<'d>(
    owner: &mut Owner<'_>,
    link: LinkHandle,
    uart: &mut Uart<'d, Blocking>,
    forced_backpressure: &mut u8,
    frame: &[u8],
    stats: &mut Stats,
) -> bool {
    let token = match owner.tx_reserve(link, 0) {
        Ok(value) => value,
        Err(problem) => {
            error!("UCN_RUST_UART TX_RESERVE_FAIL error={problem:?}");
            stats.tx_errors += 1;
            return false;
        }
    };
    let generation = token.generation();
    for attempt in 1..=8 {
        let mut driver = UartTxDriver {
            uart,
            forced_backpressure,
        };
        match owner.tx_submit(token, frame, &mut driver) {
            Ok(view) if view.state == TxState::NotSubmitted => {
                stats.backpressure_retries += 1;
                info!(
                    "UCN_RUST_UART BACKPRESSURE token_slot={} token_generation={} attempt={}",
                    token.slot(),
                    generation,
                    attempt
                );
                delay_us(1_000);
            }
            Ok(view)
                if view.state == TxState::Completed
                    && view.terminal == Some(TerminalOutcome::Success) =>
            {
                if owner.tx_retire(token).is_err() {
                    error!("UCN_RUST_UART TX_RETIRE_FAIL token_generation={generation}");
                    stats.tx_errors += 1;
                    return false;
                }
                stats.tx_frames += 1;
                return true;
            }
            Ok(view) => {
                error!(
                    "UCN_RUST_UART TX_STATE_FAIL state={:?} terminal={:?}",
                    view.state, view.terminal
                );
                stats.tx_errors += 1;
                return false;
            }
            Err(problem) => {
                error!("UCN_RUST_UART TX_SUBMIT_FAIL error={problem:?}");
                stats.tx_errors += 1;
                return false;
            }
        }
    }
    let _ = owner.tx_retire(token);
    warn!("UCN_RUST_UART TX_BACKPRESSURE_EXHAUSTED token_generation={generation}");
    stats.tx_errors += 1;
    false
}

fn send_typed_message<'d>(
    role: BoardRole,
    kind: u8,
    echoed_sequence: u32,
    next_origin_sequence: &mut u32,
    owner: &mut Owner<'_>,
    link: LinkHandle,
    uart: &mut Uart<'d, Blocking>,
    forced_backpressure: &mut u8,
    stats: &mut Stats,
) -> Option<u32> {
    let sequence = match next_sequence(next_origin_sequence) {
        Some(value) => value,
        None => {
            error!("UCN_RUST_UART SEQUENCE_EXHAUSTED");
            stats.tx_errors += 1;
            return None;
        }
    };
    let mut encoded = [0_u8; LINK_MTU];
    let length = match encode_message(role, sequence, kind, echoed_sequence, &mut encoded) {
        Ok(value) => value,
        Err(problem) => {
            error!("UCN_RUST_UART WIRE_ENCODE_FAIL error={problem:?}");
            stats.wire_errors += 1;
            return None;
        }
    };
    if submit_message(
        owner,
        link,
        uart,
        forced_backpressure,
        &encoded[..length],
        stats,
    ) {
        Some(sequence)
    } else {
        None
    }
}

fn main_loop<'d>(role: BoardRole, mut uart: Uart<'d, Blocking>) -> ! {
    let ingress = match Ingress::new(ADAPTER_INSTANCE) {
        Ok(value) => value,
        Err(problem) => panic!("ingress config: {problem:?}"),
    };
    let mut owner = match Owner::new(ADAPTER_INSTANCE, &ingress) {
        Ok(value) => value,
        Err(problem) => panic!("owner config: {problem:?}"),
    };
    let link = match owner.open_link(0, 1, LINK_MTU) {
        Ok(value) => value,
        Err(problem) => panic!("link open: {problem:?}"),
    };
    if let Err(problem) = owner.try_set_rx_enabled(true) {
        panic!("RX enable: {problem:?}");
    }

    let mut parser = CarrierParser::new();
    let mut stats = Stats::default();
    let mut next_origin_sequence = 1_u32;
    let mut last_peer_sequence = 0_u32;
    let mut pending = PendingPing::empty();
    let mut next_ping_at_us = role.first_ping_us();
    let mut next_summary_at_us = SUMMARY_INTERVAL_US;
    let mut forced_backpressure = 2_u8;
    let mut uart_rx = [0_u8; 32];
    let mut claimed = [0_u8; LINK_MTU];

    info!(
        "UCN_RUST_UART READY role={} local={} peer={} baud={} rx=GPIO19 tx=GPIO20 link_generation={}",
        role.label(),
        role.local_address(),
        role.peer_address(),
        LINK_BAUD,
        link.instance_generation()
    );

    loop {
        if uart.read_ready() {
            match uart.read(&mut uart_rx) {
                Ok(read) => {
                    let first_rx = stats.uart_rx_bytes == 0 && read != 0;
                    stats.uart_rx_bytes = stats.uart_rx_bytes.saturating_add(read as u32);
                    if first_rx {
                        info!(
                            "UCN_RUST_UART FIRST_RX_BYTES count={} b0=0x{:02X}",
                            read, uart_rx[0]
                        );
                    }
                    for byte in &uart_rx[..read] {
                        match parser.push(*byte) {
                            CarrierEvent::None => {}
                            CarrierEvent::Fault => {
                                stats.carrier_errors += 1;
                                warn!(
                                    "UCN_RUST_UART CARRIER_REJECT count={}",
                                    stats.carrier_errors
                                );
                            }
                            CarrierEvent::Frame(frame_len) => {
                                let metadata = RxMeta {
                                    timestamp_us: now_us(),
                                    sender_discriminator: role.peer_address(),
                                };
                                let published =
                                    ingress.rx_publish(link, parser.frame(frame_len), metadata);
                                if let Err(problem) = published {
                                    stats.rx_errors += 1;
                                    error!("UCN_RUST_UART RX_PUBLISH_FAIL error={problem:?}");
                                    continue;
                                }
                                let view = match owner.rx_claim(&mut claimed) {
                                    Ok(value) => value,
                                    Err(problem) => {
                                        stats.rx_errors += 1;
                                        error!("UCN_RUST_UART RX_CLAIM_FAIL error={problem:?}");
                                        continue;
                                    }
                                };
                                let decoded = decode_c1_o0_h0(
                                    &claimed[..view.frame_bytes],
                                    AddressWidth::A0,
                                    HopProfile::H0,
                                );
                                let mut ack_sequence = None;
                                match decoded {
                                    Ok(frame)
                                        if frame.source.get() == role.peer_address()
                                            && frame.destination.get() == role.local_address()
                                            && frame.service_id.get() == SERVICE_ID
                                            && frame.payload.len() == 5 =>
                                    {
                                        let origin_sequence = frame.origin_sequence.get();
                                        let echoed_sequence = u32::from_be_bytes([
                                            frame.payload[1],
                                            frame.payload[2],
                                            frame.payload[3],
                                            frame.payload[4],
                                        ]);
                                        if origin_sequence <= last_peer_sequence {
                                            stats.sequence_errors += 1;
                                            warn!(
                                                "UCN_RUST_UART SEQUENCE_REJECT current={} last={}",
                                                origin_sequence, last_peer_sequence
                                            );
                                        } else {
                                            last_peer_sequence = origin_sequence;
                                            stats.rx_frames += 1;
                                            if frame.payload[0] == MSG_PING
                                                && echoed_sequence == origin_sequence
                                            {
                                                ack_sequence = Some(origin_sequence);
                                            } else if frame.payload[0] == MSG_ACK
                                                && pending.active
                                                && echoed_sequence == pending.sequence
                                            {
                                                let rtt_us =
                                                    now_us().wrapping_sub(pending.sent_at_us);
                                                pending.active = false;
                                                stats.record_rtt(rtt_us);
                                                info!(
                                                    "UCN_RUST_UART PING_OK role={} sequence={} rtt_us={}",
                                                    role.label(),
                                                    echoed_sequence,
                                                    rtt_us
                                                );
                                            } else {
                                                stats.wire_errors += 1;
                                                warn!(
                                                    "UCN_RUST_UART PAYLOAD_REJECT kind={} echo={} origin={}",
                                                    frame.payload[0],
                                                    echoed_sequence,
                                                    origin_sequence
                                                );
                                            }
                                        }
                                    }
                                    Ok(_) | Err(_) => {
                                        stats.wire_errors += 1;
                                        warn!(
                                            "UCN_RUST_UART WIRE_REJECT count={}",
                                            stats.wire_errors
                                        );
                                    }
                                }
                                if let Err(problem) = owner.rx_retire(view.token) {
                                    stats.rx_errors += 1;
                                    error!("UCN_RUST_UART RX_RETIRE_FAIL error={problem:?}");
                                }
                                if let Some(peer_ping_sequence) = ack_sequence {
                                    let _ = send_typed_message(
                                        role,
                                        MSG_ACK,
                                        peer_ping_sequence,
                                        &mut next_origin_sequence,
                                        &mut owner,
                                        link,
                                        &mut uart,
                                        &mut forced_backpressure,
                                        &mut stats,
                                    );
                                }
                            }
                        }
                    }
                }
                Err(_) => {
                    parser.reset();
                    stats.rx_errors += 1;
                    warn!("UCN_RUST_UART UART_RX_ERROR count={}", stats.rx_errors);
                }
            }
        }

        let current_us = now_us();
        if pending.active && current_us.wrapping_sub(pending.sent_at_us) >= PING_TIMEOUT_US {
            stats.ping_timeout += 1;
            warn!(
                "UCN_RUST_UART PING_TIMEOUT role={} sequence={}",
                role.label(),
                pending.sequence
            );
            pending.active = false;
        }
        if !pending.active && current_us >= next_ping_at_us {
            if let Some(sequence) = send_typed_message(
                role,
                MSG_PING,
                next_origin_sequence,
                &mut next_origin_sequence,
                &mut owner,
                link,
                &mut uart,
                &mut forced_backpressure,
                &mut stats,
            ) {
                pending = PendingPing {
                    active: true,
                    sequence,
                    sent_at_us: now_us(),
                };
            }
            next_ping_at_us = current_us.saturating_add(PING_INTERVAL_US);
        }
        if current_us >= next_summary_at_us {
            info!(
                "UCN_RUST_UART SUMMARY role={} tx={} rx={} uart_rx_bytes={} ping_ok={} timeout={} rtt_min_us={} rtt_avg_us={} rtt_max_us={} backpressure={} carrier_err={} wire_err={} seq_err={} tx_err={} rx_err={}",
                role.label(),
                stats.tx_frames,
                stats.rx_frames,
                stats.uart_rx_bytes,
                stats.ping_ok,
                stats.ping_timeout,
                stats.rtt_min_us,
                stats.rtt_average_us(),
                stats.rtt_max_us,
                stats.backpressure_retries,
                stats.carrier_errors,
                stats.wire_errors,
                stats.sequence_errors,
                stats.tx_errors,
                stats.rx_errors
            );
            next_summary_at_us = current_us.saturating_add(SUMMARY_INTERVAL_US);
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
    } else {
        error!("UCN_RUST_UART UNSUPPORTED_BOARD mac={mac}");
        loop {
            delay_us(1_000_000);
        }
    };

    info!(
        "UCN_RUST_UART BOOT protocol={} target=ESP32-S3 mac={} role={}",
        ucn_wire::protocol_major(),
        mac,
        role.label()
    );
    let uart_config = UartConfig::default().with_baudrate(LINK_BAUD);
    let uart_rx = InputSignal::from(peripherals.GPIO19).with_gpio_matrix_forced(true);
    let uart = match Uart::new(peripherals.UART2, uart_config) {
        Ok(value) => value.with_rx(uart_rx).with_tx(peripherals.GPIO20),
        Err(problem) => panic!("UART2 config: {problem:?}"),
    };
    main_loop(role, uart)
}
