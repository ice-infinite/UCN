#![no_std]
#![no_main]
#![forbid(unsafe_code)]

use esp_backtrace as _;
use esp_hal::Blocking;
use esp_hal::clock::CpuClock;
use esp_hal::efuse;
use esp_hal::gpio::interconnect::InputSignal;
use esp_hal::main;
use esp_hal::time::{Duration, Instant};
use esp_hal::uart::{Config as UartConfig, Uart};
use log::{error, info};

esp_bootloader_esp_idf::esp_app_desc!();

const MAC_B: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x2A, 0x92, 0x44];
const MAC_C: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x29, 0x9C, 0xD8];
const LINK_BAUD: u32 = 115_200;

fn write_all(uart: &mut Uart<'_, Blocking>, bytes: &[u8]) -> bool {
    let mut offset = 0;
    while offset < bytes.len() {
        match uart.write(&bytes[offset..]) {
            Ok(0) | Err(_) => return false,
            Ok(written) => offset += written,
        }
    }
    uart.flush().is_ok()
}

#[main]
fn main() -> ! {
    esp_println::logger::init_logger_from_env();
    let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
    let peripherals = esp_hal::init(config);
    let mac = efuse::base_mac_address();
    let (role, marker, first_send_delay_us) = if mac.as_bytes() == MAC_B {
        ("B", 0xB6_u8, 1_000_000_u64)
    } else if mac.as_bytes() == MAC_C {
        ("C", 0xC7_u8, 1_050_000_u64)
    } else {
        error!("UCN_UART1_BC UNSUPPORTED_BOARD mac={mac}");
        loop {}
    };

    let uart_config = UartConfig::default().with_baudrate(LINK_BAUD);
    let uart_rx = InputSignal::from(peripherals.GPIO15).with_gpio_matrix_forced(true);
    let mut uart = Uart::new(peripherals.UART1, uart_config)
        .expect("UART1 config")
        .with_rx(uart_rx)
        .with_tx(peripherals.GPIO16);
    let matrix = esp_hal::peripherals::GPIO::regs()
        .func_in_sel_cfg(15)
        .read();
    info!(
        "UCN_UART1_BC READY role={} mac={} baud={} rx=GPIO15 tx=GPIO16 matrix_sel={} matrix_pin={}",
        role,
        mac,
        LINK_BAUD,
        matrix.sel().bit_is_set(),
        matrix.in_sel().bits()
    );
    let uart_regs = esp_hal::peripherals::UART1::regs();
    let conf0 = uart_regs.conf0().read();
    let clock_divider = uart_regs.clkdiv().read();
    let clock_config = uart_regs.clk_conf().read();
    info!(
        "UCN_UART1_BC REG role={} autobaud={} bits={} stop={} parity={} clkdiv={} frag={} sclk={} rx_clk={} tx_clk={}",
        role,
        conf0.autobaud_en().bit_is_set(),
        conf0.bit_num().bits(),
        conf0.stop_bit_num().bits(),
        conf0.parity_en().bit_is_set(),
        clock_divider.clkdiv().bits(),
        clock_divider.frag().bits(),
        clock_config.sclk_sel().bits(),
        clock_config.rx_sclk_en().bit_is_set(),
        clock_config.tx_sclk_en().bit_is_set()
    );

    let started = Instant::now();
    let mut next_send = started + Duration::from_micros(first_send_delay_us);
    let mut next_report = started + Duration::from_secs(5);
    let mut sequence = 0_u16;
    let mut tx_frames = 0_u32;
    let mut rx_bytes = 0_u32;
    let mut rx_errors = 0_u32;
    let mut gpio_rx_edges = 0_u32;
    let mut gpio_rx_level =
        ((esp_hal::peripherals::GPIO::regs().in_().read().bits() >> 15) & 1) != 0;
    let mut first_rx = false;
    let mut rx = [0_u8; 64];

    loop {
        let sampled_level =
            ((esp_hal::peripherals::GPIO::regs().in_().read().bits() >> 15) & 1) != 0;
        if sampled_level != gpio_rx_level {
            gpio_rx_level = sampled_level;
            gpio_rx_edges = gpio_rx_edges.saturating_add(1);
        }
        while uart.read_ready() {
            match uart.read_buffered(&mut rx) {
                Ok(0) => break,
                Ok(read) => {
                    rx_bytes = rx_bytes.saturating_add(read as u32);
                    if !first_rx {
                        first_rx = true;
                        info!(
                            "UCN_UART1_BC FIRST_RX role={} count={} b0=0x{:02X}",
                            role, read, rx[0]
                        );
                    }
                }
                Err(_) => {
                    rx_errors = rx_errors.saturating_add(1);
                    break;
                }
            }
        }
        if uart.check_for_rx_errors().is_err() {
            rx_errors = rx_errors.saturating_add(1);
        }

        let now = Instant::now();
        if now >= next_send {
            sequence = sequence.wrapping_add(1);
            let [hi, lo] = sequence.to_be_bytes();
            if write_all(&mut uart, &[marker, hi, lo, marker ^ lo]) {
                tx_frames = tx_frames.saturating_add(1);
            }
            next_send += Duration::from_millis(100);
        }
        if now >= next_report {
            info!(
                "UCN_UART1_BC SUMMARY role={} tx_frames={} rx_bytes={} gpio_rx_edges={} uart_rx_edges={} rx_errors={}",
                role,
                tx_frames,
                rx_bytes,
                gpio_rx_edges,
                uart_regs.rxd_cnt().read().rxd_edge_cnt().bits(),
                rx_errors
            );
            next_report += Duration::from_secs(5);
        }
    }
}
