//! Standalone BLE GATT server for first-boot WiFi provisioning, compatible
//! with the wire protocol independently documented by the third-party
//! GPL-3.0 `nano3ble` client (https://github.com/skot/nano3ble):
//! service UUID 0xFFFF, characteristics FFE1 (read: wifi scan),
//! FFE2 (write: commands), FFE3 (read: JSON status), ASCII commands
//! terminated with "$$$$".
//!
//! No BlueZ/D-Bus stack exists on this device, so this speaks raw
//! HCI/L2CAP/ATT directly over an HCI_CHANNEL_USER socket, which
//! exclusively claims the controller from userspace.
//!
//! Stage 1: HCI bring-up + LE advertising -- verified working (device
//! visible to a real scanner, LE connection establishes cleanly).
//! Stage 2 (current): minimal ATT/GATT server -- service/characteristic
//! discovery, read/write on FFE1/FFE2/FFE3, scan/setssid commands.

use std::io;
use std::mem;
use std::sync::{Arc, Mutex};

const AF_BLUETOOTH: i32 = 31;
const BTPROTO_HCI: i32 = 1;
const HCI_CHANNEL_USER: u16 = 1;
const HCI_DEV_ID: u16 = 0; // hci0

// ---------------------------------------------------------------------------
// LCD status page (nano3s_ui's "wifi-setup" page, driver.c). The real
// Avalon Life app's own success/failure indicator was found unreliable
// (see the module doc comment history) -- the device's own screen is the
// source of truth for setup progress instead.
// ---------------------------------------------------------------------------

const PAGE_FILE: &str = "/mntapp/release/linux/app/fb_page";
const BLE_WIFI_STATUS_FILE: &str = "/tmp/ble_wifi_status.txt";

fn set_display_page(page: &str) {
    if let Err(e) = std::fs::write(PAGE_FILE, page) {
        eprintln!("step: failed to write {PAGE_FILE}: {e}");
    }
}

/// Writes BLE_WIFI_STATUS_FILE for nano3s_ui's "wifi-setup" page to
/// read. `state` is one of advertising/client_connected/
/// credentials_received/connecting/connected/failed (see driver.c's
/// refresh_wifi_setup() for exactly how each maps to on-screen text).
fn write_wifi_status(state: &str, detail: &str) {
    let body = format!("state={state}\ndetail={detail}\n");
    if let Err(e) = std::fs::write(BLE_WIFI_STATUS_FILE, body) {
        eprintln!("step: failed to write {BLE_WIFI_STATUS_FILE}: {e}");
    }
}

#[repr(C)]
struct SockaddrHci {
    hci_family: u16,
    hci_dev: u16,
    hci_channel: u16,
}

fn open_hci_user_channel(dev_id: u16) -> io::Result<i32> {
    unsafe {
        let fd = libc::socket(AF_BLUETOOTH, libc::SOCK_RAW, BTPROTO_HCI);
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let addr = SockaddrHci { hci_family: AF_BLUETOOTH as u16, hci_dev: dev_id, hci_channel: HCI_CHANNEL_USER };
        let rc = libc::bind(fd, &addr as *const _ as *const libc::sockaddr, mem::size_of::<SockaddrHci>() as u32);
        if rc < 0 {
            let e = io::Error::last_os_error();
            libc::close(fd);
            return Err(e);
        }
        Ok(fd)
    }
}

/// Sends a raw HCI command packet: [0x01, opcode_lo, opcode_hi, len, params...].
fn send_hci_command(fd: i32, opcode: u16, params: &[u8]) -> io::Result<()> {
    let mut pkt = Vec::with_capacity(4 + params.len());
    pkt.push(0x01); // H:4 packet type: Command
    pkt.extend_from_slice(&opcode.to_le_bytes());
    pkt.push(params.len() as u8);
    pkt.extend_from_slice(params);
    unsafe {
        let n = libc::write(fd, pkt.as_ptr() as *const _, pkt.len());
        if n < 0 {
            return Err(io::Error::last_os_error());
        }
    }
    Ok(())
}

/// Reads HCI packets until a Command Complete (or Command Status) event
/// for `opcode` arrives, returning its status byte (0 = success) and any
/// trailing return-parameter bytes.
fn wait_command_complete(fd: i32, opcode: u16) -> io::Result<(u8, Vec<u8>)> {
    let mut buf = [0u8; 260];
    loop {
        let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut _, buf.len()) };
        if n < 0 {
            return Err(io::Error::last_os_error());
        }
        let n = n as usize;
        if n < 1 {
            continue;
        }
        if buf[0] != 0x04 {
            continue; // not an HCI Event packet
        }
        if n < 3 {
            continue;
        }
        let event_code = buf[1];
        let plen = buf[2] as usize;
        if n < 3 + plen {
            continue;
        }
        let params = &buf[3..3 + plen];
        match event_code {
            0x0E if params.len() >= 3 => {
                // Command Complete: num_packets(1) opcode(2) status(1) return_params...
                let evt_opcode = u16::from_le_bytes([params[1], params[2]]);
                if evt_opcode == opcode {
                    let status = if params.len() >= 4 { params[3] } else { 0xff };
                    let rest = if params.len() > 4 { params[4..].to_vec() } else { Vec::new() };
                    return Ok((status, rest));
                }
            }
            0x0F if params.len() >= 4 => {
                // Command Status: status(1) num_packets(1) opcode(2)
                let evt_opcode = u16::from_le_bytes([params[2], params[3]]);
                if evt_opcode == opcode {
                    return Ok((params[0], Vec::new()));
                }
            }
            _ => {}
        }
        // else: unrelated event, keep reading
    }
}

fn hci_reset(fd: i32) -> io::Result<()> {
    let opcode = (0x03u16 << 10) | 0x0003; // OGF=3 Host Control Baseband, OCF=3 Reset
    send_hci_command(fd, opcode, &[])?;
    let (status, _) = wait_command_complete(fd, opcode)?;
    if status != 0 {
        return Err(io::Error::new(io::ErrorKind::Other, format!("HCI Reset failed: status=0x{status:02x}")));
    }
    Ok(())
}

/// HCI_Set_Event_Mask (OGF=3, OCF=1). The default mask after Reset
/// (0x1FFFFFFFFFFFFFFF) does NOT include bit 61 (LE Meta Event) --
/// without this, LE Connection Complete and every other LE sub-event
/// are silently never delivered to the host, even though the link layer
/// itself accepts the connection (which is exactly the bug this fixes:
/// the client saw a successful connection while our event loop never
/// saw anything at all).
fn hci_set_event_mask(fd: i32) -> io::Result<()> {
    let opcode = (0x03u16 << 10) | 0x0001;
    let mask: u64 = 0x1FFFFFFFFFFFFFFF | (1u64 << 61);
    send_hci_command(fd, opcode, &mask.to_le_bytes())?;
    let (status, _) = wait_command_complete(fd, opcode)?;
    if status != 0 {
        return Err(io::Error::new(io::ErrorKind::Other, format!("HCI Set Event Mask failed: status=0x{status:02x}")));
    }
    Ok(())
}

/// LE_Set_Event_Mask (OGF=8, OCF=1). Explicit even though the spec
/// default already includes LE Connection Complete, to remove any doubt.
fn le_set_event_mask(fd: i32) -> io::Result<()> {
    let opcode = (0x08u16 << 10) | 0x0001;
    // Bit 0: LE Connection Complete, bit 0x01. Add a few more common/
    // useful ones: bit 1 LE Advertising Report, bit 3 LE Connection
    // Update Complete.
    let mask: u64 = 0x1F;
    send_hci_command(fd, opcode, &mask.to_le_bytes())?;
    let (status, _) = wait_command_complete(fd, opcode)?;
    if status != 0 {
        return Err(io::Error::new(io::ErrorKind::Other, format!("LE Set Event Mask failed: status=0x{status:02x}")));
    }
    Ok(())
}

fn le_set_advertising_parameters(fd: i32) -> io::Result<()> {
    let opcode = (0x08u16 << 10) | 0x0006; // OGF=8 LE Controller, OCF=6
    let mut p = Vec::new();
    p.extend_from_slice(&0x00A0u16.to_le_bytes()); // interval_min = 100ms (0.625ms units: 160=100ms)
    p.extend_from_slice(&0x00A0u16.to_le_bytes()); // interval_max = 100ms
    p.push(0x00); // adv_type = ADV_IND (connectable + scannable undirected)
    p.push(0x00); // own_addr_type = public
    p.push(0x00); // peer_addr_type = public
    p.extend_from_slice(&[0u8; 6]); // peer_addr (unused for undirected)
    p.push(0x07); // channel_map = all 3 (37,38,39)
    p.push(0x00); // filter_policy = process scan/connect from any
    send_hci_command(fd, opcode, &p)?;
    let (status, _) = wait_command_complete(fd, opcode)?;
    if status != 0 {
        return Err(io::Error::new(io::ErrorKind::Other, format!("LE Set Advertising Parameters failed: status=0x{status:02x}")));
    }
    Ok(())
}

fn le_set_advertising_data(fd: i32, name: &str) -> io::Result<()> {
    let opcode = (0x08u16 << 10) | 0x0008; // OCF=8
    let mut ad = Vec::new();
    // Flags: LE General Discoverable Mode, BR/EDR Not Supported
    ad.push(0x02);
    ad.push(0x01);
    ad.push(0x06);
    // Complete Local Name
    let name_bytes = name.as_bytes();
    ad.push((1 + name_bytes.len()) as u8);
    ad.push(0x09);
    ad.extend_from_slice(name_bytes);
    // 16-bit Service UUID: 0xFFFF (Complete List)
    ad.push(0x03);
    ad.push(0x03);
    ad.extend_from_slice(&0xFFFFu16.to_le_bytes());

    let mut p = Vec::with_capacity(32);
    p.push(ad.len() as u8);
    p.extend_from_slice(&ad);
    p.resize(32, 0); // length byte + 31 bytes of data, zero-padded
    send_hci_command(fd, opcode, &p)?;
    let (status, _) = wait_command_complete(fd, opcode)?;
    if status != 0 {
        return Err(io::Error::new(io::ErrorKind::Other, format!("LE Set Advertising Data failed: status=0x{status:02x}")));
    }
    Ok(())
}

fn le_set_advertise_enable(fd: i32, enable: bool) -> io::Result<()> {
    let opcode = (0x08u16 << 10) | 0x000A; // OCF=0x0A
    send_hci_command(fd, opcode, &[if enable { 1 } else { 0 }])?;
    let (status, _) = wait_command_complete(fd, opcode)?;
    if status != 0 {
        return Err(io::Error::new(io::ErrorKind::Other, format!("LE Set Advertise Enable failed: status=0x{status:02x}")));
    }
    Ok(())
}

/// HCI_Read_BD_ADDR (OGF=4 Informational Parameters, OCF=9). Returns the
/// controller's public address, LSB first per HCI convention.
fn read_bd_addr(fd: i32) -> io::Result<[u8; 6]> {
    let opcode = (0x04u16 << 10) | 0x0009;
    send_hci_command(fd, opcode, &[])?;
    let (status, ret) = wait_command_complete(fd, opcode)?;
    if status != 0 || ret.len() < 6 {
        return Err(io::Error::new(io::ErrorKind::Other, format!("Read BD_ADDR failed: status=0x{status:02x}")));
    }
    let mut addr = [0u8; 6];
    addr.copy_from_slice(&ret[0..6]);
    Ok(addr)
}

fn device_name_suffix(fd: i32) -> String {
    match read_bd_addr(fd) {
        // BD_ADDR bytes are LSB-first; the two least-significant octets
        // (array indices 0,1) are the ones that vary most between units.
        Ok(addr) => format!("{:02x}{:02x}", addr[1], addr[0]),
        Err(_) => "0000".to_string(),
    }
}

fn main() {
    // Only advertise when wlan0 isn't already connected -- this is
    // meant to run in a supervised restart loop from boot (see
    // mujina_display_startup.sh), so once WiFi is configured this
    // becomes a cheap periodic no-op rather than an always-on radio.
    // Real vendor behavior (per nano3ble's own doc comment) stops BLE
    // advertising ~90s after WiFi connects; this achieves the same
    // effect more simply by never starting it in the first place once
    // already connected, rechecked every 60s via the sleep+exit below.
    let force_advertise = std::env::var("BLE_SETUP_FORCE_ADVERTISE").as_deref() == Ok("1");
    if let Some(ip) = ifconfig_wlan0_ip() {
        if force_advertise {
            eprintln!("step: wlan0 connected (ip={ip}) but BLE_SETUP_FORCE_ADVERTISE=1, advertising anyway");
        } else {
            eprintln!("step: wlan0 already connected (ip={ip}), not advertising; idling before recheck");
            std::thread::sleep(std::time::Duration::from_secs(60));
            return;
        }
    }

    let fd = open_hci_user_channel(HCI_DEV_ID).unwrap_or_else(|e| {
        eprintln!("failed to open HCI_CHANNEL_USER on hci{HCI_DEV_ID}: {e}");
        std::process::exit(1);
    });
    eprintln!("step: HCI user channel opened, fd={fd}");

    hci_reset(fd).unwrap_or_else(|e| {
        eprintln!("HCI reset failed: {e}");
        std::process::exit(1);
    });
    eprintln!("step: HCI reset OK");

    hci_set_event_mask(fd).unwrap_or_else(|e| {
        eprintln!("set event mask failed: {e}");
        std::process::exit(1);
    });
    le_set_event_mask(fd).unwrap_or_else(|e| {
        eprintln!("LE set event mask failed: {e}");
        std::process::exit(1);
    });
    eprintln!("step: event masks set (LE Meta Event enabled)");

    let name = format!("nan3s_{}", device_name_suffix(fd));
    eprintln!("step: advertising as '{name}'");

    le_set_advertising_parameters(fd).unwrap_or_else(|e| {
        eprintln!("set adv params failed: {e}");
        std::process::exit(1);
    });
    eprintln!("step: advertising parameters set");

    le_set_advertising_data(fd, &name).unwrap_or_else(|e| {
        eprintln!("set adv data failed: {e}");
        std::process::exit(1);
    });
    eprintln!("step: advertising data set");

    le_set_advertise_enable(fd, true).unwrap_or_else(|e| {
        eprintln!("advertise enable failed: {e}");
        std::process::exit(1);
    });
    println!("RESULT: advertising enabled as '{name}' -- check for it on a scanner now.");
    write_wifi_status("advertising", &name);
    set_display_page("wifi-setup");

    event_loop(fd, &name);
}

// ---------------------------------------------------------------------------
// GATT attribute database
// ---------------------------------------------------------------------------

const UUID_PRIMARY_SERVICE: u16 = 0x2800;
const UUID_CHARACTERISTIC: u16 = 0x2803;
const UUID_SERVICE_FFFF: u16 = 0xFFFF;
const UUID_CHAR_FFE1: u16 = 0xFFE1; // read: wifi scan results
const UUID_CHAR_FFE2: u16 = 0xFFE2; // write: commands
const UUID_CHAR_FFE3: u16 = 0xFFE3; // read: JSON status

// Standard Generic Access service. Real BLE stacks (the ones the real
// Avalon app's index-based reads were tuned against) auto-expose this
// ahead of any custom service, with a READ-capable Device Name
// characteristic. iOS's CoreBluetooth filters 0x1800/0x1801 out of
// `discoverServices()` results by platform policy; Android's
// BluetoothGatt does not. That one-service difference is exactly the
// +1 offset the app's Dart source hardcodes between its iOS and
// Android read(index:) calls for the wifi-scan-results and status
// characteristics (see the app's own GPL-3.0 source,
// Canaan-Creative/avalon_family) -- without exposing this service
// ourselves, Android's indices land one slot early and silently read
// the wrong (empty) characteristic instead of erroring.
const UUID_SERVICE_GAP: u16 = 0x1800;
const UUID_CHAR_DEVICE_NAME: u16 = 0x2A00;

const CHR_PROP_READ: u8 = 0x02;
const CHR_PROP_WRITE_NO_RSP: u8 = 0x04;
const CHR_PROP_WRITE: u8 = 0x08;

// Fixed attribute handles (no dynamic allocation needed -- the
// database never changes shape at runtime). GAP service first (lowest
// handles), matching what real BLE stacks auto-generate.
const H_GAP_SERVICE: u16 = 0x0001;
const H_GAP_CHAR_DECL: u16 = 0x0002;
const H_GAP_CHAR_VAL: u16 = 0x0003; // Device Name

const H_SERVICE: u16 = 0x0004;
const H_CHAR1_DECL: u16 = 0x0005;
const H_CHAR1_VAL: u16 = 0x0006; // FFE1
const H_CHAR2_DECL: u16 = 0x0007;
const H_CHAR2_VAL: u16 = 0x0008; // FFE2
const H_CHAR3_DECL: u16 = 0x0009;
const H_CHAR3_VAL: u16 = 0x000A; // FFE3
const H_MAX: u16 = 0x000A;

struct GattState {
    ffe1_value: Vec<u8>, // scan results
    ffe3_value: Vec<u8>, // status JSON
    cmd_buf: Vec<u8>,    // FFE2 write accumulator
    device_name: String,
}

// ---------------------------------------------------------------------------
// ATT PDU helpers
// ---------------------------------------------------------------------------

const ATT_OP_ERROR_RSP: u8 = 0x01;
const ATT_OP_EXCHANGE_MTU_REQ: u8 = 0x02;
const ATT_OP_EXCHANGE_MTU_RSP: u8 = 0x03;
const ATT_OP_FIND_INFO_REQ: u8 = 0x04;
const ATT_OP_FIND_INFO_RSP: u8 = 0x05;
const ATT_OP_READ_BY_TYPE_REQ: u8 = 0x08;
const ATT_OP_READ_BY_TYPE_RSP: u8 = 0x09;
const ATT_OP_READ_REQ: u8 = 0x0A;
const ATT_OP_READ_RSP: u8 = 0x0B;
const ATT_OP_READ_BLOB_REQ: u8 = 0x0C;
const ATT_OP_READ_BLOB_RSP: u8 = 0x0D;
const ATT_OP_READ_BY_GROUP_TYPE_REQ: u8 = 0x10;
const ATT_OP_READ_BY_GROUP_TYPE_RSP: u8 = 0x11;
const ATT_OP_WRITE_REQ: u8 = 0x12;
const ATT_OP_WRITE_RSP: u8 = 0x13;
const ATT_OP_WRITE_CMD: u8 = 0x52;

const ATT_ECODE_INVALID_HANDLE: u8 = 0x01;
const ATT_ECODE_READ_NOT_PERMITTED: u8 = 0x02;
const ATT_ECODE_WRITE_NOT_PERMITTED: u8 = 0x03;
const ATT_ECODE_INVALID_OFFSET: u8 = 0x07;
const ATT_ECODE_ATTRIBUTE_NOT_FOUND: u8 = 0x0A;

/// Sends an L2CAP/ATT PDU to `handle` over the raw HCI socket, wrapped in
/// a single (unfragmented -- our payloads are always small) ACL Data
/// packet.
fn send_l2cap(fd: i32, conn_handle: u16, cid: u16, payload: &[u8]) {
    let mut pkt = Vec::with_capacity(9 + payload.len());
    pkt.push(0x02); // H:4 packet type: ACL Data
    let handle_flags = conn_handle & 0x0FFF; // PB=0 (first, non-auto-flush), BC=0
    pkt.extend_from_slice(&handle_flags.to_le_bytes());
    let l2cap_len = 4 + payload.len(); // l2cap header(4) + payload
    pkt.extend_from_slice(&(l2cap_len as u16).to_le_bytes()); // ACL data total length
    pkt.extend_from_slice(&(payload.len() as u16).to_le_bytes()); // L2CAP payload length
    pkt.extend_from_slice(&cid.to_le_bytes());
    pkt.extend_from_slice(payload);
    unsafe {
        libc::write(fd, pkt.as_ptr() as *const _, pkt.len());
    }
}

const ATT_CID: u16 = 0x0004;

fn att_error(fd: i32, conn_handle: u16, opcode: u8, handle: u16, ecode: u8) {
    let payload = [ATT_OP_ERROR_RSP, opcode, (handle & 0xff) as u8, (handle >> 8) as u8, ecode];
    send_l2cap(fd, conn_handle, ATT_CID, &payload);
}

/// Returns this attribute's UUID and current value bytes, or `None` if
/// `handle` doesn't exist.
fn attr_lookup(handle: u16, state: &GattState) -> Option<(u16, Vec<u8>)> {
    match handle {
        H_GAP_SERVICE => Some((UUID_PRIMARY_SERVICE, UUID_SERVICE_GAP.to_le_bytes().to_vec())),
        H_GAP_CHAR_DECL => Some((UUID_CHARACTERISTIC, char_decl_value(CHR_PROP_READ, H_GAP_CHAR_VAL, UUID_CHAR_DEVICE_NAME))),
        H_GAP_CHAR_VAL => Some((UUID_CHAR_DEVICE_NAME, state.device_name.clone().into_bytes())),
        H_SERVICE => Some((UUID_PRIMARY_SERVICE, UUID_SERVICE_FFFF.to_le_bytes().to_vec())),
        H_CHAR1_DECL => Some((UUID_CHARACTERISTIC, char_decl_value(CHR_PROP_READ, H_CHAR1_VAL, UUID_CHAR_FFE1))),
        H_CHAR1_VAL => Some((UUID_CHAR_FFE1, state.ffe1_value.clone())),
        // Deliberately WRITE-only (no READ): with the GAP service above
        // now providing the one-extra-characteristic offset Android's
        // real BLE stack expects, the read-capable list is [FFE1, FFE3]
        // on both platforms after platform-side filtering, matching the
        // app's hardcoded read(index:) values exactly (iOS: 0/1,
        // Android: 1/2 -- one ahead of iOS since Android's stack doesn't
        // filter the GAP service out of discovery like iOS's does).
        // Previously this had READ added too, which happened to line up
        // Android's *status* read but broke its *wifi-scan-results* read
        // (both landed on FFE2's empty value one slot early) -- this and
        // the GAP service together fix both reads on both platforms.
        H_CHAR2_DECL => Some((UUID_CHARACTERISTIC, char_decl_value(CHR_PROP_WRITE | CHR_PROP_WRITE_NO_RSP, H_CHAR2_VAL, UUID_CHAR_FFE2))),
        H_CHAR2_VAL => Some((UUID_CHAR_FFE2, Vec::new())),
        H_CHAR3_DECL => Some((UUID_CHARACTERISTIC, char_decl_value(CHR_PROP_READ, H_CHAR3_VAL, UUID_CHAR_FFE3))),
        H_CHAR3_VAL => Some((UUID_CHAR_FFE3, state.ffe3_value.clone())),
        _ => None,
    }
}

fn char_decl_value(props: u8, value_handle: u16, uuid: u16) -> Vec<u8> {
    let mut v = vec![props];
    v.extend_from_slice(&value_handle.to_le_bytes());
    v.extend_from_slice(&uuid.to_le_bytes());
    v
}

fn handle_exchange_mtu(fd: i32, conn_handle: u16, req: &[u8], mtu: &mut usize) {
    let client_mtu = if req.len() >= 3 { u16::from_le_bytes([req[1], req[2]]) as usize } else { 23 };
    // We don't need more than a few hundred bytes for this protocol;
    // cap at something comfortably above the 512-byte command buffer's
    // chunking needs without over-claiming.
    let server_mtu: usize = 247;
    *mtu = client_mtu.min(server_mtu).max(23);
    let payload = [ATT_OP_EXCHANGE_MTU_RSP, (server_mtu & 0xff) as u8, (server_mtu >> 8) as u8];
    send_l2cap(fd, conn_handle, ATT_CID, &payload);
    eprintln!("step: MTU exchange: client={client_mtu} server={server_mtu} negotiated={mtu}");
}

fn handle_read_by_group_type(fd: i32, conn_handle: u16, req: &[u8]) {
    if req.len() < 7 {
        att_error(fd, conn_handle, ATT_OP_READ_BY_GROUP_TYPE_REQ, 0, ATT_ECODE_INVALID_HANDLE);
        return;
    }
    let start = u16::from_le_bytes([req[1], req[2]]);
    let end = u16::from_le_bytes([req[3], req[4]]);
    let group_type = u16::from_le_bytes([req[5], req[6]]);
    if group_type != UUID_PRIMARY_SERVICE {
        att_error(fd, conn_handle, ATT_OP_READ_BY_GROUP_TYPE_REQ, start, ATT_ECODE_ATTRIBUTE_NOT_FOUND);
        return;
    }
    // Two services/groups now: GAP first (lowest handles), then our
    // custom FFFF service. Real clients discover iteratively, re-querying
    // from the last group's end handle + 1, so only a group whose *start*
    // handle falls in [start, end] belongs in this response -- both UUIDs
    // are 16-bit, so entry length (2+2+2=6) matches and they can share one
    // response when a single wide query (the common case) covers both.
    let groups: [(u16, u16, u16); 2] =
        [(H_GAP_SERVICE, H_GAP_CHAR_VAL, UUID_SERVICE_GAP), (H_SERVICE, H_MAX, UUID_SERVICE_FFFF)];
    let mut payload = vec![ATT_OP_READ_BY_GROUP_TYPE_RSP, 6]; // length per entry = 2+2+2
    let mut any = false;
    for &(group_start, group_end, uuid) in &groups {
        if group_start >= start && group_start <= end {
            payload.extend_from_slice(&group_start.to_le_bytes());
            payload.extend_from_slice(&group_end.to_le_bytes());
            payload.extend_from_slice(&uuid.to_le_bytes());
            any = true;
        }
    }
    if !any {
        att_error(fd, conn_handle, ATT_OP_READ_BY_GROUP_TYPE_REQ, start, ATT_ECODE_ATTRIBUTE_NOT_FOUND);
        return;
    }
    send_l2cap(fd, conn_handle, ATT_CID, &payload);
}

fn handle_read_by_type(fd: i32, conn_handle: u16, req: &[u8], state: &Arc<Mutex<GattState>>) {
    let guard = state.lock().unwrap_or_else(|e| e.into_inner());
    let state: &GattState = &guard;
    if req.len() < 7 {
        att_error(fd, conn_handle, ATT_OP_READ_BY_TYPE_REQ, 0, ATT_ECODE_INVALID_HANDLE);
        return;
    }
    let start = u16::from_le_bytes([req[1], req[2]]);
    let end = u16::from_le_bytes([req[3], req[4]]);
    let attr_type = u16::from_le_bytes([req[5], req[6]]);

    if attr_type == UUID_CHARACTERISTIC {
        let decls = [H_GAP_CHAR_DECL, H_CHAR1_DECL, H_CHAR2_DECL, H_CHAR3_DECL];
        let mut entries: Vec<(u16, Vec<u8>)> = Vec::new();
        for &h in &decls {
            if h >= start && h <= end {
                if let Some((_, val)) = attr_lookup(h, state) {
                    entries.push((h, val));
                }
            }
        }
        if entries.is_empty() {
            att_error(fd, conn_handle, ATT_OP_READ_BY_TYPE_REQ, start, ATT_ECODE_ATTRIBUTE_NOT_FOUND);
            return;
        }
        let entry_len = 2 + entries[0].1.len();
        let mut payload = vec![ATT_OP_READ_BY_TYPE_RSP, entry_len as u8];
        for (h, val) in &entries {
            if 2 + val.len() != entry_len {
                break; // mixed-length entries must go in separate responses; we never mix, so this never triggers
            }
            payload.extend_from_slice(&h.to_le_bytes());
            payload.extend_from_slice(val);
        }
        send_l2cap(fd, conn_handle, ATT_CID, &payload);
        return;
    }

    // Any other "read by type" (e.g. probing for specific descriptor
    // UUIDs by type) -- we have none, report not found.
    att_error(fd, conn_handle, ATT_OP_READ_BY_TYPE_REQ, start, ATT_ECODE_ATTRIBUTE_NOT_FOUND);
}

fn handle_find_info(fd: i32, conn_handle: u16, req: &[u8]) {
    // We have no descriptors (no CCCD -- no notifications in this
    // minimal server), so every Find Information query is "not found".
    let start = if req.len() >= 3 { u16::from_le_bytes([req[1], req[2]]) } else { 0 };
    att_error(fd, conn_handle, ATT_OP_FIND_INFO_REQ, start, ATT_ECODE_ATTRIBUTE_NOT_FOUND);
}

/// Handles both Read Request (0x0A, offset always 0) and Read Blob
/// Request (0x0C, explicit offset) with shared logic -- both must never
/// send more than `mtu - 1` bytes of attribute value per response. An
/// earlier version of this ignored that limit entirely and sent
/// arbitrarily large values in one Read Response; the FFE1 scan-results
/// value (hundreds of bytes for a real scan) violated ATT MTU and is
/// the confirmed cause of Windows disconnecting immediately after
/// reading it. Per spec, a value truncated to exactly `mtu - 1` bytes
/// signals "there's more" to a well-behaved client, which then issues
/// Read Blob Request(s) with increasing offsets to fetch the rest.
fn handle_read_generic(fd: i32, conn_handle: u16, opcode: u8, handle: u16, offset: usize, state: &Arc<Mutex<GattState>>, mtu: usize) {
    let guard = state.lock().unwrap_or_else(|e| e.into_inner());
    let state: &GattState = &guard;
    match attr_lookup(handle, state) {
        Some((_uuid, val)) => {
            if offset > val.len() {
                att_error(fd, conn_handle, opcode, handle, ATT_ECODE_INVALID_OFFSET);
                return;
            }
            let max_chunk = mtu.saturating_sub(1);
            let end = (offset + max_chunk).min(val.len());
            let rsp_opcode = if opcode == ATT_OP_READ_BLOB_REQ { ATT_OP_READ_BLOB_RSP } else { ATT_OP_READ_RSP };
            let mut payload = vec![rsp_opcode];
            payload.extend_from_slice(&val[offset..end]);
            send_l2cap(fd, conn_handle, ATT_CID, &payload);
        }
        _ => att_error(fd, conn_handle, opcode, handle, ATT_ECODE_INVALID_HANDLE),
    }
}

fn handle_read(fd: i32, conn_handle: u16, req: &[u8], state: &Arc<Mutex<GattState>>, mtu: usize) {
    if req.len() < 3 {
        att_error(fd, conn_handle, ATT_OP_READ_REQ, 0, ATT_ECODE_INVALID_HANDLE);
        return;
    }
    let handle = u16::from_le_bytes([req[1], req[2]]);
    handle_read_generic(fd, conn_handle, ATT_OP_READ_REQ, handle, 0, state, mtu);
}

fn handle_read_blob(fd: i32, conn_handle: u16, req: &[u8], state: &Arc<Mutex<GattState>>, mtu: usize) {
    if req.len() < 5 {
        att_error(fd, conn_handle, ATT_OP_READ_BLOB_REQ, 0, ATT_ECODE_INVALID_HANDLE);
        return;
    }
    let handle = u16::from_le_bytes([req[1], req[2]]);
    let offset = u16::from_le_bytes([req[3], req[4]]) as usize;
    handle_read_generic(fd, conn_handle, ATT_OP_READ_BLOB_REQ, handle, offset, state, mtu);
}

fn handle_write(fd: i32, conn_handle: u16, req: &[u8], state: &Arc<Mutex<GattState>>, with_response: bool) {
    if req.len() < 3 {
        if with_response {
            att_error(fd, conn_handle, ATT_OP_WRITE_REQ, 0, ATT_ECODE_INVALID_HANDLE);
        }
        return;
    }
    let handle = u16::from_le_bytes([req[1], req[2]]);
    let value = &req[3..];
    if handle != H_CHAR2_VAL {
        if with_response {
            att_error(fd, conn_handle, ATT_OP_WRITE_REQ, handle, ATT_ECODE_WRITE_NOT_PERMITTED);
        }
        return;
    }
    if with_response {
        send_l2cap(fd, conn_handle, ATT_CID, &[ATT_OP_WRITE_RSP]);
    }
    on_ffe2_write(value, state, fd);
}

/// Accumulates FFE2 write bytes (across possibly many MTU-sized chunks)
/// and dispatches once the `$$$$` terminator is seen. Dispatch itself
/// runs on a background thread (see `dispatch_command`) so a slow
/// command (wifi scan takes several seconds; a real connect attempt
/// longer still) never blocks this event loop from continuing to
/// service the HCI socket -- an earlier version blocked here directly
/// and the resulting multi-second gap in ATT/HCI responsiveness
/// coincided with Windows' GATT stack dropping the connection.
fn on_ffe2_write(chunk: &[u8], state: &Arc<Mutex<GattState>>, _fd: i32) {
    let cmd = {
        let mut s = state.lock().unwrap_or_else(|e| e.into_inner());
        s.cmd_buf.extend_from_slice(chunk);
        if s.cmd_buf.len() > 512 {
            eprintln!("step: FFE2 command buffer overflowed 512 bytes, resetting");
            s.cmd_buf.clear();
            return;
        }
        const TERMINATOR: &[u8] = b"$$$$";
        match find_subslice(&s.cmd_buf, TERMINATOR) {
            Some(pos) => {
                let cmd = s.cmd_buf[..pos].to_vec();
                s.cmd_buf.drain(..pos + TERMINATOR.len());
                Some(cmd)
            }
            None => None,
        }
    };
    if let Some(cmd) = cmd {
        let state = Arc::clone(state);
        std::thread::spawn(move || dispatch_command(&cmd, &state));
    }
}

fn find_subslice(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|w| w == needle)
}

// ---------------------------------------------------------------------------
// Commands: "scan$$$$" and "setssid,<hex_ssid>,<hex_pass>,<hex_id>,<auth>$$$$"
// ---------------------------------------------------------------------------

fn hex_decode(s: &str) -> Option<String> {
    if s.len() % 2 != 0 {
        return None;
    }
    let bytes: Option<Vec<u8>> = (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect();
    bytes.and_then(|b| String::from_utf8(b).ok())
}

/// Runs on a background thread (spawned by `on_ffe2_write`) -- never
/// called from the event loop directly, so it's free to block on
/// subprocesses/sleeps for as long as it needs without stalling ATT/HCI
/// processing.
fn dispatch_command(cmd: &[u8], state: &Arc<Mutex<GattState>>) {
    let text = String::from_utf8_lossy(cmd);
    eprintln!("step: command received: {text:?}");
    let mut parts = text.split(',');
    match parts.next() {
        Some("scan") => run_wifi_scan(state),
        Some("setssid") => {
            let hex_ssid = parts.next().unwrap_or("");
            let hex_pass = parts.next().unwrap_or("");
            let hex_id = parts.next().unwrap_or("");
            let auth: i32 = parts.next().unwrap_or("0").trim().parse().unwrap_or(0);
            let ssid = hex_decode(hex_ssid).unwrap_or_default();
            let pass = hex_decode(hex_pass).unwrap_or_default();
            let identity = hex_decode(hex_id).unwrap_or_default();
            eprintln!("step: setssid ssid={ssid:?} auth={auth} identity={identity:?} (password redacted)");
            apply_wifi_credentials(&ssid, &pass, auth, state);
        }
        _ => eprintln!("step: unknown command: {text:?}"),
    }
}

fn run_wifi_scan(state: &Arc<Mutex<GattState>>) {
    // wpa_supplicant is already managing wlan0 (STA mode); ask it to
    // trigger a scan and read cached results back, rather than fighting
    // it for the interface with a separate iwlist/iw call.
    let _ = std::process::Command::new("wpa_cli").args(["-i", "wlan0", "scan"]).status();
    std::thread::sleep(std::time::Duration::from_secs(3));
    let out = std::process::Command::new("wpa_cli").args(["-i", "wlan0", "scan_results"]).output();
    let mut entries = Vec::new();
    if let Ok(out) = out {
        let text = String::from_utf8_lossy(&out.stdout);
        for line in text.lines().skip(1) {
            // wpa_cli scan_results columns: bssid / frequency / signal level / flags / ssid
            let cols: Vec<&str> = line.split('\t').collect();
            if cols.len() < 5 {
                continue;
            }
            let ssid = cols[4];
            if ssid.is_empty() {
                continue;
            }
            let rssi: i32 = cols[2].parse().unwrap_or(-100);
            let flags = cols[3];
            let auth = if flags.contains("WPA2") {
                7
            } else if flags.contains("WPA") {
                5
            } else {
                1
            };
            entries.push(format!("{},{},{}", hex::encode(ssid), auth, rssi));
        }
    }
    let count = entries.len();
    state.lock().unwrap_or_else(|e| e.into_inner()).ffe1_value = entries.join(",").into_bytes();
    eprintln!("step: scan complete, {count} networks");
}

fn apply_wifi_credentials(ssid: &str, password: &str, auth: i32, state: &Arc<Mutex<GattState>>) {
    write_wifi_status("credentials_received", ssid);
    let conf = if auth == 1 {
        format!(
            "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n\nnetwork={{\n\tssid=\"{ssid}\"\n\tkey_mgmt=NONE\n}}\n"
        )
    } else {
        format!(
            "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n\nnetwork={{\n\tssid=\"{ssid}\"\n\tpsk=\"{password}\"\n\tkey_mgmt=WPA-PSK\n}}\n"
        )
    };
    match std::fs::write("/data/userconfig/wpa_supplicant.conf", &conf) {
        Ok(()) => {
            eprintln!("step: wrote /data/userconfig/wpa_supplicant.conf, restarting wpa_supplicant");
            let _ = std::process::Command::new("wpa_cli").args(["-i", "wlan0", "reconfigure"]).status();
            set_status(state, ssid, auth, 1, None);
            write_wifi_status("connecting", ssid);
            poll_connection_result(state, ssid, auth);
        }
        Err(e) => eprintln!("step: failed to write wpa_supplicant.conf: {e}"),
    }
}

fn set_status(state: &Arc<Mutex<GattState>>, ssid: &str, auth: i32, conn_state: i32, ip: Option<&str>) {
    let mut s = state.lock().unwrap_or_else(|e| e.into_inner());
    let device_name = s.device_name.clone();
    let ip_field = ip.map(|ip| format!(",\"ip\":\"{ip}\"")).unwrap_or_default();
    s.ffe3_value = format!("{{\"device\":\"{device_name}\",\"ssid\":\"{ssid}\",\"auth\":{auth},\"conn_state\":{conn_state}{ip_field}}}").into_bytes();
}

/// Polls the real wpa_supplicant connection state (and wlan0's assigned
/// IP) after applying new credentials, updating FFE3's `conn_state` to
/// 2 (Connected, with `ip`) or 3 (Failed) once known -- an earlier
/// version left `conn_state` permanently stuck at 1 (Connecting), which
/// worked functionally (the real WiFi connection succeeded) but meant
/// polling clients like nano3ble's `set-wifi` would time out waiting
/// for a status that would never arrive.
fn poll_connection_result(state: &Arc<Mutex<GattState>>, ssid: &str, auth: i32) {
    // Real-world test against the Avalon Life app showed it polls FFE3
    // only a handful of times before giving up and reporting "failed"
    // to the user -- even though the underlying WiFi connection had, in
    // fact, already succeeded by then. The app's patience window is
    // short enough that this loop's own poll latency matters: checking
    // every 300ms (was 2s) instead of every 2s buys back real margin
    // against however short that window actually is, without changing
    // anything about how fast wpa_supplicant itself can reassociate.
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
    while std::time::Instant::now() < deadline {
        std::thread::sleep(std::time::Duration::from_millis(300));
        let wpa_state = std::process::Command::new("wpa_cli")
            .args(["-i", "wlan0", "status"])
            .output()
            .ok()
            .map(|o| String::from_utf8_lossy(&o.stdout).to_string())
            .unwrap_or_default();
        let completed = wpa_state.lines().any(|l| l == "wpa_state=COMPLETED");
        if completed {
            let ip = ifconfig_wlan0_ip();
            if let Some(ip) = ip {
                eprintln!("step: wifi connected, ip={ip}");
                set_status(state, ssid, auth, 2, Some(&ip));
                write_wifi_status("connected", &ip);
                set_display_page("nano3s");
                return;
            }
        }
    }
    eprintln!("step: wifi connection did not complete within 30s, reporting failed");
    set_status(state, ssid, auth, 3, None);
    write_wifi_status("failed", "");
}

/// Parses `ifconfig wlan0`'s `inet addr:X.X.X.X` line. No `ip` binary on
/// this device's busybox, only the legacy `ifconfig`/`net-tools` output
/// format.
fn ifconfig_wlan0_ip() -> Option<String> {
    let out = std::process::Command::new("ifconfig").arg("wlan0").output().ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("inet addr:") {
            return rest.split_whitespace().next().map(|s| s.to_string());
        }
    }
    None
}

// ---------------------------------------------------------------------------
// Connection + main event loop
// ---------------------------------------------------------------------------

/// Reads raw HCI packets forever: LE Connection Complete brings a
/// connection up; ACL Data on that handle carries L2CAP/ATT traffic;
/// LE Disconnection Complete tears it down and (since this is a single
/// connectable advertiser) we simply keep advertising for the next one
/// -- the controller keeps advertising automatically after a connection
/// per our earlier LE Set Advertising Enable, no need to re-enable.
fn event_loop(fd: i32, device_name: &str) {
    let state = Arc::new(Mutex::new(GattState {
        ffe1_value: Vec::new(),
        ffe3_value: format!("{{\"device\":\"{device_name}\",\"conn_state\":0}}").into_bytes(),
        cmd_buf: Vec::new(),
        device_name: device_name.to_string(),
    }));
    let mut conn_handle: Option<u16> = None;
    let mut mtu: usize = 23;
    let mut buf = [0u8; 1024];

    loop {
        let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut _, buf.len()) };
        if n <= 0 {
            continue;
        }
        let n = n as usize;
        match buf[0] {
            0x04 if n >= 3 => {
                // HCI Event packet.
                let event_code = buf[1];
                let plen = buf[2] as usize;
                if n < 3 + plen {
                    continue;
                }
                let params = &buf[3..3 + plen];
                if event_code == 0x3E && !params.is_empty() {
                    match params[0] {
                        0x01 if params.len() >= 4 => {
                            // LE Connection Complete: status(1) handle(2) role(1)...
                            let status = params[1];
                            let handle = u16::from_le_bytes([params[2], params[3]]);
                            if status == 0 {
                                eprintln!("step: LE connection established, handle=0x{handle:04x}");
                                conn_handle = Some(handle);
                                mtu = 23;
                                state.lock().unwrap_or_else(|e| e.into_inner()).cmd_buf.clear();
                                write_wifi_status("client_connected", "");
                            } else {
                                eprintln!("step: LE connection failed, status=0x{status:02x}");
                            }
                        }
                        sub => eprintln!("step: LE meta subevent 0x{sub:02x} (unhandled) params={params:02x?}"),
                    }
                } else if event_code == 0x05 && params.len() >= 4 {
                    // Disconnection Complete: status(1) handle(2) reason(1)
                    let handle = u16::from_le_bytes([params[1], params[2]]);
                    if Some(handle) == conn_handle {
                        eprintln!("step: LE disconnected (reason=0x{:02x}), resuming advertising for next client", params[3]);
                        conn_handle = None;
                        state.lock().unwrap_or_else(|e| e.into_inner()).cmd_buf.clear();
                        // Advertising stops automatically once a
                        // connection completes and does NOT resume on
                        // its own after disconnect -- without this the
                        // device becomes invisible after exactly one
                        // client connects and leaves.
                        if let Err(e) = le_set_advertise_enable(fd, true) {
                            eprintln!("step: failed to resume advertising after disconnect: {e}");
                        }
                    }
                } else if event_code != 0x0E && event_code != 0x0F && event_code != 0x13 {
                    // Suppress Command Complete/Status/Number of
                    // Completed Packets -- routine noise once
                    // connected. Log anything else (encryption change,
                    // LL-level errors, etc).
                    eprintln!("step: HCI event 0x{event_code:02x} params={params:02x?}");
                }
            }
            0x02 if n >= 5 => {
                // ACL Data: handle_flags(2) total_len(2) l2cap[len(2) cid(2) payload...]
                let handle_flags = u16::from_le_bytes([buf[1], buf[2]]);
                let handle = handle_flags & 0x0FFF;
                if Some(handle) != conn_handle {
                    continue;
                }
                let acl_len = u16::from_le_bytes([buf[3], buf[4]]) as usize;
                if n < 5 + acl_len || acl_len < 4 {
                    continue;
                }
                let l2cap = &buf[5..5 + acl_len];
                let cid = u16::from_le_bytes([l2cap[2], l2cap[3]]);
                let payload = &l2cap[4..];
                if cid != ATT_CID || payload.is_empty() {
                    continue;
                }
                let ch = handle; // connection handle for responses
                eprintln!(
                    "step: ATT recv opcode=0x{:02x} len={} bytes={:02x?}",
                    payload[0],
                    payload.len(),
                    &payload[..payload.len().min(23)]
                );
                match payload[0] {
                    ATT_OP_EXCHANGE_MTU_REQ => handle_exchange_mtu(fd, ch, payload, &mut mtu),
                    ATT_OP_READ_BY_GROUP_TYPE_REQ => handle_read_by_group_type(fd, ch, payload),
                    ATT_OP_READ_BY_TYPE_REQ => handle_read_by_type(fd, ch, payload, &state),
                    ATT_OP_FIND_INFO_REQ => handle_find_info(fd, ch, payload),
                    ATT_OP_READ_REQ => handle_read(fd, ch, payload, &state, mtu),
                    ATT_OP_READ_BLOB_REQ => handle_read_blob(fd, ch, payload, &state, mtu),
                    ATT_OP_WRITE_REQ => handle_write(fd, ch, payload, &state, true),
                    ATT_OP_WRITE_CMD => handle_write(fd, ch, payload, &state, false),
                    other => {
                        // Best-effort spec-compliant error: most ATT
                        // request PDUs carry the range/attribute start
                        // handle as the first 2 bytes after the opcode
                        // (Find By Type Value, Read Multiple, etc).
                        // Responding with handle=0 (as an earlier
                        // version of this code did) is non-compliant
                        // and was likely why Windows' GATT stack
                        // dropped the connection outright rather than
                        // just ignoring the unsupported request.
                        let handle_in_err = if payload.len() >= 3 { u16::from_le_bytes([payload[1], payload[2]]) } else { 0 };
                        eprintln!("step: unhandled ATT opcode 0x{other:02x}, responding Attribute Not Found for handle 0x{handle_in_err:04x}");
                        att_error(fd, ch, other, handle_in_err, ATT_ECODE_ATTRIBUTE_NOT_FOUND);
                    }
                }
            }
            _ => {}
        }
    }
}
