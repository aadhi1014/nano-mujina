use std::mem;
use libc::{c_long, c_void, pid_t};

const NT_PRSTATUS: libc::c_int = 1;
const PTRACE_ATTACH:    u32 = 16;
const PTRACE_DETACH:    u32 = 17;
const PTRACE_GETREGSET: u32 = 0x4204;
const PTRACE_SETREGSET: u32 = 0x4205;
const PTRACE_CONT:      u32 = 7;
const PTRACE_PEEKDATA:  u32 = 2;
const PTRACE_POKEDATA:  u32 = 5;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
struct RiscvRegs {
    pc:  u64,
    ra:  u64, sp:  u64, gp: u64, tp: u64,
    t0:  u64, t1:  u64, t2: u64,
    s0:  u64, s1:  u64,
    a0:  u64, a1:  u64, a2: u64, a3: u64, a4: u64, a5: u64, a6: u64, a7: u64,
    s2:  u64, s3:  u64, s4: u64, s5: u64, s6: u64, s7: u64, s8: u64, s9: u64,
    s10: u64, s11: u64,
    t3:  u64, t4:  u64, t5: u64, t6: u64,
}

unsafe fn xptrace(req: u32, pid: pid_t, addr: usize, data: usize) -> c_long {
    libc::ptrace(req, pid, addr as *mut c_void, data as *mut c_void)
}

fn get_regs(pid: pid_t) -> Result<RiscvRegs, String> {
    let mut regs: RiscvRegs = unsafe { mem::zeroed() };
    let mut iov = libc::iovec {
        iov_base: &mut regs as *mut _ as *mut c_void,
        iov_len:  mem::size_of::<RiscvRegs>(),
    };
    let r = unsafe { xptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS as usize, &mut iov as *mut _ as usize) };
    if r == -1 { Err(format!("GETREGSET: {}", std::io::Error::last_os_error())) } else { Ok(regs) }
}

fn set_regs(pid: pid_t, regs: &RiscvRegs) -> Result<(), String> {
    let mut r = *regs;
    let mut iov = libc::iovec {
        iov_base: &mut r as *mut _ as *mut c_void,
        iov_len:  mem::size_of::<RiscvRegs>(),
    };
    let ret = unsafe { xptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS as usize, &mut iov as *mut _ as usize) };
    if ret == -1 { Err(format!("SETREGSET: {}", std::io::Error::last_os_error())) } else { Ok(()) }
}

fn peek_word(pid: pid_t, addr: u64) -> u64 {
    unsafe { xptrace(PTRACE_PEEKDATA, pid, addr as usize, 0) as u64 }
}
fn poke_word(pid: pid_t, addr: u64, val: u64) {
    unsafe { xptrace(PTRACE_POKEDATA, pid, addr as usize, val as usize); }
}

fn waitpid_stop(pid: pid_t) {
    let mut status: i32 = 0;
    loop {
        let r = unsafe { libc::waitpid(pid, &mut status, 0) };
        if r == pid { break; }
        if r == -1 && std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) { break; }
    }
}

/// Pre-scan: find an ecall (0x00000073) in the process text WITHOUT attaching.
/// As root we can open /proc/PID/mem for reading directly.
fn prescan_ecall(pid: pid_t) -> Option<u64> {
    use std::io::{BufRead, Read, Seek, SeekFrom};

    let maps = std::fs::File::open(format!("/proc/{}/maps", pid)).ok()?;
    let mut mem = std::fs::File::open(format!("/proc/{}/mem", pid)).ok()?;

    for line in std::io::BufReader::new(maps).lines() {
        let line = line.ok()?;
        let mut cols = line.split_whitespace();
        let range = cols.next()?;
        let perms = cols.next()?;
        let _off  = cols.next()?;
        let _dev  = cols.next()?;
        let _ino  = cols.next()?;
        let path  = cols.next().unwrap_or("");

        // Only scan executable, private, file-backed regions (the binary's own .text)
        if !perms.contains('x') { continue; }
        if perms.contains('s') { continue; }
        if path.is_empty() || path.starts_with('[') { continue; }

        let mut b = range.split('-');
        let start = u64::from_str_radix(b.next()?, 16).ok()?;
        let end   = u64::from_str_radix(b.next()?, 16).ok()?;

        let mut addr = start;
        while addr + 4 <= end {
            let chunk = ((end - addr) as usize).min(4096);
            let mut buf = vec![0u8; chunk];
            if mem.seek(SeekFrom::Start(addr)).is_err() { break; }
            let n = mem.read(&mut buf).unwrap_or(0);
            if n < 4 { break; }
            for i in (0..n.saturating_sub(3)).step_by(4) {
                if buf[i..i+4] == [0x73, 0x00, 0x00, 0x00] { // ecall LE
                    return Some(addr + i as u64);
                }
            }
            addr += n as u64;
        }
    }
    None
}

fn detach(pid: pid_t) {
    unsafe { xptrace(PTRACE_DETACH, pid, 0, 0); }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        eprintln!("Usage: {} <pid> <fb0_addr_hex> <fb0_size_hex>", args[0]);
        eprintln!("  Example: {} 239 3fcc28c000 1d000", args[0]);
        std::process::exit(1);
    }

    let pid  = args[1].parse::<pid_t>().expect("bad pid");
    let addr = u64::from_str_radix(&args[2], 16).expect("bad addr");
    let size = u64::from_str_radix(&args[3], 16).expect("bad size");

    println!("[*] Target: PID {} fb0=0x{:x} size=0x{:x}", pid, addr, size);

    // --- Step 1: pre-scan for ecall BEFORE attaching (keeps pause window tiny) ---
    print!("[*] Scanning for ecall (no attach yet)... ");
    let _ = std::io::Write::flush(&mut std::io::stdout());
    let ecall_va = match prescan_ecall(pid) {
        Some(v) => { println!("found at 0x{:x}", v); v }
        None => { eprintln!("not found"); std::process::exit(1); }
    };

    // --- Step 2: attach (process pauses here — be fast from now on) ---
    if unsafe { xptrace(PTRACE_ATTACH, pid, 0, 0) } == -1 {
        eprintln!("[-] ATTACH: {}", std::io::Error::last_os_error());
        std::process::exit(1);
    }
    waitpid_stop(pid);
    println!("[*] Attached (watchdog clock ticking)");

    // Save registers
    let saved = match get_regs(pid) {
        Ok(r) => r,
        Err(e) => { eprintln!("[-] {}", e); detach(pid); std::process::exit(1); }
    };

    // Plant EBREAK (0x00100073) at ecall+4
    let bp_addr    = ecall_va + 4;
    let saved_word = peek_word(pid, bp_addr);
    // Replace only the low 32 bits with EBREAK, keep high 32 bits intact
    let ebreak_word = (saved_word & 0xFFFF_FFFF_0000_0000) | 0x0010_0073;
    poke_word(pid, bp_addr, ebreak_word);

    // Set up mmap syscall registers on the stopped thread:
    // mmap(addr, size, PROT_RW, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0)
    let mut regs  = saved;
    regs.pc = ecall_va;
    regs.a7 = 222;           // __NR_mmap
    regs.a0 = addr;
    regs.a1 = size;
    regs.a2 = 0x03;          // PROT_READ|PROT_WRITE
    regs.a3 = 0x32;          // MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED
    regs.a4 = u64::MAX;      // fd = -1
    regs.a5 = 0;

    if let Err(e) = set_regs(pid, &regs) {
        eprintln!("[-] {}", e);
        poke_word(pid, bp_addr, saved_word);
        detach(pid); std::process::exit(1);
    }

    // PTRACE_CONT — process runs, executes ecall, hits EBREAK, stops again
    if unsafe { xptrace(PTRACE_CONT, pid, 0, 0) } == -1 {
        eprintln!("[-] CONT: {}", std::io::Error::last_os_error());
        poke_word(pid, bp_addr, saved_word);
        detach(pid); std::process::exit(1);
    }
    waitpid_stop(pid);  // wait for SIGTRAP from EBREAK

    // Read mmap return value
    let after = match get_regs(pid) {
        Ok(r) => r,
        Err(e) => { eprintln!("[-] {}", e); poke_word(pid, bp_addr, saved_word); detach(pid); std::process::exit(1); }
    };
    let ret = after.a0 as i64;

    // Restore breakpoint bytes and original registers, detach ASAP
    poke_word(pid, bp_addr, saved_word);
    let _ = set_regs(pid, &saved);
    detach(pid);

    if ret == addr as i64 {
        println!("[+] SUCCESS  mmap returned 0x{:x}", after.a0);
        println!("[+] mm_miner LVGL writes -> anonymous buffer (discarded)");
        println!("[+] /dev/fb0 is yours — write freely");
    } else {
        eprintln!("[-] mmap failed: {} (errno {})", ret, -ret);
    }
}
