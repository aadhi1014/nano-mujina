//! Button controller for Avalon Nano3s.
//! event2 is exclusively grabbed for display/mode control; event1 supplies
//! audible confirmation through the PWM beeper. event0 (system reset) is not touched.

use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::fd::AsRawFd;
use std::os::raw::{c_int, c_ulong};
use std::thread;
use std::time::{Duration, Instant};

const BUTTON_DEV: &str = "/dev/input/event2";
const BEEPER_DEV: &str = "/dev/input/event1";
const PAGE_FILE: &str = "/mntapp/release/linux/app/fb_page";
const EV_KEY: u16 = 0x01;
const EV_SND: u16 = 0x12;
const EV_SYN: u16 = 0x00;
const SND_TONE: u16 = 0x02;
const EVIOCGRAB: c_ulong = 0x4004_4590;

#[repr(C)]
struct PollFd { fd: c_int, events: i16, revents: i16 }

unsafe extern "C" {
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    fn poll(fds: *mut PollFd, nfds: usize, timeout: c_int) -> c_int;
}

fn write_event(file: &mut File, typ: u16, code: u16, value: i32) -> std::io::Result<()> {
    // struct input_event on the K230's 64-bit Linux ABI: timeval + type/code/value.
    let mut event = [0u8; 24];
    event[16..18].copy_from_slice(&typ.to_ne_bytes());
    event[18..20].copy_from_slice(&code.to_ne_bytes());
    event[20..24].copy_from_slice(&value.to_ne_bytes());
    file.write_all(&event)
}

fn beep(beeper: &mut File, millis: u64) {
    let _ = write_event(beeper, EV_SND, SND_TONE, 2200);
    let _ = write_event(beeper, EV_SYN, 0, 0);
    thread::sleep(Duration::from_millis(millis));
    let _ = write_event(beeper, EV_SND, SND_TONE, 0);
    let _ = write_event(beeper, EV_SYN, 0, 0);
}

// REMOVED (2026-08-06): cgminer()/CGMINER_API/next_mode()/
// DOUBLE_CLICK_WINDOW -- the double-click "cycle workmode via cgminer
// API" feature. Confirmed dead on the real device (nothing listens on
// port 4028, no cgminer/mm_miner process at all -- this project's own
// rtos_core.elf/mujina-minerd stack replaced it months ago), and every
// single click was paying its ~450ms disambiguation tax regardless.
// See main()'s own comment for the full story -- this was the real
// cause of the ">1s delay, sometimes need multiple presses" complaint.

// Page cycle for the custom rtos_core/mujina stack (fb_draw's
// "live-nano3s" mode) -- replaces the old stock-cgminer "stats" ->
// "temps" cycle, since "temps" depended on cgminer's PVT_T[] stats
// reply, which doesn't exist once mm_miner/cgminer are gone. "pizza"
// and "ip" are stack-independent (static image, ifconfig) and stay.
fn toggle_page() {
    let current = fs::read_to_string(PAGE_FILE).unwrap_or_else(|_| "nano3s".to_owned());
    let next = match current.trim() {
        "nano3s" => "nano3s-diag",
        "nano3s-diag" => "ip",
        "ip" => "pizza",
        "pizza" => "nano3s",
        _ => "nano3s",
    };
    let _ = fs::write(PAGE_FILE, format!("{next}\n"));
}

fn main() {
    let mut button = OpenOptions::new().read(true).open(BUTTON_DEV)
        .unwrap_or_else(|e| panic!("open {BUTTON_DEV}: {e}"));
    let grabbed = unsafe { ioctl(button.as_raw_fd(), EVIOCGRAB, 1i32) };
    if grabbed != 0 { panic!("EVIOCGRAB {BUTTON_DEV} failed: {}", std::io::Error::last_os_error()); }
    let mut beeper = OpenOptions::new().write(true).open(BEEPER_DEV)
        .unwrap_or_else(|e| panic!("open {BEEPER_DEV}: {e}"));
    eprintln!("fb_button: exclusively controlling {BUTTON_DEV}; event0 reset remains unchanged");

    let mut down: Option<Instant> = None;
    let mut event = [0u8; 24];
    loop {
        // REMOVED (2026-08-06, real user complaint: ">1s delay between
        // button press and page change, sometimes needs multiple
        // presses"): every single click used to wait out the full
        // DOUBLE_CLICK_WINDOW (450ms) before toggle_page() fired, to
        // disambiguate it from a double-click (which called next_mode(),
        // a cgminer-API-based workmode cycler). Confirmed live on the
        // real device that this feature has been dead for a long time --
        // `netstat` shows nothing listening on CGMINER_API's port 4028,
        // no cgminer/mm_miner process exists at all (this project
        // replaced that whole stack with rtos_core.elf/mujina-minerd
        // months ago). So every click was paying a real ~450ms
        // unresponsiveness tax for a feature that could never succeed --
        // and if a frustrated user clicked again inside that window
        // (a natural reaction to apparent unresponsiveness), the SECOND
        // click got misread as a double-click, firing next_mode()
        // (always failing) instead of a second page toggle -- directly
        // explaining "sometimes I need to click multiple times" too.
        // Blocking poll with no timeout now -- toggle_page() fires the
        // instant a real key-up is seen, with zero artificial delay.
        let mut fds = PollFd { fd: button.as_raw_fd(), events: 1, revents: 0 };
        let ready = unsafe { poll(&mut fds, 1, -1) };
        if ready < 0 || button.read_exact(&mut event).is_err() { continue; }
        let typ = u16::from_ne_bytes([event[16], event[17]]);
        let value = i32::from_ne_bytes([event[20], event[21], event[22], event[23]]);
        if typ != EV_KEY { continue; }
        if value == 1 {
            down = Some(Instant::now());
            beep(&mut beeper, 25);
        }
        if value == 0 {
            let held = down.take().map(|t| t.elapsed()).unwrap_or_default();
            if held >= Duration::from_millis(5) {
                toggle_page();
            }
        }
    }
}
