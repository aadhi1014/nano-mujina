//! API v0 endpoints.
//!
//! Version 0 signals an unstable API -- breaking changes are expected
//! until the miner reaches 1.0.

use axum::{
    Json,
    extract::{Path, State},
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
    FanCurveResponse, MinerPatchRequest, MinerTelemetry, PoolConfigRequest, PoolConfigResponse,
    SourceTelemetry,
};

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
        .routes(routes!(patch_board_fan))
        .routes(routes!(get_board_fan_curve, patch_board_fan_curve))
        .routes(routes!(patch_board_pause))
        .routes(routes!(get_board_led, patch_board_led))
        .routes(routes!(post_reboot))
        .routes(routes!(get_pool, patch_pool))
        .routes(routes!(get_sources))
        .routes(routes!(get_source))
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
        && !(20.0..=130.0).contains(&w)
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
        && !(20.0..=130.0).contains(&w)
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
