//! Standalone diagnostic that pairs a voltage and frequency preset from
//! /data/factory/hashrate_cali.ini's cali_param0 fields and applies both
//! together, then reads SmartSpeed fail rate (spd_dh) from
//! /sharefs/ipc6.log.
//!
//! Voltage is set via IPC_MSG_SET_VOLTAGE_RAW
//! (nano3s_ipc_set_voltage_raw()) before the matching SET_MODE frequency
//! call. In cali_param0, field 4 is the base PLL frequency and field 5 is
//! p_int (per-domain step), so a preset's full pll_freq is
//! [base, base+step, base+2*step, base+3*step].

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

// Voltage/frequency candidates to test: (voltage_mv, pll_freq, label).
const CANDIDATES: &[(i32, [u32; 4], &str)] = &[
    (3704, [338, 358, 378, 398], "MED freq + 3704mV (mode2 calibrated voltage)"),
];

const IPC6_LOG: &str = "/sharefs/ipc6.log";
const VOLT_SETTLE_SECS: u64 = 3;
const FREQ_SETTLE_SECS: u64 = 4;
const MAX_WAIT_SECS: u64 = 300;
const POLL_INTERVAL_MS: u64 = 2000;

fn read_last_status(contents: &str) -> Option<(u64, u64, f64)> {
    let lines: Vec<&str> = contents.lines().collect();
    for i in (0..lines.len()).rev() {
        if let Some(rest) = lines[i].strip_prefix("STATUS sent ghsmm=") {
            let ghsmm: u64 = rest.split_whitespace().next()?.parse().ok()?;
            if let Some(next) = lines.get(i + 1) {
                if let Some(rest2) = next.strip_prefix("STATUS ghsspd=") {
                    let ghsspd: u64 = rest2.split_whitespace().next()?.parse().ok()?;
                    let dh_str = rest2.split("spd_dh=").nth(1)?;
                    let dh_num = dh_str.trim_start_matches('%').split('%').next()?;
                    let spd_dh: f64 = dh_num.parse().ok()?;
                    return Some((ghsmm, ghsspd, spd_dh));
                }
            }
        }
    }
    None
}

fn main() {
    let rc = unsafe { nano3s_ipc_open() };
    if rc != 0 {
        eprintln!("[mode_probe] nano3s_ipc_open failed rc={rc}");
        std::process::exit(1);
    }
    eprintln!("[mode_probe] connected");

    let rc = unsafe { nano3s_ipc_resume() };
    eprintln!("[mode_probe] resume rc={rc}");
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

    let rc = unsafe {
        nano3s_ipc_send_job(
            0xF0FF_EE10,
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
    eprintln!("[mode_probe] initial send_job rc={rc}");
    std::thread::sleep(Duration::from_secs(2));

    let mut last_seen: Option<(u64, u64, f64)> = None;

    for (mv, freq, label) in CANDIDATES {
        eprintln!("[mode_probe] === candidate {label} voltage={mv}mV pll_freq={freq:?} ===");

        // Voltage is set before frequency.
        let rc = unsafe { nano3s_ipc_set_voltage_raw(*mv) };
        eprintln!("[mode_probe] set_voltage_raw({mv}) rc={rc}, settling {VOLT_SETTLE_SECS}s");
        std::thread::sleep(Duration::from_secs(VOLT_SETTLE_SECS));

        let rc = unsafe { nano3s_ipc_set_mode(freq.as_ptr(), 0) };
        eprintln!("[mode_probe] set_mode rc={rc}, settling {FREQ_SETTLE_SECS}s");
        std::thread::sleep(Duration::from_secs(FREQ_SETTLE_SECS));

        let deadline = Instant::now() + Duration::from_secs(MAX_WAIT_SECS);
        let mut prev_poll: Option<(u64, u64, f64)> = None;
        let mut confirmed: Option<(u64, u64, f64)> = None;

        while Instant::now() < deadline {
            match fs::read_to_string(IPC6_LOG) {
                Ok(contents) => {
                    if let Some(reading) = read_last_status(&contents) {
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
                Err(e) => eprintln!("[mode_probe] read {IPC6_LOG} failed: {e}"),
            }
            std::thread::sleep(Duration::from_millis(POLL_INTERVAL_MS));
        }

        match confirmed {
            Some((ghsmm, ghsspd, spd_dh)) => {
                let eff = if ghsmm > 0 {
                    100.0 * ghsspd as f64 / ghsmm as f64
                } else {
                    0.0
                };
                eprintln!(
                    "[mode_probe] candidate {label} CONFIRMED: ghsmm={ghsmm} ghsspd={ghsspd} spd_dh={spd_dh:.4}% efficiency={eff:.2}%"
                );
                last_seen = Some((ghsmm, ghsspd, spd_dh));
            }
            None => {
                eprintln!(
                    "[mode_probe] candidate {label} TIMED OUT after {MAX_WAIT_SECS}s -- no fresh stable reading"
                );
            }
        }
    }

    unsafe { nano3s_ipc_close() };
    eprintln!("[mode_probe] done, closed");
}
