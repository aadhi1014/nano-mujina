//! API v0 endpoints.
//!
//! Version 0 signals an unstable API -- breaking changes are expected
//! until the miner reaches 1.0.

use axum::{
    Json,
    body::Bytes,
    extract::{DefaultBodyLimit, Path, State},
    http::StatusCode,
};
use std::time::Duration;

use tokio::sync::oneshot;
use utoipa_axum::{router::OpenApiRouter, routes};

use super::commands::SchedulerCommand;
use super::server::SharedState;
use crate::api_client::types::{
    BoardFanRequest, BoardLedRequest, BoardLedState, BoardPauseRequest, BoardPowerTargetRequest,
    BoardTelemetry, BoardTempTargetRequest, BoardTuningRequest, FanCurvePoint, FanCurveRequest,
    FanCurveResponse, FirmwareBundleResponse, FirmwareUploadResponse, MinerPatchRequest,
    MinerTelemetry, PoolConfigRequest, PoolConfigResponse, PsuOverrideRequest, PsuStatusResponse,
    SourceTelemetry,
};

/// Upper bound on a firmware upload body -- generous headroom over the
/// real binaries (mujina-minerd ~19MB stripped, the harness ~740KB), just
/// large enough to reject obviously-wrong uploads before they hit disk.
const FIRMWARE_MAX_UPLOAD_BYTES: usize = 64 * 1024 * 1024;
/// Reject anything implausibly small -- catches an empty/truncated upload
/// before it ever reaches the ELF-magic check.
const FIRMWARE_MIN_UPLOAD_BYTES: usize = 64 * 1024;
/// Free space required on the target partition beyond the upload's own
/// size, so a swap never runs the partition to 0 free the way a stacked
/// series of manual deploys did earlier in this project's history.
const FIRMWARE_FREE_SPACE_MARGIN_BYTES: u64 = 2 * 1024 * 1024;

/// Build the v0 API routes with OpenAPI metadata.
pub fn routes() -> OpenApiRouter<SharedState> {
    OpenApiRouter::new()
        .routes(routes!(health))
        .routes(routes!(get_miner, patch_miner))
        .routes(routes!(get_boards))
        .routes(routes!(get_board))
        .routes(routes!(patch_board_tuning))
        .routes(routes!(patch_board_power_target))
        .routes(routes!(patch_board_temp_target))
        .routes(routes!(get_board_psu, patch_board_psu))
        .routes(routes!(patch_board_fan))
        .routes(routes!(get_board_fan_curve, patch_board_fan_curve))
        .routes(routes!(patch_board_pause))
        .routes(routes!(get_board_led, patch_board_led))
        .routes(routes!(post_reboot))
        .routes(routes!(get_pool, patch_pool))
        .routes(routes!(post_firmware_upload))
        .routes(routes!(post_firmware_bundle))
        .routes(routes!(get_sources))
        .routes(routes!(get_source))
        .layer(DefaultBodyLimit::max(FIRMWARE_MAX_UPLOAD_BYTES))
}

/// Health check endpoint.
#[utoipa::path(
    get,
    path = "/health",
    tag = "health",
    responses(
        (status = OK, description = "Server is running", body = String),
    ),
)]
async fn health() -> &'static str {
    "OK"
}

/// Return the current miner state snapshot.
#[utoipa::path(
    get,
    path = "/miner",
    tag = "miner",
    responses(
        (status = OK, description = "Current miner telemetry", body = MinerTelemetry),
    ),
)]
async fn get_miner(State(state): State<SharedState>) -> Json<MinerTelemetry> {
    Json(state.miner_telemetry())
}

/// Apply partial updates to the miner configuration.
#[utoipa::path(
    patch,
    path = "/miner",
    tag = "miner",
    request_body = MinerPatchRequest,
    responses(
        (status = OK, description = "Updated miner telemetry", body = MinerTelemetry),
        (status = INTERNAL_SERVER_ERROR, description = "Command channel error"),
    ),
)]
async fn patch_miner(
    State(state): State<SharedState>,
    Json(req): Json<MinerPatchRequest>,
) -> Result<Json<MinerTelemetry>, StatusCode> {
    if let Some(paused) = req.paused {
        let (tx, rx) = oneshot::channel();
        let cmd = if paused {
            SchedulerCommand::PauseMining { reply: tx }
        } else {
            SchedulerCommand::ResumeMining { reply: tx }
        };
        state
            .scheduler_cmd_tx
            .send(cmd)
            .await
            .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        // Result layers: timeout / channel-closed / command-error.
        let Ok(Ok(Ok(()))) = tokio::time::timeout(Duration::from_secs(5), rx).await else {
            return Err(StatusCode::INTERNAL_SERVER_ERROR);
        };
    }

    Ok(Json(state.miner_telemetry()))
}

/// Return all connected boards.
#[utoipa::path(
    get,
    path = "/boards",
    tag = "boards",
    responses(
        (status = OK, description = "List of connected boards", body = Vec<BoardTelemetry>),
    ),
)]
async fn get_boards(State(state): State<SharedState>) -> Json<Vec<BoardTelemetry>> {
    Json(
        state
            .board_registry
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .boards(),
    )
}

/// Return a single board by name, or 404 if not found.
#[utoipa::path(
    get,
    path = "/boards/{name}",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    responses(
        (status = OK, description = "Board details", body = BoardTelemetry),
        (status = NOT_FOUND, description = "Board not found"),
    ),
)]
async fn get_board(
    State(state): State<SharedState>,
    Path(name): Path<String>,
) -> Result<Json<BoardTelemetry>, StatusCode> {
    state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .find(|b| b.name == name)
        .map(Json)
        .ok_or(StatusCode::NOT_FOUND)
}

/// Apply a live PLL frequency and/or core voltage change over IPC, no
/// reboot required.
///
/// Only implemented for the nano3s board driver (builds without the
/// `nano3s` Cargo feature return 501). At least one of `pll_freq_mhz`/
/// `voltage_mv`/`power_target_w`/`temp_target_c` must be set; values are
/// range-checked before being applied.
#[utoipa::path(
    patch,
    path = "/boards/{name}/tuning",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    request_body = BoardTuningRequest,
    responses(
        (status = OK, description = "Tuning command accepted and applied"),
        (status = BAD_REQUEST, description = "No fields set, or a value is outside the allowed range"),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support live tuning"),
    ),
)]
async fn patch_board_tuning(
    State(state): State<SharedState>,
    Path(name): Path<String>,
    Json(req): Json<BoardTuningRequest>,
) -> Result<StatusCode, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }

    if req.pll_freq_mhz.is_none()
        && req.voltage_mv.is_none()
        && req.power_target_w.is_none()
        && req.temp_target_c.is_none()
    {
        return Err(StatusCode::BAD_REQUEST);
    }
    // Frequency range check; the four PLL ramp domains must be non-decreasing.
    if let Some(f) = req.pll_freq_mhz {
        let in_range = f.iter().all(|&mhz| (100..=500).contains(&mhz));
        let non_decreasing = f[0] <= f[1] && f[1] <= f[2] && f[2] <= f[3];
        if !in_range || !non_decreasing {
            return Err(StatusCode::BAD_REQUEST);
        }
    }
    // Voltage range check.
    if let Some(v) = req.voltage_mv
        && !(3300..=3800).contains(&v)
    {
        return Err(StatusCode::BAD_REQUEST);
    }
    // Same range check as the dedicated power-target endpoint.
    if let Some(w) = req.power_target_w
        && !(20.0..=137.0).contains(&w)
    {
        return Err(StatusCode::BAD_REQUEST);
    }
    // Same range check as the dedicated temp-target endpoint.
    if let Some(c) = req.temp_target_c
        && !(30.0..=95.0).contains(&c)
    {
        return Err(StatusCode::BAD_REQUEST);
    }

    #[cfg(feature = "nano3s")]
    {
        crate::board::nano3s::write_tuning_command(
            req.pll_freq_mhz,
            req.voltage_mv,
            req.power_target_w,
            req.temp_target_c,
        )
        .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        Ok(StatusCode::OK)
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Live-edit the power-target voltage loop's target, no reboot required.
///
/// `target_w: null`/absent disables the loop (voltage stays wherever it
/// last was); a present value takes effect on the loop's next check.
#[utoipa::path(
    patch,
    path = "/boards/{name}/power-target",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    request_body = BoardPowerTargetRequest,
    responses(
        (status = OK, description = "Power target updated (or cleared)"),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support the power-target loop"),
    ),
)]
async fn patch_board_power_target(
    State(state): State<SharedState>,
    Path(name): Path<String>,
    Json(req): Json<BoardPowerTargetRequest>,
) -> Result<StatusCode, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }
    // Range check before the value reaches the power-target loop.
    if let Some(w) = req.target_w
        && !(20.0..=137.0).contains(&w)
    {
        return Err(StatusCode::BAD_REQUEST);
    }

    #[cfg(feature = "nano3s")]
    {
        crate::board::nano3s::write_power_target_command(req.target_w)
            .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        Ok(StatusCode::OK)
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Live-edit the temp-target PLL frequency auto-throttle's target, no
/// reboot required.
///
/// `target_c: null`/absent disables the loop (frequency stays wherever it
/// last was, no automatic recovery); a present value takes effect on the
/// loop's next status refresh (~15s). Steps frequency down when the
/// hottest chip (`temp_max`) exceeds target and back up when comfortably
/// under, recovering only up to the last dashboard/API-commanded
/// frequency. Distinct from `PATCH /boards/{name}/fan`'s
/// `target_temp_c`, which drives fan duty instead and never touches
/// frequency.
#[utoipa::path(
    patch,
    path = "/boards/{name}/temp-target",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    request_body = BoardTempTargetRequest,
    responses(
        (status = OK, description = "Temp target updated (or cleared)"),
        (status = BAD_REQUEST, description = "target_c outside the allowed range"),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support the temp-target loop"),
    ),
)]
async fn patch_board_temp_target(
    State(state): State<SharedState>,
    Path(name): Path<String>,
    Json(req): Json<BoardTempTargetRequest>,
) -> Result<StatusCode, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }
    // Range check before the value reaches the temp-target loop.
    if let Some(c) = req.target_c
        && !(30.0..=95.0).contains(&c)
    {
        return Err(StatusCode::BAD_REQUEST);
    }

    #[cfg(feature = "nano3s")]
    {
        crate::board::nano3s::write_temp_target_command(req.target_c)
            .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        Ok(StatusCode::OK)
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Read the current USB-C PD power-contract state -- live INA226 bus
/// voltage, HUSB238A attach status, the auto-detected contract ceiling,
/// any manual override, and the ceiling actually being enforced.
#[utoipa::path(
    get,
    path = "/boards/{name}/psu",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    responses(
        (status = OK, description = "Current PSU/PD status", body = PsuStatusResponse),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support PD detection"),
    ),
)]
async fn get_board_psu(
    State(state): State<SharedState>,
    Path(name): Path<String>,
) -> Result<Json<PsuStatusResponse>, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }

    #[cfg(feature = "nano3s")]
    {
        let (bus_v, attached, detected_ceiling_w, override_max_w, effective_ceiling_w) =
            crate::board::nano3s::read_psu_status();
        Ok(Json(PsuStatusResponse {
            bus_v,
            attached,
            detected_ceiling_w,
            override_max_w,
            effective_ceiling_w,
        }))
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Set or clear the manual PSU power-contract override -- see
/// `PsuOverrideRequest`'s doc comment. Only ever tightens the ceiling
/// the power-target safety trip enforces, never loosens it.
#[utoipa::path(
    patch,
    path = "/boards/{name}/psu",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    request_body = PsuOverrideRequest,
    responses(
        (status = OK, description = "Override updated (or cleared)"),
        (status = BAD_REQUEST, description = "override_max_w outside the allowed range"),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support PD detection"),
    ),
)]
async fn patch_board_psu(
    State(state): State<SharedState>,
    Path(name): Path<String>,
    Json(req): Json<PsuOverrideRequest>,
) -> Result<StatusCode, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }
    if let Some(w) = req.override_max_w
        && !(15.0..=140.0).contains(&w)
    {
        return Err(StatusCode::BAD_REQUEST);
    }

    #[cfg(feature = "nano3s")]
    {
        crate::board::nano3s::write_psu_override_command(req.override_max_w)
            .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        Ok(StatusCode::OK)
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Live-edit fan control, no reboot required.
///
/// Fan commands never touch `power_en`, so they can be sent at any time
/// without affecting the mining chain.
#[utoipa::path(
    patch,
    path = "/boards/{name}/fan",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    request_body = BoardFanRequest,
    responses(
        (status = OK, description = "Fan command accepted and applied"),
        (status = BAD_REQUEST, description = "mode=\"manual\" without manual_duty_percent, an invalid mode string, or a value out of range"),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support live fan control"),
    ),
)]
async fn patch_board_fan(
    State(state): State<SharedState>,
    Path(name): Path<String>,
    Json(req): Json<BoardFanRequest>,
) -> Result<StatusCode, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }
    if req.mode.is_none() && req.manual_duty_percent.is_none() {
        return Err(StatusCode::BAD_REQUEST);
    }

    #[cfg(feature = "nano3s")]
    {
        // Always exactly 2 comma-separated fields (mode, duty); empty
        // means "don't change". Parsed by mujina_test_harness.c's
        // control_poll_loop().
        let pct = match req.mode.as_deref() {
            Some("manual") => Some(req.manual_duty_percent.ok_or(StatusCode::BAD_REQUEST)?),
            Some("auto") => None,
            Some(_) => return Err(StatusCode::BAD_REQUEST),
            None => None,
        };
        if req.mode.as_deref() == Some("manual") && pct.is_some_and(|p| p > 100) {
            return Err(StatusCode::BAD_REQUEST);
        }
        let fields = [
            req.mode.clone().unwrap_or_default(),
            pct.map(|p| p.to_string()).unwrap_or_default(),
        ];
        crate::board::nano3s::write_fan_control_command(&format!("fan:{}", fields.join(",")))
            .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        Ok(StatusCode::OK)
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Return the currently persisted/applied fan curve.
#[utoipa::path(
    get,
    path = "/boards/{name}/fan-curve",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    responses(
        (status = OK, description = "Current fan curve points, sorted ascending by temp_c", body = FanCurveResponse),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support a fan curve"),
    ),
)]
async fn get_board_fan_curve(
    State(state): State<SharedState>,
    Path(name): Path<String>,
) -> Result<Json<FanCurveResponse>, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }

    #[cfg(feature = "nano3s")]
    {
        let points = crate::board::nano3s::read_fan_curve()
            .into_iter()
            .map(|(temp_c, duty_pct)| FanCurvePoint { temp_c, duty_pct })
            .collect();
        Ok(Json(FanCurveResponse { points }))
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Set the fan's outlet-temp baseline curve, no reboot required.
///
/// 2-8 points, any order (sorted by `temp_c` before being applied). This
/// is only the baseline -- it's overridden by 100% whenever the hottest
/// chip crosses the active mode's real tuning limit
/// (`PATCH /boards/{name}/temp-target`'s design), independent of outlet
/// temp. Applied live within ~1s and persisted across reboots.
#[utoipa::path(
    patch,
    path = "/boards/{name}/fan-curve",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    request_body = FanCurveRequest,
    responses(
        (status = OK, description = "Fan curve updated", body = FanCurveResponse),
        (status = BAD_REQUEST, description = "Fewer than 2 or more than 8 points, or a point outside the allowed range"),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support a fan curve"),
    ),
)]
async fn patch_board_fan_curve(
    State(state): State<SharedState>,
    Path(name): Path<String>,
    Json(req): Json<FanCurveRequest>,
) -> Result<Json<FanCurveResponse>, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }
    if req.points.len() < 2 || req.points.len() > 8 {
        return Err(StatusCode::BAD_REQUEST);
    }
    for p in &req.points {
        if !(0.0..=150.0).contains(&p.temp_c) || !(0.0..=100.0).contains(&p.duty_pct) {
            return Err(StatusCode::BAD_REQUEST);
        }
    }

    #[cfg(feature = "nano3s")]
    {
        let points: Vec<(f64, f64)> = req.points.iter().map(|p| (p.temp_c, p.duty_pct)).collect();
        crate::board::nano3s::write_fan_curve_command(&points)
            .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        let mut sorted = req.points;
        sorted.sort_by(|a, b| a.temp_c.total_cmp(&b.temp_c));
        Ok(Json(FanCurveResponse { points: sorted }))
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Pause or resume mining, no reboot required.
///
/// Goes through `board::nano3s::write_pause_command()`, an
/// IPC-coordinated path distinct from the generic
/// `PATCH /api/v0/miner {"paused": ...}` endpoint (which only sets a
/// scheduler flag and does not reach hardware for this board). Pause
/// idles the chain; resume reapplies its operating frequency and voltage.
#[utoipa::path(
    patch,
    path = "/boards/{name}/pause",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    request_body = BoardPauseRequest,
    responses(
        (status = OK, description = "Pause/resume command accepted"),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support live pause control"),
    ),
)]
async fn patch_board_pause(
    State(state): State<SharedState>,
    Path(name): Path<String>,
    Json(req): Json<BoardPauseRequest>,
) -> Result<StatusCode, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }

    #[cfg(feature = "nano3s")]
    {
        crate::board::nano3s::write_pause_command(req.paused)
            .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
        Ok(StatusCode::OK)
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Return the currently commanded LED state.
#[utoipa::path(
    get,
    path = "/boards/{name}/led",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    responses(
        (status = OK, description = "Current LED state", body = BoardLedState),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support LED control"),
    ),
)]
async fn get_board_led(
    State(state): State<SharedState>,
    Path(name): Path<String>,
) -> Result<Json<BoardLedState>, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }

    #[cfg(feature = "nano3s")]
    {
        Ok(Json(crate::board::nano3s::get_led_state()))
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Live-edit the status LED strip, no reboot required.
///
/// `effect="auto"` (the default) returns the strip to automatic status
/// indication; any other effect hands it to manual control until
/// switched back. Fields left unset keep their current value.
#[utoipa::path(
    patch,
    path = "/boards/{name}/led",
    tag = "boards",
    params(
        ("name" = String, Path, description = "Board name"),
    ),
    request_body = BoardLedRequest,
    responses(
        (status = OK, description = "LED command accepted and applied"),
        (status = BAD_REQUEST, description = "Unknown effect name or malformed color"),
        (status = NOT_FOUND, description = "Board not found"),
        (status = NOT_IMPLEMENTED, description = "This build's board driver doesn't support LED control"),
    ),
)]
async fn patch_board_led(
    State(state): State<SharedState>,
    Path(name): Path<String>,
    Json(req): Json<BoardLedRequest>,
) -> Result<StatusCode, StatusCode> {
    let known = state
        .board_registry
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .boards()
        .into_iter()
        .any(|b| b.name == name);
    if !known {
        return Err(StatusCode::NOT_FOUND);
    }

    #[cfg(feature = "nano3s")]
    {
        crate::board::nano3s::write_led_command(req.effect, req.color, req.brightness, req.speed)
            .map_err(|_| StatusCode::BAD_REQUEST)?;
        Ok(StatusCode::OK)
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

/// Reboot the device.
///
/// Shells out to the system `reboot` command. Responds `OK` immediately;
/// the actual reboot is spawned after a short delay on a detached task so
/// the HTTP response reaches the client before the connection drops.
#[utoipa::path(
    post,
    path = "/reboot",
    tag = "miner",
    responses(
        (status = OK, description = "Reboot initiated"),
    ),
)]
async fn post_reboot() -> StatusCode {
    tokio::spawn(async {
        tokio::time::sleep(Duration::from_millis(300)).await;
        let _ = std::process::Command::new("reboot").status();
    });
    StatusCode::OK
}

/// Live path this binary is actually deployed to, and its holding
/// filesystem -- used for the pre-write free-space check.
fn firmware_target_path(target: &str) -> Option<(&'static str, &'static str)> {
    match target {
        "mujina-minerd" => Some(("/data/mujina-minerd", "/data")),
        "harness" => Some(("/mntapp/release/linux/app/mujina_test_harness", "/mntapp")),
        _ => None,
    }
}

/// Free space on `mount_point`, in bytes, via `df -k` -- no new crate
/// dependency, consistent with this codebase's existing pattern of
/// shelling out to system tools (i2cset, reboot, etc.) for one-off system
/// queries rather than linking libc/statvfs directly.
fn free_space_bytes(mount_point: &str) -> Option<u64> {
    let out = std::process::Command::new("df").arg("-k").arg(mount_point).output().ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    let fields: Vec<&str> = text.lines().nth(1)?.split_whitespace().collect();
    // `df -k` columns: Filesystem, 1K-blocks, Used, Available, Use%, Mounted.
    fields.get(3)?.parse::<u64>().ok().map(|kb| kb * 1024)
}

/// Upload and apply a new `mujina-minerd` or harness binary, then reboot.
///
/// `target` is `mujina-minerd` or `harness`. Body is the raw ELF binary
/// (`application/octet-stream`), no multipart wrapper. Validates ELF magic
/// and, for `mujina-minerd` specifically, the presence of `nano3s_ipc_`
/// symbol strings -- the same sanity check `build_mujina_minerd.sh` itself
/// runs, catching a build that silently compiled out the `nano3s` Cargo
/// feature (a real, previously-hit failure mode: the binary builds and
/// runs, but the board driver is missing entirely). Writes to `<path>.new`
/// first, checking free space beforehand so a swap never runs the target
/// partition to 0 bytes free.
///
/// One-step by design: on success, the binary is already swapped in when
/// this responds, and a full device reboot is already in flight (not a
/// process-only restart -- swapping either binary in place and only
/// killing the process leaves the RT-Smart core's IPC handle stale, a
/// real, previously-hit failure mode for `mujina-minerd`; the harness
/// holds no such handle but reboots the same way for consistency and
/// because a fresh boot is the only state this endpoint has actually
/// verified working). There is no confirmation step -- a successful
/// upload commits to applying it.
#[utoipa::path(
    post,
    path = "/firmware/{target}",
    tag = "miner",
    params(
        ("target" = String, Path, description = "\"mujina-minerd\" or \"harness\""),
    ),
    request_body(content = Vec<u8>, content_type = "application/octet-stream"),
    responses(
        (status = OK, description = "Binary swapped in, reboot initiated", body = FirmwareUploadResponse),
        (status = BAD_REQUEST, description = "Unknown target, empty/oversized body, bad ELF magic, or (mujina-minerd) missing nano3s feature symbols"),
        (status = INSUFFICIENT_STORAGE, description = "Not enough free space on the target partition"),
        (status = INTERNAL_SERVER_ERROR, description = "Write or rename failed"),
    ),
)]
async fn post_firmware_upload(
    Path(target): Path<String>,
    body: Bytes,
) -> Result<Json<FirmwareUploadResponse>, (StatusCode, String)> {
    let live_path = validate_and_stage_firmware(&target, &body)?;
    std::fs::rename(&format!("{live_path}.new"), &live_path)
        .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, format!("failed to swap in {live_path}: {e}")))?;

    let size = body.len();
    schedule_reboot();

    Ok(Json(FirmwareUploadResponse {
        ok: true,
        target,
        size,
        detail: "swapped in, rebooting now".to_string(),
    }))
}

/// Validates `body` for `target` (unknown target, size bounds, ELF magic,
/// and -- for `mujina-minerd` -- `nano3s_ipc_*` symbols) and free space on
/// the target partition, then writes `body` to `<live_path>.new` (mode
/// 0755). Does **not** rename it into place -- callers do that themselves
/// once every binary in a batch has staged successfully, so a bundle
/// upload can't apply the first binary and then fail on the second,
/// leaving the pair mismatched. Returns the live path on success.
fn validate_and_stage_firmware(target: &str, body: &[u8]) -> Result<String, (StatusCode, String)> {
    let Some((live_path, mount_point)) = firmware_target_path(target) else {
        return Err((StatusCode::BAD_REQUEST, format!("unknown target {target:?} (want \"mujina-minerd\" or \"harness\")")));
    };

    if body.len() < FIRMWARE_MIN_UPLOAD_BYTES {
        return Err((StatusCode::BAD_REQUEST, format!("{target}: upload too small ({} bytes) -- looks empty or truncated", body.len())));
    }
    if body.len() > FIRMWARE_MAX_UPLOAD_BYTES {
        return Err((StatusCode::BAD_REQUEST, format!("{target}: upload too large ({} bytes)", body.len())));
    }
    if body.len() < 4 || &body[0..4] != b"\x7fELF" {
        return Err((StatusCode::BAD_REQUEST, format!("{target}: not an ELF binary (bad magic)")));
    }
    if target == "mujina-minerd" {
        let has_nano3s_symbols = body.windows(b"nano3s_ipc_".len()).any(|w| w == b"nano3s_ipc_");
        if !has_nano3s_symbols {
            return Err((
                StatusCode::BAD_REQUEST,
                format!(
                    "{target}: no nano3s_ipc_* symbols found in this binary -- it was very likely built \
                     without the nano3s Cargo feature and would silently run with no board driver at all"
                ),
            ));
        }
    }

    let needed = body.len() as u64 + FIRMWARE_FREE_SPACE_MARGIN_BYTES;
    match free_space_bytes(mount_point) {
        Some(free) if free < needed => {
            return Err((
                StatusCode::INSUFFICIENT_STORAGE,
                format!(
                    "{target}: not enough free space on {mount_point}: {free} bytes free, need {needed} \
                     (upload size + {FIRMWARE_FREE_SPACE_MARGIN_BYTES}-byte margin)"
                ),
            ));
        }
        Some(_) => {}
        None => {
            return Err((StatusCode::INTERNAL_SERVER_ERROR, format!("{target}: couldn't determine free space on {mount_point}")));
        }
    }

    let staged_path = format!("{live_path}.new");
    std::fs::write(&staged_path, body).map_err(|e| {
        let _ = std::fs::remove_file(&staged_path);
        (StatusCode::INTERNAL_SERVER_ERROR, format!("{target}: failed to write {staged_path}: {e}"))
    })?;
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&staged_path, std::fs::Permissions::from_mode(0o755)).map_err(|e| {
            let _ = std::fs::remove_file(&staged_path);
            (StatusCode::INTERNAL_SERVER_ERROR, format!("{target}: failed to chmod {staged_path}: {e}"))
        })?;
    }

    Ok(live_path.to_string())
}

fn schedule_reboot() {
    tokio::spawn(async {
        tokio::time::sleep(Duration::from_millis(300)).await;
        let _ = std::process::Command::new("reboot").status();
    });
}

/// Parses a `POST /firmware/bundle` body into its (up to 2) sections.
///
/// Format (chosen to avoid pulling in a tar/zip crate for two files):
/// 8-byte magic `b"MJBUNDL1"`, then for each of mujina-minerd and harness
/// in that fixed order: a 4-byte little-endian length followed by that
/// many bytes (length 0 means "not included in this bundle" -- the
/// bundle only needs to touch one binary if that's all the caller wants
/// to change, while still going through the single-reboot bundle path).
/// See `pack_firmware_bundle.py` for the matching packer.
fn parse_firmware_bundle(body: &[u8]) -> Result<(Option<&[u8]>, Option<&[u8]>), (StatusCode, String)> {
    const MAGIC: &[u8] = b"MJBUNDL1";
    let bad = |msg: &str| (StatusCode::BAD_REQUEST, format!("malformed bundle: {msg}"));

    if body.len() < MAGIC.len() || &body[..MAGIC.len()] != MAGIC {
        return Err(bad("bad magic (expected \"MJBUNDL1\") -- build the bundle with pack_firmware_bundle.py, don't concatenate the binaries by hand"));
    }
    let mut cursor = MAGIC.len();
    let mut sections = [None, None];
    for section in sections.iter_mut() {
        let Some(len_bytes) = body.get(cursor..cursor + 4) else {
            return Err(bad("truncated -- missing a length field"));
        };
        let len = u32::from_le_bytes(len_bytes.try_into().unwrap()) as usize;
        cursor += 4;
        if len > 0 {
            let Some(data) = body.get(cursor..cursor + len) else {
                return Err(bad("truncated -- section length runs past end of file"));
            };
            *section = Some(data);
            cursor += len;
        }
    }
    let [mujina_minerd, harness] = sections;
    if mujina_minerd.is_none() && harness.is_none() {
        return Err(bad("both sections empty -- nothing to apply"));
    }
    Ok((mujina_minerd, harness))
}

/// Upload one file that updates mujina-minerd and/or the harness in a
/// single reboot, instead of running the single-target endpoint twice
/// (which would reboot twice -- once per upload). See
/// `parse_firmware_bundle`'s doc comment for the file format and
/// `pack_firmware_bundle.py` for the packer that builds one from your
/// locally-built binaries.
///
/// Both included binaries are fully validated (same checks as the
/// single-target endpoint) and staged to `<path>.new` *before* either is
/// renamed into place, so a bad second binary can't leave the pair
/// mismatched -- either both apply or neither does.
#[utoipa::path(
    post,
    path = "/firmware/bundle",
    tag = "miner",
    request_body(content = Vec<u8>, content_type = "application/octet-stream"),
    responses(
        (status = OK, description = "All included binaries swapped in, reboot initiated", body = FirmwareBundleResponse),
        (status = BAD_REQUEST, description = "Malformed bundle, or a section failed the same checks the single-target endpoint runs"),
        (status = INSUFFICIENT_STORAGE, description = "Not enough free space for one of the included binaries"),
        (status = INTERNAL_SERVER_ERROR, description = "Write, chmod, or rename failed"),
    ),
)]
async fn post_firmware_bundle(body: Bytes) -> Result<Json<FirmwareBundleResponse>, (StatusCode, String)> {
    let (mujina_minerd, harness) = parse_firmware_bundle(&body)?;

    // Validate + stage everything first; only rename (apply) once every
    // included section has staged successfully.
    let mut staged: Vec<&'static str> = Vec::new();
    if let Some(data) = mujina_minerd {
        validate_and_stage_firmware("mujina-minerd", data)?;
        staged.push("mujina-minerd");
    }
    if let Some(data) = harness {
        validate_and_stage_firmware("harness", data)?;
        staged.push("harness");
    }

    for target in &staged {
        let (live_path, _) = firmware_target_path(target).expect("validated above");
        std::fs::rename(&format!("{live_path}.new"), live_path)
            .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, format!("{target}: staged but failed to swap in {live_path}: {e}")))?;
    }

    schedule_reboot();

    Ok(Json(FirmwareBundleResponse {
        ok: true,
        applied: staged.iter().map(|s| s.to_string()).collect(),
        detail: "swapped in, rebooting now".to_string(),
    }))
}

/// Return the currently persisted pool configuration.
///
/// This is what will apply on the miner's *next* restart, not necessarily
/// what the running process connected with at its own last startup -- see
/// `GET /api/v0/sources` for the live connection's actual URL.
#[utoipa::path(
    get,
    path = "/pool",
    tag = "miner",
    responses(
        (status = OK, description = "Currently persisted pool config", body = PoolConfigResponse),
    ),
)]
async fn get_pool() -> Json<PoolConfigResponse> {
    let cfg = crate::pool_config::load();
    Json(PoolConfigResponse {
        url: cfg.url,
        user: cfg.user,
        password_set: cfg.pass.is_some(),
    })
}

/// Update the persisted pool configuration.
///
/// Saved to `/data/userconfig/pool.conf` immediately; takes effect on the
/// miner's next startup since the stratum client has no hot-reload path.
/// Applying it requires a full device reboot (`POST /api/v0/reboot`), not
/// just restarting the `mujina-minerd` process -- killing and relaunching
/// the process in place leaves the RT-Smart core's IPC handle stale
/// (hashrate stuck at 0 even though the API looks healthy), so this API
/// deliberately does not offer a lighter process-only restart.
#[utoipa::path(
    patch,
    path = "/pool",
    tag = "miner",
    request_body = PoolConfigRequest,
    responses(
        (status = OK, description = "Pool config saved", body = PoolConfigResponse),
        (status = BAD_REQUEST, description = "No fields set, or url isn't a stratum+tcp://\
            or stratum+ssl:// URL"),
        (status = INTERNAL_SERVER_ERROR, description = "Failed to write the config file"),
    ),
)]
async fn patch_pool(Json(req): Json<PoolConfigRequest>) -> Result<Json<PoolConfigResponse>, StatusCode> {
    if req.url.is_none() && req.user.is_none() && req.password.is_none() {
        return Err(StatusCode::BAD_REQUEST);
    }
    if let Some(url) = &req.url
        && !(url.starts_with("stratum+tcp://") || url.starts_with("stratum+ssl://"))
    {
        return Err(StatusCode::BAD_REQUEST);
    }

    let cfg = crate::pool_config::save_merged(req.url.as_deref(), req.user.as_deref(), req.password.as_deref())
        .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    Ok(Json(PoolConfigResponse {
        url: cfg.url,
        user: cfg.user,
        password_set: cfg.pass.is_some(),
    }))
}

/// Return all registered job sources.
#[utoipa::path(
    get,
    path = "/sources",
    tag = "sources",
    responses(
        (status = OK, description = "List of job sources", body = Vec<SourceTelemetry>),
    ),
)]
async fn get_sources(State(state): State<SharedState>) -> Json<Vec<SourceTelemetry>> {
    Json(state.miner_telemetry_rx.borrow().sources.clone())
}

/// Return a single source by name, or 404 if not found.
#[utoipa::path(
    get,
    path = "/sources/{name}",
    tag = "sources",
    params(
        ("name" = String, Path, description = "Source name"),
    ),
    responses(
        (status = OK, description = "Source details", body = SourceTelemetry),
        (status = NOT_FOUND, description = "Source not found"),
    ),
)]
async fn get_source(
    State(state): State<SharedState>,
    Path(name): Path<String>,
) -> Result<Json<SourceTelemetry>, StatusCode> {
    state
        .miner_telemetry_rx
        .borrow()
        .sources
        .iter()
        .find(|s| s.name == name)
        .cloned()
        .map(Json)
        .ok_or(StatusCode::NOT_FOUND)
}
