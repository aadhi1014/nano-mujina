"""Live cyberpunk dashboard for mujina-minerd, driven by the REST API.

Polls http://127.0.0.1:7785/api/v0/miner and serves an auto-refreshing
neon dashboard: a fleet overview (aggregated across every board the
daemon reports) plus per-board tabs with full telemetry.

    python dashboard.py
    -> open http://127.0.0.1:8088
"""
import json, urllib.request, urllib.parse, urllib.error, http.server, socketserver, threading, time, os, subprocess, tempfile

API_ROOT = "http://127.0.0.1:7785/api/v0"
API = API_ROOT + "/miner"

# nano3s_doom's web gamepad lives on the Nano3s itself (see
# nano3s_doom/README.md), a separate host/port from the miner API above --
# hardcoded to match build_and_deploy.sh's own DEVICE constant.
DOOM_DEVICE_HOST = "root@192.168.1.152"
DOOM_GAMEPAD_URL = "http://192.168.1.152:8080"
PORT = 8088
_BOARD_HISTORY = {}  # board_name -> rolling hashrate samples in GH/s
_FLEET_HISTORY = []  # rolling aggregate hashrate samples in GH/s
_HIST_LOCK = threading.Lock()
_LAST_SAMPLE_TS = 0.0
_BEST_SHARE_DIFF = 0.0
SAMPLE_INTERVAL_S = 1.5


def fmt_diff(val):
    if val is None:
        return "—"
    try:
        v = float(val)
        if v >= 1e12:
            return f"{v/1e12:.2f}T"
        if v >= 1e9:
            return f"{v/1e9:.2f}G"
        if v >= 1e6:
            return f"{v/1e6:.2f}M"
        if v >= 1e3:
            return f"{v/1e3:.2f}K"
        return f"{v:.1f}" if (v % 1 != 0) else f"{int(v)}"
    except Exception:
        return str(val)


def fetch_api():
    with urllib.request.urlopen(API, timeout=3) as r:
        return json.load(r)


def patch_fan(board, payload):
    """Forward a fan-control change to the miner's PATCH endpoint.

    `payload` is {auto, target_c, min_percent, percent}; the miner keeps
    any field left unset. Returns (ok, detail)."""
    if not board:
        return False, "no board connected"
    url = f"{API_ROOT}/boards/{urllib.parse.quote(board)}/fan"
    body = json.dumps(payload).encode()
    req = urllib.request.Request(
        url, data=body, method="PATCH",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return True, r.status
    except Exception as e:
        return False, str(e)


def patch_tuning(board, payload):
    """Forward a frequency/voltage change to the miner's tuning endpoint.

    `payload` is {frequency_mhz?, core_voltage_mv?}; the miner clamps each
    to a safe range. A live frequency ramp can take a few seconds."""
    if not board:
        return False, "no board connected"
    url = f"{API_ROOT}/boards/{urllib.parse.quote(board)}/tuning"
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(), method="PATCH",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return True, r.status
    except Exception as e:
        return False, str(e)


HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTUNE_PATH = os.path.join(HERE, "mujina-autotune.json")
_AUTOTUNE_LOCK = threading.Lock()


def load_autotune_store():
    """Load autotune calibration history from disk with lock protection."""
    if not os.path.exists(AUTOTUNE_PATH):
        return {}
    with _AUTOTUNE_LOCK:
        try:
            with open(AUTOTUNE_PATH, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return {}


def save_autotune_store(store):
    """Save autotune calibration history atomically with thread safety."""
    with _AUTOTUNE_LOCK:
        tmp_path = AUTOTUNE_PATH + ".tmp"
        try:
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(store, f, indent=2)
            os.replace(tmp_path, AUTOTUNE_PATH)
        except Exception:
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except Exception:
                    pass


def fetch_autotune(board):
    """Read a board's auto-tune status (state, phase, best, activity log).
    
    Persists best-known calibration points to disk (mujina-autotune.json).
    """
    if not board:
        return None
    url = f"{API_ROOT}/boards/{urllib.parse.quote(board)}/autotune"
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            data = json.load(r)
            if data and data.get("best"):
                store = load_autotune_store()
                store[board] = {
                    "best": data["best"],
                    "profile": data.get("profile"),
                    "target": data.get("target"),
                    "efficiency_j_th": data.get("efficiency_j_th"),
                    "updated_at": time.time(),
                }
                save_autotune_store(store)
            return data
    except Exception:
        store = load_autotune_store()
        return store.get(board)


def patch_autotune(board, payload):
    """Enable/disable auto-tuning and set the profile/target. `payload` is
    {enabled, profile?, target?}."""
    if not board:
        return False, "no board connected"
    url = f"{API_ROOT}/boards/{urllib.parse.quote(board)}/autotune"
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(), method="PATCH",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            store = load_autotune_store()
            bstore = store.setdefault(board, {})
            bstore["enabled"] = bool(payload.get("enabled"))
            if payload.get("profile"):
                bstore["profile"] = payload["profile"]
            if payload.get("target"):
                bstore["target"] = payload["target"]
            bstore["updated_at"] = time.time()
            save_autotune_store(store)
            return True, r.status
    except Exception as e:
        return False, str(e)


def fetch_settings():
    """Read the miner's name and pool settings.

    The password is never returned by the API -- only a `password_set`
    flag -- so the dashboard shows a placeholder rather than a real value."""
    try:
        with urllib.request.urlopen(API_ROOT + "/settings", timeout=3) as r:
            return json.load(r)
    except Exception:
        return None


def patch_settings(payload):
    """Save the miner name and/or pool settings.

    These are read by the daemon only at startup, so the response's
    `restart_required` is what the UI reports back -- not a claim that the
    change is already live. Returns (ok, detail)."""
    req = urllib.request.Request(
        API_ROOT + "/settings", data=json.dumps(payload).encode(), method="PATCH",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            return True, json.load(r)
    except urllib.error.HTTPError as e:
        # The miner rejects a bad pool URL with a plain-text reason. Without
        # reading the body all the user sees is "HTTP Error 400".
        try:
            detail = e.read().decode(errors="replace").strip()
        except Exception:
            detail = ""
        return False, detail or str(e)
    except Exception as e:
        return False, str(e)


HERE = os.path.dirname(os.path.abspath(__file__))


def restart_miner():
    """Stop the daemon gracefully, then start it again.

    Goes through stop-mujina.ps1 rather than killing the process: a
    force-kill skips the ASIC reset and serial-port teardown and can wedge
    the board's USB port until it is physically replugged. That script
    already falls back to a hard kill on its own if the daemon ignores
    Ctrl-C, so that decision stays in one place.

    Returns (ok, detail)."""
    def run(script, timeout):
        # Output goes to a temp FILE, never to a pipe.
        #
        # start-mujina.ps1 spawns the daemon with Start-Process, and the
        # daemon inherits whatever stdout/stderr handles PowerShell had.
        # With capture_output=True those are pipes, and subprocess.run waits
        # for EOF on them -- which only arrives when the *daemon* exits, not
        # when PowerShell does. That deadlocked the call until the timeout
        # fired, despite the restart itself having worked. A file handle is
        # inherited just as happily and closes nothing we wait on.
        with tempfile.TemporaryFile(mode="w+", encoding="utf-8",
                                    errors="replace") as out:
            proc = subprocess.run(
                ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                 "-File", os.path.join(HERE, script)],
                stdin=subprocess.DEVNULL, stdout=out, stderr=out,
                timeout=timeout,
            )
            out.seek(0)
            return proc.returncode, out.read().strip()

    try:
        for script, label in (("stop-mujina.ps1", "stop"),
                              ("start-mujina.ps1", "start")):
            code, output = run(script, 60)
            if code != 0:
                return False, f"{label} failed: {output[:300]}"
    except subprocess.TimeoutExpired:
        return False, "restart timed out"
    except Exception as e:
        return False, str(e)

    # Don't report success until the API answers: start-mujina.ps1 returns as
    # soon as the process is spawned, well before it is serving.
    #
    # This is "the daemon is up", not "the boards are back" -- the API binds
    # about ten seconds before USB enumeration finishes. Waiting for boards
    # would mean guessing how many should appear, and a genuinely absent
    # board would look like a failed restart. The caller says so rather than
    # claiming more than was checked.
    for _ in range(30):
        try:
            with urllib.request.urlopen(API_ROOT + "/health", timeout=2):
                return True, "daemon up — boards re-enumerating"
        except Exception:
            time.sleep(1)
    return False, "miner did not come back within 30s"


def launch_doom():
    """(Re)launch nano3s_doom on the Nano3s and switch the panel to it.

    Kills whatever instance is already running first -- the gamepad's own
    HTTP server can't rebind :8080 while an old process still holds it --
    then flips the display page file and starts a fresh instance
    backgrounded with nohup so it survives this SSH session closing. Same
    manual sequence used throughout that project's development, just
    triggered remotely. Returns (ok, detail); detail is the gamepad URL
    to open on success, an error message on failure.

    Deliberately no nested quotes/`$()` substitution in the remote
    command -- those got mangled going through Python's subprocess list
    -> Windows CreateProcess -> ssh.exe -> remote shell chain in testing,
    even though the same command worked fine typed directly over ssh.
    `killall`, not `pkill -f`, because this device's busybox has no pkill.
    """
    cmd = (
        "killall nano3s_doom 2>/dev/null; sleep 1; "
        "echo doom > /mntapp/release/linux/app/fb_page; "
        "cd /data && nohup ./nano3s_doom -iwad /data/doom1.wad -gfxmode rgb565 "
        "> /data/nano3s_doom.log 2>&1 &"
    )
    try:
        proc = subprocess.run(
            ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=5",
             DOOM_DEVICE_HOST, cmd],
            capture_output=True, text=True, timeout=15,
        )
        if proc.returncode != 0:
            return False, proc.stderr.strip() or f"ssh exited {proc.returncode}"
        return True, DOOM_GAMEPAD_URL
    except Exception as e:
        return False, str(e)


def _pick(items, key, field, default=None):
    for it in items or []:
        if it.get("name") == key:
            return it.get(field, default)
    return default


def _board_hashrate_hs(board, all_boards, miner_hashrate_hs):
    """Per-board hashrate, by the first rule below that applies.

    1. Real per-thread data, once Mujina populates BoardTelemetry.threads
       with actual hashrates. Exact; supersedes everything else.
    2. Exactly one board reports thread_count > 0. Every other board has
       no hash threads at all, so it contributes exactly zero and the
       miner-wide aggregate belongs entirely to the one that does. This
       is attribution by elimination, not a guess.
    3. Exactly one board, period -- the aggregate is trivially its own.
    4. Otherwise None. Several boards are hashing and, without per-thread
       data, splitting the aggregate between them would be fabrication.

    Rule 2 deliberately declines as soon as a second board starts hashing
    (a CPU board alongside the Bitaxe today, the NerdQAxe++ once
    multi-chip chain support lands). At that point rule 1 is the only
    honest answer and per-thread accounting has to be built.

    thread_count is absent on daemons older than the field. It reads as 0
    for every board there, so rule 2 never fires and behaviour falls back
    to rules 3 and 4 exactly as before.
    """
    threads = board.get("threads") or []
    if threads:
        return sum(t.get("hashrate") or 0 for t in threads)

    hashing = [b for b in all_boards if (b.get("thread_count") or 0) > 0]
    if len(hashing) == 1:
        # Zero, not None, for the idle boards: "this board is hashing
        # nothing" is a known fact here, and recording it keeps their
        # history charts drawing a real flat line instead of staying blank.
        return miner_hashrate_hs if hashing[0] is board else 0

    if len(all_boards) == 1:
        return miner_hashrate_hs
    return None


def _extract_board(board, all_boards, miner_hashrate_hs, record):
    name = board.get("name")
    fans = board.get("fans") or []
    temps = board.get("temperatures") or []
    powers = board.get("powers") or []
    fan0 = fans[0] if fans else {}

    bhr_hs = _board_hashrate_hs(board, all_boards, miner_hashrate_hs)
    bhr_ghs = (bhr_hs / 1e9) if bhr_hs is not None else None
    core_w = _pick(powers, "core", "power_w")

    with _HIST_LOCK:
        hist = _BOARD_HISTORY.setdefault(name, [])
        if record and bhr_ghs is not None:
            hist.append(round(bhr_ghs, 2))
            del hist[:-120]
        series = list(hist)

    return {
        "board_name": name,
        "model": board.get("model") or "Mujina board",
        "serial": board.get("serial"),
        "hashrate_ghs": round(bhr_ghs, 2) if bhr_ghs is not None else None,
        "hashrate_ths": round(bhr_ghs / 1000, 3) if bhr_ghs is not None else None,
        "series": series,
        "freq_mhz": board.get("frequency_mhz"),
        "core_v": _pick(powers, "core", "voltage_v"),
        "core_w": core_w,
        "core_a": _pick(powers, "core", "current_a"),
        "input_v": _pick(powers, "input", "voltage_v"),
        "efficiency_jth": (
            round(core_w / (bhr_ghs / 1000), 2)
            if core_w and bhr_ghs and bhr_ghs > 0
            else None
        ),
        "asic_c": _pick(temps, "asic", "temperature_c"),
        "vr_c": _pick(temps, "vr", "temperature_c"),
        # Per-ASIC breakdown, flattened across the board's threads. Only
        # chains whose silicon identifies the chip that found each nonce
        # report this, so single-chip boards give an empty list and the UI
        # shows nothing rather than a pointless one-row table.
        "chips": [
            {
                "index": c.get("index"),
                "ghs": round((c.get("hashrate") or 0) / 1e9, 2),
            }
            for t in (board.get("threads") or [])
            for c in (t.get("chips") or [])
        ],
        "fans": [
            {
                "name": f.get("name"),
                "rpm": f.get("rpm"),
                "percent": f.get("percent"),
                "target_percent": f.get("target_percent"),
            }
            for f in fans
        ],
        "fan_rpm": fan0.get("rpm"),
        "fan_pct": fan0.get("percent"),
        "fan_target_pct": fan0.get("target_percent"),
        "fan_auto": fan0.get("auto"),
        "fan_target_c": fan0.get("target_c"),
        "fan_min_pct": fan0.get("min_percent"),
        "autotune": fetch_autotune(name),
    }


def read_state():
    global _LAST_SAMPLE_TS, _BEST_SHARE_DIFF
    try:
        m = fetch_api()
    except Exception as e:
        return {"ok": False, "reason": f"miner API unavailable ({e})"}

    boards_raw = m.get("boards") or []
    src = (m.get("sources") or [None])[0]

    hr_hs = m.get("hashrate") or 0
    hr_ghs = hr_hs / 1e9

    if _BEST_SHARE_DIFF == 0.0:
        store = load_autotune_store()
        _BEST_SHARE_DIFF = float(store.get("_global", {}).get("best_share_diff", 0.0))

    best_diff_raw = (
        m.get("best_share")
        or m.get("best_diff")
        or m.get("highest_difficulty")
        or (src or {}).get("best_share")
        or (src or {}).get("highest_difficulty")
    )
    if best_diff_raw is not None:
        try:
            val = float(best_diff_raw)
            if val > _BEST_SHARE_DIFF:
                _BEST_SHARE_DIFF = val
                store = load_autotune_store()
                store.setdefault("_global", {})["best_share_diff"] = _BEST_SHARE_DIFF
                save_autotune_store(store)
        except Exception:
            pass

    display_diff = _BEST_SHARE_DIFF

    now = time.monotonic()
    with _HIST_LOCK:
        record = (now - _LAST_SAMPLE_TS) >= SAMPLE_INTERVAL_S
        if record:
            _LAST_SAMPLE_TS = now
        if record and hr_ghs > 0:
            _FLEET_HISTORY.append(round(hr_ghs, 2))
            del _FLEET_HISTORY[:-120]
        fleet_series = list(_FLEET_HISTORY)

    boards_out = [_extract_board(b, boards_raw, hr_hs, record) for b in boards_raw]
    if record:
        present_names = {b.get("name") for b in boards_raw if b.get("name")}
        with _HIST_LOCK:
            for bname, bhist in _BOARD_HISTORY.items():
                if bname not in present_names:
                    bhist.append(0.0)
                    del bhist[:-120]

    def avg(vals):
        vals = [v for v in vals if v is not None]
        return (sum(vals) / len(vals)) if vals else None

    total_power = sum(b["core_w"] for b in boards_out if b["core_w"] is not None)
    have_power = any(b["core_w"] is not None for b in boards_out)

    fleet = {
        "hashrate_ghs": round(hr_ghs, 2),
        "hashrate_ths": round(hr_ghs / 1000, 3),
        "boards_online": len(boards_out),
        "avg_freq_mhz": avg([b["freq_mhz"] for b in boards_out]),
        "avg_core_v": avg([b["core_v"] for b in boards_out]),
        "avg_asic_c": avg([b["asic_c"] for b in boards_out]),
        "avg_vr_c": avg([b["vr_c"] for b in boards_out]),
        "total_power_w": round(total_power, 1) if have_power else None,
        "efficiency_jth": (
            round(total_power / (hr_ghs / 1000), 2)
            if have_power and hr_ghs > 0
            else None
        ),
        "avg_fan_rpm": avg([b["fan_rpm"] for b in boards_out]),
        "series": fleet_series,
    }

    return {
        "ok": True,
        "boards": boards_out,
        "fleet": fleet,
        "shares": m.get("shares_submitted"),
        "best_diff": fmt_diff(display_diff),
        "uptime": fmt_uptime(m.get("uptime_secs")),
        "paused": m.get("paused"),
        # The name the daemon is actually running under. A rename saved but
        # not yet restarted into deliberately does not show up here.
        "miner_name": m.get("name"),
        "pool_name": (src or {}).get("name"),
        "pool_url": (src or {}).get("url"),
        "difficulty": fmt_diff((src or {}).get("difficulty")),
    }


def fmt_uptime(secs):
    if secs is None:
        return None
    secs = int(secs)
    d, secs = divmod(secs, 86400)
    h, secs = divmod(secs, 3600)
    mi, s = divmod(secs, 60)
    if d:
        return f"{d}d {h}h {mi}m"
    if h:
        return f"{h}h {mi}m"
    return f"{mi}m {s}s"


HTML_PATH = os.path.join(HERE, "dashboard.html")
LOGO_PATH = os.path.join(HERE, "mujina-head-mark.svg")


def load_page_html():
    """Read the dashboard UI dynamically from disk on every request.

    This enables instant live reloading on browser refresh without
    restarting the Python server or miner process.
    """
    try:
        with open(HTML_PATH, "r", encoding="utf-8") as f:
            return f.read()
    except Exception as e:
        return f"<h1>Error loading dashboard.html</h1><p>{e}</p>"


def load_logo_svg():
    """The header brandmark -- mujina-head-mark.svg from rkuester's
    mujina-logo-set (github.com/rkuester/mujina-logo-set), used with the
    author's permission. Served as its own cacheable request rather than
    inlined into dashboard.html: at ~320KB it would otherwise get
    re-downloaded on every page load, since load_page_html()'s live-reload
    design intentionally sends dashboard.html with no-store."""
    try:
        with open(LOGO_PATH, "rb") as f:
            return f.read()
    except Exception:
        return b""


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json", cacheable=False):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if cacheable:
            # Static asset, never rewritten at runtime -- unlike
            # dashboard.html's intentional no-store (see load_page_html),
            # there's no live-reload reason to force a re-download every
            # request, and it's large enough (~320KB) to be worth avoiding.
            self.send_header("Cache-Control", "public, max-age=86400")
        else:
            self.send_header("Cache-Control", "no-store, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/data"):
            self._send(200, json.dumps(read_state()).encode())
        elif self.path.startswith("/mujina-head-mark.svg"):
            self._send(200, load_logo_svg(), "image/svg+xml", cacheable=True)
        elif self.path.startswith("/minersettings"):
            self._send(200, json.dumps(fetch_settings() or {}).encode())
        else:
            self._send(200, load_page_html().encode(), "text/html; charset=utf-8")

    def do_POST(self):
        if not (self.path.startswith("/settings") or self.path.startswith("/tuning")
                or self.path.startswith("/autotune")
                or self.path.startswith("/minersettings")
                or self.path.startswith("/restart")
                or self.path.startswith("/doom/launch")):
            self._send(404, b'{"ok":false}')
            return
        try:
            n = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(n) or b"{}")
        except Exception as e:
            self._send(400, json.dumps({"ok": False, "reason": str(e)}).encode())
            return

        if self.path.startswith("/restart"):
            ok, detail = restart_miner()
            self._send(200 if ok else 502,
                       json.dumps({"ok": ok, "detail": detail}).encode())
            return

        if self.path.startswith("/doom/launch"):
            ok, detail = launch_doom()
            self._send(200 if ok else 502,
                       json.dumps({"ok": ok, "detail": detail}).encode())
            return

        # Miner-wide, so it names no board and is handled before the
        # board_name requirement below.
        if self.path.startswith("/minersettings"):
            ok, detail = patch_settings(req)
            body = {"ok": ok}
            if ok:
                body["settings"] = detail
            else:
                body["detail"] = detail
            self._send(200 if ok else 502, json.dumps(body).encode())
            return

        # The client always names the target board explicitly now that a
        # daemon may report more than one -- there's no single implicit
        # default to fall back to.
        board = req.get("board_name")
        if not board:
            self._send(400, json.dumps({"ok": False, "reason": "board_name required"}).encode())
            return

        if self.path.startswith("/autotune"):
            payload = {"enabled": bool(req.get("enabled"))}
            if req.get("target"):
                payload["target"] = req["target"]
            elif req.get("profile"):
                payload["profile"] = req["profile"]
            ok, detail = patch_autotune(board, payload)
        elif self.path.startswith("/tuning"):
            payload = {}
            for k in ("frequency_mhz", "core_voltage_mv"):
                if req.get(k) is not None:
                    payload[k] = req[k]
            ok, detail = patch_tuning(board, payload)
        else:
            payload = {"auto": bool(req.get("auto"))}
            for k in ("target_c", "min_percent", "percent"):
                if req.get(k) is not None:
                    payload[k] = req[k]
            ok, detail = patch_fan(board, payload)

        code = 200 if ok else 502
        self._send(code, json.dumps({"ok": ok, "detail": detail}).encode())


class Srv(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

with Srv(("0.0.0.0", PORT), H) as httpd:
    print(f"dashboard on http://0.0.0.0:{PORT}  (source: {API})")
    httpd.serve_forever()
