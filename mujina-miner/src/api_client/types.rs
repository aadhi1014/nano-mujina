//! API data transfer objects.
//!
//! These types define the API contract shared between the server and
//! clients (CLI, TUI). See `docs/api.md` (at the repository root)
//! for the full API contract documentation, including conventions
//! for null values and units.

use serde::{Deserialize, Serialize};
use utoipa::ToSchema;

use crate::types::Temperature;

/// Full miner telemetry snapshot.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct MinerTelemetry {
    pub uptime_secs: u64,
    /// Aggregate hashrate in hashes per second.
    pub hashrate: u64,
    pub shares_submitted: u64,
    pub paused: bool,
    pub boards: Vec<BoardTelemetry>,
    pub sources: Vec<SourceTelemetry>,
}

/// Board telemetry snapshot.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct BoardTelemetry {
    /// URL-friendly identifier (e.g. "bitaxe-e2f56f9b").
    pub name: String,
    pub model: String,
    pub serial: Option<String>,
    pub fans: Vec<Fan>,
    pub temperatures: Vec<TemperatureSensor>,
    pub powers: Vec<PowerMeasurement>,
    pub threads: Vec<ThreadTelemetry>,
}

/// Fan status.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct Fan {
    pub name: String,
    /// Measured RPM, or null if the tachometer read failed.
    pub rpm: Option<u32>,
    /// Measured duty cycle, or null if the read failed.
    pub percent: Option<u8>,
    /// Target duty cycle, or null if the fan is in automatic mode.
    pub target_percent: Option<u8>,
}

/// Temperature sensor reading.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct TemperatureSensor {
    pub name: String,
    #[serde(rename = "temperature_c")]
    #[schema(value_type = Option<f32>)]
    pub temperature: Option<Temperature>,
}

/// Voltage, current, and power from a single measurement point.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct PowerMeasurement {
    pub name: String,
    pub voltage_v: Option<f32>,
    pub current_a: Option<f32>,
    pub power_w: Option<f32>,
}

/// Per-thread telemetry.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct ThreadTelemetry {
    pub name: String,
    /// Hashrate in hashes per second.
    pub hashrate: u64,
    pub is_active: bool,
}

/// Writable fields for `PATCH /api/v0/miner`.
///
/// All fields are optional; only those present in the request body are
/// applied. Read-only fields like `uptime_secs` and `hashrate` are not
/// included and cannot be set.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct MinerPatchRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub paused: Option<bool>,
}

/// Request body for setting a fan's target duty cycle.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct SetFanTargetRequest {
    /// Target duty cycle percentage (0--100), or null for automatic control.
    pub target_percent: Option<u8>,
}

/// Request body for `PATCH /api/v0/boards/{name}/tuning`.
///
/// At least one field must be present. Applied live over IPC
/// (`IPC_MSG_SET_MODE`/`IPC_MSG_SET_VOLTAGE_RAW`), no reboot needed.
/// `pll_freq_mhz` is the per-domain ramp target (four values, one per PLL
/// domain); `voltage_mv` is the core supply voltage. Range validation
/// happens in the handler, not here.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct BoardTuningRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pll_freq_mhz: Option<[u32; 4]>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub voltage_mv: Option<u32>,
    /// Sets the power-target loop's target together with freq/voltage in
    /// one atomic write. `None` leaves the power-target loop untouched;
    /// use `PATCH /power-target {target_w: null}` to disable the loop.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub power_target_w: Option<f64>,
    /// Sets the temp-target PLL frequency auto-throttle's target together
    /// with freq/voltage/power-target in one atomic write. `None` leaves
    /// the throttle untouched; use `PATCH /temp-target {target_c: null}`
    /// to disable it. See `PATCH /boards/{name}/temp-target` for the full
    /// design (steps frequency down when the hottest chip exceeds target,
    /// recovers back up to the last commanded frequency).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub temp_target_c: Option<f64>,
}

/// Request body for `PATCH /api/v0/boards/{name}/power-target`.
///
/// Live-edits the power-target voltage loop's target without a reboot.
/// `target_w: null`/absent disables the loop (voltage stays wherever it
/// last was); a present value sets a new target, applied on the loop's
/// next check.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct BoardPowerTargetRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub target_w: Option<f64>,
}

/// Request body for `PATCH /api/v0/boards/{name}/temp-target`.
///
/// Live-edits the temp-target PLL frequency auto-throttle without a
/// reboot. `target_c: null`/absent disables the loop (frequency stays
/// wherever it last was, no automatic recovery); a present value sets a
/// new target, applied on the loop's next status refresh (~15s). Steps
/// PLL frequency down when the hottest chip (`temp_max`) exceeds target
/// and back up when comfortably under, recovering only up to the last
/// dashboard/API-commanded frequency -- distinct from fan control
/// (`PATCH /boards/{name}/fan`/`/fan-curve`), which adjusts fan duty
/// instead and never touches frequency.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct BoardTempTargetRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub target_c: Option<f64>,
}

/// Request body for `PATCH /api/v0/boards/{name}/fan`.
///
/// Live-edits fan mode without a reboot. Fan commands never touch
/// `power_en`.
///
/// `mode`: `"auto"` (the default) runs the fan curve
/// (`PATCH /boards/{name}/fan-curve`) plus the chip-temp escalation that
/// overrides it near the active mode's tuning limit; `"manual"` requires
/// `manual_duty_percent` and holds the fan at that fixed duty (0-100),
/// suspending both the curve and the escalation, until switched back to
/// `"auto"`.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct BoardFanRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mode: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub manual_duty_percent: Option<u8>,
    /// Live-edits the chip-temp thermostat target (see
    /// `mujina_test_harness.c`'s `g_fan_chip_thermostat_target_c` doc
    /// comment) -- the duty the curve+escalation `"auto"` mode
    /// proportionally holds chip temp near, instead of jumping straight
    /// to 100% at the mode's real safety limit. Independent of `mode`;
    /// can be set in the same request or on its own.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub chip_temp_target_c: Option<f64>,
}

/// One point on the fan curve: at `temp_c` outlet temperature, run the
/// fan at `duty_pct`. Points between the ones you set are linearly
/// interpolated by the harness; below the lowest point's temp the curve
/// clamps to that point's duty, above the highest it clamps to that
/// point's duty (never extrapolates).
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct FanCurvePoint {
    pub temp_c: f64,
    pub duty_pct: f64,
}

/// Request body for `PATCH /api/v0/boards/{name}/fan-curve`.
///
/// Sets the fan's outlet-temp baseline curve (2-8 points, any order --
/// the harness sorts by `temp_c` before applying/persisting). This is
/// only the baseline: it's overridden by 100% whenever the hottest chip
/// crosses the active mode's real tuning limit (`temp_target_c`'s design,
/// `PATCH /boards/{name}/temp-target`), independent of outlet temp.
/// Applied live (harness picks it up within ~1s) and persisted, so it
/// survives reboots.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct FanCurveRequest {
    pub points: Vec<FanCurvePoint>,
}

/// Response body for `GET /api/v0/boards/{name}/fan-curve` -- the
/// currently persisted/applied curve, sorted ascending by `temp_c`.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct FanCurveResponse {
    pub points: Vec<FanCurvePoint>,
}

/// Request body for `PATCH /api/v0/boards/{name}/psu`.
///
/// Sets a manual override for the USB-C PD power-contract ceiling used
/// by the power-target safety trip. `override_max_w: null`/absent
/// clears the override, reverting to auto-detection from the measured
/// INA226 bus voltage. The override can only ever tighten the existing
/// hardware ceiling, never loosen it.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct PsuOverrideRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub override_max_w: Option<f64>,
}

/// Response body for `GET /api/v0/boards/{name}/psu`.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct PsuStatusResponse {
    /// Live USB-C PD input rail voltage (INA226 bus voltage), volts.
    /// `null` if the sensor couldn't be read.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub bus_v: Option<f64>,
    /// HUSB238A ATTACH bit -- whether a PD contract is currently
    /// established. `null` if the chip couldn't be read.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub attached: Option<bool>,
    /// Power ceiling classified from `bus_v` alone (see
    /// `pd_ceiling_w_for_voltage`'s doc comment). `null` if `bus_v` is
    /// unavailable.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub detected_ceiling_w: Option<f64>,
    /// The manual override currently in effect, if any.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub override_max_w: Option<f64>,
    /// The ceiling actually enforced by the power-target safety trip:
    /// `min(POWER_TARGET_SAFETY_W, (override_max_w or detected_ceiling_w) - margin)`.
    pub effective_ceiling_w: f64,
}

/// Request body for `PATCH /api/v0/boards/{name}/autotune-hashrate`.
///
/// Enables/edits hashrate-mode autotune: searches frequency+voltage
/// (interpolated between the LOW/MED/HIGH calibration points) for the
/// lowest-power combination that reliably sustains `target_ths`.
/// `target_ths: null`/absent disables it and leaves frequency/voltage
/// wherever they last were. Mutually exclusive with power-target/
/// temp-target -- enabling this disables both; a manual tuning command
/// disables this in turn.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct AutotuneHashrateRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub target_ths: Option<f64>,
}

/// Response body for `GET /api/v0/boards/{name}/autotune-hashrate`.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct AutotuneHashrateResponse {
    pub enabled: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub target_ths: Option<f64>,
    /// Current search position, 0.0 (LOW) to 1.0 (HIGH).
    pub level: f64,
    /// PLL frequency this level currently maps to (domain 0).
    pub level_freq_mhz: u32,
    /// Core voltage this level currently maps to.
    pub level_voltage_mv: u32,
}

/// One line of recent autotune history.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct AutotuneLogLine {
    /// Unix seconds -- clients format this in their own local timezone.
    pub ts: u64,
    pub line: String,
}

/// Response body for `GET /api/v0/boards/{name}/autotune-log`.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct AutotuneLogResponse {
    /// Oldest first, capped to the board driver's own in-memory ring
    /// buffer size -- not a complete history back to process start.
    pub lines: Vec<AutotuneLogLine>,
}

/// Request body for `PATCH /api/v0/boards/{name}/pause`.
///
/// Goes through `board::nano3s::write_pause_command()`: pause sends an
/// IPC message to `rtos_core.elf` before cutting power; resume re-applies
/// the chain's operating frequency and voltage rather than just
/// re-powering it. No reboot required either direction.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct BoardPauseRequest {
    pub paused: bool,
}

/// Request body for `PATCH /api/v0/boards/{name}/led`.
///
/// Live-edits a board's status LED strip, no reboot required. `effect`
/// is one of `"auto"` (default; automatic status indication), `"off"`,
/// `"solid"`, `"rainbow"`, `"colorloop"`, `"breathe"`, `"blink"`,
/// `"chase"`, `"chase_rainbow"`, `"scanner"`, `"twinkle"`, or
/// `"fire_flicker"` -- see `board::nano3s::LedEffect` for what each does.
/// Not every effect uses every field (e.g. `"rainbow"` ignores `color`).
/// Any field left unset keeps its current value -- e.g. a
/// `brightness`-only request doesn't reset `effect`. `color` is a
/// `#RRGGBB` hex string.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct BoardLedRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub effect: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub color: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub brightness: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub speed: Option<u8>,
}

/// Response body for `GET /api/v0/boards/{name}/led` -- the currently
/// commanded LED state. Note this is the last command applied, not
/// necessarily the color on the strip right now: under `effect: "auto"`
/// the strip follows board status independent of `color`/`speed`.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct BoardLedState {
    pub effect: String,
    pub color: String,
    pub brightness: u8,
    pub speed: u8,
}

/// Request body for `PATCH /api/v0/pool`.
///
/// Persists to `/data/userconfig/pool.conf`, taking effect on the miner's
/// next startup -- the stratum client has no hot-reload path, so an
/// in-place pool change isn't possible without dropping and recreating
/// its connection task. Applying a saved change requires a full device
/// reboot (`POST /api/v0/reboot`); restarting just the `mujina-minerd`
/// process leaves the RT-Smart core's IPC handle stale. At least one
/// field must be present. Fields left unset keep their currently
/// persisted value; `password` left unset keeps whatever password is
/// already stored.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct PoolConfigRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub user: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub password: Option<String>,
}

/// Response body for `GET /api/v0/pool` -- the currently persisted pool
/// config (what will apply on the miner's next restart), which is not
/// necessarily what the running process actually connected with at its
/// own last startup (see `GET /api/v0/sources` for that). `password` is
/// never returned in cleartext; `password_set` reports whether one is
/// stored.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct PoolConfigResponse {
    pub url: Option<String>,
    pub user: Option<String>,
    pub password_set: bool,
}

/// Response body for `POST /api/v0/firmware/{target}`.
///
/// By the time this is returned, the binary is already swapped in and a
/// full device reboot is already scheduled -- there is no separate apply
/// step. See the handler's own doc comment for why a reboot (not a
/// process-only restart) always follows a successful upload.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct FirmwareUploadResponse {
    pub ok: bool,
    /// Echoes the `target` path segment ("mujina-minerd" or "harness").
    pub target: String,
    pub size: usize,
    pub detail: String,
}

/// Response body for `POST /api/v0/firmware/bundle`.
///
/// `applied` lists which of `["mujina-minerd", "harness"]` were actually
/// included and swapped in. By the time this is returned every included
/// binary is already applied and a single reboot is already scheduled --
/// see `FirmwareUploadResponse`'s doc comment for why it's a reboot and
/// not a process-only restart.
#[derive(Clone, Debug, Deserialize, Serialize, ToSchema)]
pub struct FirmwareBundleResponse {
    pub ok: bool,
    pub applied: Vec<String>,
    pub detail: String,
}

/// Job source telemetry.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct SourceTelemetry {
    pub name: String,
    /// Connection URL (e.g. "stratum+tcp://pool:3333"), if applicable.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    /// Current share difficulty set by the source.
    #[serde(
        skip_serializing_if = "Option::is_none",
        serialize_with = "serialize_opt_f64_as_integer_when_whole"
    )]
    pub difficulty: Option<f64>,
}

/// Serialize an `Option<f64>` so that whole numbers appear without a
/// fractional part (e.g. `2328` instead of `2328.0`).
fn serialize_opt_f64_as_integer_when_whole<S: serde::Serializer>(
    value: &Option<f64>,
    serializer: S,
) -> Result<S::Ok, S::Error> {
    match value {
        None => serializer.serialize_none(),
        Some(v) if v.fract() == 0.0 && v.is_finite() => serializer.serialize_i64(*v as i64),
        Some(v) => serializer.serialize_f64(*v),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn whole_difficulty_serializes_as_integer() {
        let source = SourceTelemetry {
            difficulty: Some(2048.0),
            ..Default::default()
        };
        let json: serde_json::Value = serde_json::to_value(&source).unwrap();
        assert!(
            json["difficulty"].is_u64(),
            "expected integer, got {}",
            json["difficulty"]
        );
    }

    #[test]
    fn fractional_difficulty_serializes_as_float() {
        let source = SourceTelemetry {
            difficulty: Some(2048.5),
            ..Default::default()
        };
        let json: serde_json::Value = serde_json::to_value(&source).unwrap();
        assert!(
            json["difficulty"].is_f64(),
            "expected float, got {}",
            json["difficulty"]
        );
    }
}
