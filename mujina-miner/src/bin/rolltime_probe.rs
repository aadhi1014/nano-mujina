//! Standalone diagnostic that sweeps TOAST_ROLLTIME_ADDR register values at a
//! fixed difficulty and measures nonce yield for each value.
//!
//! TOAST_ROLLTIME_ADDR is the hardware's autonomous ntime-rolling window,
//! letting a chip keep searching between round-robin visits without needing
//! fresh work every poll cycle. `main.c`/`toast.c` compute it as
//! `TOAST_TIMEOUT_CONST/freq3`. This probe sets several raw values via the
//! IPC_MSG_SET_ROLLTIME_RAW debug command, sends work at a fixed difficulty,
//! and counts nonces returned over a fixed poll window per value.
//!
//! Run in place of mujina-minerd (same IPC port), against an
//! already-running, already-bring-up-complete rtos_core.elf.

use std::time::{Duration, Instant};

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Nano3sNonce {
    job_id: u32,
    nonce2: u32,
    nonce: u32,
    asic_id: u16,
    miner_id: u8,
    ntime: u8,
    mid_id: u8,
}

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
    fn nano3s_ipc_poll_nonce(out: *mut Nano3sNonce) -> i32;
    fn nano3s_ipc_resume() -> i32;
    fn nano3s_ipc_set_rolltime_raw(value: u32) -> i32;
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

// diff=1 target used for all candidates.
const TARGETL: u32 = 0x0000_0000;
const TARGETH: u32 = 0xffff_0000;

// BASELINE = TOAST_TIMEOUT_CONST (335_000_000) / freq3 (270) = 1_240_740.
// Candidates sweep 0.25x-4x around this value.
const BASELINE: u32 = 1_240_740;
const CANDIDATES: &[(u32, &str)] = &[
    (BASELINE / 4, "0.25x baseline"),
    (BASELINE / 2, "0.5x baseline"),
    (BASELINE, "1.0x baseline (control)"),
    (BASELINE * 2, "2.0x baseline"),
    (BASELINE * 4, "4.0x baseline"),
];

const POLL_SECS: u64 = 20;

fn main() {
    let rc = unsafe { nano3s_ipc_open() };
    if rc != 0 {
        eprintln!("[rolltime_probe] nano3s_ipc_open failed rc={rc}");
        std::process::exit(1);
    }
    eprintln!("[rolltime_probe] connected");

    let rc = unsafe { nano3s_ipc_resume() };
    eprintln!("[rolltime_probe] resume rc={rc}");
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

    for (i, &(value, label)) in CANDIDATES.iter().enumerate() {
        let job_id = 0xD0FF_EE10u32.wrapping_add(i as u32);

        let rc = unsafe { nano3s_ipc_set_rolltime_raw(value) };
        eprintln!(
            "[rolltime_probe] === candidate {label} (raw={value}) set_rolltime_raw rc={rc} ==="
        );
        // Give the worker loop a moment to apply it (next tick, well
        // under its own poll granularity) before sending work.
        std::thread::sleep(Duration::from_millis(500));

        let rc = unsafe {
            nano3s_ipc_send_job(
                job_id,
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
        if rc != 0 {
            eprintln!("[rolltime_probe] send_job failed rc={rc}, skipping this candidate");
            continue;
        }

        let deadline = Instant::now() + Duration::from_secs(POLL_SECS);
        let mut found = 0u32;
        let mut last_tick = Instant::now();
        const DETAIL_LOG_CAP: u32 = 5;
        while Instant::now() < deadline {
            let mut nonce = Nano3sNonce::default();
            let got = unsafe { nano3s_ipc_poll_nonce(&mut nonce as *mut _) };
            if got == 1 {
                found += 1;
                if found <= DETAIL_LOG_CAP {
                    println!(
                        "[rolltime_probe] NONCE candidate={label} nonce=0x{:08x} asic_id={} mid_id={}",
                        nonce.nonce, nonce.asic_id, nonce.mid_id
                    );
                } else if last_tick.elapsed() >= Duration::from_secs(1) {
                    eprintln!("[rolltime_probe] candidate {label}: {found} found so far...");
                    last_tick = Instant::now();
                }
            } else {
                std::thread::sleep(Duration::from_millis(50));
            }
        }
        let rate = found as f64 / POLL_SECS as f64;
        eprintln!(
            "[rolltime_probe] candidate {label} done: {found} nonce(s) in {POLL_SECS}s ({rate:.2}/s)"
        );
    }

    unsafe { nano3s_ipc_close() };
    eprintln!("[rolltime_probe] done, closed");
}
