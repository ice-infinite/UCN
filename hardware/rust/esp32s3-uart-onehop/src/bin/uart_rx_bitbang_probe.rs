#![no_std]
#![no_main]
#![forbid(unsafe_code)]

use esp_backtrace as _;
use esp_hal::clock::CpuClock;
use esp_hal::efuse;
use esp_hal::gpio::{Input, InputConfig, Level, Output, OutputConfig, Pull};
use esp_hal::main;
use esp_hal::time::{Duration, Instant};
use log::{error, info};

esp_bootloader_esp_idf::esp_app_desc!();

const MAC_B: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x2A, 0x92, 0x44];
const MAC_C: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x29, 0x9C, 0xD8];
const LINK_BAUD: u32 = 9_600;
const BIT_TIME_US: u64 = 104;

#[esp_hal::ram]
fn delay_us(us: u64) {
    esp_hal::rom::ets_delay_us(us as u32);
}

#[esp_hal::ram]
fn send_byte(tx: &mut Output<'_>, byte: u8) {
    tx.set_low();
    delay_us(BIT_TIME_US);
    let mut mask = 1_u8;
    while mask != 0 {
        if byte & mask == 0 {
            tx.set_low();
        } else {
            tx.set_high();
        }
        delay_us(BIT_TIME_US);
        mask = mask.wrapping_shl(1);
    }
    tx.set_high();
    delay_us(BIT_TIME_US);
}

fn tx_loop(mut tx: Output<'_>) -> ! {
    tx.set_high();
    delay_us(1_000_000);
    let mut frames = 0_u32;
    let mut next_report = Instant::now() + Duration::from_secs(5);
    loop {
        send_byte(&mut tx, 0x55);
        send_byte(&mut tx, 0xA5);
        frames = frames.saturating_add(1);
        delay_us(20_000);
        let now = Instant::now();
        if now >= next_report {
            info!("UCN_BITBANG TX_SUMMARY role=B frames={frames}");
            next_report += Duration::from_secs(5);
        }
    }
}

#[esp_hal::ram]
fn receive_byte(rx: &Input<'_>) -> Option<u8> {
    // A reset or a previously malformed word can leave the decoder observing
    // the line in the middle of a low cell. First require a return to idle;
    // otherwise every low data bit would be mistaken for a new start bit.
    while rx.is_low() {}
    while rx.is_high() {}

    // Sample bit 0 in the middle of its cell. The falling edge starts the
    // start bit, so the first data-bit centre is 1.5 bit periods later.
    delay_us(BIT_TIME_US + (BIT_TIME_US / 2));
    let mut value = 0_u8;
    for bit in 0..8 {
        if rx.is_high() {
            value |= 1_u8 << bit;
        }
        delay_us(BIT_TIME_US);
    }

    // We are now at the centre of the stop bit. Reject malformed words, but
    // leave the caller able to resynchronise on the next falling edge.
    rx.is_high().then_some(value)
}

fn rx_loop(rx: Input<'_>) -> ! {
    let mut bytes = 0_u32;
    let mut errors = 0_u32;
    let mut frames = 0_u32;
    let mut pattern_errors = 0_u32;
    let mut first_rx = false;
    let mut expected = 0x55_u8;
    let mut next_report = Instant::now() + Duration::from_secs(5);
    loop {
        match receive_byte(&rx) {
            Some(byte) => {
                bytes = bytes.saturating_add(1);
                if !first_rx {
                    first_rx = true;
                    info!("UCN_BITBANG FIRST_RX role=C b0=0x{byte:02X}");
                }
                if byte != expected {
                    pattern_errors = pattern_errors.saturating_add(1);
                    expected = 0x55;
                } else if expected == 0x55 {
                    expected = 0xA5;
                } else {
                    frames = frames.saturating_add(1);
                    expected = 0x55;
                }
            }
            None => errors = errors.saturating_add(1),
        }
        let now = Instant::now();
        if now >= next_report {
            info!(
                "UCN_BITBANG RX_SUMMARY role=C bytes={} frames={} framing_errors={} pattern_errors={}",
                bytes, frames, errors, pattern_errors
            );
            next_report += Duration::from_secs(5);
        }
    }
}

#[main]
fn main() -> ! {
    esp_println::logger::init_logger_from_env();
    let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
    let peripherals = esp_hal::init(config);
    let mac = efuse::base_mac_address();
    if mac.as_bytes() == MAC_B {
        info!(
            "UCN_BITBANG READY role=B mac={} baud={} tx=GPIO16",
            mac, LINK_BAUD
        );
        tx_loop(Output::new(
            peripherals.GPIO16,
            Level::High,
            OutputConfig::default(),
        ));
    }
    if mac.as_bytes() == MAC_C {
        let rx = Input::new(
            peripherals.GPIO15,
            InputConfig::default().with_pull(Pull::Up),
        );
        info!(
            "UCN_BITBANG READY role=C mac={} baud={} rx=GPIO15 decoder=software",
            mac, LINK_BAUD
        );
        rx_loop(rx);
    }
    error!("UCN_BITBANG UNSUPPORTED_BOARD mac={mac}");
    loop {
        delay_us(1_000_000);
    }
}
