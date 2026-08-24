//! Avalon Nano3s board driver.
//!
//! Bridges the mujina-miner scheduler/stratum stack to `rtos_core.elf`
//! (the RTOS/big-core side that owns UART3 to the A3197S chain) over the
//! IPCM protocol, via a C shim (`rtos_core/tools/nano3s_ipc_shim.c`,
//! linked in by `build.rs` when the `nano3s` feature is enabled).
//!
//! Modeled as a SINGLE [`HashThread`] for the whole 12-chip chain, not
//! twelve -- there is one UART and one IPC channel, and `rtos_core.elf`
//! does its own per-chip addressing internally (`toast_chip_select`).
//!
//! # Known limitation: extranonce2 width
//!
//! The pool advertises `extranonce2_size=8`; the A3197S chip has exactly
//! one 32-bit `nonce2` slot (`struct ipc_job.nonce2_start`, a `uint32_t`).
//! The lower 32 bits of the scheduler's 8-byte extranonce2 go into the
//! chip's nonce2 register; the fixed upper 32 bits (`en2_high`) are
//! spliced into the coinbase right after it, so the coinbase length and
//! merkle root match what the pool expects.
//!
//! # Byte-order notes
//!
//! - `prev_blockhash` goes into the header buffer via
//!   `BlockHash::to_byte_array()` unchanged -- no word-reversal, no
//!   byte-swap.
//! - Merkle branches (`MerkleRootTemplate::merkle_branches`) are used
//!   raw, no reversal, matching
//!   `MerkleRootTemplate::compute_merkle_root()`'s climb algorithm.
//! - The 4-byte nonce2 is written into the coinbase in little-endian byte
//!   order (`nonce2.to_le_bytes()`), matching `miner_gen_nonce2_work()`'s
//!   raw `memcpy` of nonce2's native in-memory bytes.
//! - version/ntime/nbits are stored as plain native (little-endian on
//!   this riscv64 target) `u32` words, no swap.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{Result, anyhow};
use async_trait::async_trait;
use bitcoin::hashes::Hash;
use bitcoin::hashes::sha256d;
use tokio::sync::{mpsc, watch};

use super::{BackplaneConnector, BoardInfo, VirtualBoardDescriptor};
use crate::api_client::types::{
    BoardTelemetry, Fan, PowerMeasurement, TemperatureSensor, ThreadTelemetry,
};
use crate::asic::hash_thread::{
    HashTask, HashThread, HashThreadCapabilities, HashThreadEvent, HashThreadStatus, Share,
};
use crate::job_source::{Extranonce2, GeneralPurposeBits, MerkleRootKind, MerkleRootTemplate};
use crate::types::{Difficulty, HashRate, Temperature};

// ---------------------------------------------------------------------------
// FFI: rtos_core/tools/nano3s_ipc_shim.{h,c}
// ---------------------------------------------------------------------------

/// Mirrors `nano3s_nonce_t` (nano3s_ipc_shim.h) field-for-field.
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

const NANO3S_STATUS_MAX_CHIPS: usize = 12;

/// Mirrors `nano3s_chip_status_t` (nano3s_ipc_shim.h) field-for-field.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Nano3sChipStatus {
    temp_c: f32,
    volt_mv: f32,
    pll_cnt: [u8; 4],
    pll_freq: [u16; 4],
    nonce_timeout: u32,
    nonce_heartbeat: u32,
    nonce_data: u32,
    ghsspd: f32,
    spd_dh: f32,
}

/// Mirrors `nano3s_status_t` (nano3s_ipc_shim.h) field-for-field. Carries a
/// per-chip breakdown plus measured hashrate/fail-rate and chain-wide
/// error counters.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Nano3sStatus {
    asics_total: u32,
    ghsmm: u32,
    ghsspd: u32,
    spd_dh: f32,
    temp_avg: f32,
    temp_max: f32,
    pll_freq: [u32; 4],
    err_crc: u32,
    voltage_mv: u32,
    paused: u8,
    nonce_read_err: u32,
    nonce_bad_len: u32,
    nonce_overflow: u32,
    regread_err: u32,
    chip_count: u8,
    chips: [Nano3sChipStatus; NANO3S_STATUS_MAX_CHIPS],
}

/// Per-chip detail for the dashboard's Info page -- JSON-friendly
/// reshaping of `Nano3sChipStatus` (wire types stay as-is, no unit
/// conversion here; the page does that).
#[derive(Clone, Debug, Default, serde::Serialize)]
pub(crate) struct Nano3sChipDetail {
    pub chip: u8,
    pub temp_c: f32,
    pub volt_mv: f32,
    pub pll_cnt: [u8; 4],
    pub pll_freq: [u16; 4],
    pub nonce_timeout: u32,
    pub nonce_heartbeat: u32,
    pub nonce_data: u32,
    pub ghsspd: f32,
    pub spd_dh: f32,
}

/// Full chain + per-chip detail snapshot -- everything the dashboard's
/// Info page shows, sourced from the `IPC_MSG_STATUS` wire struct plus
/// the INA226 power read and pool accept/reject counters. Built once per
/// status poll (~200ms) in `run_worker()`, stored in `NANO3S_DETAIL`,
/// read by `dashboard.rs`'s `/nano3s-detail` route. Kept separate from
/// the generic multi-board `BoardTelemetry` (api_client/types.rs), which
/// has no per-chip fields.
#[derive(Clone, Debug, Default, serde::Serialize)]
pub(crate) struct Nano3sDetail {
    pub ipc_connected: bool,
    pub paused: bool,
    pub asics_total: u32,
    /// Theoretical nameplate hashrate (PLL config * core count), GH/s.
    pub ghsmm: u32,
    /// Measured hashrate (SmartSpeed pass-count based), GH/s.
    pub ghsspd: u32,
    /// Internal SmartSpeed fail rate, percent.
    pub spd_dh: f32,
    pub temp_avg: f32,
    pub temp_max: f32,
    /// Last commanded (not read back) PLL frequency per domain, MHz.
    pub pll_freq_commanded: [u32; 4],
    /// Last commanded (not read back) core voltage, mV.
    pub voltage_mv_commanded: u32,
    pub err_crc: u32,
    pub nonce_read_err: u32,
    pub nonce_bad_len: u32,
    pub nonce_overflow: u32,
    /// Chain-wide only; no per-chip breakdown exists.
    pub regread_err: u32,
    pub chips: Vec<Nano3sChipDetail>,
    /// USB-C PD input rail (INA226), not the ASIC core rail. ~27V typical.
    pub ina_bus_v: f64,
    pub ina_current_a: f64,
    pub ina_power_w: f64,
    pub shares_found: u32,
    pub shares_accepted: u64,
    pub shares_rejected: u64,
    pub difficulty: f64,
    pub job_id: Option<u32>,
    /// Highest difficulty achieved by any nonce this process has seen
    /// (session-scoped, resets on restart), regardless of whether it
    /// cleared the pool's share target. See [`BEST_SHARE_DIFF`].
    pub best_share_diff: f64,
}

static NANO3S_DETAIL: std::sync::Mutex<Option<Nano3sDetail>> = std::sync::Mutex::new(None);

/// Running max of every nonce's achieved difficulty, updated in
/// `verify_and_build_share()`. Same overflow guard as the hashrate
/// estimator (`NANO3S_MAX_SANE_DIFFICULTY`) -- a corrupted/garbled nonce
/// can otherwise report a spurious near-infinite difficulty that would
/// dominate this forever.
static BEST_SHARE_DIFF: std::sync::Mutex<f64> = std::sync::Mutex::new(0.0);

/// Latest full detail snapshot, or `None` if no STATUS has arrived yet
/// (e.g. IPC not connected). Called from `dashboard.rs`'s `/nano3s-detail`
/// route.
pub(crate) fn get_detail_snapshot() -> Option<Nano3sDetail> {
    NANO3S_DETAIL
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone()
}

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
    fn nano3s_ipc_get_status(out: *mut Nano3sStatus) -> i32;
    fn nano3s_ipc_pause() -> i32;
    fn nano3s_ipc_resume() -> i32;
    fn nano3s_ipc_set_mode(pll_freq: *const u32, work_mode: u8) -> i32;
    /// Triggers rtos_core's IPC_MSG_SET_VOLTAGE round trip at runtime with
    /// an externally-supplied target. Used by the dashboard/API tuning
    /// endpoint (`write_tuning_command()` below) to change core voltage
    /// live, no reboot required.
    fn nano3s_ipc_set_voltage_raw(target_mv: i32) -> i32;
    fn nano3s_ipc_close();
}

/// PLL ramp target sent once at board startup via IPC_MSG_SET_MODE.
/// `[0]` is the ramp target rtos_core ramps toward; the rest step by
/// `[1]-[0]` per domain (see main.c's stage6_worker()).
///
/// Values are the factory calibration for work_mode=0 (LOW), from
/// `/data/factory/hashrate_cali.ini` (`[mode0] cali_param0 =
/// 62-80-3392-210-20`), parsed per `cali_param_t`:
/// `max_pout(W)-temp(C)-volt(mV)-pll_start(MHz)-pll_interval(MHz)`.
/// `pll_start`=210MHz, `pll_interval`=20MHz -> ramp target
/// [210,230,250,270], kept under a 280MHz safety ceiling.
const NANO3S_PLL_FREQ_TARGET: [u32; 4] = [210, 230, 250, 270];
/// Placeholder pre-STATUS hashrate estimate, used only until real STATUS
/// telemetry (cal_ghsmm() in rtos_core/src/toast.c) arrives.
const NANO3S_EXPECTED_HASHRATE_GHS: f64 = 100.0;

/// Ceiling on a single nonce's recorded difficulty for the fleet-level
/// hashrate estimator. `verify_and_build_share()` records every reported
/// nonce's achieved difficulty; `Difficulty::from_hash()` returns
/// `Difficulty::MAX` if the computed hash is exactly zero (e.g. from a
/// corrupted/garbled nonce), which would otherwise dominate the
/// estimator's sliding window. Values above this ceiling are clamped
/// before being fed into the estimator.
const NANO3S_MAX_SANE_DIFFICULTY: u64 = 100_000_000_000;

/// Power-target voltage loop: given a target power in watts
/// (`MUJINA_NANO3S_POWER_TARGET_W`, unset = feature off), steps core
/// voltage up when measured power is below target and down when above,
/// based on `ina_power_w` feedback (see `read_power_estimate()`), via the
/// same live-tuning IPC path the dashboard/API use.
///
/// Step size matches the DC/DC controller's voltage quantization
/// (`mV = 3600 +/- magnitude*26`), so every step lands on a value the
/// hardware can represent exactly.
const POWER_TARGET_STEP_MV: i32 = 26;
/// Dead-band around the target -- avoids stepping on every check for
/// sample-to-sample noise in `ina_power_w`.
const POWER_TARGET_DEADBAND_W: f64 = 5.0;
/// Allowed voltage range for MED's frequency table (338-398MHz). Steps
/// never go outside this range regardless of how far off-target power
/// reads.
const POWER_TARGET_MIN_MV: i32 = 3496;
const POWER_TARGET_MAX_MV: i32 = 3800;
/// Voltage value to avoid landing on exactly (causes core-domain
/// migration collapse). A step that would land here takes one further
/// step in the same direction instead.
const POWER_TARGET_AVOID_MV: i32 = 3600;
/// Hard safety trip, independent of the configured target: step down
/// immediately, ignoring hysteresis/interval, if power ever reads above
/// this, short of the device's 133W hard ceiling.
const POWER_TARGET_SAFETY_W: f64 = 120.0;
/// How often this loop is allowed to change voltage -- gated past the
/// SmartSpeed accumulation window (~131.1s, see toast.c's
/// read_asic_spdlog()) so each step gets a chance to settle before being
/// judged. The condition is still evaluated on every ~15s STATUS refresh,
/// just not acted on that often.
const POWER_TARGET_CHECK_INTERVAL: Duration = Duration::from_secs(150);

/// Builds the 8 AsicBoost version-rolling candidates for a job's mid_id
/// slots. `set_vmask()`-equivalent: candidate list is
/// `[0, full_mask, individual_bit_15, individual_bit_16, ...,
/// individual_bit_28]` (bits 13-14 of the mask never get their own slot;
/// the loop starts at bit 15), each OR'd with the job's base version.
/// Callers bswap the result before sending it over the wire
/// (`get_vmask()`-equivalent).
fn nano3s_vmask_candidates(base: bitcoin::block::Version, mask: u32) -> [u32; 8] {
    let mut candidates = [0u32; 8];
    let mut idx = 0usize;
    candidates[idx] = 0;
    idx += 1;
    if idx < 8 {
        candidates[idx] = mask;
        idx += 1;
    }
    for bit in 15..=28u32 {
        if idx >= 8 {
            break;
        }
        if (mask >> bit) & 1 == 1 {
            candidates[idx] = 1u32 << bit;
            idx += 1;
        }
    }
    let base_u32 = base.to_consensus() as u32;
    candidates.map(|c| base_u32 | c)
}

/// Recovers the pool's raw 32-bit version-rolling mask (e.g. 0x1fffe000)
/// from `VersionTemplate::gp_bits_mask()`'s compact 16-bit form (bits
/// 13-28), for use with `nano3s_vmask_candidates()`, which needs the real
/// mask value, not a pre-extracted bit pattern.
fn gp_bits_mask_to_raw(gp_mask: GeneralPurposeBits) -> u32 {
    (u16::from_be_bytes(*gp_mask.as_bytes()) as u32) << 13
}

/// Reverses the byte order of each 4-byte word in `bytes` in place
/// position -- i.e. `[a,b,c,d, e,f,g,h, ...]` becomes
/// `[d,c,b,a, h,g,f,e, ...]`, word positions unchanged. `bytes.len()`
/// must be a multiple of 4.
fn word_bswap32(bytes: &[u8]) -> Vec<u8> {
    bytes.chunks_exact(4).flat_map(|w| w.iter().rev().copied()).collect()
}

/// Header layout offset for the merkle root field, matching the wire
/// contract `rtos_core.elf`'s `miner_gen_nonce2_work()` parses.
const HEADER_MERKLE_OFFSET: i32 = 36;
/// Nonce2 field width -- the A3197S's single 32-bit nonce2 register.
const NONCE2_SIZE: i32 = 4;

// ---------------------------------------------------------------------------
// Board registration
// ---------------------------------------------------------------------------

inventory::submit! {
    VirtualBoardDescriptor {
        device_type: "nano3s",
        name: "Avalon Nano3s",
        create_fn: || Box::pin(create_nano3s_board()),
    }
}

fn env_enabled() -> bool {
    std::env::var("MUJINA_NANO3S_ENABLE")
        .map(|v| v == "1")
        .unwrap_or(false)
}

async fn create_nano3s_board() -> Result<BackplaneConnector> {
    if !env_enabled() {
        anyhow::bail!("nano3s board not configured (MUJINA_NANO3S_ENABLE not set to 1)");
    }

    let info = BoardInfo {
        model: "Avalon Nano3s (A3197S x12)".into(),
        firmware_version: None,
        serial_number: Some("nano3s-0".into()),
    };

    let initial_state = BoardTelemetry {
        name: info.serial_number.clone().unwrap(),
        model: info.model.clone(),
        serial: info.serial_number.clone(),
        ..Default::default()
    };
    // telemetry_tx is moved into the worker thread (`run_worker`), which
    // sends an update on every fresh IPC_MSG_STATUS.
    let (telemetry_tx, telemetry_rx) = watch::channel(initial_state);

    let thread = Nano3sHashThread::new(telemetry_tx)?;

    Ok(BackplaneConnector {
        info,
        threads: vec![Box::new(thread)],
        telemetry_rx,
        shutdown: None,
    })
}

// ---------------------------------------------------------------------------
// Job bookkeeping (needed to verify/reconstruct a Share once a nonce comes
// back -- rtos_core.elf only echoes job_id/nonce2/nonce, not the full job).
// ---------------------------------------------------------------------------

struct JobContext {
    version_base: bitcoin::block::Version,
    prev_blockhash: bitcoin::BlockHash,
    bits: bitcoin::pow::CompactTarget,
    /// Raw coinbase parts, kept so a per-nonce2 coinbase can be rebuilt
    /// exactly as `rtos_core.elf`'s `miner_gen_nonce2_work()` builds it.
    coinbase1: Vec<u8>,
    coinbase2: Vec<u8>,
    extranonce1: Vec<u8>,
    /// Fixed upper 32 bits of the pool's 8-byte extranonce2 space for
    /// this job. The chip only varies the lower 32 bits (its one 32-bit
    /// nonce2 register); this fixed upper half is spliced into the
    /// coinbase right after it so the full 8-byte field, coinbase length,
    /// and merkle root match what the pool expects.
    en2_high: u32,
    merkle_branches: Vec<bitcoin::TxMerkleNode>,
    ntime: u32,
    share_target: bitcoin::pow::Target,
    share_tx: mpsc::Sender<Share>,
    /// Pre-bswap `base_version | candidate[mid_id]` values, one per
    /// AsicBoost mid_id slot -- see `nano3s_vmask_candidates()`.
    /// `send_task_to_chain()` bswaps these before wire transmission;
    /// `verify_and_build_share()` uses them as stored here.
    vmask_candidates: [u32; 8],
}

/// Rebuilds the coinbase for a specific 4-byte nonce2 (little-endian) and
/// climbs the Merkle branches to produce the root, matching
/// `rtos_core.elf`'s `miner_gen_nonce2_work()`. Hashes the coinbase as an
/// opaque byte string (not via `bitcoin`'s `Transaction` deserializer):
/// the coinbase is 4 bytes shorter than the script-length prefix baked
/// into `coinbase1` declares, so it is not a well-formed, parseable
/// transaction, but `sha256d` doesn't care.
fn compute_merkle_root_for_nonce2(ctx: &JobContext, nonce2: u32) -> bitcoin::TxMerkleNode {
    let mut coinbase = Vec::with_capacity(
        ctx.coinbase1.len() + ctx.extranonce1.len() + NONCE2_SIZE as usize + 4 + ctx.coinbase2.len(),
    );
    coinbase.extend_from_slice(&ctx.coinbase1);
    coinbase.extend_from_slice(&ctx.extranonce1);
    coinbase.extend_from_slice(&nonce2.to_le_bytes());
    coinbase.extend_from_slice(&ctx.en2_high.to_le_bytes());
    coinbase.extend_from_slice(&ctx.coinbase2);

    let mut current = sha256d::Hash::hash(&coinbase).to_byte_array();
    for branch in &ctx.merkle_branches {
        let mut combined = [0u8; 64];
        combined[..32].copy_from_slice(&current);
        combined[32..].copy_from_slice(branch.as_byte_array());
        current = sha256d::Hash::hash(&combined).to_byte_array();
    }
    bitcoin::TxMerkleNode::from_byte_array(current)
}

/// Rebuilds a share's header hash from a reported nonce and mid_id, and
/// returns `(share, local_verify_passed)`. The `Share` is always built
/// and submitted regardless of `local_verify_passed`, which is returned
/// for diagnostic logging only.
///
/// `mid_id` (mirrors `Nano3sNonce.mid_id` / `struct toast_nonce.mid_id`)
/// selects which of the 8 AsicBoost vmask candidates the chip used for
/// this nonce; the header's version field is set from
/// `ctx.vmask_candidates[mid_id]`, falling back to `ctx.version_base` for
/// an out-of-range index. `ctx.ntime` is used directly, with no
/// carry-reconstruction (ntime-rolling is disabled on this chip).
///
/// The 80-byte header message is hashed with the following byte layout:
/// version and nonce in natural big-endian byte form (no transform);
/// prev_blockhash, ntime, and bits each with their 32-bit words
/// byte-reversed in place (word positions unchanged); merkle_root used
/// raw, exactly as `compute_merkle_root_for_nonce2()` returns it.
/// `bitcoin::block::Header::block_hash()` cannot express this byte
/// layout (it always uses standard consensus serialization), so `nonce`
/// is byte-swapped before being handed to `Header`, which then produces
/// the correct hash via its normal little-endian consensus encoding.
fn verify_and_build_share(ctx: &JobContext, nonce2: u32, nonce: u32, mid_id: u8) -> (Share, bool) {
    let merkle_root = compute_merkle_root_for_nonce2(ctx, nonce2);
    // Select this mid_id slot's vmask candidate as the version field.
    let version_word = ctx
        .vmask_candidates
        .get(mid_id as usize)
        .copied()
        .unwrap_or(ctx.version_base.to_consensus() as u32);
    let version = bitcoin::block::Version::from_consensus(version_word as i32);

    // Standard consensus serialization is the canonical hash once
    // prevhash/ntime/bits/version are each in their plain,
    // untransformed form. `nonce` is byte-swapped here so that
    // `Header`'s little-endian consensus encoding reproduces the
    // chip's natural big-endian nonce byte order.
    // `share.nonce` below stays unswapped -- `SubmitParams::to_stratum_json()`
    // formats it via `format!("{:08x}", nonce)`, which already emits the
    // nonce's natural byte order for the raw value.
    let std_header = bitcoin::block::Header {
        version,
        prev_blockhash: ctx.prev_blockhash,
        merkle_root,
        time: ctx.ntime,
        bits: ctx.bits,
        nonce: nonce.swap_bytes(),
    };
    let hash = std_header.block_hash();
    let diff = Difficulty::from_hash(&hash);
    // See NANO3S_MAX_SANE_DIFFICULTY's doc comment. Only the value fed
    // into the hashrate estimator below is capped -- `hash`/`nonce` used
    // for pool submission and local verification are untouched.
    let capped_diff = if diff.as_u64() > NANO3S_MAX_SANE_DIFFICULTY {
        Difficulty::from_f64(NANO3S_MAX_SANE_DIFFICULTY as f64)
    } else {
        diff
    };

    {
        let mut best = BEST_SHARE_DIFF.lock().unwrap_or_else(|e| e.into_inner());
        let d = capped_diff.as_f64();
        if d > *best {
            *best = d;
        }
    }

    // Local pre-verification against ctx.share_target does not gate
    // submission from this function; scheduler.rs's own gate re-checks
    // `hash` independently before submitting.
    let local_verify_passed = ctx.share_target.is_met_by(hash);
    let share = Share {
        nonce,
        hash,
        version,
        ntime: ctx.ntime,
        // Full 8-byte value (fixed en2_high | chip-variable nonce2), not
        // just NONCE2_SIZE=4 bytes -- must match `state.extranonce2_size`
        // (8) exactly.
        extranonce2: Extranonce2::new(((ctx.en2_high as u64) << 32) | (nonce2 as u64), 8).ok(),
        // Uses this nonce's own achieved difficulty (`diff`, computed
        // above) rather than `ctx.share_target`, since every reported
        // nonce is recorded (not just accepted shares) -- an unbiased
        // "shares method" estimator once averaged over many nonces.
        expected_work: capped_diff.to_target().to_work(),
    };
    (share, local_verify_passed)
}

// ---------------------------------------------------------------------------
// HashThread implementation
// ---------------------------------------------------------------------------

enum WorkerCommand {
    UpdateTask {
        task: HashTask,
        replace: bool,
        response_tx: tokio::sync::oneshot::Sender<Result<Option<HashTask>>>,
    },
    GoIdle {
        response_tx: tokio::sync::oneshot::Sender<Result<Option<HashTask>>>,
    },
    Shutdown,
}

pub struct Nano3sHashThread {
    command_tx: std::sync::mpsc::Sender<WorkerCommand>,
    event_rx: Option<mpsc::Receiver<HashThreadEvent>>,
    status: Arc<Mutex<HashThreadStatus>>,
    capabilities: HashThreadCapabilities,
    _thread_handle: Option<std::thread::JoinHandle<()>>,
}

impl Nano3sHashThread {
    fn new(telemetry_tx: watch::Sender<BoardTelemetry>) -> Result<Self> {
        let (cmd_tx, cmd_rx) = std::sync::mpsc::channel();
        let (evt_tx, evt_rx) = mpsc::channel(100);
        let status = Arc::new(Mutex::new(HashThreadStatus::default()));
        let status_clone = Arc::clone(&status);

        let handle = std::thread::Builder::new()
            .name("nano3s-worker".into())
            .spawn(move || run_worker(cmd_rx, evt_tx, status_clone, telemetry_tx))
            .map_err(|e| anyhow!("failed to spawn nano3s worker thread: {e}"))?;

        Ok(Self {
            command_tx: cmd_tx,
            event_rx: Some(evt_rx),
            status,
            capabilities: HashThreadCapabilities::default(),
            _thread_handle: Some(handle),
        })
    }
}

impl Drop for Nano3sHashThread {
    fn drop(&mut self) {
        let _ = self.command_tx.send(WorkerCommand::Shutdown);
    }
}

#[async_trait]
impl HashThread for Nano3sHashThread {
    fn name(&self) -> &str {
        "Avalon Nano3s (A3197S x12)"
    }

    fn capabilities(&self) -> &HashThreadCapabilities {
        &self.capabilities
    }

    async fn configure(&mut self) -> Result<()> {
        // nano3s_ipc_open() is a blocking call (documented to take up to
        // ~180s, sometimes longer -- see nano3s_ipc_shim.h) already
        // performed by the worker thread's startup before it processes
        // any commands; nothing to do here beyond declaring an initial
        // expected hashrate. Real hashrate arrives via status polling
        // once rtos_core.elf starts reporting IPC_MSG_STATUS.
        Ok(())
    }

    async fn update_task(&mut self, new_task: HashTask) -> Result<Option<HashTask>> {
        let (response_tx, response_rx) = tokio::sync::oneshot::channel();
        self.command_tx
            .send(WorkerCommand::UpdateTask {
                task: new_task,
                replace: false,
                response_tx,
            })
            .map_err(|_| anyhow!("nano3s worker command channel closed"))?;
        response_rx
            .await
            .map_err(|_| anyhow!("no response from nano3s worker"))?
    }

    async fn replace_task(&mut self, new_task: HashTask) -> Result<Option<HashTask>> {
        let (response_tx, response_rx) = tokio::sync::oneshot::channel();
        self.command_tx
            .send(WorkerCommand::UpdateTask {
                task: new_task,
                replace: true,
                response_tx,
            })
            .map_err(|_| anyhow!("nano3s worker command channel closed"))?;
        response_rx
            .await
            .map_err(|_| anyhow!("no response from nano3s worker"))?
    }

    async fn go_idle(&mut self) -> Result<Option<HashTask>> {
        let (response_tx, response_rx) = tokio::sync::oneshot::channel();
        self.command_tx
            .send(WorkerCommand::GoIdle { response_tx })
            .map_err(|_| anyhow!("nano3s worker command channel closed"))?;
        response_rx
            .await
            .map_err(|_| anyhow!("no response from nano3s worker"))?
    }

    fn take_event_receiver(&mut self) -> Option<mpsc::Receiver<HashThreadEvent>> {
        self.event_rx.take()
    }

    fn status(&self) -> HashThreadStatus {
        self.status.lock().unwrap().clone()
    }
}

/// Builds the 128-byte header buffer `rtos_core.elf` expects -- see the
/// module doc comment's byte-order notes.
///
/// `version` is written little-endian. `prev_blockhash` is written via
/// `word_bswap32()` (each 4-byte word reversed in place) to recover the
/// raw wire byte order the chip's register-loading expects. `ntime` and
/// `bits` are written big-endian.
fn build_header_bytes(version: bitcoin::block::Version, prev_blockhash: bitcoin::BlockHash, ntime: u32, bits: bitcoin::pow::CompactTarget) -> [u8; 128] {
    let mut header = [0u8; 128];
    header[0..4].copy_from_slice(&(version.to_consensus() as u32).to_le_bytes());
    header[4..36].copy_from_slice(&word_bswap32(&prev_blockhash.to_byte_array()));
    // header[36:68] (merkle root) intentionally left zero -- rtos_core.elf
    // fills this in on every work refresh from coinbase+merkles+nonce2.
    header[68..72].copy_from_slice(&ntime.to_be_bytes());
    // `bits` also feeds the chip's difficulty comparator via the separate
    // `target` IPC parameter (target_bytes_for(ctx.share_target)), not
    // via this header buffer.
    header[72..76].copy_from_slice(&bits.to_consensus().to_be_bytes());
    header
}

/// Computes share difficulty from the target via
/// `Difficulty::from_target()` (`Target::difficulty_float()`).
fn difficulty_of(share_target: bitcoin::pow::Target) -> f64 {
    share_target.difficulty_float()
}

/// Extracts target bytes [20:28) of the 256-bit little-endian target,
/// which is stored as four sequential 64-bit LE "digits" at byte offsets
/// 24/16/8/0 (most-significant digit at offset 24). Bytes [20:28) span
/// the low half of that top digit plus the high half of the next one
/// down, where the target's magnitude lives for realistic difficulties.
fn target_bytes_for(share_target: bitcoin::pow::Target) -> [u8; 32] {
    let full = share_target.to_le_bytes();
    let mut target = [0u8; 32];
    target[0..8].copy_from_slice(&full[20..28]);
    target
}

fn send_task_to_chain(job_id: u32, task: &HashTask, work_restart: bool) -> Result<Option<JobContext>> {
    let template = task.template.as_ref();
    let mrt: &MerkleRootTemplate = match &template.merkle_root {
        MerkleRootKind::Computed(mrt) => mrt,
        MerkleRootKind::Fixed(_) => {
            anyhow::bail!(
                "nano3s: header-only (Stratum v2 simple mode) jobs are not supported -- \
                 this hardware needs raw coinbase+branches for per-chip nonce2 rolling"
            );
        }
    };

    let en2 = task
        .en2
        .ok_or_else(|| anyhow!("nano3s: task has no extranonce2 assigned"))?;
    // Chip has a single 32-bit nonce2 register, so only the low 32 bits
    // of the scheduler's (up to 8-byte) en2 allocation are what the chip
    // actually varies.
    let nonce2_start = (en2.value() & 0xFFFF_FFFF) as u32;
    // Upper 32 bits of the 8-byte en2 allocation, fixed for this job
    // since only the chip's one register varies. Spliced into the
    // coinbase right after the chip's 4-byte region, so the coinbase
    // length (and merkle root) match what the pool's reconstruction
    // expects, and `verify_and_build_share()` reports the full 8-byte
    // value at submission time. rtos_core.elf only ever varies bytes
    // [nonce2_offset, nonce2_offset+4) and copies the rest of the
    // coinbase (including this fixed segment) verbatim.
    let en2_high = (en2.value() >> 32) as u32;

    let coinbase1 = mrt.coinbase1.clone();
    let extranonce1 = mrt.extranonce1.clone();
    let coinbase2 = mrt.coinbase2.clone();
    let nonce2_offset = (coinbase1.len() + extranonce1.len()) as i32;

    // Nonce2 embedded little-endian, matching miner_gen_nonce2_work()'s
    // convention. rtos_core.elf's miner_gen_nonce2_work() overwrites these
    // bytes on every task_send_work() call using its own internal nonce2
    // counter, so this initial value only matters for the first send.
    let mut coinbase = Vec::with_capacity(coinbase1.len() + extranonce1.len() + NONCE2_SIZE as usize + 4 + coinbase2.len());
    coinbase.extend_from_slice(&coinbase1);
    coinbase.extend_from_slice(&extranonce1);
    coinbase.extend_from_slice(&nonce2_start.to_le_bytes());
    coinbase.extend_from_slice(&en2_high.to_le_bytes());
    coinbase.extend_from_slice(&coinbase2);

    let nmerkles = mrt.merkle_branches.len();
    if nmerkles > 30 {
        anyhow::bail!("nano3s: job has {nmerkles} merkle branches, hardware wire format supports at most 30");
    }
    let mut merkles_flat = vec![0u8; nmerkles * 32];
    for (i, branch) in mrt.merkle_branches.iter().enumerate() {
        merkles_flat[i * 32..(i + 1) * 32].copy_from_slice(branch.as_byte_array());
    }

    let header = build_header_bytes(template.version.base(), template.prev_blockhash, task.ntime, template.bits);
    // `template.share_target` is the current pool session target (Layer
    // 3), not the scheduler's flood-control target (Layer 2). No floor is
    // applied.
    let target = target_bytes_for(template.share_target);
    // `vmask_candidates` (pre-bswap, per-slot consensus version) is
    // stored in JobContext for verify_and_build_share() to use unchanged;
    // `dynamic_vmask` (sent over the wire) is the bswapped form.
    let raw_mask = gp_bits_mask_to_raw(template.version.gp_bits_mask());
    let vmask_candidates = nano3s_vmask_candidates(template.version.base(), raw_mask);
    let dynamic_vmask: [u32; 8] = vmask_candidates.map(|v| v.swap_bytes());

    let rc = unsafe {
        nano3s_ipc_send_job(
            job_id,
            nonce2_start,
            nonce2_offset,
            NONCE2_SIZE,
            coinbase.as_ptr(),
            coinbase.len() as u32,
            HEADER_MERKLE_OFFSET,
            merkles_flat.as_ptr(),
            nmerkles as i32,
            header.as_ptr(),
            target.as_ptr(),
            if work_restart { 1 } else { 0 },
            dynamic_vmask.as_ptr(),
        )
    };
    if rc != 0 {
        anyhow::bail!("nano3s_ipc_send_job failed (job_id=0x{job_id:08x})");
    }

    Ok(Some(JobContext {
        version_base: template.version.base(),
        prev_blockhash: template.prev_blockhash,
        bits: template.bits,
        coinbase1,
        coinbase2,
        extranonce1,
        en2_high,
        merkle_branches: mrt.merkle_branches.clone(),
        ntime: task.ntime,
        share_target: task.share_target,
        share_tx: task.share_tx.clone(),
        vmask_candidates,
    }))
}

/// INA226 power monitor. Bus voltage: 1.25 mV/LSB (reg 0x02). Shunt
/// voltage: 2.5 uV/LSB, signed (reg 0x01). `i2cget`'s word mode returns
/// SMBus LSB-first byte order, swapped back before applying the scale.
/// Power = bus_v * (shunt_v / assumed_shunt_ohms), assuming a 10mOhm
/// shunt. Returns 0.0 on any read failure ("unavailable", not "0W
/// measured").
const POWER_I2C_BUS: u32 = 2;
const POWER_I2C_ADDR: u32 = 0x40;

fn i2c_read_word(bus: u32, addr: u32, reg: u32) -> Option<u32> {
    let output = std::process::Command::new("i2cget")
        .args(["-y", &bus.to_string(), &format!("0x{addr:02x}"), &format!("0x{reg:02x}"), "w"])
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout);
    let text = text.trim();
    let val = if let Some(hex) = text.strip_prefix("0x") {
        u32::from_str_radix(hex, 16).ok()?
    } else {
        text.parse().ok()?
    };
    if val > 0xFFFF { None } else { Some(val) }
}

/// Reads the INA226 and returns `(bus_v, current_a, power_w)`. This is
/// the USB-C PD input rail, not the ASIC core rail.
fn read_power_estimate() -> (f64, f64, f64) {
    let (Some(raw_bus), Some(raw_shunt)) = (
        i2c_read_word(POWER_I2C_BUS, POWER_I2C_ADDR, 0x02),
        i2c_read_word(POWER_I2C_BUS, POWER_I2C_ADDR, 0x01),
    ) else {
        return (0.0, 0.0, 0.0);
    };

    let bv_swapped = ((raw_bus & 0xff) << 8) | ((raw_bus >> 8) & 0xff);
    let sv_swapped = ((raw_shunt & 0xff) << 8) | ((raw_shunt >> 8) & 0xff);
    let shunt_signed = sv_swapped as u16 as i16;

    let bus_v = bv_swapped as f64 * 1.25 / 1000.0;
    let shunt_v = shunt_signed as f64 * 0.0025 / 1000.0;
    let current_a = shunt_v / 0.010; // assumed 10mOhm shunt, unconfirmed

    (bus_v, current_a, bus_v * current_a)
}

/// Control file the harness (mujina_test_harness.c) polls independently
/// of IPC to drive power_en (GPIO34) and the fan.
const HARNESS_CONTROL_FILE: &str = "/tmp/harness_control";

fn write_harness_control(cmd: &str) {
    if let Err(e) = std::fs::write(HARNESS_CONTROL_FILE, cmd) {
        eprintln!("[nano3s] failed to write {HARNESS_CONTROL_FILE}: {e}");
    }
}

/// Status file the harness's `fan_tach_loop()` writes once per second:
/// `rpm=<int>\nduty=<int>\n`. `rpm=-1` means the tach device couldn't be
/// opened/armed at harness startup.
const FAN_STATUS_FILE: &str = "/tmp/fan_status";

/// Reads [`FAN_STATUS_FILE`], or `None` if the harness hasn't written one
/// yet (not started, or too old a build without tach support).
fn read_fan_status() -> Option<Fan> {
    let contents = std::fs::read_to_string(FAN_STATUS_FILE).ok()?;
    let mut rpm = None;
    let mut percent = None;
    for line in contents.lines() {
        let Some((key, val)) = line.split_once('=') else {
            continue;
        };
        match key {
            "rpm" => rpm = val.parse::<i32>().ok().filter(|v| *v >= 0).map(|v| v as u32),
            "duty" => percent = val.parse::<u8>().ok(),
            _ => {}
        }
    }
    Some(Fan {
        name: "fan0".into(),
        rpm,
        percent,
        target_percent: None,
    })
}

/// Control file this worker's own loop polls for pause/resume/tune
/// commands, bypassing the generic scheduler pause API
/// (`PATCH /api/v0/miner {"paused":true}`).
const MUJINA_CONTROL_FILE: &str = "/tmp/mujina_control";

/// Writes a `tune:` directive to [`MUJINA_CONTROL_FILE`] for the run loop
/// to pick up on its next poll (~200ms latency). Encodes frequency,
/// voltage, and power-target into one line so a single write applies
/// atomically. Format: `tune:f0,f1,f2,f3,v,pt` -- always exactly 6
/// comma-separated fields, empty string for any omitted value (e.g.
/// voltage-only is `tune:,,,,3704,`, frequency-only is
/// `tune:338,358,378,398,,`). At least one of
/// `pll_freq_mhz`/`voltage_mv`/`power_target_w` must be `Some`, or this
/// is a no-op. `power_target_w: None` leaves the power-target loop
/// untouched (does not disable it).
pub(crate) fn write_tuning_command(
    pll_freq_mhz: Option<[u32; 4]>,
    voltage_mv: Option<u32>,
    power_target_w: Option<f64>,
) -> std::io::Result<()> {
    if pll_freq_mhz.is_none() && voltage_mv.is_none() && power_target_w.is_none() {
        return Ok(());
    }
    let f = pll_freq_mhz;
    let fields = [
        f.map(|f| f[0].to_string()).unwrap_or_default(),
        f.map(|f| f[1].to_string()).unwrap_or_default(),
        f.map(|f| f[2].to_string()).unwrap_or_default(),
        f.map(|f| f[3].to_string()).unwrap_or_default(),
        voltage_mv.map(|v| v.to_string()).unwrap_or_default(),
        power_target_w.map(|w| w.to_string()).unwrap_or_default(),
    ];
    std::fs::write(MUJINA_CONTROL_FILE, format!("tune:{}", fields.join(",")))
}

/// Live-edits the power-target voltage loop -- see `POWER_TARGET_STEP_MV`'s
/// doc comment for the full loop design. `Some(w)` sets a new target in
/// watts, taking effect on the next check rather than waiting out the
/// normal 150s interval; `None` disables the loop (voltage stays wherever
/// it last was).
pub(crate) fn write_power_target_command(target_w: Option<f64>) -> std::io::Result<()> {
    let body = match target_w {
        Some(w) => format!("power_target:{w}"),
        None => "power_target:off".to_string(),
    };
    std::fs::write(MUJINA_CONTROL_FILE, body)
}

/// Live idle/resume, for a dashboard "Pause Mining" button. Writes
/// "pause"/"resume" to `MUJINA_CONTROL_FILE`, handled by
/// `run_worker()`'s manual-pause match arms: `pause` sends
/// `nano3s_ipc_pause()` before touching the harness; `resume` calls
/// `resume_from_idle()` (re-power + settle).
pub(crate) fn write_pause_command(pause: bool) -> std::io::Result<()> {
    std::fs::write(MUJINA_CONTROL_FILE, if pause { "pause" } else { "resume" })
}

/// Live fan control, written to HARNESS_CONTROL_FILE (the C harness
/// process's control channel, separate from MUJINA_CONTROL_FILE which
/// run_worker() reads). `fan:` commands only touch PWM/fan state on the
/// harness side, never `power_en_set()`/`harness_apply_idle()`.
pub(crate) fn write_fan_control_command(cmd: &str) -> std::io::Result<()> {
    std::fs::write(HARNESS_CONTROL_FILE, cmd)
}

/// Returns Some("pause"/"resume"/"tune:..."/...) and clears the file, or
/// None if empty/unreadable. Read-then-truncate.
fn read_and_clear_control_file() -> Option<String> {
    let contents = std::fs::read_to_string(MUJINA_CONTROL_FILE).ok()?;
    let cmd = contents.trim();
    if cmd.is_empty() {
        return None;
    }
    let cmd = cmd.to_string();
    let _ = std::fs::write(MUJINA_CONTROL_FILE, "");
    Some(cmd)
}

/// Status LED strip driven via the `/dev/ws2812` char device (see
/// `NANO3S_GPIO_MAP.md`'s I2S-WS2812 entry). Wire format confirmed by
/// direct hardware test (not from any vendor source): one `write()` of
/// exactly `WS2812_LED_COUNT * 3` bytes, 3 bytes per LED in
/// green,red,blue order. Physical LED index 0 is the rightmost LED as
/// viewed from the front of the board; index `WS2812_LED_COUNT - 1` is
/// leftmost -- irrelevant here since every state below lights the whole
/// strip one uniform color.
const WS2812_DEV_PATH: &str = "/dev/ws2812";
const WS2812_LED_COUNT: usize = 9;

/// Status colors, deliberately dim (out of 255 per channel) since this is
/// a close-range status indicator, not a display.
mod led_color {
    pub const INITIALIZING: (u8, u8, u8) = (0, 0, 40); // dim blue
    pub const HASHING: (u8, u8, u8) = (0, 40, 0); // dim green
    pub const IDLE: (u8, u8, u8) = (40, 25, 0); // dim amber
    pub const FAULT: (u8, u8, u8) = (60, 0, 0); // dim red
}

struct Ws2812Strip {
    file: Option<std::fs::File>,
    last: Option<[(u8, u8, u8); WS2812_LED_COUNT]>,
}

impl Ws2812Strip {
    fn open() -> Self {
        let file = std::fs::OpenOptions::new()
            .write(true)
            .open(WS2812_DEV_PATH)
            .map_err(|e| eprintln!("[nano3s] failed to open {WS2812_DEV_PATH}: {e}"))
            .ok();
        Self { file, last: None }
    }

    /// Sets each LED individually. No-op (no write issued) if this is the
    /// same set of colors already applied.
    fn set(&mut self, colors: [(u8, u8, u8); WS2812_LED_COUNT]) {
        if self.last == Some(colors) {
            return;
        }
        if let Some(file) = self.file.as_mut() {
            use std::io::Write;
            let mut buf = [0u8; WS2812_LED_COUNT * 3];
            for (chunk, (r, g, b)) in buf.chunks_exact_mut(3).zip(colors.iter()) {
                chunk.copy_from_slice(&[*g, *r, *b]);
            }
            if let Err(e) = file.write_all(&buf) {
                eprintln!("[nano3s] failed to write {WS2812_DEV_PATH}: {e}");
            }
        }
        self.last = Some(colors);
    }

    /// Sets every LED to the same color.
    fn set_all(&mut self, rgb: (u8, u8, u8)) {
        self.set([rgb; WS2812_LED_COUNT]);
    }

    fn off(&mut self) {
        self.set_all((0, 0, 0));
    }
}

/// Manual LED override, written via `PATCH /api/v0/boards/{name}/led`
/// (see [`write_led_command`]) and read every ~200ms tick by
/// `run_worker()`. `Auto` (the default) means "no override -- show
/// automatic status color" (`led_color`); the other effects hand the
/// whole strip to the `/led` dashboard page until switched back.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum LedEffect {
    Auto,
    Off,
    Solid,
    /// Full hue cycle spread evenly across the strip, rotating.
    Rainbow,
    /// Whole strip one color, that color's hue slowly rotating (unlike
    /// Rainbow, every LED always matches).
    Colorloop,
    /// `color` at `brightness`, sinusoidally pulsing.
    Breathe,
    /// `color` at `brightness`, hard on/off toggle.
    Blink,
    /// WLED "Theater Chase": every 3rd LED lit in `color`, the pattern
    /// shifting by one LED per step.
    Chase,
    /// Chase, but each lit group's hue advances over time instead of
    /// using a fixed `color`.
    ChaseRainbow,
    /// Single lit LED (with a soft decaying tail) bouncing end to end
    /// ("Larson scanner"/Cylon eye), in `color`.
    Scanner,
    /// Random LEDs flash to full brightness in `color`, then decay.
    Twinkle,
    /// Warm flicker (WLED "Fire Flicker"): every LED independently
    /// jitters brightness and hue within a red/orange/amber range.
    /// Ignores `color` -- always warm.
    FireFlicker,
}

impl LedEffect {
    fn as_str(self) -> &'static str {
        match self {
            LedEffect::Auto => "auto",
            LedEffect::Off => "off",
            LedEffect::Solid => "solid",
            LedEffect::Rainbow => "rainbow",
            LedEffect::Colorloop => "colorloop",
            LedEffect::Breathe => "breathe",
            LedEffect::Blink => "blink",
            LedEffect::Chase => "chase",
            LedEffect::ChaseRainbow => "chase_rainbow",
            LedEffect::Scanner => "scanner",
            LedEffect::Twinkle => "twinkle",
            LedEffect::FireFlicker => "fire_flicker",
        }
    }

    fn parse(s: &str) -> Result<Self, String> {
        match s {
            "auto" => Ok(LedEffect::Auto),
            "off" => Ok(LedEffect::Off),
            "solid" => Ok(LedEffect::Solid),
            "rainbow" => Ok(LedEffect::Rainbow),
            "colorloop" => Ok(LedEffect::Colorloop),
            "breathe" => Ok(LedEffect::Breathe),
            "blink" => Ok(LedEffect::Blink),
            "chase" => Ok(LedEffect::Chase),
            "chase_rainbow" => Ok(LedEffect::ChaseRainbow),
            "scanner" => Ok(LedEffect::Scanner),
            "twinkle" => Ok(LedEffect::Twinkle),
            "fire_flicker" => Ok(LedEffect::FireFlicker),
            other => Err(format!(
                "unknown LED effect '{other}' (want auto/off/solid/rainbow/colorloop/breathe/blink/chase/chase_rainbow/scanner/twinkle/fire_flicker)"
            )),
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct LedOverrideState {
    effect: LedEffect,
    color: (u8, u8, u8),
    brightness: u8,
    speed: u8,
}

/// Global manual LED state, shared between the API handler (async, in the
/// axum server task) and `run_worker()`'s own OS thread. Defaults to
/// `Auto`/off-brightness-doesn't-matter until a `PATCH .../led` request
/// sets something else.
static LED_OVERRIDE: std::sync::Mutex<LedOverrideState> = std::sync::Mutex::new(LedOverrideState {
    effect: LedEffect::Auto,
    color: (255, 0, 0),
    brightness: 128,
    speed: 128,
});

fn parse_hex_color(s: &str) -> Result<(u8, u8, u8), String> {
    let s = s.trim().trim_start_matches('#');
    if s.len() != 6 {
        return Err(format!("invalid color '{s}' (want #RRGGBB)"));
    }
    let byte = |i: usize| u8::from_str_radix(&s[i..i + 2], 16).map_err(|_| format!("invalid color '{s}' (want #RRGGBB)"));
    Ok((byte(0)?, byte(2)?, byte(4)?))
}

/// Live-edits the WS2812 status strip's manual override, no reboot
/// required. See [`crate::api_client::types::BoardLedRequest`] for field
/// semantics. Any argument left `None` keeps that field's current value.
/// Returns an error string (for the handler to turn into a 400) on an
/// unknown effect name or malformed color.
pub(crate) fn write_led_command(effect: Option<String>, color: Option<String>, brightness: Option<u8>, speed: Option<u8>) -> Result<(), String> {
    let mut state = LED_OVERRIDE.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(e) = effect {
        state.effect = LedEffect::parse(&e)?;
    }
    if let Some(c) = color {
        state.color = parse_hex_color(&c)?;
    }
    if let Some(b) = brightness {
        state.brightness = b;
    }
    if let Some(s) = speed {
        state.speed = s;
    }
    Ok(())
}

/// Current commanded LED state, for `GET /api/v0/boards/{name}/led`.
pub(crate) fn get_led_state() -> crate::api_client::types::BoardLedState {
    let state = *LED_OVERRIDE.lock().unwrap_or_else(|e| e.into_inner());
    crate::api_client::types::BoardLedState {
        effect: state.effect.as_str().to_string(),
        color: format!("#{:02x}{:02x}{:02x}", state.color.0, state.color.1, state.color.2),
        brightness: state.brightness,
        speed: state.speed,
    }
}

/// Standard HSV -> RGB conversion. `h` in degrees (any range, wrapped
/// mod 360); `s`/`v` in `[0,1]`.
fn hsv_to_rgb(h: f32, s: f32, v: f32) -> (u8, u8, u8) {
    let h = h.rem_euclid(360.0);
    let c = v * s;
    let x = c * (1.0 - ((h / 60.0) % 2.0 - 1.0).abs());
    let m = v - c;
    let (r1, g1, b1) = match (h / 60.0) as u32 {
        0 => (c, x, 0.0),
        1 => (x, c, 0.0),
        2 => (0.0, c, x),
        3 => (0.0, x, c),
        4 => (x, 0.0, c),
        _ => (c, 0.0, x),
    };
    (((r1 + m) * 255.0).round() as u8, ((g1 + m) * 255.0).round() as u8, ((b1 + m) * 255.0).round() as u8)
}

/// Degrees the rainbow-chase phase advances per ~200ms tick at
/// `speed=255` -- one full 9-LED hue spacing per tick. `speed=0` freezes
/// the pattern (step is 0).
fn rainbow_step_deg(speed: u8) -> f32 {
    (speed as f32 / 255.0) * (360.0 / WS2812_LED_COUNT as f32)
}

/// Renders one frame of the rainbow-chase effect: a full hue cycle spread
/// evenly across the strip at the given phase (degrees; the caller owns
/// and advances this each tick by [`rainbow_step_deg`], wrapping happens
/// naturally via `hsv_to_rgb`'s `rem_euclid`). `brightness` (0-255)
/// scales value (HSV `v`), not saturation, so colors stay fully
/// saturated while dimming.
fn rainbow_frame(phase_deg: f32, brightness: u8) -> [(u8, u8, u8); WS2812_LED_COUNT] {
    let v = brightness as f32 / 255.0;
    let mut colors = [(0u8, 0u8, 0u8); WS2812_LED_COUNT];
    for (i, c) in colors.iter_mut().enumerate() {
        let hue = phase_deg + i as f32 * (360.0 / WS2812_LED_COUNT as f32);
        *c = hsv_to_rgb(hue, 1.0, v);
    }
    colors
}

/// Scales an (r,g,b) color by a `[0,1]` level (clamped), for effects that
/// dim/pulse a fixed user color rather than working in HSV.
fn scale_color(rgb: (u8, u8, u8), level: f32) -> (u8, u8, u8) {
    let level = level.clamp(0.0, 1.0);
    ((rgb.0 as f32 * level).round() as u8, (rgb.1 as f32 * level).round() as u8, (rgb.2 as f32 * level).round() as u8)
}

/// Tiny xorshift32 PRNG for Twinkle/FireFlicker's per-pixel randomness --
/// not worth pulling in the `rand` crate for a couple of sparkle rolls
/// per tick. Not cryptographic, doesn't need to be.
fn xorshift32(state: &mut u32) -> u32 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    x
}

fn rand_unit(state: &mut u32) -> f32 {
    (xorshift32(state) as f32) / (u32::MAX as f32)
}

/// Per-tick animation memory for effects that need more than the single
/// [`LedOverrideState`] snapshot: phase accumulators, per-pixel decay,
/// PRNG state. Owned by `run_worker()`'s thread. Auto/Off/Solid are
/// simple enough to render directly in `run_worker()` and don't use
/// this; every other [`LedEffect`] goes through [`AnimState::frame`].
struct AnimState {
    effect: LedEffect,
    phase_deg: f32,
    chase_pos: f32,
    scanner_pos: f32,
    scanner_dir: f32,
    blink_on: bool,
    blink_accum: f32,
    twinkle: [f32; WS2812_LED_COUNT],
    fire: [f32; WS2812_LED_COUNT],
    rng: u32,
}

impl AnimState {
    fn new() -> Self {
        let seed = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.subsec_nanos()).unwrap_or(0).max(1);
        Self {
            effect: LedEffect::Auto,
            phase_deg: 0.0,
            chase_pos: 0.0,
            scanner_pos: 0.0,
            scanner_dir: 1.0,
            blink_on: true,
            blink_accum: 0.0,
            twinkle: [0.0; WS2812_LED_COUNT],
            fire: [0.6; WS2812_LED_COUNT],
            rng: seed,
        }
    }

    /// Resets phase/decay memory when the commanded effect changes, so
    /// e.g. Scanner doesn't resume mid-sweep after time spent on Solid.
    fn sync_effect(&mut self, effect: LedEffect) {
        if self.effect != effect {
            self.effect = effect;
            self.phase_deg = 0.0;
            self.chase_pos = 0.0;
            self.scanner_pos = 0.0;
            self.scanner_dir = 1.0;
            self.blink_on = true;
            self.blink_accum = 0.0;
            self.twinkle = [0.0; WS2812_LED_COUNT];
        }
    }

    /// Renders one frame for `ov.effect` and advances this state by one
    /// ~200ms tick. Must be preceded by [`Self::sync_effect`] in the same
    /// tick. Panics-free fallback (all-off) for Auto/Off/Solid, which
    /// `run_worker()` never actually routes here.
    fn frame(&mut self, ov: LedOverrideState) -> [(u8, u8, u8); WS2812_LED_COUNT] {
        let v = ov.brightness as f32 / 255.0;
        let speed_frac = ov.speed as f32 / 255.0;
        match ov.effect {
            LedEffect::Rainbow => {
                self.phase_deg += rainbow_step_deg(ov.speed);
                rainbow_frame(self.phase_deg, ov.brightness)
            }
            LedEffect::Colorloop => {
                self.phase_deg = (self.phase_deg + speed_frac * 6.0) % 360.0;
                [hsv_to_rgb(self.phase_deg, 1.0, v); WS2812_LED_COUNT]
            }
            LedEffect::Breathe => {
                self.phase_deg = (self.phase_deg + speed_frac * 12.0) % 360.0;
                // Floor at 8% so the color never fully vanishes into a
                // dead-looking black between breaths.
                let level = 0.08 + 0.92 * (0.5 - 0.5 * self.phase_deg.to_radians().cos());
                [scale_color(ov.color, v * level); WS2812_LED_COUNT]
            }
            LedEffect::Blink => {
                self.blink_accum += 4.0 + speed_frac * 40.0;
                if self.blink_accum >= 100.0 {
                    self.blink_accum = 0.0;
                    self.blink_on = !self.blink_on;
                }
                let level = if self.blink_on { v } else { 0.0 };
                [scale_color(ov.color, level); WS2812_LED_COUNT]
            }
            LedEffect::Chase => {
                self.chase_pos = (self.chase_pos + 0.2 + speed_frac * 2.0) % WS2812_LED_COUNT as f32;
                let mut colors = [(0u8, 0u8, 0u8); WS2812_LED_COUNT];
                let pos = self.chase_pos as i32;
                for (i, c) in colors.iter_mut().enumerate() {
                    if (i as i32 - pos).rem_euclid(3) == 0 {
                        *c = scale_color(ov.color, v);
                    }
                }
                colors
            }
            LedEffect::ChaseRainbow => {
                self.chase_pos = (self.chase_pos + 0.2 + speed_frac * 2.0) % WS2812_LED_COUNT as f32;
                self.phase_deg = (self.phase_deg + 3.0 + speed_frac * 6.0) % 360.0;
                let mut colors = [(0u8, 0u8, 0u8); WS2812_LED_COUNT];
                let pos = self.chase_pos as i32;
                for (i, c) in colors.iter_mut().enumerate() {
                    if (i as i32 - pos).rem_euclid(3) == 0 {
                        *c = hsv_to_rgb(self.phase_deg + i as f32 * 15.0, 1.0, v);
                    }
                }
                colors
            }
            LedEffect::Scanner => {
                let span = (WS2812_LED_COUNT - 1) as f32;
                self.scanner_pos += self.scanner_dir * (0.1 + speed_frac * 0.9);
                if self.scanner_pos >= span {
                    self.scanner_pos = span;
                    self.scanner_dir = -1.0;
                } else if self.scanner_pos <= 0.0 {
                    self.scanner_pos = 0.0;
                    self.scanner_dir = 1.0;
                }
                let mut colors = [(0u8, 0u8, 0u8); WS2812_LED_COUNT];
                for (i, c) in colors.iter_mut().enumerate() {
                    let dist = (i as f32 - self.scanner_pos).abs();
                    let level = (1.0 - dist / 1.5).clamp(0.0, 1.0);
                    *c = scale_color(ov.color, v * level);
                }
                colors
            }
            LedEffect::Twinkle => {
                // Spawn chance per LED per tick scales with speed; each
                // spark decays geometrically toward 0.
                let spawn_chance = 0.02 + speed_frac * 0.2;
                for t in self.twinkle.iter_mut() {
                    if *t <= 0.01 && rand_unit(&mut self.rng) < spawn_chance {
                        *t = 1.0;
                    } else {
                        *t *= 0.80;
                    }
                }
                let mut colors = [(0u8, 0u8, 0u8); WS2812_LED_COUNT];
                for (i, c) in colors.iter_mut().enumerate() {
                    *c = scale_color(ov.color, v * self.twinkle[i]);
                }
                colors
            }
            LedEffect::FireFlicker => {
                // Always warm (deep red -> orange -> amber), ignoring
                // `ov.color` entirely -- `color` persists across effect
                // switches (see write_led_command()'s "unset keeps
                // current value" semantics), so a fixed-hue interpretation
                // of it here would inherit whatever was last picked for a
                // *different* effect (e.g. white from Twinkle), which
                // isn't fire-colored at all.
                let mut colors = [(0u8, 0u8, 0u8); WS2812_LED_COUNT];
                for (i, c) in colors.iter_mut().enumerate() {
                    let jitter = 0.55 + rand_unit(&mut self.rng) * 0.45;
                    self.fire[i] = self.fire[i] * 0.5 + jitter * 0.5;
                    let hue = 8.0 + rand_unit(&mut self.rng) * 22.0;
                    *c = hsv_to_rgb(hue, 1.0, v * self.fire[i]);
                }
                colors
            }
            LedEffect::Auto | LedEffect::Off | LedEffect::Solid => [(0, 0, 0); WS2812_LED_COUNT],
        }
    }
}

/// Status file read by fb_draw (the LCD renderer). Fields with no direct
/// source here are approximated or omitted; fb_draw treats a missing key
/// as "unknown"/false rather than a wrong value.
const NANO3S_LIVE_FILE: &str = "/tmp/nano3s_live.txt";

#[allow(clippy::too_many_arguments)]
fn write_live_status(ipc_ok: bool, current_job_id: Option<u32>, shares_found: u32, is_idle: bool, st: &Nano3sStatus, power_w: f64, difficulty: f64) {
    let job_id_str = match current_job_id {
        Some(id) => format!("{id:016x}"),
        None => "-".to_string(),
    };
    let body = format!(
        "pool_connected={ipc}\n\
         ipc_connected={ipc}\n\
         job_id={job_id_str}\n\
         shares_found={shares_found}\n\
         power_w={power_w:.1}\n\
         difficulty={difficulty:.4}\n\
         paused={paused}\n\
         asics_total={asics_total}\n\
         hashrate_ghs={ghsmm}\n\
         temp_avg={temp_avg:.1}\n\
         temp_max={temp_max:.1}\n\
         pll0={pll0}\n\
         pll1={pll1}\n\
         pll2={pll2}\n\
         pll3={pll3}\n\
         err_crc={err_crc}\n\
         voltage_mv={voltage_mv}\n",
        ipc = if ipc_ok { 1 } else { 0 },
        paused = if is_idle { 1 } else { 0 },
        asics_total = st.asics_total,
        ghsmm = st.ghsmm,
        temp_avg = st.temp_avg,
        temp_max = st.temp_max,
        pll0 = st.pll_freq[0],
        pll1 = st.pll_freq[1],
        pll2 = st.pll_freq[2],
        pll3 = st.pll_freq[3],
        err_crc = st.err_crc,
        voltage_mv = st.voltage_mv,
    );
    if let Err(e) = std::fs::write(NANO3S_LIVE_FILE, body) {
        eprintln!("[nano3s] failed to write {NANO3S_LIVE_FILE}: {e}");
    }
}

/// Re-powers the chain and waits for a settle gap (power_en HIGH to RST
/// release) before resuming rtos_core.elf, which then does its own
/// RST-deassert/re-enum sequence.
fn resume_from_idle() {
    write_harness_control("resume");
    std::thread::sleep(Duration::from_secs(5));
    let rc = unsafe { nano3s_ipc_resume() };
    if rc != 0 {
        eprintln!("[nano3s] nano3s_ipc_resume failed");
    }
}

fn run_worker(
    cmd_rx: std::sync::mpsc::Receiver<WorkerCommand>,
    event_tx: mpsc::Sender<HashThreadEvent>,
    status: Arc<Mutex<HashThreadStatus>>,
    telemetry_tx: watch::Sender<BoardTelemetry>,
) {
    let mut led = Ws2812Strip::open();
    led.set_all(led_color::INITIALIZING);
    // Phase/decay/PRNG memory for the animated LED effects (everything
    // except Auto/Off/Solid); persists across ticks.
    let mut anim = AnimState::new();

    let rc = unsafe { nano3s_ipc_open() };
    let ipc_ok = rc == 0;
    if !ipc_ok {
        eprintln!("[nano3s] nano3s_ipc_open failed -- board will report no status/hashrate");
        led.set_all(led_color::FAULT);
        // Fall through: still process commands so the scheduler doesn't
        // hang waiting on responses, but nothing will ever hash.
    } else {
        // Ramp the chain up from its ~100MHz cold bring-up default via
        // IPC_MSG_SET_MODE. Fire-and-forget -- rtos_core applies the
        // gradual ramp on its own worker thread.
        let rc = unsafe { nano3s_ipc_set_mode(NANO3S_PLL_FREQ_TARGET.as_ptr(), 0) };
        if rc != 0 {
            eprintln!("[nano3s] nano3s_ipc_set_mode failed -- chain will stay at cold bring-up clock");
        }

        // Declare a known-good estimate immediately rather than leaving
        // this at the trait's zero default. Status updates below
        // supersede this once rtos_core.elf starts reporting.
        let _ = event_tx.try_send(HashThreadEvent::ExpectedHashRate(HashRate::from_gigahashes(NANO3S_EXPECTED_HASHRATE_GHS)));
    }

    let job_id_counter = AtomicU32::new(1);
    let mut current_task: Option<HashTask> = None;
    let mut jobs: HashMap<u32, JobContext> = HashMap::new();
    let mut current_job_id: Option<u32> = None;
    // Cached power-draw estimate, refreshed only when a new IPC_MSG_STATUS
    // arrives (~every 15s), not on every 200ms loop tick -- an i2cget
    // subprocess is too expensive to run that often.
    let mut last_power_w: f64 = 0.0;
    // Tracks whether GoIdle has been called without a subsequent task --
    // resuming from idle needs the harness to re-power the chain (and
    // settle) before rtos_core.elf can accept a fresh job.
    let mut is_idle = false;
    // Set only by MUJINA_CONTROL_FILE (manual/external pause), never by the
    // scheduler's own GoIdle. Distinguishes "stay idle even though a new
    // task just arrived" (manual) from "a task just arrived, auto-resume"
    // (scheduler-driven idle) -- see UpdateTask's handling below.
    let mut manual_pause = false;
    let mut shares_found: u32 = 0;
    let mut current_difficulty: f64 = 0.0;
    // Power-target voltage loop -- see POWER_TARGET_STEP_MV's comment
    // block for the full design. The env var sets the startup value;
    // live-editable via a `power_target:<W>`/`power_target:off`
    // control-file command (see the match arm below and
    // `write_power_target_command()`/`PATCH /api/v0/boards/{name}/power-target`).
    // `None` means the feature is off.
    let mut power_target_w: Option<f64> = std::env::var("MUJINA_NANO3S_POWER_TARGET_W")
        .ok()
        .and_then(|s| s.trim().parse::<f64>().ok());
    let mut next_power_check = std::time::Instant::now() + POWER_TARGET_CHECK_INTERVAL;
    // `resume_from_idle()` re-powers and re-enumerates the chain, which
    // lands back at rtos_core's ~100MHz cold bring-up default. Track what
    // was actually last applied via a `tune:` command (defaulting to the
    // startup ramp target) and reapply both after every resume.
    let mut last_applied_pll_freq: [u32; 4] = NANO3S_PLL_FREQ_TARGET;
    let mut last_applied_voltage_mv: Option<i32> = None;

    loop {
        // Poll for an external pause/resume via MUJINA_CONTROL_FILE,
        // bypassing the scheduler's own pause API.
        match read_and_clear_control_file().as_deref() {
            Some("pause") if !manual_pause => {
                let rc = unsafe { nano3s_ipc_pause() };
                if rc != 0 {
                    eprintln!("[nano3s] nano3s_ipc_pause failed");
                }
                write_harness_control("pause");
                is_idle = true;
                manual_pause = true;
                eprintln!("[nano3s] manual PAUSE via {MUJINA_CONTROL_FILE}");
            }
            Some("resume") if manual_pause => {
                resume_from_idle();
                // Re-apply the last commanded PLL frequency and voltage,
                // since a fresh re-enum resets the chip's PLL to 100MHz.
                // Fire-and-forget, same as the initial startup ramp.
                let rc = unsafe { nano3s_ipc_set_mode(last_applied_pll_freq.as_ptr(), 0) };
                if rc != 0 {
                    eprintln!("[nano3s] post-resume nano3s_ipc_set_mode failed -- chain may stay at cold bring-up clock");
                }
                if let Some(mv) = last_applied_voltage_mv {
                    let rc = unsafe { nano3s_ipc_set_voltage_raw(mv) };
                    if rc != 0 {
                        eprintln!("[nano3s] post-resume nano3s_ipc_set_voltage_raw failed");
                    }
                }
                is_idle = false;
                manual_pause = false;
                eprintln!("[nano3s] manual RESUME via {MUJINA_CONTROL_FILE} (reapplied pll_freq={last_applied_pll_freq:?} voltage_mv={last_applied_voltage_mv:?})");
            }
            Some(s) if s.starts_with("tune:") => {
                // See write_tuning_command()'s doc comment for the exact
                // "always 6 fields" wire format this expects.
                let fields: Vec<&str> = s["tune:".len()..].split(',').collect();
                if fields.len() != 6 {
                    eprintln!("[nano3s] malformed tune directive (want 6 fields): {s}");
                } else {
                    if !fields[0].is_empty() {
                        match fields[0..4].iter().map(|f| f.parse::<u32>()).collect::<Result<Vec<u32>, _>>() {
                            Ok(freq) => {
                                let rc = unsafe { nano3s_ipc_set_mode(freq.as_ptr(), 0) };
                                if rc != 0 {
                                    eprintln!("[nano3s] tuning: nano3s_ipc_set_mode failed");
                                } else {
                                    last_applied_pll_freq = [freq[0], freq[1], freq[2], freq[3]];
                                    eprintln!("[nano3s] tuning: SET_MODE pll_freq={freq:?} (via dashboard/API)");
                                }
                            }
                            Err(_) => eprintln!("[nano3s] malformed tune frequency fields: {s}"),
                        }
                    }
                    if !fields[4].is_empty() {
                        match fields[4].parse::<i32>() {
                            Ok(mv) => {
                                let rc = unsafe { nano3s_ipc_set_voltage_raw(mv) };
                                if rc != 0 {
                                    eprintln!("[nano3s] tuning: nano3s_ipc_set_voltage_raw failed");
                                } else {
                                    last_applied_voltage_mv = Some(mv);
                                    eprintln!("[nano3s] tuning: SET_VOLTAGE_RAW target_mv={mv} (via dashboard/API)");
                                }
                            }
                            Err(_) => eprintln!("[nano3s] malformed tune voltage field: {s}"),
                        }
                    }
                    if !fields[5].is_empty() {
                        match fields[5].parse::<f64>() {
                            Ok(w) => {
                                power_target_w = Some(w);
                                next_power_check = std::time::Instant::now();
                                eprintln!("[nano3s] tuning: power-target set to {w:.1}W (via dashboard/API)");
                            }
                            Err(_) => eprintln!("[nano3s] malformed tune power-target field: {s}"),
                        }
                    }
                }
            }
            Some(s) if s.starts_with("power_target:") => {
                let v = &s["power_target:".len()..];
                if v == "off" {
                    power_target_w = None;
                    eprintln!("[nano3s] power-target: disabled via dashboard/API");
                } else {
                    match v.parse::<f64>() {
                        Ok(w) => {
                            power_target_w = Some(w);
                            // Act on the new target at the next check
                            // rather than waiting out the old interval.
                            next_power_check = std::time::Instant::now();
                            eprintln!("[nano3s] power-target: live target set to {w:.1}W via dashboard/API");
                        }
                        Err(_) => eprintln!("[nano3s] malformed power_target directive: {s}"),
                    }
                }
            }
            _ => {}
        }

        // Drain pending commands (non-blocking).
        loop {
            match cmd_rx.try_recv() {
                Ok(WorkerCommand::UpdateTask { task, replace, response_tx }) => {
                    if manual_pause {
                        // Stay idle -- remember the latest task to apply
                        // once a "resume" arrives. Do not touch RST/UART
                        // here.
                        let old = current_task.replace(task);
                        let _ = response_tx.send(Ok(old));
                        continue;
                    }
                    if is_idle {
                        resume_from_idle();
                        is_idle = false;
                    }
                    let _ = replace; // both paths behave the same on this hardware today
                    let job_id = job_id_counter.fetch_add(1, Ordering::Relaxed);
                    let work_restart = replace;
                    match send_task_to_chain(job_id, &task, work_restart) {
                        Ok(ctx) => {
                            if let Some(ctx) = ctx {
                                current_difficulty = difficulty_of(ctx.share_target);
                                jobs.insert(job_id, ctx);
                                // Bound job history -- keep the last 4.
                                if jobs.len() > 4 {
                                    if let Some(&oldest) = jobs.keys().min() {
                                        jobs.remove(&oldest);
                                    }
                                }
                                current_job_id = Some(job_id);
                            }
                            let old = current_task.replace(task);
                            let _ = response_tx.send(Ok(old));
                        }
                        Err(e) => {
                            eprintln!("[nano3s] send_task_to_chain failed: {e}");
                            let _ = response_tx.send(Err(e));
                        }
                    }
                }
                Ok(WorkerCommand::GoIdle { response_tx }) => {
                    let rc = unsafe { nano3s_ipc_pause() };
                    if rc != 0 {
                        eprintln!("[nano3s] nano3s_ipc_pause failed");
                    }
                    // Tell the harness to cut power_en/fan too, after
                    // rtos_core asserts RST.
                    write_harness_control("pause");
                    is_idle = true;
                    let old = current_task.take();
                    let _ = response_tx.send(Ok(old));
                }
                Ok(WorkerCommand::Shutdown) => {
                    led.off();
                    unsafe { nano3s_ipc_close() };
                    return;
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => break,
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    led.off();
                    unsafe { nano3s_ipc_close() };
                    return;
                }
            }
        }

        // Drain any nonce reports and verify/submit shares.
        loop {
            let mut nonce = Nano3sNonce::default();
            let got = unsafe { nano3s_ipc_poll_nonce(&mut nonce as *mut _) };
            if got != 1 {
                break;
            }
            // Submission is not gated on local_verify_passed; the pool
            // judges shares independently.
            match jobs.get(&nonce.job_id) {
                None => {
                    eprintln!(
                        "[nano3s] DIAG nonce for unknown job_id=0x{:08x} nonce2=0x{:08x} nonce=0x{:08x} mid_id={} (known job_ids: {:?})",
                        nonce.job_id, nonce.nonce2, nonce.nonce, nonce.mid_id,
                        jobs.keys().map(|k| format!("0x{k:08x}")).collect::<Vec<_>>()
                    );
                }
                Some(ctx) => {
                    let (share, _local_verify_passed) =
                        verify_and_build_share(ctx, nonce.nonce2, nonce.nonce, nonce.mid_id);
                    shares_found += 1;
                    let _ = ctx.share_tx.blocking_send(share);
                }
            }
        }

        // Refresh cached status from the latest IPC_MSG_STATUS.
        let mut st = Nano3sStatus::default();
        if unsafe { nano3s_ipc_get_status(&mut st as *mut _) } == 1 {
            let mut s = status.lock().unwrap();
            s.hashrate = HashRate::from_gigahashes(st.ghsmm as f64);
            s.temperature_c = if st.temp_avg > 0.0 { Some(st.temp_avg) } else { None };
            s.is_active = current_task.is_some() && st.paused == 0;
            drop(s);

            // Push the hardware-measured hashrate (ghsmm, from
            // rtos_core.elf's cal_ghsmm()) to the scheduler as a
            // StatusUpdate event, so it can prefer this over its
            // shares-based statistical estimator.
            let _ = event_tx.try_send(HashThreadEvent::StatusUpdate(HashThreadStatus {
                hashrate: HashRate::from_gigahashes(st.ghsmm as f64),
                temperature_c: if st.temp_avg > 0.0 { Some(st.temp_avg) } else { None },
                is_active: current_task.is_some() && st.paused == 0,
                ..Default::default()
            }));

            let (last_bus_v, last_current_a, power_w) = read_power_estimate();
            last_power_w = power_w;

            // Power-target voltage loop -- see POWER_TARGET_STEP_MV's doc
            // comment. Safety check runs on every status refresh (~15s);
            // normal target-seeking steps are gated to
            // POWER_TARGET_CHECK_INTERVAL.
            if let Some(target_w) = power_target_w {
                let cur_mv = st.voltage_mv as i32;
                let now = std::time::Instant::now();

                let step = if power_w > POWER_TARGET_SAFETY_W {
                    // Hard safety trip -- always allowed, ignores the
                    // interval gate.
                    Some(-POWER_TARGET_STEP_MV)
                } else if now >= next_power_check {
                    if power_w < target_w - POWER_TARGET_DEADBAND_W {
                        Some(POWER_TARGET_STEP_MV)
                    } else if power_w > target_w + POWER_TARGET_DEADBAND_W {
                        Some(-POWER_TARGET_STEP_MV)
                    } else {
                        None
                    }
                } else {
                    None
                };

                if let Some(step) = step {
                    let mut new_mv = cur_mv + step;
                    if new_mv == POWER_TARGET_AVOID_MV {
                        new_mv += step;
                    }
                    new_mv = new_mv.clamp(POWER_TARGET_MIN_MV, POWER_TARGET_MAX_MV);

                    if new_mv != cur_mv {
                        eprintln!(
                            "[nano3s] power-target: power={power_w:.1}W target={target_w:.1}W cur={cur_mv}mV -> {new_mv}mV"
                        );
                        if let Err(e) = write_tuning_command(None, Some(new_mv as u32), None) {
                            eprintln!("[nano3s] power-target: failed to write tuning command: {e}");
                        }
                    }
                    next_power_check = now + POWER_TARGET_CHECK_INTERVAL;
                }
            }

            // Publish telemetry to the REST API's board registry. Modeled
            // as one HashThread == one ThreadTelemetry entry (the whole
            // 12-chip chain is a single HashThread).
            telemetry_tx.send_modify(|t| {
                t.threads = vec![ThreadTelemetry {
                    name: "chain".into(),
                    hashrate: (st.ghsmm as u64).saturating_mul(1_000_000_000),
                    is_active: current_task.is_some() && st.paused == 0,
                }];
                t.temperatures = vec![TemperatureSensor {
                    name: "asic".into(),
                    temperature: if st.temp_avg > 0.0 {
                        Some(Temperature::from_celsius(st.temp_avg))
                    } else {
                        None
                    },
                }];
                t.powers = vec![PowerMeasurement {
                    name: "core".into(),
                    voltage_v: Some(st.voltage_mv as f32 / 1000.0),
                    current_a: if last_current_a > 0.0 {
                        Some(last_current_a as f32)
                    } else {
                        None
                    },
                    power_w: if last_power_w > 0.0 {
                        Some(last_power_w as f32)
                    } else {
                        None
                    },
                }];
                t.fans = read_fan_status().into_iter().collect();
            });

            // Full per-chip + chain-wide detail snapshot for the
            // dashboard's Info page (GET /nano3s-detail).
            let (shares_accepted, shares_rejected) =
                crate::job_source::stratum_v1::share_accept_reject_counts();
            let chips: Vec<Nano3sChipDetail> = (0..st.chip_count as usize)
                .filter(|&i| i < NANO3S_STATUS_MAX_CHIPS)
                .map(|i| {
                    let c = &st.chips[i];
                    Nano3sChipDetail {
                        chip: i as u8,
                        temp_c: c.temp_c,
                        volt_mv: c.volt_mv,
                        pll_cnt: c.pll_cnt,
                        pll_freq: c.pll_freq,
                        nonce_timeout: c.nonce_timeout,
                        nonce_heartbeat: c.nonce_heartbeat,
                        nonce_data: c.nonce_data,
                        ghsspd: c.ghsspd,
                        spd_dh: c.spd_dh,
                    }
                })
                .collect();
            let detail = Nano3sDetail {
                ipc_connected: ipc_ok,
                paused: st.paused != 0,
                asics_total: st.asics_total,
                ghsmm: st.ghsmm,
                ghsspd: st.ghsspd,
                spd_dh: st.spd_dh,
                temp_avg: st.temp_avg,
                temp_max: st.temp_max,
                pll_freq_commanded: st.pll_freq,
                voltage_mv_commanded: st.voltage_mv,
                err_crc: st.err_crc,
                nonce_read_err: st.nonce_read_err,
                nonce_bad_len: st.nonce_bad_len,
                nonce_overflow: st.nonce_overflow,
                regread_err: st.regread_err,
                chips,
                ina_bus_v: last_bus_v,
                ina_current_a: last_current_a,
                ina_power_w: last_power_w,
                shares_found,
                shares_accepted,
                shares_rejected,
                difficulty: current_difficulty,
                job_id: current_job_id,
                best_share_diff: *BEST_SHARE_DIFF.lock().unwrap_or_else(|e| e.into_inner()),
            };
            if let Ok(mut d) = NANO3S_DETAIL.lock() {
                *d = Some(detail);
            }
        }
        write_live_status(ipc_ok, current_job_id, shares_found, is_idle, &st, last_power_w, current_difficulty);

        {
            let ov = *LED_OVERRIDE.lock().unwrap_or_else(|e| e.into_inner());
            anim.sync_effect(ov.effect);
            match ov.effect {
                LedEffect::Auto => {
                    led.set_all(if !ipc_ok {
                        led_color::FAULT
                    } else if is_idle || manual_pause {
                        led_color::IDLE
                    } else if current_task.is_some() {
                        led_color::HASHING
                    } else {
                        led_color::INITIALIZING
                    });
                }
                LedEffect::Off => led.off(),
                LedEffect::Solid => led.set_all(scale_color(ov.color, ov.brightness as f32 / 255.0)),
                _ => led.set(anim.frame(ov)),
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}
