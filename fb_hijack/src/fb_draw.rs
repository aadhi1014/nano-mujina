/// fb_draw — simple page renderer for Avalon Nano3s 240×240 RGB565 display
///
/// Usage (chain commands left-to-right, all applied then flushed to /dev/fb0):
///   fb_draw clear [RRGGBB]
///   fb_draw text <x> <y> <scale> <fg_RRGGBB> <bg_RRGGBB> <"message">
///   fb_draw rect <x> <y> <w> <h> <RRGGBB>
///   fb_draw hline <x> <y> <len> <RRGGBB>
///   fb_draw vline <x> <y> <len> <RRGGBB>
///   fb_draw image <file>         (raw 115200-byte RGB565 file)
///   fb_draw read                 (read current fb0 into working buffer first)
///   fb_draw save <file>          (save current buffer to raw RGB565 file)
///
/// Multiple commands can be chained:
///   fb_draw clear 001122 text 10 10 2 ffffff 001122 "Hello Nano3s"
///
/// Colors are 24-bit RGB hex (RRGGBB), converted to RGB565 internally.

use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Write, Seek, SeekFrom};
use std::net::{Shutdown, TcpStream};
use std::thread;
use std::time::{Duration, Instant};

const W: usize = 240;
const H: usize = 240;
const FB_BYTES: usize = W * H * 2;
const FB_DEV: &str = "/dev/fb0";
const CGMINER_API: &str = "127.0.0.1:4028";
const PAGE_FILE: &str = "/mntapp/release/linux/app/fb_page";
const PIZZA_FILE: &str = "/mntapp/release/linux/app/pizza.rgb565";
/// Written by rtos_core/tools/mujina.c (our own custom mining stack --
/// see its LIVE_STATUS_FILE comment). Plain key=value lines, no JSON
/// dependency needed on either side.
const NANO3S_LIVE_FILE: &str = "/tmp/nano3s_live.txt";

// ── RGB565 helpers ──────────────────────────────────────────────────────────

fn rgb888_to_565(r: u8, g: u8, b: u8) -> u16 {
    ((r as u16 & 0xF8) << 8) | ((g as u16 & 0xFC) << 3) | (b as u16 >> 3)
}

fn parse_color(s: &str) -> u16 {
    let v = u32::from_str_radix(s.trim_start_matches('#'), 16)
        .unwrap_or_else(|_| panic!("bad color '{}' (expect RRGGBB hex)", s));
    rgb888_to_565(((v >> 16) & 0xFF) as u8, ((v >> 8) & 0xFF) as u8, (v & 0xFF) as u8)
}

// ── Screen buffer ────────────────────────────────────────────────────────────

struct Screen {
    buf: [u16; W * H],
}

impl Screen {
    fn blank() -> Self { Screen { buf: [0u16; W * H] } }

    fn from_fb() -> Self {
        let mut s = Self::blank();
        if let Ok(mut f) = File::open(FB_DEV) {
            let bytes = unsafe { std::slice::from_raw_parts_mut(s.buf.as_mut_ptr() as *mut u8, FB_BYTES) };
            let _ = f.read_exact(bytes);
        }
        s
    }

    fn flush(&self) {
        let mut f = OpenOptions::new().write(true).open(FB_DEV)
            .unwrap_or_else(|e| panic!("open {}: {}", FB_DEV, e));
        let bytes = unsafe { std::slice::from_raw_parts(self.buf.as_ptr() as *const u8, FB_BYTES) };
        f.seek(SeekFrom::Start(0)).unwrap();
        f.write_all(bytes).unwrap();
    }

    fn save(&self, path: &str) {
        let mut f = File::create(path).unwrap_or_else(|e| panic!("create {}: {}", path, e));
        let bytes = unsafe { std::slice::from_raw_parts(self.buf.as_ptr() as *const u8, FB_BYTES) };
        f.write_all(bytes).unwrap();
    }

    fn px(&mut self, x: usize, y: usize, c: u16) {
        if x < W && y < H { self.buf[y * W + x] = c; }
    }

    fn clear(&mut self, c: u16) {
        self.buf.fill(c);
    }

    fn rect(&mut self, x: usize, y: usize, w: usize, h: usize, c: u16) {
        for row in y..(y + h).min(H) {
            for col in x..(x + w).min(W) {
                self.px(col, row, c);
            }
        }
    }

    fn hline(&mut self, x: usize, y: usize, len: usize, c: u16) {
        self.rect(x, y, len, 1, c);
    }

    fn vline(&mut self, x: usize, y: usize, len: usize, c: u16) {
        self.rect(x, y, 1, len, c);
    }

    /// Draw a single character at pixel (px, py) with given scale (1 or 2).
    fn draw_char(&mut self, px: usize, py: usize, ch: char, scale: usize, fg: u16, bg: u16) {
        let idx = (ch as usize).saturating_sub(0x20).min(FONT.len() - 1);
        let glyph = &FONT[idx];
        for row in 0..8usize {
            let byte = glyph[row];
            for col in 0..8usize {
                let set = (byte >> col) & 1 != 0;
                let color = if set { fg } else { bg };
                for sy in 0..scale {
                    for sx in 0..scale {
                        self.px(px + col * scale + sx, py + row * scale + sy, color);
                    }
                }
            }
        }
    }

    fn draw_text(&mut self, mut x: usize, y: usize, scale: usize, fg: u16, bg: u16, text: &str) {
        let cw = 8 * scale;
        for ch in text.chars() {
            if x + cw > W { break; }
            self.draw_char(x, y, ch, scale, fg, bg);
            x += cw;
        }
    }

    fn draw_text_centered(&mut self, y: usize, scale: usize, fg: u16, bg: u16, text: &str) {
        let width = text.chars().count().saturating_mul(8 * scale);
        self.draw_text(W.saturating_sub(width) / 2, y, scale, fg, bg, text);
    }

    fn load_image(&mut self, path: &str) {
        let mut f = File::open(path).unwrap_or_else(|e| panic!("open {}: {}", path, e));
        let bytes = unsafe { std::slice::from_raw_parts_mut(self.buf.as_mut_ptr() as *mut u8, FB_BYTES) };
        f.read_exact(bytes).unwrap_or_else(|e| panic!("read {}: {} (need {} bytes)", path, e, FB_BYTES));
    }
}

/// Request one JSON response from the local cgminer API.
fn cgminer_command(command: &str) -> Result<String, String> {
    let mut stream = TcpStream::connect_timeout(
        &CGMINER_API.parse().expect("valid cgminer API address"),
        Duration::from_secs(2),
    ).map_err(|e| format!("connect: {e}"))?;
    stream.set_read_timeout(Some(Duration::from_secs(2))).map_err(|e| e.to_string())?;
    stream.set_write_timeout(Some(Duration::from_secs(2))).map_err(|e| e.to_string())?;
    stream.write_all(format!(r#"{{"command":"{}"}}"#, command).as_bytes())
        .map_err(|e| format!("write: {e}"))?;
    let _ = stream.shutdown(Shutdown::Write);

    let mut reply = Vec::new();
    stream.read_to_end(&mut reply).map_err(|e| format!("read: {e}"))?;
    if let Some(end) = reply.iter().position(|&b| b == 0) {
        reply.truncate(end);
    }
    String::from_utf8(reply).map_err(|e| format!("non-UTF8 reply: {e}"))
}

fn number_after(reply: &str, key: &str) -> Option<f64> {
    let start = reply.find(key)? + key.len();
    let value = reply[start..].trim_start_matches(|c: char| c == ':' || c.is_ascii_whitespace());
    let end = value.find(|c: char| !(c.is_ascii_digit() || matches!(c, '.' | '-' | '+' | 'e' | 'E')))
        .unwrap_or(value.len());
    value[..end].parse().ok()
}

fn wall_power(reply: &str) -> Option<u32> {
    // Avalon supplies PS[status, ..., wall-power]; wall power is item 7.
    let start = reply.find("PS[")? + 3;
    let end = reply[start..].find(']')? + start;
    reply[start..end].split_whitespace().nth(6)?.parse().ok()
}

fn numbers_in_brackets_i32(reply: &str, key: &str) -> Option<Vec<i32>> {
    let start = reply.find(key)? + key.len();
    let end = reply[start..].find(']')? + start;
    let nums = reply[start..end]
        .split_whitespace()
        .filter_map(|s| s.parse::<i32>().ok())
        .collect::<Vec<_>>();
    if nums.is_empty() { None } else { Some(nums) }
}

fn chip_temps(reply: &str) -> Vec<i32> {
    let mut temps = Vec::new();
    for miner in 0..8 {
        let key = format!("PVT_T{miner}[");
        if let Some(mut values) = numbers_in_brackets_i32(reply, &key) {
            temps.append(&mut values);
        }
    }
    temps.into_iter().filter(|t| *t > -100).take(12).collect()
}

fn read_live_stats() -> Result<(f64, u32, u8, Vec<i32>), String> {
    let stats = cgminer_command("stats")?;
    let hashrate = number_after(&stats, "GHSspd[")
        .ok_or_else(|| "GHSspd missing from stats response".to_owned())?;
    let power = wall_power(&stats)
        .ok_or_else(|| "PS wall-power missing from stats response".to_owned())?;
    let mode = number_after(&stats, "WORKMODE[")
        .ok_or_else(|| "WORKMODE missing from stats response".to_owned())? as u8;
    Ok((hashrate, power, mode, chip_temps(&stats)))
}

fn render_live_stats(hashrate: f64, power: u32, mode: u8) {
    let mut scr = Screen::blank();
    let bg = parse_color("00101f");
    scr.clear(bg);
    // Keep all UI content within the visible central circle. The physical panel
    // clips the square framebuffer's corners, so do not use rectangular chrome.
    let (mode_name, mode_color) = match mode {
        0 => ("LOW MODE", "39d353"),
        1 => ("MED MODE", "ffff00"),
        2 => ("HIGH MODE", "ff5555"),
        _ => ("MODE ?", "ffffff"),
    };
    scr.draw_text_centered(38, 2, parse_color(mode_color), bg, mode_name);
    scr.draw_text_centered(76, 1, parse_color("7fbcff"), bg, "HASHRATE");
    scr.draw_text_centered(96, 2, parse_color("ffffff"), bg, &format!("{:.2} TH/S", hashrate / 1000.0));
    scr.hline(52, 132, 136, parse_color("1d4e89"));
    scr.draw_text_centered(150, 1, parse_color("7fbcff"), bg, "POWER DRAW");
    scr.draw_text_centered(170, 2, parse_color("ffff00"), bg, &format!("{power} W"));
    scr.flush();
}

fn temp_color(temp: i32) -> u16 {
    if temp < 65 {
        parse_color("39d353")
    } else if temp < 75 {
        parse_color("ffff00")
    } else if temp < 85 {
        parse_color("ff9f1c")
    } else {
        parse_color("ff5555")
    }
}

fn render_chip_temps(temps: &[i32]) {
    let mut scr = Screen::blank();
    let bg = parse_color("100915");
    let panel = parse_color("21162d");
    let border = parse_color("72518f");
    let white = parse_color("ffffff");
    let muted = parse_color("b58ad6");
    scr.clear(bg);
    scr.draw_text_centered(32, 2, parse_color("ff8bd1"), bg, "CHIP TEMPS");

    if temps.is_empty() {
        scr.draw_text_centered(96, 2, parse_color("ff5555"), bg, "NO TEMPS");
        scr.draw_text_centered(128, 1, white, bg, "PVT_T DATA MISSING");
        scr.flush();
        return;
    }

    let avg = temps.iter().sum::<i32>() as f32 / temps.len() as f32;
    let max = temps.iter().copied().max().unwrap_or(0);
    scr.draw_text_centered(58, 1, muted, bg, &format!("AVG {:.0}C  MAX {max}C", avg));

    // 12 compact chip tiles, kept inside the visible 240px round panel.
    // Do not push content into the square framebuffer corners; the panel clips
    // them. This grid fits inside roughly a 113px radius from screen center.
    let x0 = 34usize;
    let y0 = 82usize;
    let cell_w = 41usize;
    let cell_h = 32usize;
    let gap_x = 5usize;
    let gap_y = 5usize;
    for i in 0..12usize {
        let col = i % 4;
        let row = i / 4;
        let x = x0 + col * (cell_w + gap_x);
        let y = y0 + row * (cell_h + gap_y);
        if let Some(temp) = temps.get(i).copied() {
            let c = temp_color(temp);
            let fill_h = ((temp.clamp(35, 95) - 35) as usize * (cell_h - 8) / 60).max(3);

            // Solid chip body with a colored top rail and left thermal bar.
            scr.rect(x, y, cell_w, cell_h, border);
            scr.rect(x + 1, y + 1, cell_w - 2, cell_h - 2, panel);
            scr.rect(x + 2, y + 2, cell_w - 4, 3, c);
            scr.rect(x + 4, y + cell_h - 4 - fill_h, 5, fill_h, c);
            scr.hline(x + 11, y + 13, cell_w - 15, parse_color("3a2749"));

            scr.draw_text(x + 13, y + 6, 1, muted, panel, &format!("#{:02}", i + 1));
            scr.draw_text(x + 12, y + 20, 1, c, panel, &format!("{temp}"));
        } else {
            scr.rect(x, y, cell_w, cell_h, border);
            scr.rect(x + 1, y + 1, cell_w - 2, cell_h - 2, panel);
            scr.draw_text(x + 13, y + 12, 1, parse_color("666666"), panel, &format!("#{:02}", i + 1));
        }
    }
    scr.flush();
}

fn wlan_ip() -> Option<String> {
    let output = std::process::Command::new("ifconfig").arg("wlan0").output().ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    let start = text.find("inet addr:")? + "inet addr:".len();
    let end = text[start..].find(|c: char| c.is_whitespace())? + start;
    let ip = text[start..end].trim();
    if ip.is_empty() { None } else { Some(ip.to_owned()) }
}

fn render_ip() {
    let mut scr = Screen::blank();
    let bg = parse_color("001510");
    scr.clear(bg);
    scr.draw_text_centered(48, 2, parse_color("39d353"), bg, "NETWORK");
    match wlan_ip() {
        Some(ip) => {
            scr.draw_text_centered(90, 1, parse_color("7fbcff"), bg, "IP ADDRESS");
            scr.draw_text_centered(112, 2, parse_color("ffffff"), bg, &ip);
        }
        None => {
            scr.draw_text_centered(104, 2, parse_color("ff5555"), bg, "NO WIFI");
        }
    }
    scr.flush();
}

// ── live-nano3s: our own custom mining stack's live telemetry ──────────────
// (rtos_core.elf --ipc6 + mujina, not stock asic_miner/mm_miner/cgminer --
// see NANO3S_LIVE_FILE above and mujina.c's write_live_status().)

fn read_nano3s_status() -> Option<HashMap<String, String>> {
    let text = std::fs::read_to_string(NANO3S_LIVE_FILE).ok()?;
    let mut kv = HashMap::new();
    for line in text.lines() {
        if let Some((k, v)) = line.split_once('=') {
            kv.insert(k.trim().to_owned(), v.trim().to_owned());
        }
    }
    Some(kv)
}

fn kv_str<'a>(kv: &'a HashMap<String, String>, key: &str) -> Option<&'a str> {
    kv.get(key).map(|s| s.as_str())
}
fn kv_f64(kv: &HashMap<String, String>, key: &str) -> Option<f64> {
    kv_str(kv, key)?.parse().ok()
}
fn kv_u32(kv: &HashMap<String, String>, key: &str) -> Option<u32> {
    kv_str(kv, key)?.parse().ok()
}
fn kv_bool(kv: &HashMap<String, String>, key: &str) -> bool {
    kv_str(kv, key) == Some("1")
}

/// Same thermal thresholds as temp_color(), but in Celsius (mujina's
/// temp_avg/temp_max are already °C, unlike the cgminer path's raw
/// PVT_T codes) -- this chip runs cooler in practice (observed 27-46C
/// across every real soak test this project has done), so the bands
/// are tighter than temp_color()'s cgminer-derived ones.
fn temp_color_c(temp: f64) -> u16 {
    if temp < 38.0 {
        parse_color("39d353")
    } else if temp < 44.0 {
        parse_color("ffff00")
    } else if temp < 50.0 {
        parse_color("ff9f1c")
    } else {
        parse_color("ff5555")
    }
}

// Real Mujina brand tokens (256foundation/mujina-website,
// .vitepress/theme/custom.css, .dark block) -- not our own invention.
// "Muted armor red" accent on neutral graphite, matching the actual
// open-source Mujina firmware project's identity (this device talks to
// pool.256foundation.org, the same org). Semantic status colors
// (mining/connecting/offline, temp bands) stay outside the accent, per
// that same stylesheet's own principle: "the accent alone carries the
// warmth."
const MUJINA_BG: &str = "0e0f12";
const MUJINA_TEXT_1: &str = "e7e8ec";
const MUJINA_TEXT_2: &str = "b5b7bf";
const MUJINA_TEXT_3: &str = "999ba4";
const MUJINA_DIVIDER: &str = "272931";
const MUJINA_ACCENT: &str = "d05555";

// Plain idle/startup splash -- text only, no mascot. This project shipped
// two different mascot attempts on 2026-08-06 (a from-scratch placeholder,
// then a real-artwork swap-in sourced from github.com/rkuester/
// mujina-logo-set with the author's permission); the user reviewed the
// real artwork live on-device and asked for it to be removed entirely
// rather than iterated on. See BUILD_NOTES.md's 2026-08-06 entries for
// the full history if a mascot is ever revisited.
fn render_nano3s_waiting(message: &str) {
    let mut scr = Screen::blank();
    let bg = parse_color(MUJINA_BG);
    scr.clear(bg);
    scr.draw_text_centered(112, 2, parse_color(MUJINA_TEXT_1), bg, message);
    scr.flush();
}

fn render_nano3s_live(kv: &HashMap<String, String>) {
    let mut scr = Screen::blank();
    let bg = parse_color(MUJINA_BG);
    let muted = parse_color(MUJINA_TEXT_3);
    let dim = parse_color(MUJINA_TEXT_2);
    let white = parse_color(MUJINA_TEXT_1);
    let accent = parse_color(MUJINA_ACCENT);
    scr.clear(bg);

    let pool_connected = kv_bool(kv, "pool_connected");
    let ipc_connected = kv_bool(kv, "ipc_connected");
    let have_hashrate = kv.contains_key("hashrate_ghs");
    let paused = kv_bool(kv, "paused");

    let (status_text, status_color) = if paused {
        ("PAUSED", parse_color("ffff00"))
    } else if pool_connected && ipc_connected && have_hashrate {
        ("MINING", parse_color("39d353"))
    } else if pool_connected || ipc_connected {
        ("CONNECTING", parse_color("ffff00"))
    } else {
        ("OFFLINE", parse_color("ff5555"))
    };
    scr.draw_text_centered(24, 1, status_color, bg, status_text);

    // Hashrate block
    scr.draw_text_centered(44, 1, dim, bg, "HASHRATE");
    if paused {
        scr.draw_text_centered(64, 2, muted, bg, "IDLE");
    } else {
        match kv_u32(kv, "hashrate_ghs") {
            Some(ghs) if ghs > 0 => {
                let ths = ghs as f64 / 1000.0;
                scr.draw_text_centered(64, 2, white, bg, &format!("{:.2} TH/S", ths));
            }
            _ => {
                scr.draw_text_centered(64, 2, muted, bg, "WARMING UP");
            }
        }
    }

    scr.hline(52, 96, 136, parse_color(MUJINA_DIVIDER));

    // Chain temperature block
    scr.draw_text_centered(108, 1, dim, bg, "CHAIN TEMP");
    match (kv_f64(kv, "temp_avg"), kv_f64(kv, "temp_max")) {
        (Some(avg), Some(max)) => {
            scr.draw_text_centered(128, 2, temp_color_c(avg), bg, &format!("{:.1}C AVG", avg));
            scr.draw_text_centered(150, 1, muted, bg, &format!("{:.1}C MAX", max));
        }
        _ => {
            scr.draw_text_centered(128, 2, muted, bg, "--");
        }
    }

    scr.hline(52, 160, 136, parse_color(MUJINA_DIVIDER));

    // Pool / job block
    scr.draw_text_centered(172, 1, dim, bg, "POOL");
    let diff_txt = match kv_f64(kv, "difficulty") {
        Some(d) if d >= 1000.0 => format!("DIFF {:.0}K", d / 1000.0),
        Some(d) => format!("DIFF {:.0}", d),
        None => "DIFF --".to_owned(),
    };
    let shares = kv_str(kv, "shares_found").unwrap_or("0");
    scr.draw_text_centered(190, 1, accent, bg, &format!("{}  {} SHARES", diff_txt, shares));

    // Power draw (INA226 estimate, ~10mOhm shunt assumed -- see mujina.c's
    // read_power_estimate_w(), not a lab-calibrated reading).
    if let Some(w) = kv_f64(kv, "power_w") {
        scr.draw_text_centered(210, 1, muted, bg, &format!("~{:.1} W", w));
    }

    scr.flush();
}

fn render_nano3s_diag(kv: &HashMap<String, String>) {
    let mut scr = Screen::blank();
    let bg = parse_color(MUJINA_BG);
    let muted = parse_color(MUJINA_TEXT_3);
    let dim = parse_color(MUJINA_TEXT_2);
    let white = parse_color(MUJINA_TEXT_1);
    let accent = parse_color(MUJINA_ACCENT);
    scr.clear(bg);

    scr.draw_text_centered(24, 1, accent, bg, "DIAGNOSTICS");

    scr.draw_text_centered(46, 1, dim, bg, "CHIPS ONLINE");
    match kv_u32(kv, "asics_total") {
        Some(n) => scr.draw_text_centered(66, 2, white, bg, &format!("{n} / 12")),
        None => scr.draw_text_centered(66, 2, muted, bg, "--"),
    }

    scr.hline(52, 96, 136, parse_color(MUJINA_DIVIDER));

    scr.draw_text_centered(108, 1, dim, bg, "PLL LOW MODE (MHZ)");
    match (kv_u32(kv, "pll0"), kv_u32(kv, "pll1"), kv_u32(kv, "pll2"), kv_u32(kv, "pll3")) {
        (Some(a), Some(b), Some(c), Some(d)) => {
            scr.draw_text_centered(128, 1, white, bg, &format!("{a}  {b}  {c}  {d}"));
        }
        _ => scr.draw_text_centered(128, 1, muted, bg, "--"),
    }

    scr.hline(52, 154, 136, parse_color(MUJINA_DIVIDER));

    scr.draw_text_centered(166, 1, dim, bg, "CORE VOLTAGE");
    match kv_u32(kv, "voltage_mv") {
        Some(v) => scr.draw_text_centered(186, 2, white, bg, &format!("{v} MV")),
        None => scr.draw_text_centered(186, 2, muted, bg, "--"),
    }
    let err = kv_str(kv, "err_crc").unwrap_or("0");
    scr.draw_text_centered(210, 1, muted, bg, &format!("ERR_CRC {err}"));

    scr.flush();
}

// REAL 2026-08-06 FIX (user complaint: ">1s delay between button press
// and page change"): this loop used to read PAGE_FILE once, draw, then
// thread::sleep(interval) for the FULL 2s production interval before
// checking again -- a button press landing right after a check could sit
// unreflected on screen for up to 2s. Root cause #2 alongside
// fb_button.rs's now-removed 450ms double-click tax (worst case ~2.45s,
// matching the report). Fix: poll PAGE_FILE on a short POLL_INTERVAL and
// redraw immediately the instant the page value changes, while still
// only doing a full periodic stat-refresh redraw (to pick up new
// NANO3S_LIVE_FILE telemetry on an unchanged page) at the original
// `interval` cadence.
fn run_live_nano3s(interval: Duration) -> ! {
    const POLL_INTERVAL: Duration = Duration::from_millis(100);

    let mut last_page = String::new();
    let mut last_refresh = Instant::now() - interval;

    loop {
        let page = std::fs::read_to_string(PAGE_FILE)
            .unwrap_or_else(|_| "nano3s".to_owned());
        let page = page.trim().to_owned();
        let page_changed = page != last_page;

        if page == "ip" || page == "pizza" {
            if page_changed || last_refresh.elapsed() >= interval {
                last_page = page.clone();
                last_refresh = Instant::now();
                if page == "ip" {
                    render_ip();
                } else {
                    let mut scr = Screen::blank();
                    scr.load_image(PIZZA_FILE);
                    scr.flush();
                }
            }
            thread::sleep(POLL_INTERVAL);
            continue;
        }

        match read_nano3s_status() {
            Some(kv) if page == "nano3s-diag" => {
                if page_changed || last_refresh.elapsed() >= interval {
                    last_page = page.clone();
                    last_refresh = Instant::now();
                    render_nano3s_diag(&kv);
                }
            }
            Some(kv) => {
                if page_changed || last_refresh.elapsed() >= interval {
                    last_page = page.clone();
                    last_refresh = Instant::now();
                    render_nano3s_live(&kv);
                }
            }
            None => {
                if page_changed || last_refresh.elapsed() >= interval {
                    last_page = page.clone();
                    last_refresh = Instant::now();
                    render_nano3s_waiting("STARTING");
                }
            }
        }
        thread::sleep(POLL_INTERVAL);
    }
}

fn run_live_stats(interval: Duration) -> ! {
    loop {
        let page = std::fs::read_to_string(PAGE_FILE)
            .unwrap_or_else(|_| "stats".to_owned());
        let page = page.trim();
        if page == "ip" {
            render_ip();
            thread::sleep(interval);
            continue;
        }
        match read_live_stats() {
            Ok((hashrate, power, mode, temps)) if page == "pizza" => {
                let mut scr = Screen::blank();
                scr.load_image(PIZZA_FILE);
                scr.flush();
                let _ = (hashrate, power, mode, temps);
            }
            Ok((_hashrate, _power, _mode, temps)) if page == "temps" => render_chip_temps(&temps),
            Ok((hashrate, power, mode, _temps)) => render_live_stats(hashrate, power, mode),
            Err(error) => {
                eprintln!("fb_draw live: {error}");
                let mut scr = Screen::blank();
                let bg = parse_color("00101f");
                scr.clear(bg);
                scr.draw_text_centered(88, 2, parse_color("ff5555"), bg, "NO DATA");
                scr.draw_text_centered(120, 1, parse_color("ffffff"), bg, "CGMINER API OFFLINE");
                scr.flush();
            }
        }
        thread::sleep(interval);
    }
}

// ── Command parser ───────────────────────────────────────────────────────────

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        eprintln!("fb_draw — 240x240 RGB565 framebuffer renderer");
        eprintln!();
        eprintln!("Commands (chain them, flushed once at end):");
        eprintln!("  clear [RRGGBB]");
        eprintln!("  text <x> <y> <scale> <fg> <bg> <message>");
        eprintln!("  rect <x> <y> <w> <h> <RRGGBB>");
        eprintln!("  hline <x> <y> <len> <RRGGBB>");
        eprintln!("  vline <x> <y> <len> <RRGGBB>");
        eprintln!("  image <file>    (raw 115200-byte RGB565)");
        eprintln!("  read            (start from current fb0 content)");
        eprintln!("  save <file>     (save buffer to file, don't flush)");
        eprintln!("  live [seconds]  show Avalon GHSspd and wall power continuously (stock cgminer stack)");
        eprintln!("  live-nano3s [seconds]  show mujina.c's live telemetry (our custom rtos_core/mujina stack)");
        eprintln!();
        eprintln!("Example:");
        eprintln!("  fb_draw clear 002244 text 20 100 2 ffff00 002244 \"Hello World\"");
        std::process::exit(0);
    }

    let mut scr = Screen::blank();
    let mut i = 0usize;
    let mut do_flush = true;

    while i < args.len() {
        match args[i].as_str() {
            "live" => {
                let seconds = if i + 1 < args.len() && !is_cmd(&args[i + 1]) {
                    i += 1;
                    args[i].parse::<u64>().expect("live interval seconds")
                } else { 2 };
                run_live_stats(Duration::from_secs(seconds.max(1)));
            }
            "live-nano3s" => {
                let seconds = if i + 1 < args.len() && !is_cmd(&args[i + 1]) {
                    i += 1;
                    args[i].parse::<u64>().expect("live-nano3s interval seconds")
                } else { 2 };
                run_live_nano3s(Duration::from_secs(seconds.max(1)));
            }
            "read" => {
                scr = Screen::from_fb();
                i += 1;
            }
            "clear" => {
                let color = if i + 1 < args.len() && !is_cmd(&args[i + 1]) {
                    i += 1; parse_color(&args[i])
                } else { 0 };
                scr.clear(color);
                i += 1;
            }
            "text" => {
                // text <x> <y> <scale> <fg> <bg> <message>
                assert!(i + 6 < args.len(), "text needs: x y scale fg bg message");
                let x  = args[i+1].parse::<usize>().expect("text x");
                let y  = args[i+2].parse::<usize>().expect("text y");
                let sc = args[i+3].parse::<usize>().expect("text scale");
                let fg = parse_color(&args[i+4]);
                let bg = parse_color(&args[i+5]);
                let msg = &args[i+6];
                scr.draw_text(x, y, sc.max(1), fg, bg, msg);
                i += 7;
            }
            "rect" => {
                // rect <x> <y> <w> <h> <color>
                assert!(i + 5 < args.len(), "rect needs: x y w h color");
                let x = args[i+1].parse::<usize>().expect("rect x");
                let y = args[i+2].parse::<usize>().expect("rect y");
                let w = args[i+3].parse::<usize>().expect("rect w");
                let h = args[i+4].parse::<usize>().expect("rect h");
                let c = parse_color(&args[i+5]);
                scr.rect(x, y, w, h, c);
                i += 6;
            }
            "hline" => {
                assert!(i + 4 < args.len(), "hline needs: x y len color");
                let x   = args[i+1].parse::<usize>().expect("hline x");
                let y   = args[i+2].parse::<usize>().expect("hline y");
                let len = args[i+3].parse::<usize>().expect("hline len");
                let c   = parse_color(&args[i+4]);
                scr.hline(x, y, len, c);
                i += 5;
            }
            "vline" => {
                assert!(i + 4 < args.len(), "vline needs: x y len color");
                let x   = args[i+1].parse::<usize>().expect("vline x");
                let y   = args[i+2].parse::<usize>().expect("vline y");
                let len = args[i+3].parse::<usize>().expect("vline len");
                let c   = parse_color(&args[i+4]);
                scr.vline(x, y, len, c);
                i += 5;
            }
            "image" => {
                assert!(i + 1 < args.len(), "image needs: file");
                scr.load_image(&args[i+1]);
                i += 2;
            }
            "save" => {
                assert!(i + 1 < args.len(), "save needs: file");
                scr.save(&args[i+1]);
                do_flush = false;
                i += 2;
            }
            cmd => {
                eprintln!("unknown command '{}' (args so far ok)", cmd);
                std::process::exit(1);
            }
        }
    }

    if do_flush { scr.flush(); }
}

fn is_cmd(s: &str) -> bool {
    matches!(s, "clear" | "text" | "rect" | "hline" | "vline" | "image" | "read" | "save" | "live" | "live-nano3s")
}

// ── 8×8 bitmap font (printable ASCII 0x20..0x7e) ────────────────────────────
// Classic IBM CP437 8×8 font — public domain.
// Each entry = 8 bytes, one per row, bit7 = leftmost pixel.
#[rustfmt::skip]
const FONT: [[u8; 8]; 96] = [
    // 0x20 space
    [0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00],
    // 0x21 !
    [0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00],
    // 0x22 "
    [0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00],
    // 0x23 #
    [0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00],
    // 0x24 $
    [0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00],
    // 0x25 %
    [0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00],
    // 0x26 &
    [0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00],
    // 0x27 '
    [0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00],
    // 0x28 (
    [0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00],
    // 0x29 )
    [0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00],
    // 0x2a *
    [0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00],
    // 0x2b +
    [0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00],
    // 0x2c ,
    [0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06],
    // 0x2d -
    [0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00],
    // 0x2e .
    [0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00],
    // 0x2f /
    [0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00],
    // 0x30 0
    [0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00],
    // 0x31 1
    [0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00],
    // 0x32 2
    [0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00],
    // 0x33 3
    [0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00],
    // 0x34 4
    [0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00],
    // 0x35 5
    [0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00],
    // 0x36 6
    [0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00],
    // 0x37 7
    [0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00],
    // 0x38 8
    [0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00],
    // 0x39 9
    [0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00],
    // 0x3a :
    [0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00],
    // 0x3b ;
    [0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06],
    // 0x3c <
    [0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00],
    // 0x3d =
    [0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00],
    // 0x3e >
    [0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00],
    // 0x3f ?
    [0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00],
    // 0x40 @
    [0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00],
    // 0x41 A
    [0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00],
    // 0x42 B
    [0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00],
    // 0x43 C
    [0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00],
    // 0x44 D
    [0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00],
    // 0x45 E
    [0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00],
    // 0x46 F
    [0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00],
    // 0x47 G
    [0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00],
    // 0x48 H
    [0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00],
    // 0x49 I
    [0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00],
    // 0x4a J
    [0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00],
    // 0x4b K
    [0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00],
    // 0x4c L
    [0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00],
    // 0x4d M
    [0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00],
    // 0x4e N
    [0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00],
    // 0x4f O
    [0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00],
    // 0x50 P
    [0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00],
    // 0x51 Q
    [0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00],
    // 0x52 R
    [0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00],
    // 0x53 S
    [0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00],
    // 0x54 T
    [0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00],
    // 0x55 U
    [0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00],
    // 0x56 V
    [0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00],
    // 0x57 W
    [0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00],
    // 0x58 X
    [0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00],
    // 0x59 Y
    [0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00],
    // 0x5a Z
    [0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00],
    // 0x5b [
    [0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00],
    // 0x5c backslash
    [0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00],
    // 0x5d ]
    [0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00],
    // 0x5e ^
    [0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00],
    // 0x5f _
    [0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF],
    // 0x60 `
    [0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00],
    // 0x61 a
    [0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00],
    // 0x62 b
    [0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00],
    // 0x63 c
    [0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00],
    // 0x64 d
    [0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00],
    // 0x65 e
    [0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00],
    // 0x66 f
    [0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00],
    // 0x67 g
    [0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F],
    // 0x68 h
    [0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00],
    // 0x69 i
    [0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00],
    // 0x6a j
    [0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E],
    // 0x6b k
    [0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00],
    // 0x6c l
    [0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00],
    // 0x6d m
    [0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00],
    // 0x6e n
    [0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00],
    // 0x6f o
    [0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00],
    // 0x70 p
    [0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F],
    // 0x71 q
    [0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78],
    // 0x72 r
    [0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00],
    // 0x73 s
    [0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00],
    // 0x74 t
    [0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00],
    // 0x75 u
    [0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00],
    // 0x76 v
    [0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00],
    // 0x77 w
    [0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00],
    // 0x78 x
    [0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00],
    // 0x79 y
    [0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F],
    // 0x7a z
    [0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00],
    // 0x7b {
    [0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00],
    // 0x7c |
    [0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00],
    // 0x7d }
    [0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00],
    // 0x7e ~
    [0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00],
    // 0x7f (DEL / fallback)
    [0xFF,0x81,0xBD,0xA5,0xBD,0x81,0xFF,0x00],
];
