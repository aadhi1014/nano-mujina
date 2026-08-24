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

/// Request body for `PATCH /api/v0/boards/{name}/fan`.
///
/// Live-edits the fan PID controller (`mujina_test_harness.c`'s
/// `fan_pid_control()`) without a reboot. Fan commands never touch
/// `power_en`.
///
/// `mode`: `"auto"` runs PID control (the default); `"manual"` requires
/// `manual_duty_percent` and holds the fan at that fixed duty (0-100)
/// until switched back to `"auto"`. `target_temp_c` overrides the PID's
/// target temperature (default 85C) independent of `mode`.
#[derive(Clone, Debug, Default, Deserialize, Serialize, ToSchema)]
pub struct BoardFanRequest {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mode: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub manual_duty_percent: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub target_temp_c: Option<f64>,
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
