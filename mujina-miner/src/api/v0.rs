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
    BoardFanRequest, BoardPauseRequest, BoardPowerTargetRequest, BoardTelemetry,
    BoardTuningRequest, MinerPatchRequest, MinerTelemetry, SourceTelemetry,
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
        .routes(routes!(patch_board_fan))
        .routes(routes!(patch_board_pause))
        .routes(routes!(post_reboot))
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
/// `voltage_mv`/`power_target_w` must be set; values are range-checked
/// before being applied.
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

    if req.pll_freq_mhz.is_none() && req.voltage_mv.is_none() && req.power_target_w.is_none() {
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

    #[cfg(feature = "nano3s")]
    {
        crate::board::nano3s::write_tuning_command(req.pll_freq_mhz, req.voltage_mv, req.power_target_w)
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
    if req.mode.is_none() && req.manual_duty_percent.is_none() && req.target_temp_c.is_none() {
        return Err(StatusCode::BAD_REQUEST);
    }
    if let Some(t) = req.target_temp_c
        && !(30.0..=95.0).contains(&t)
    {
        return Err(StatusCode::BAD_REQUEST);
    }

    #[cfg(feature = "nano3s")]
    {
        // Single combined write: the harness polls HARNESS_CONTROL_FILE
        // once/second and each write replaces the file's whole contents, so
        // separate writes would race. Always exactly 3 comma-separated
        // fields (mode, duty, target_temp_c); empty means "don't change".
        // Parsed by mujina_test_harness.c's control_poll_loop().
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
            req.target_temp_c.map(|t| t.to_string()).unwrap_or_default(),
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
