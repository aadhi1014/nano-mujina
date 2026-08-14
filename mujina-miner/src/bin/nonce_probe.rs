//! Standalone diagnostic that sends fixed, fully-known synthetic jobs at a
//! sweep of difficulties directly to `rtos_core.elf` via the
//! `nano3s_ipc_shim` IPC path, and logs every returned nonce for offline
//! hash verification (`merkle_root = sha256d(coinbase)` since
//! `nmerkles=0`, computed by `rtos_core.elf` from the coinbase this
//! sends).
//!
//! Must be run in place of `mujina-minerd` (same IPC port, only one
//! process may hold it) against an `rtos_core.elf` that's already up and
//! past its own bring-up; this program does no hardware bring-up of its
//! own.

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
    fn nano3s_ipc_close();
}

// vmask table sent with every job.
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

// (TARGETL, TARGETH, label) per sweep level, difficulty halving from 256
// down to 1.
const SWEEP: &[(u32, u32, &str)] = &[
    (0x0000_0000, 0x00ff_ff00, "diff=256"),
    (0x0000_0000, 0x01ff_fe00, "diff=128"),
    (0x0000_0000, 0x03ff_fc00, "diff=64"),
    (0x0000_0000, 0x07ff_f800, "diff=32"),
    (0x0000_0000, 0x0fff_f000, "diff=16"),
    (0x0000_0000, 0x1fff_e000, "diff=8"),
    (0x0000_0000, 0x3fff_c000, "diff=4"),
    (0x0000_0000, 0x7fff_8000, "diff=2"),
    (0x0000_0000, 0xffff_0000, "diff=1"),
];

// Coarse sweep, not used by SWEEP above: 5 levels spanning a wider
// difficulty range, including a flood-level target.
#[allow(dead_code)]
const SWEEP_COARSE: &[(u32, u32, &str)] = &[
    (0xc000_0000, 0x0000_3fff, "diff=262144"),
    (0x0000_0000, 0x000f_fff0, "diff=4096"),
    (0x0000_0000, 0x03ff_fc00, "diff=64"),
    (0x0000_0000, 0xffff_0000, "diff=1"),
    (0xffff_ffff, 0xffff_ffff, "diff=maxeasy"),
];

const POLL_SECS: u64 = 30;

fn main() {
    let rc = unsafe { nano3s_ipc_open() };
    if rc != 0 {
        eprintln!("[probe] nano3s_ipc_open failed rc={rc}");
        std::process::exit(1);
    }
    eprintln!("[probe] connected");

    // Resume ensures the chain isn't held in reset before sending work.
    let rc = unsafe { nano3s_ipc_resume() };
    eprintln!("[probe] resume rc={rc}");
    std::thread::sleep(Duration::from_secs(2));

    // Fixed 64-byte coinbase (0x42 repeated). rtos_core embeds nonce2 at
    // [nonce2_offset:nonce2_offset+nonce2_size) itself before hashing.
    let coinbase = [0x42u8; 64];

    // Header buffer layout: [0:4)=version LE, [4:36)=prevhash,
    // [36:68)=merkle root (left zero, filled in by rtos_core since
    // nmerkles=0), [68:72)=ntime LE, [72:76)=bits LE. Values are fixed and
    // fully known since this is a synthetic job.
    let mut header = [0u8; 128];
    header[0..4].copy_from_slice(&1u32.to_le_bytes());
    // Sequential distinct bytes so any byte-swap/reversal applied to
    // prevhash is visible by direct comparison.
    for (i, b) in header[4..36].iter_mut().enumerate() {
        *b = i as u8;
    }
    header[68..72].copy_from_slice(&0x5f5e_1000u32.to_le_bytes());
    header[72..76].copy_from_slice(&0x1d00_ffffu32.to_le_bytes());

    for (i, &(targetl, targeth, label)) in SWEEP.iter().enumerate() {
        let job_id = 0xC0FF_EE10u32.wrapping_add(i as u32);
        let mut target = [0u8; 32];
        target[0..4].copy_from_slice(&targetl.to_le_bytes());
        target[4..8].copy_from_slice(&targeth.to_le_bytes());

        eprintln!(
            "[probe] === sweep level {label} (job_id=0x{job_id:08x} targetl=0x{targetl:08x} targeth=0x{targeth:08x}) ==="
        );

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
            eprintln!("[probe] send_job failed rc={rc}, skipping this level");
            continue;
        }

        let deadline = Instant::now() + Duration::from_secs(POLL_SECS);
        let mut found = 0u32;
        // Prints the first DETAIL_LOG_CAP nonces per level in full, then a
        // running count once per second.
        const DETAIL_LOG_CAP: u32 = 20;
        let mut last_tick = Instant::now();
        while Instant::now() < deadline {
            let mut nonce = Nano3sNonce::default();
            let got = unsafe { nano3s_ipc_poll_nonce(&mut nonce as *mut _) };
            if got == 1 {
                found += 1;
                if found <= DETAIL_LOG_CAP {
                    println!(
                        "[probe] NONCE level={label} job_id=0x{:08x} nonce2=0x{:08x} nonce=0x{:08x} asic_id={} miner_id={} mid_id={} chip_ntime=0x{:02x}",
                        nonce.job_id, nonce.nonce2, nonce.nonce, nonce.asic_id, nonce.miner_id, nonce.mid_id, nonce.ntime
                    );
                } else if last_tick.elapsed() >= Duration::from_secs(1) {
                    eprintln!("[probe] level {label}: {found} found so far...");
                    last_tick = Instant::now();
                }
            } else {
                std::thread::sleep(Duration::from_millis(50));
            }
        }
        eprintln!("[probe] level {label} done: {found} nonce(s) found in {POLL_SECS}s");
    }

    unsafe { nano3s_ipc_close() };
    eprintln!("[probe] done, closed");
}
