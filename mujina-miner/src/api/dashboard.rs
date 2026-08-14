//! Static dashboard UI plus a `/data` endpoint reshaping [`MinerTelemetry`]
//! into the JSON shape the `mujina-dashboard` frontend expects.
//!
//! Serves `/dashboard` (the page) and `/data` (its polling endpoint).
//! Read-only: no settings/tuning/autotune/restart endpoints. Fan/tuning
//! control for this board goes through `mujina_test_harness.c`'s
//! file-based mechanism instead.

use std::collections::{HashMap, VecDeque};

use axum::{
    Json,
    http::StatusCode,
    response::{Html, IntoResponse},
    routing, Router,
};
use serde_json::{json, Value};

use super::server::SharedState;
use crate::api_client::types::BoardTelemetry;

const DASHBOARD_HTML: &str = include_str!("../../assets/dashboard.html");
/// Per-chip and chain-wide detail page, separate from `/dashboard`'s
/// fleet-overview cards.
const INFO_HTML: &str = include_str!("../../assets/info.html");
/// Header brandmark -- mujina-head-mark.svg from rkuester's
/// mujina-logo-set (github.com/rkuester/mujina-logo-set), used with the
/// author's permission. Served from its own route rather than inlined
/// into DASHBOARD_HTML: at ~320KB it would otherwise be retransmitted on
/// every `/dashboard` load, where inlining a static asset that never
/// changes buys nothing.
const LOGO_SVG: &str = include_str!("../../assets/mujina-head-mark.svg");

/// How many samples the fleet/board hashrate charts keep. Sampled once per
/// `/data` poll -- the only caller is the dashboard's own `tick()`, on a
/// 2s interval, so this is ~5 minutes of history. Not a fixed-clock
/// background sampler: history only accumulates while something is
/// actually watching, which is exactly what a live chart needs and avoids
/// a spurious task running for a device that's rarely viewed.
const HISTORY_LEN: usize = 150;

/// Rolling hashrate history behind the fleet/board charts, keyed by board
/// name for the per-board series. Lives in [`SharedState`] behind a
/// `Mutex` so it survives across polls; the frontend previously always
/// got an empty `series` array here, because nothing kept any history at
/// all between polls, not because of a wiring bug.
#[derive(Default)]
pub(crate) struct HashrateHistory {
    fleet: VecDeque<f64>,
    boards: HashMap<String, VecDeque<f64>>,
}

impl HashrateHistory {
    /// Appends `ghs` to the fleet series and returns a snapshot.
    fn push_fleet(&mut self, ghs: f64) -> Vec<f64> {
        push_capped(&mut self.fleet, ghs);
        self.fleet.iter().copied().collect()
    }

    /// Appends `ghs` to `board_name`'s series and returns a snapshot.
    fn push_board(&mut self, board_name: &str, ghs: f64) -> Vec<f64> {
        let buf = self.boards.entry(board_name.to_string()).or_default();
        push_capped(buf, ghs);
        buf.iter().copied().collect()
    }
}

fn push_capped(buf: &mut VecDeque<f64>, v: f64) {
    buf.push_back(v);
    while buf.len() > HISTORY_LEN {
        buf.pop_front();
    }
}

pub fn routes() -> Router<SharedState> {
    Router::new()
        .route("/dashboard", routing::get(serve_page))
        .route("/data", routing::get(serve_data))
        .route("/info", routing::get(serve_info_page))
        .route("/nano3s-detail", routing::get(serve_nano3s_detail))
        .route("/mujina-head-mark.svg", routing::get(serve_logo_svg))
        .route("/doom/launch", routing::post(launch_doom))
}

async fn serve_page() -> impl IntoResponse {
    Html(DASHBOARD_HTML)
}

async fn serve_logo_svg() -> impl IntoResponse {
    (
        [
            (axum::http::header::CONTENT_TYPE, "image/svg+xml"),
            (axum::http::header::CACHE_CONTROL, "public, max-age=86400"),
        ],
        LOGO_SVG,
    )
}

/// Dashboard easter egg: five clicks on the header logo launches
/// `nano3s_doom` and switches the panel to it (see nano3s_doom/README.md).
/// Runs entirely on-device -- unlike the standalone `mujina-dashboard`
/// Python tool's equivalent action, which has to SSH in from wherever it's
/// running, this handler already *is* on the Nano3s, so it just shells
/// out locally. Kills any already-running instance first since the
/// gamepad's own HTTP server can't rebind :8080 otherwise; `killall`, not
/// `pkill -f`, because this device's busybox has no pkill.
async fn launch_doom() -> Json<Value> {
    let cmd = "killall nano3s_doom 2>/dev/null; sleep 1; \
               echo doom > /mntapp/release/linux/app/fb_page; \
               cd /data && nohup ./nano3s_doom -iwad /data/doom1.wad -gfxmode rgb565 \
               > /data/nano3s_doom.log 2>&1 &";
    match tokio::process::Command::new("sh").arg("-c").arg(cmd).output().await {
        Ok(out) if out.status.success() => Json(json!({"ok": true, "detail": "launched"})),
        Ok(out) => Json(json!({
            "ok": false,
            "detail": String::from_utf8_lossy(&out.stderr).trim().to_string(),
        })),
        Err(e) => Json(json!({"ok": false, "detail": e.to_string()})),
    }
}

async fn serve_info_page() -> impl IntoResponse {
    Html(INFO_HTML)
}

/// Per-chip and chain-wide detail from the latest `IPC_MSG_STATUS`.
/// `501` on builds without the `nano3s` feature; `503` if the feature is
/// present but no STATUS has arrived yet.
async fn serve_nano3s_detail() -> Result<Json<Value>, StatusCode> {
    #[cfg(feature = "nano3s")]
    {
        crate::board::nano3s::get_detail_snapshot()
            .map(|d| Json(serde_json::to_value(d).unwrap_or(Value::Null)))
            .ok_or(StatusCode::SERVICE_UNAVAILABLE)
    }
    #[cfg(not(feature = "nano3s"))]
    {
        Err(StatusCode::NOT_IMPLEMENTED)
    }
}

async fn serve_data(
    axum::extract::State(state): axum::extract::State<SharedState>,
) -> Json<Value> {
    let m = state.miner_telemetry();

    let hr_ghs = m.hashrate as f64 / 1e9;
    let src = m.sources.first();

    let boards: Vec<Value> = {
        let mut history = state
            .hashrate_history
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        m.boards
            .iter()
            .map(|b| {
                let series = history.push_board(&b.name, board_hashrate_ghs(b));
                board_json(b, series)
            })
            .collect()
    };
    let fleet_series = state
        .hashrate_history
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .push_fleet(hr_ghs);

    let total_power: f64 = m
        .boards
        .iter()
        .filter_map(|b| core_power_w(b))
        .sum();
    let have_power = m.boards.iter().any(|b| core_power_w(b).is_some());

    let fan_rpms: Vec<f64> = boards
        .iter()
        .flat_map(|b| b["fans"].as_array().into_iter().flatten())
        .filter_map(|f| f["rpm"].as_f64())
        .collect();
    let avg_fan_rpm = if fan_rpms.is_empty() {
        None
    } else {
        Some(round1(fan_rpms.iter().sum::<f64>() / fan_rpms.len() as f64))
    };

    Json(json!({
        "ok": true,
        "boards": boards,
        "fleet": {
            "hashrate_ghs": round2(hr_ghs),
            "hashrate_ths": round3(hr_ghs / 1000.0),
            "boards_online": m.boards.len(),
            "total_power_w": if have_power { Some(round1(total_power)) } else { None },
            "efficiency_jth": if have_power && hr_ghs > 0.0 {
                Some(round2(total_power / (hr_ghs / 1000.0)))
            } else {
                None
            },
            "avg_fail_rate_pct": avg_field(&boards, "fail_rate_pct"),
            "avg_freq_mhz": avg_field(&boards, "freq_mhz"),
            "avg_core_v": avg_field(&boards, "core_v"),
            "avg_asic_c": avg_field(&boards, "asic_c"),
            "avg_fan_rpm": avg_fan_rpm,
            "series": fleet_series,
        },
        "shares": m.shares_submitted,
        "best_diff": boards
            .iter()
            .filter_map(|b| b["best_share_diff"].as_f64())
            .fold(None::<f64>, |best, v| Some(best.map_or(v, |b| b.max(v))))
            .map(fmt_diff)
            .unwrap_or_else(|| "—".into()),
        "uptime": fmt_uptime(m.uptime_secs),
        "paused": m.paused,
        "miner_name": Value::Null,
        "pool_name": src.map(|s| s.name.clone()),
        "pool_url": src.and_then(|s| s.url.clone()),
        "difficulty": src.and_then(|s| s.difficulty).map(fmt_diff).unwrap_or_else(|| "—".into()),
    }))
}

/// Mean of `key` across boards that have it set, rounded to 2 places, or
/// `None` if no board has a value.
fn avg_field(boards: &[Value], key: &str) -> Option<f64> {
    let vals: Vec<f64> = boards.iter().filter_map(|b| b[key].as_f64()).collect();
    if vals.is_empty() {
        None
    } else {
        Some(round2(vals.iter().sum::<f64>() / vals.len() as f64))
    }
}

fn core_power_w(b: &BoardTelemetry) -> Option<f64> {
    b.powers
        .iter()
        .find(|p| p.name == "core")
        .and_then(|p| p.power_w)
        .map(|w| w as f64)
}

fn board_hashrate_ghs(b: &BoardTelemetry) -> f64 {
    let hr_hs: u64 = b.threads.iter().map(|t| t.hashrate).sum();
    hr_hs as f64 / 1e9
}

fn board_json(b: &BoardTelemetry, series: Vec<f64>) -> Value {
    let hr_ghs = board_hashrate_ghs(b);

    let core = b.powers.iter().find(|p| p.name == "core");
    let core_w = core.and_then(|p| p.power_w).map(|w| w as f64);
    let core_v = core.and_then(|p| p.voltage_v).map(|v| v as f64);
    let core_a = core.and_then(|p| p.current_a).map(|a| a as f64);

    let asic_c = b
        .temperatures
        .iter()
        .find(|t| t.name == "asic")
        .and_then(|t| t.temperature)
        .map(|t| t.as_degrees_c() as f64);

    // Per-chip fields, commanded PLL frequency, and the USB-C PD input rail
    // don't exist on the generic BoardTelemetry -- pulled from the
    // nano3s-specific detail snapshot instead. nano3s also has no discrete
    // VR temperature sensor, so that card slot is repurposed for
    // SmartSpeed fail rate, a real, currently-unsurfaced per-chain health
    // metric, rather than showing a permanent "--". Other board types
    // simply get empty/null here, same as before.
    #[cfg(feature = "nano3s")]
    let (chips, fail_rate_pct, freq_mhz, input_v, best_share_diff) =
        match crate::board::nano3s::get_detail_snapshot() {
            Some(detail) => {
                let chips: Vec<Value> = detail
                    .chips
                    .iter()
                    .map(|c| json!({ "index": c.chip, "ghs": c.ghsspd, "temp_c": c.temp_c }))
                    .collect();
                (
                    chips,
                    Some(detail.spd_dh as f64),
                    Some(detail.pll_freq_commanded[0] as f64),
                    Some(detail.ina_bus_v),
                    (detail.best_share_diff > 0.0).then_some(detail.best_share_diff),
                )
            }
            None => (Vec::new(), None, None, None, None),
        };
    #[cfg(not(feature = "nano3s"))]
    #[allow(clippy::type_complexity)]
    let (chips, fail_rate_pct, freq_mhz, input_v, best_share_diff): (
        Vec<Value>,
        Option<f64>,
        Option<f64>,
        Option<f64>,
        Option<f64>,
    ) = (Vec::new(), None, None, None, None);

    json!({
        "board_name": b.name,
        "model": b.model,
        "serial": b.serial,
        "hashrate_ghs": round2(hr_ghs),
        "hashrate_ths": round3(hr_ghs / 1000.0),
        "series": series,
        "core_v": core_v,
        "core_w": core_w,
        "core_a": core_a,
        "input_v": input_v,
        "freq_mhz": freq_mhz,
        "efficiency_jth": if let (Some(w), true) = (core_w, hr_ghs > 0.0) {
            Some(round2(w / (hr_ghs / 1000.0)))
        } else {
            None
        },
        "asic_c": asic_c,
        "fail_rate_pct": fail_rate_pct,
        "best_share_diff": best_share_diff,
        "chips": chips,
        "fans": b.fans.iter().map(|f| json!({
            "rpm": f.rpm,
            "percent": f.percent,
            "target_percent": f.target_percent,
        })).collect::<Vec<_>>(),
    })
}

fn round1(v: f64) -> f64 {
    (v * 10.0).round() / 10.0
}
fn round2(v: f64) -> f64 {
    (v * 100.0).round() / 100.0
}
fn round3(v: f64) -> f64 {
    (v * 1000.0).round() / 1000.0
}

fn fmt_diff(v: f64) -> String {
    if v >= 1e12 {
        format!("{:.2}T", v / 1e12)
    } else if v >= 1e9 {
        format!("{:.2}G", v / 1e9)
    } else if v >= 1e6 {
        format!("{:.2}M", v / 1e6)
    } else if v >= 1e3 {
        format!("{:.2}K", v / 1e3)
    } else if v.fract() != 0.0 {
        format!("{v:.1}")
    } else {
        format!("{}", v as i64)
    }
}

fn fmt_uptime(secs: u64) -> String {
    let d = secs / 86400;
    let h = (secs % 86400) / 3600;
    let mi = (secs % 3600) / 60;
    let s = secs % 60;
    if d > 0 {
        format!("{d}d {h}h {mi}m")
    } else if h > 0 {
        format!("{h}h {mi}m")
    } else {
        format!("{mi}m {s}s")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fmt_diff_formats_by_magnitude() {
        assert_eq!(fmt_diff(1.0), "1");
        assert_eq!(fmt_diff(1500.0), "1.50K");
        assert_eq!(fmt_diff(2_500_000.0), "2.50M");
    }

    #[test]
    fn fmt_uptime_picks_largest_unit() {
        assert_eq!(fmt_uptime(45), "0m 45s");
        assert_eq!(fmt_uptime(3661), "1h 1m");
        assert_eq!(fmt_uptime(90000), "1d 1h 0m");
    }
}
