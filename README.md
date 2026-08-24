# mujina-miner

Open-source Bitcoin mining firmware.

## About This Fork

This is a fork of [256foundation/mujina](https://github.com/256foundation/mujina),
adapted to run on the **Avalon Nano3s** (Canaan K230 SoC, dual-core
RISC-V64: a big core running a bare-metal RTOS alongside the ASIC
chain, and a little core running Linux).

The stock Nano3s ships closed firmware on both cores. This fork
replaces it end to end:

- **[`rtos_core/`](rtos_core/)** -- a from-scratch big-core firmware
  (`rtos_core.elf`) that replaces the stock RTOS: ASIC chain
  enumeration and job/nonce framing over UART, PLL/voltage control, and
  IPC to the Linux side. Also includes `mujina_test_harness.c`, a
  Linux-side helper for power/fan control (real duty-based PWM control
  on the board's fan channel, driven by a proportional+integral
  controller against the chip's real thermal limits).
- **[`mujina-miner/src/board/nano3s.rs`](mujina-miner/src/board/nano3s.rs)**
  -- the board driver wiring this hardware into Mujina's normal
  scheduler/job-source/API pipeline, same as any other supported board.
- A [web dashboard](mujina-miner/assets/dashboard.html) (served by
  `mujina-minerd` itself) with live telemetry, tuning, and fan control
  specific to this board.

Everything here was built by independently reverse-engineering the
device's real behavior -- device-tree GPIO/PWM mapping, the ASIC UART
wire protocol, PLL/voltage calibration tables, real fan hardware
characteristics.

## Installing on Nano3s Hardware

This installs custom firmware on both cores of a real device. Two
ways to get there: flash a pre-built image (fastest, no build
environment needed), or build from source and deploy over SSH
(below, for development).

### Quick Install: Flash a Pre-built Image

Download **[nano-mujina-alpha-v1.kdimg](https://github.com/aadhi1014/nano-mujina/releases/download/alpha-v1/nano-mujina-alpha-v1.kdimg)**
(123MB, see the [release notes](https://github.com/aadhi1014/nano-mujina/releases/tag/alpha-v1)
for what's in it) and burn it with the same official K230 Burning Tool
Canaan's own [flashing instructions](https://github.com/Canaan-Creative/Avalon_Nano3s#3-imges-burning)
use. Unlike the stock image, no WiFi credentials or other
device-specific data are baked in -- the device boots straight into
first-time BLE setup.

1) **Download the burning tool**: [K230BurningTool-Windows-v2.1.0](https://kendryte-download.canaan-creative.com/k230/downloads/burn_tool/v2.1.0/K230BurningTool-Windows-v2.1.0-0-gd24909e.zip)
   and unzip it.

2) **Open and select the image**: run `K230BurningTool.exe`, click
   Open, select `nano-mujina-alpha-v1.kdimg`, set Image part name to
   "all", and set Medium to "SPI NAND".
   ![](https://raw.githubusercontent.com/Canaan-Creative/Avalon_Nano3s/master/docs/burn_tool_open.png)
   ![](https://raw.githubusercontent.com/Canaan-Creative/Avalon_Nano3s/master/docs/burn_tool_select.png)

3) **Put the device in burn mode**: use a double type-A USB cable to
   connect the Nano3s to your PC, hold the recessed button, then power
   the device on while still holding it.
   ![](https://raw.githubusercontent.com/Canaan-Creative/Avalon_Nano3s/master/docs/nano3s_pin_press.png)
   The tool's debug box on the right confirms burn mode once it's
   detected.
   ![](https://raw.githubusercontent.com/Canaan-Creative/Avalon_Nano3s/master/docs/burn_tool_debug_box_burn_mode.png)

4) **Click Start** and wait -- a few minutes, until "Downloading
   Completed" appears.
   ![](https://raw.githubusercontent.com/Canaan-Creative/Avalon_Nano3s/master/docs/burn_tool_start.png)
   ![](https://raw.githubusercontent.com/Canaan-Creative/Avalon_Nano3s/master/docs/burn_tool_upgrade_completed.png)

5) **First boot**: the device comes up advertising over Bluetooth
   (look for a name starting `nan3s_` in the official
   [Avalon Family app](https://play.google.com/store/apps/details?id=com.canaan.avalon)
   -- also on the [App Store](https://apps.apple.com/us/app/avalon-family/id6479229114)),
   with the LCD showing a setup progress ring. Connect, enter your
   WiFi network's SSID and password in the app, and the device joins
   your network and starts mining -- the LCD tracks each step (waiting
   for phone, credentials received, connecting, connected). No serial
   console or SSH access needed.

Burning-tool screenshots and the recovery-pin photo above are from
Canaan's own [Avalon_Nano3s](https://github.com/Canaan-Creative/Avalon_Nano3s)
repository, since it's literally the same official tool.

### Build From Source

The rest of this section covers building both firmwares yourself and
deploying over SSH -- useful for development, or if you want to modify
the firmware before flashing.

### 1. Get the cross-toolchains

Two separate toolchains are needed, one per core:

- **Big core (`rtos_core.elf`)**: a real musl RISC-V64 cross-compiler
  (not glibc -- a glibc binary silently fails to start under RT-Smart's
  `msh`). Get one from e.g. [musl.cc](https://musl.cc)'s
  `riscv64-linux-musl` cross toolchain. Point `TOOLCHAIN_BIN` at its
  `bin/` directory, or extract it to
  `../toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/` next to
  this repo (`rtos_core/build_wsl.sh`'s default).
- **Little core (`mujina-minerd`)**: the `riscv64gc-unknown-linux-gnu`
  Rust target (`rustup target add riscv64gc-unknown-linux-gnu`) plus a
  `riscv64-linux-gnu-gcc` cross-compiler and a matching riscv64 sysroot
  (for `libudev`/`libssl`) -- see `rtos_core/tools/build_mujina_minerd.sh`
  for the exact `PKG_CONFIG_SYSROOT_DIR`/`PKG_CONFIG_PATH` it expects;
  adjust the paths there to wherever your sysroot actually lives.

The one vendor dependency `rtos_core.elf` links against
(`libipcmsg_slave.a`, Canaan's IPC library) is fetched automatically by
`build_wsl.sh` the first time you build -- see
`rtos_core/tools/install_sdk.sh`.

### 2. Build and deploy

From WSL, with both toolchains on `PATH` (or their paths set via the
env vars above), one command builds both firmwares and pushes the
entire stack to the device over SSH:

```bash
DEVICE_IP=192.168.1.x bash rtos_core/tools/build_and_deploy_all.sh
```

This chains three independently-runnable scripts, if you'd rather run
them one at a time or re-run just one after a small change:

```bash
bash rtos_core/build_wsl.sh                              # builds rtos_core.elf
bash rtos_core/tools/build_mujina_minerd.sh               # builds mujina-minerd
DEVICE_IP=192.168.1.x bash rtos_core/tools/deploy_stock_to_nano3s.sh  # pushes everything, reboots
```

`deploy_stock_to_nano3s.sh` only runs against a device still on stock
firmware (it checks the target's `/etc/init.d/rcS` first and refuses if
it looks already-migrated), and it backs up the device's original
`rcS` and `mm_miner` binary, timestamped, before changing anything --
see the script for exact paths if you ever need to revert.

### 3. Verify

Once the device reboots, the dashboard is served on port 80:

```
http://192.168.1.x/
```

## Why Mujina

You bought the hardware, but someone else controls the software. Whether
you have thousands of machines in a data center or one in your basement,
the firmware running them is closed. It comes from the manufacturer or a
third-party vendor, and you can't read it, audit it, or change it.

Mujina is here to change that: one open-source codebase to run any
hashboard from any vendor on any control board, written by hardware
engineers, protocol authors, and mining operators from across the industry.
Read every line, modify it without permission, control it through a
documented API, and pay no dev fee. Own your firmware.

## Current Status

Mujina is under active development. Today's supported hardware is a
starting point:

**Working now**

- **[Bitaxe Gamma](mujina-miner/src/board/bitaxe_gamma.md)** (single
  BM1370 ASIC): an open-source single-chip miner. Good for developers
  and advanced users who want to run Mujina on real hardware today.
- **CPU backend**: software SHA-256 hashing, no hardware required.
  Useful for exercising Mujina itself, testing pool and other server
  software against a working miner client, and teaching the full mining
  pipeline. See [CPU Mining](docs/cpu-mining.md).

**Landing now**

- **[EmberOne00](https://github.com/256foundation/emberone00-pcb)**
  (twelve BM1362 ASICs): a sister project from the 256 Foundation. An
  open-source hashboard designed to be driven by open firmware.

**Near-term targets**

- Installable images for the Antminer S19 series
- The 256 Foundation's forthcoming
  [Libreboard](https://github.com/256foundation/libreboard) control
  board
- Broader support for commercial mining machines

APIs are still moving and parts of the docs lag the code.

## Quick Start

Build Mujina and watch it run end to end, no mining hardware required.
On Debian or Ubuntu:

```bash
git clone https://github.com/256foundation/mujina.git
cd mujina
sudo apt-get install libudev-dev libssl-dev
MUJINA_CPUMINER_THREADS=1 MUJINA_CPUMINER_DUTY=50 MUJINA_USB_DISABLE=1 \
  cargo run --bin mujina-minerd
```

In this example, the CPU backend hashes in software against a dummy job
source, exercising the full pipeline: job distribution, hashing, share
detection, logging, and the API. When you're ready to mine for real,
continue below.

## Build Requirements

Mujina builds with the current stable
[Rust toolchain](https://rustup.rs). Install the additional packages
below for your platform.

### Linux

On Debian or Ubuntu:

```bash
sudo apt-get install libudev-dev libssl-dev
```

Other distributions need their equivalents of the udev and openssl
development packages.

### macOS

macOS is supported. Install Xcode Command Line Tools alongside the Rust
toolchain. A build failure on `openssl-sys` usually means the build
can't find openssl; see the
[openssl crate's macOS notes](https://docs.rs/openssl/latest/openssl/#automatic)
for the supported installation and environment options.

## Building

mujina-miner is a cargo workspace. Build and test it the usual way:

```bash
cargo build
cargo test
```

The workspace contains several binaries: `mujina-minerd` (the daemon),
`mujina-cli`, and others. Running requires picking one:

```bash
cargo run --bin mujina-minerd
```

If you'll be working in the repo regularly, install
[just](https://github.com/casey/just) (`cargo install just`) for
shorter aliases that avoid retyping the `--bin` flag:

```bash
just run        # same as cargo run --bin mujina-minerd
just test       # same as cargo test
just checks     # fmt, lint, and test in one step
```

Examples in the rest of this README use plain cargo so they work
without `just` installed.

## Running

Mujina is currently configured through environment variables.
Persistent configuration via the REST API and CLI will follow as those
interfaces mature.

### Connecting to a job source

Point Mujina at a Stratum v1 mining pool:

```bash
MUJINA_POOL_URL="stratum+tcp://pool.example.com:3333" \
MUJINA_POOL_USER="your-address.worker" \
cargo run --bin mujina-minerd
```

`MUJINA_POOL_USER` defaults to `mujina-testing` and `MUJINA_POOL_PASS`
defaults to `x`, so only `MUJINA_POOL_URL` is strictly required.

### Testing without a pool

Omit `MUJINA_POOL_URL` to use a dummy job source that generates
synthetic mining work. Useful for development without a network
connection.

```bash
cargo run --bin mujina-minerd
```

### Controlling log output

The default filter emits Mujina log entries at info level and
third-party crates at warn. Two environment variables adjust it.
`MUJINA_LOG` filters Mujina's own modules, named exactly as the log
output shows them, and a bare level applies to Mujina as a whole.
`RUST_LOG` keeps its usual Rust meaning: a directive that names a
crate adds to the defaults, and a bare level takes full control of
the filter. `MUJINA_LOG` wins where the two overlap.

```bash
# Default: info for Mujina, warn for third-party crates
cargo run --bin mujina-minerd

# Trace all of Mujina, third-party crates stay at warn
MUJINA_LOG=trace cargo run --bin mujina-minerd

# Trace the Stratum v1 client, everything else unchanged
MUJINA_LOG=stratum_v1=trace cargo run --bin mujina-minerd

# Debug Stratum v1 and trace BM13xx at the same time
MUJINA_LOG=stratum_v1=debug,asic::bm13xx=trace \
  cargo run --bin mujina-minerd

# Trace a third-party crate, Mujina's defaults unchanged
RUST_LOG=nusb=trace cargo run --bin mujina-minerd
```

Debug shows logical stages and summaries: chip initialization, jobs
received from the pool, shares submitted. Trace adds step-by-step
execution detail: individual serial frames, I2C transactions, and USB
device events.

### Using the REST API

Mujina logs the API bind address at startup. By default it's
`127.0.0.1:7785`; set `MUJINA_API_LISTEN` to change it:

```bash
# All interfaces, default port
MUJINA_API_LISTEN="0.0.0.0" cargo run --bin mujina-minerd

# All interfaces, custom port
MUJINA_API_LISTEN="0.0.0.0:9000" cargo run --bin mujina-minerd
```

See [REST API](docs/api.md) for endpoints and conventions. The
`/api/v0/` prefix signals the API is still in flux. Authentication
is on the roadmap.

## Contributing

We welcome contributions! Whether you're fixing bugs, adding features,
improving documentation, or simply exploring the codebase to learn
about Bitcoin mining protocols and hardware, your involvement is
valued.

Please see our [Contribution Guide](CONTRIBUTING.md) for details on
how to get started. For user-oriented discussion and support, visit
the [Mujina forum](https://forum.256foundation.org/c/mujina/7). For
real-time chat, join our [Telegram group](https://t.me/the256foundation)---note
that Telegram is ephemeral; decisions and important context belong on
GitHub.

## Further Reading

### Design and operation

- [Architecture Overview](docs/architecture.md): system design and
  component interaction
- [REST API](docs/api.md): endpoints, conventions, and OpenAPI spec
- [CPU Mining](docs/cpu-mining.md): the CPU backend in detail
- [Container Image](docs/container.md): build and run Mujina as a
  container

### Protocols

- [BM13xx Chip Reference](mujina-miner/src/asic/bm13xx/REFERENCE.md):
  serial protocol, registers, and behavior of the BM13xx mining-chip
  family
- [Bitaxe-Raw Control Protocol](mujina-miner/src/mgmt_protocol/bitaxe_raw/PROTOCOL.md):
  management protocol for Bitaxe board peripherals

### Hardware

- [Bitaxe Gamma Board Guide](mujina-miner/src/board/bitaxe_gamma.md):
  board hardware, firmware flashing, and Mujina integration
- [Nano3s Hardware Register Reference](docs/nano3s-registers.md):
  GPIO/IOMUX/PWM/I2C register map, independently reverse-engineered

### Contributor reference

- [Contribution Guide](CONTRIBUTING.md): process and requirements
- [Code Style Guide](CODE_STYLE.md): formatting and mechanical style
- [Coding Guidelines](CODING_GUIDELINES.md): design patterns and best
  practices

## Related Projects

- [Bitaxe](https://github.com/bitaxeorg): open-source Bitcoin mining
  hardware
- [bitaxe-raw](https://github.com/bitaxeorg/bitaxe-raw): pass-through firmware for
  Bitaxe boards required for use by Mujina
- [EmberOne00](https://github.com/256foundation/emberone00-pcb): 256
  Foundation's first open-source Bitcoin mining hashboard
- [Libreboard](https://github.com/256foundation/libreboard): 256
  Foundation's open-source mining control board

## License

This project is licensed under the GNU General Public License v3.0 or
later. See the [LICENSE](LICENSE) file for details.
