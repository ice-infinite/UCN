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

const MAC_A: [u8; 6] = [0x7C, 0xE8, 0xB1, 0xB1, 0xEC, 0xF4];
const MAC_B: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x2A, 0x92, 0x44];
const MAC_C: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x29, 0x9C, 0xD8];
const MAC_D: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x2A, 0x8F, 0x98];
const MAC_E: [u8; 6] = [0x98, 0xA3, 0x16, 0xE7, 0x80, 0x0C];
const MAC_F: [u8; 6] = [0xDC, 0xDA, 0x0C, 0x22, 0xAA, 0xD4];

#[esp_hal::ram]
fn delay_us(us: u64) {
    let until = Instant::now() + Duration::from_micros(us);
    while Instant::now() < until {}
}

#[main]
fn main() -> ! {
    esp_println::logger::init_logger_from_env();
    let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
    let peripherals = esp_hal::init(config);
    let mac = efuse::base_mac_address();
    let (role, tx20_period_us, tx16_period_us) = if mac.as_bytes() == MAC_A {
        ("A", 31_000_u64, 37_000_u64)
    } else if mac.as_bytes() == MAC_B {
        ("B", 41_000, 43_000)
    } else if mac.as_bytes() == MAC_C {
        ("C", 47_000, 53_000)
    } else if mac.as_bytes() == MAC_D {
        ("D", 59_000, 61_000)
    } else if mac.as_bytes() == MAC_E {
        ("E", 67_000, 71_000)
    } else if mac.as_bytes() == MAC_F {
        ("F", 73_000, 79_000)
    } else {
        error!("UCN_GPIO_PROBE UNSUPPORTED_BOARD mac={mac}");
        loop {
            delay_us(1_000_000);
        }
    };

    let mut tx20 = Output::new(peripherals.GPIO20, Level::High, OutputConfig::default());
    let rx19 = Input::new(
        peripherals.GPIO19,
        InputConfig::default().with_pull(Pull::Up),
    );
    let mut tx16 = Output::new(peripherals.GPIO16, Level::High, OutputConfig::default());
    let rx15 = Input::new(
        peripherals.GPIO15,
        InputConfig::default().with_pull(Pull::Up),
    );
    let mut output20_high = true;
    let mut input19_high = rx19.is_high();
    let mut output16_high = true;
    let mut input15_high = rx15.is_high();
    let mut output20_edges = 0_u32;
    let mut input19_edges = 0_u32;
    let mut output16_edges = 0_u32;
    let mut input15_edges = 0_u32;
    let mut next_toggle20 = Instant::now() + Duration::from_micros(tx20_period_us);
    let mut next_toggle16 = Instant::now() + Duration::from_micros(tx16_period_us);
    let mut next_report = Instant::now() + Duration::from_secs(5);

    info!(
        "UCN_GPIO_PROBE READY role={} mac={} link0=RX19/TX20/{}us link1=RX15/TX16/{}us",
        role, mac, tx20_period_us, tx16_period_us
    );
    loop {
        let now = Instant::now();
        if now >= next_toggle20 {
            output20_high = !output20_high;
            if output20_high {
                tx20.set_high();
            } else {
                tx20.set_low();
            }
            output20_edges = output20_edges.saturating_add(1);
            next_toggle20 += Duration::from_micros(tx20_period_us);
        }
        if now >= next_toggle16 {
            output16_high = !output16_high;
            if output16_high {
                tx16.set_high();
            } else {
                tx16.set_low();
            }
            output16_edges = output16_edges.saturating_add(1);
            next_toggle16 += Duration::from_micros(tx16_period_us);
        }

        let sampled19_high = rx19.is_high();
        if sampled19_high != input19_high {
            input19_high = sampled19_high;
            input19_edges = input19_edges.saturating_add(1);
        }
        let sampled15_high = rx15.is_high();
        if sampled15_high != input15_high {
            input15_high = sampled15_high;
            input15_edges = input15_edges.saturating_add(1);
        }

        if now >= next_report {
            info!(
                "UCN_GPIO_PROBE SUMMARY role={} tx20_edges={} rx19_edges={} rx19_high={} tx16_edges={} rx15_edges={} rx15_high={}",
                role,
                output20_edges,
                input19_edges,
                input19_high,
                output16_edges,
                input15_edges,
                input15_high
            );
            next_report += Duration::from_secs(5);
        }
    }
}
