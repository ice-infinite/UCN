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

const MAC_A: [u8; 6] = [0x7C, 0xE8, 0xB1, 0xB1, 0xEC, 0xF4];
const MAC_B: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x2A, 0x92, 0x44];
const MAC_C: [u8; 6] = [0x7C, 0x4F, 0xAD, 0x29, 0x9C, 0xD8];
const LINK_BAUD: u32 = 691_200;

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
    let (role, marker, first_send_delay_us) = if mac.as_bytes() == MAC_A {
        ("A", 0xA5_u8, 1_000_000_u64)
    } else if mac.as_bytes() == MAC_B {
        ("B", 0x5A_u8, 1_050_000_u64)
    } else if mac.as_bytes() == MAC_C {
        ("C", 0xC7_u8, 1_100_000_u64)
    } else {
        error!("UCN_UART_RAW UNSUPPORTED_BOARD mac={mac}");
        loop {}
    };

    let uart_config = UartConfig::default().with_baudrate(LINK_BAUD);
    #[cfg(not(feature = "probe-swapped-pins"))]
    let uart_rx = InputSignal::from(peripherals.GPIO19).with_gpio_matrix_forced(true);
    #[cfg(not(feature = "probe-swapped-pins"))]
    let uart_tx = peripherals.GPIO20;
    #[cfg(not(feature = "probe-swapped-pins"))]
    let (rx_pin, tx_pin) = (19_u8, 20_u8);
    #[cfg(feature = "probe-swapped-pins")]
    let uart_rx = InputSignal::from(peripherals.GPIO20).with_gpio_matrix_forced(true);
    #[cfg(feature = "probe-swapped-pins")]
    let uart_tx = peripherals.GPIO19;
    #[cfg(feature = "probe-swapped-pins")]
    let (rx_pin, tx_pin) = (20_u8, 19_u8);
    #[cfg(feature = "probe-uart1")]
    let (mut uart, uart_instance) = (
        Uart::new(peripherals.UART1, uart_config)
            .expect("UART1 config")
            .with_rx(uart_rx)
            .with_tx(uart_tx),
        1_u8,
    );
    #[cfg(not(feature = "probe-uart1"))]
    let (mut uart, uart_instance) = (
        Uart::new(peripherals.UART2, uart_config)
            .expect("UART2 config")
            .with_rx(uart_rx)
            .with_tx(uart_tx),
        2_u8,
    );
    #[cfg(feature = "probe-loopback")]
    {
        let registers = if uart_instance == 1 {
            esp_hal::peripherals::UART1::regs()
        } else {
            esp_hal::peripherals::UART2::regs()
        };
        registers
            .conf0()
            .modify(|_, write| write.loopback().set_bit());
    }
    #[cfg(feature = "probe-force-rx-clock")]
    {
        let system = esp_hal::peripherals::SYSTEM::regs();
        system
            .perip_clk_en0()
            .modify(|_, write| write.uart_mem_clk_en().set_bit());
        system
            .perip_rst_en0()
            .modify(|_, write| write.uart_mem_rst().clear_bit());
        let registers = if uart_instance == 1 {
            esp_hal::peripherals::UART1::regs()
        } else {
            esp_hal::peripherals::UART2::regs()
        };
        registers
            .clk_conf()
            .modify(|_, write| write.rst_core().clear_bit());
        registers.mem_conf().modify(|_, write| {
            write.mem_force_pd().clear_bit();
            write.mem_force_pu().set_bit()
        });
    }
    #[cfg(any(feature = "probe-loopback", feature = "probe-force-rx-clock"))]
    {
        let registers = if uart_instance == 1 {
            esp_hal::peripherals::UART1::regs()
        } else {
            esp_hal::peripherals::UART2::regs()
        };
        registers.id().write(|write| {
            write.high_speed().clear_bit();
            write.reg_update().set_bit()
        });
        while registers.id().read().reg_update().bit_is_set() {}
    }
    let started = Instant::now();
    let mut next_send = started + Duration::from_micros(first_send_delay_us);
    let mut next_report = started + Duration::from_secs(5);
    let mut sequence = 0_u16;
    let mut tx_frames = 0_u32;
    let mut tx_bytes = 0_u32;
    let mut rx_reads = 0_u32;
    let mut rx_bytes = 0_u32;
    let mut rx_errors = 0_u32;
    let mut tx_errors = 0_u32;
    let mut gpio_rx_edges = 0_u32;
    let mut gpio_rx_level =
        ((esp_hal::peripherals::GPIO::regs().in_().read().bits() >> rx_pin) & 1) != 0;
    let mut first_rx = false;
    let mut rx = [0_u8; 64];

    info!(
        "UCN_UART_RAW READY role={} mac={} uart={} baud={} rx=GPIO{} tx=GPIO{}",
        role, mac, uart_instance, LINK_BAUD, rx_pin, tx_pin
    );
    let matrix = esp_hal::peripherals::GPIO::regs()
        .func_in_sel_cfg(18)
        .read();
    info!(
        "UCN_UART_RAW MATRIX role={} sel={} in_sel={} inv={}",
        role,
        matrix.sel().bit_is_set(),
        matrix.in_sel().bits(),
        matrix.in_inv_sel().bit_is_set()
    );
    let diagnostic_registers = if uart_instance == 1 {
        esp_hal::peripherals::UART1::regs()
    } else {
        esp_hal::peripherals::UART2::regs()
    };
    let diagnostic_system = esp_hal::peripherals::SYSTEM::regs();
    let diagnostic_mem = diagnostic_registers.mem_conf().read();
    info!(
        "UCN_UART_RAW RX_REG role={} uart_mem_clk={} uart_mem_rst={} rst_core={} loopback={} rx_size={} tx_size={} force_pd={} force_pu={}",
        role,
        diagnostic_system
            .perip_clk_en0()
            .read()
            .uart_mem_clk_en()
            .bit_is_set(),
        diagnostic_system
            .perip_rst_en0()
            .read()
            .uart_mem_rst()
            .bit_is_set(),
        diagnostic_registers
            .clk_conf()
            .read()
            .rst_core()
            .bit_is_set(),
        diagnostic_registers.conf0().read().loopback().bit_is_set(),
        diagnostic_mem.rx_size().bits(),
        diagnostic_mem.tx_size().bits(),
        diagnostic_mem.mem_force_pd().bit_is_set(),
        diagnostic_mem.mem_force_pu().bit_is_set()
    );
    #[cfg(feature = "probe-blocking-rx")]
    if role == "B" {
        let mut blocking_rx = [0_u8; 4];
        info!("UCN_UART_RAW BLOCKING_RX_WAIT role=B");
        match uart.read(&mut blocking_rx) {
            Ok(read) => info!(
                "UCN_UART_RAW BLOCKING_RX_OK role=B count={} b0=0x{:02X}",
                read, blocking_rx[0]
            ),
            Err(problem) => error!("UCN_UART_RAW BLOCKING_RX_ERROR role=B error={problem:?}"),
        }
    }
    loop {
        let sampled_level =
            ((esp_hal::peripherals::GPIO::regs().in_().read().bits() >> rx_pin) & 1) != 0;
        if sampled_level != gpio_rx_level {
            gpio_rx_level = sampled_level;
            gpio_rx_edges = gpio_rx_edges.saturating_add(1);
        }
        while uart.read_ready() {
            match uart.read_buffered(&mut rx) {
                Ok(0) => break,
                Ok(read) => {
                    rx_reads = rx_reads.saturating_add(1);
                    rx_bytes = rx_bytes.saturating_add(read as u32);
                    if !first_rx {
                        first_rx = true;
                        info!(
                            "UCN_UART_RAW FIRST_RX role={} count={} b0=0x{:02X}",
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
            let [sequence_hi, sequence_lo] = sequence.to_be_bytes();
            let frame = [marker, sequence_hi, sequence_lo, marker ^ sequence_lo];
            if write_all(&mut uart, &frame) {
                tx_frames = tx_frames.saturating_add(1);
                tx_bytes = tx_bytes.saturating_add(frame.len() as u32);
            } else {
                tx_errors = tx_errors.saturating_add(1);
            }
            next_send += Duration::from_millis(100);
        }

        if now >= next_report {
            info!(
                "UCN_UART_RAW SUMMARY role={} tx_frames={} tx_bytes={} rx_reads={} rx_bytes={} gpio_rx_edges={} tx_errors={} rx_errors={}",
                role, tx_frames, tx_bytes, rx_reads, rx_bytes, gpio_rx_edges, tx_errors, rx_errors
            );
            next_report += Duration::from_secs(5);
        }
    }
}
