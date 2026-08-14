//! Standalone diagnostic that holds PLL frequency fixed at the production
//! baseline (210,230,250,270MHz) and sweeps core voltage via
//! nano3s_ipc_set_voltage_raw(), reading SmartSpeed fail rate (spd_dh)
//! from /sharefs/ipc6.log for each candidate. Resets the SmartSpeed
//! counter (IPC_MSG_RESET_SPDLOG_RAW) before each candidate and aborts
//! the sweep, reverting voltage to 3392mV, if temp_max exceeds
//! TEMP_MAX_ABORT_C.

use std::fs;
use std::time::{Duration, Instant};

#[link(name = "nano3s_ipc_shim", kind = "static")]
#[link(name = "ipcmsg", kind = "static")]
unsafe extern "C" {
    fn nano3s_ipc_open() -> i32;
    #[allow(clippy::too_many_arguments)]
    fn nano3s_ipc_send_job(
        job_id: u32,
        nonce2_start: u32,
        nonce2_offset: i32,
        nonce2_size: i32,
        coinbase: *const u8,
        coinbase_len: u32,
        merkle_offset: i32,
        merkles: *const u8,
        nmerkles: i32,
        header: *const u8,
        target: *const u8,
        work_restart: u8,
        vmask: *const u32,
    ) -> i32;
    fn nano3s_ipc_resume() -> i32;
    fn nano3s_ipc_set_mode(pll_freq: *const u32, work_mode: u8) -> i32;
    fn nano3s_ipc_set_voltage_raw(target_mv: i32) -> i32;
    fn nano3s_ipc_reset_spdlog_raw() -> i32;
    fn nano3s_ipc_close();
}

const VMASK: [u32; 8] = [
    0x0000_0020,
    0x00E0_FF3F,
    0x0080_0020,
    0x0000_0120,
    0x0000_0220,
    0x0000_0420,
    0x0000_0820,
    0x0000_1020,
];

const TARGETL: u32 = 0x0000_0000;
const TARGETH: u32 = 0xffff_0000; // diff=1

// Frequency held fixed at production baseline throughout -- only
// voltage varies.
const FREQ: [u32; 4] = [210, 230, 250, 270];

// Voltage candidates to test at the fixed frequency, in mV, with the
// SmartSpeed counter reset before each.
const CANDIDATES: &[(i32, &str)] = &[
    (3496, "control"),
    (3550, "+158mV"),
    (3600, "+208mV"),
    (3650, "+258mV"),
    (3704, "+312mV (ceiling)"),
];

const IPC6_LOG: &str = "/sharefs/ipc6.log";
const VOLT_SETTLE_SECS: u64 = 4;
// Bounded wait per candidate for a fresh, stable reading after a
// SmartSpeed counter reset.
const MAX_WAIT_SECS: u64 = 300;
const POLL_INTERVAL_MS: u64 = 2000;

// Sweep aborts and reverts voltage to 3392mV if temp_max exceeds this.
const TEMP_MAX_ABORT_C: f64 = 75.0;

fn read_last_status(contents: &str) -> Option<(u64, u64, f64, f64, f64)> {
    let lines: Vec<&str> = contents.lines().collect();
    for i in (0..lines.len()).rev() {
        if let Some(rest) = lines[i].strip_prefix("STATUS sent ghsmm=") {
            let mut it = rest.split_whitespace();
            let ghsmm: u64 = it.next()?.parse().ok()?;
            let temp_avg: f64 = it.next()?.strip_prefix("temp_avg=")?.parse().ok()?;
            let temp_max: f64 = it.next()?.strip_prefix("temp_max=")?.parse().ok()?;
            if let Some(next) = lines.get(i + 1) {
                if let Some(rest2) = next.strip_prefix("STATUS ghsspd=") {
                    let ghsspd: u64 = rest2.split_whitespace().next()?.parse().ok()?;
                    let dh_str = rest2.split("spd_dh=").nth(1)?;
                    let dh_num = dh_str.trim_start_matches('%').split('%').next()?;
                    let spd_dh: f64 = dh_num.parse().ok()?;
                    return Some((ghsmm, ghsspd, spd_dh, temp_avg, temp_max));
                }
            }
        }
    }
    None
}

fn main() {
    let rc = unsafe { nano3s_ipc_open() };
    if rc != 0 {
        eprintln!("[volt_at_freq_probe] nano3s_ipc_open failed rc={rc}");
        std::process::exit(1);
    }
    eprintln!("[volt_at_freq_probe] connected");

    let rc = unsafe { nano3s_ipc_resume() };
    eprintln!("[volt_at_freq_probe] resume rc={rc}");
    std::thread::sleep(Duration::from_secs(2));

    let coinbase = [0x42u8; 64];
    let mut header = [0u8; 128];
    header[0..4].copy_from_slice(&1u32.to_le_bytes());
    for (i, b) in header[4..36].iter_mut().enumerate() {
        *b = i as u8;
    }
    header[68..72].copy_from_slice(&0x5f5e_1000u32.to_le_bytes());
    header[72..76].copy_from_slice(&0x1d00_ffffu32.to_le_bytes());

    let mut target = [0u8; 32];
    target[0..4].copy_from_slice(&TARGETL.to_le_bytes());
    target[4..8].copy_from_slice(&TARGETH.to_le_bytes());

    // Set the fixed frequency once, up front -- only voltage changes per
    // candidate from here on.
    let rc = unsafe { nano3s_ipc_set_mode(FREQ.as_ptr(), 0) };
    eprintln!("[volt_at_freq_probe] initial set_mode({FREQ:?}) rc={rc}");
    std::thread::sleep(Duration::from_secs(3));

    let rc = unsafe {
        nano3s_ipc_send_job(
            0xA0FF_EE10,
            0,
            4,
            4,
            coinbase.as_ptr(),
            coinbase.len() as u32,
            36,
            std::ptr::null(),
            0,
            header.as_ptr(),
            target.as_ptr(),
            1,
            VMASK.as_ptr(),
        )
    };
    eprintln!("[volt_at_freq_probe] initial send_job rc={rc}");
    std::thread::sleep(Duration::from_secs(2));

    let mut last_seen: Option<(u64, u64, f64, f64, f64)> = None;

    for (mv, label) in CANDIDATES {
        eprintln!("[volt_at_freq_probe] === candidate {label} voltage={mv}mV (freq fixed at {FREQ:?}) ===");
        let rc = unsafe { nano3s_ipc_set_voltage_raw(*mv) };
        eprintln!("[volt_at_freq_probe] set_voltage_raw({mv}) rc={rc}, settling {VOLT_SETTLE_SECS}s");
        std::thread::sleep(Duration::from_secs(VOLT_SETTLE_SECS));

        let rc = unsafe { nano3s_ipc_reset_spdlog_raw() };
        eprintln!("[volt_at_freq_probe] reset_spdlog_raw() rc={rc} -- forcing fresh SmartSpeed window");
        std::thread::sleep(Duration::from_secs(2));

        let deadline = Instant::now() + Duration::from_secs(MAX_WAIT_SECS);
        let mut prev_poll: Option<(u64, u64, f64, f64, f64)> = None;
        let mut confirmed: Option<(u64, u64, f64, f64, f64)> = None;
        let mut aborted = false;

        while Instant::now() < deadline {
            match fs::read_to_string(IPC6_LOG) {
                Ok(contents) => {
                    if let Some(reading) = read_last_status(&contents) {
                        if reading.4 > TEMP_MAX_ABORT_C {
                            eprintln!(
                                "[volt_at_freq_probe] SAFETY ABORT: temp_max={:.1}C exceeds {:.1}C ceiling at candidate {label} -- reverting voltage to 3392mV and stopping sweep",
                                reading.4, TEMP_MAX_ABORT_C
                            );
                            let _ = unsafe { nano3s_ipc_set_voltage_raw(3392) };
                            aborted = true;
                            break;
                        }
                        let is_fresh = last_seen.map(|l| l != reading).unwrap_or(true);
                        let is_nonzero = reading.0 > 0;
                        let is_stable = prev_poll == Some(reading);
                        if is_fresh && is_nonzero && is_stable {
                            confirmed = Some(reading);
                            break;
                        }
                        prev_poll = Some(reading);
                    }
                }
                Err(e) => eprintln!("[volt_at_freq_probe] read {IPC6_LOG} failed: {e}"),
            }
            std::thread::sleep(Duration::from_millis(POLL_INTERVAL_MS));
        }

        if aborted {
            break;
        }

        match confirmed {
            Some((ghsmm, ghsspd, spd_dh, temp_avg, temp_max)) => {
                let eff = if ghsmm > 0 {
                    100.0 * ghsspd as f64 / ghsmm as f64
                } else {
                    0.0
                };
                eprintln!(
                    "[volt_at_freq_probe] candidate {label} CONFIRMED: ghsmm={ghsmm} ghsspd={ghsspd} spd_dh={spd_dh:.4}% efficiency={eff:.2}% temp_avg={temp_avg:.1}C temp_max={temp_max:.1}C"
                );
                last_seen = Some((ghsmm, ghsspd, spd_dh, temp_avg, temp_max));
            }
            None => {
                eprintln!(
                    "[volt_at_freq_probe] candidate {label} TIMED OUT after {MAX_WAIT_SECS}s -- no fresh stable reading"
                );
            }
        }
    }

    unsafe { nano3s_ipc_close() };
    eprintln!("[volt_at_freq_probe] done, closed");
}
