# Nano3s Hardware Register Reference

Real, independently reverse-engineered register-level findings for the
Avalon Nano3s (Canaan K230 SoC), gathered while building the `rtos_core`
and `nano3s.rs` board driver in this fork. Nothing here comes from
vendor source -- it's derived from live device inspection (device tree,
`/sys/class/gpio`, IOMUX register reads, `dmesg`), disassembly of the
device's own stock firmware for cross-checking, and direct functional
testing (measuring real PWM/GPIO behavior with the device running).

Where a finding is empirically confirmed (measured live, reproducibly)
it's marked as such. Where it's inferred from a register value without
a live functional test, that's noted too -- a couple of entries below
turned out to be wrong when actually tested, which is exactly why the
distinction matters.

**Board:** h1_f1_v1_nano3s_3197S
**SoC:** Kendryte K230 (dual-core RISC-V64: big core RTOS + little core
Linux)
**Kernel:** Linux 5.10.4, GPIO bank A @ `0x9140b000` (GPIO0-31), bank B
@ `0x9140c000` (GPIO32-71)
**IOMUX base:** `0x91105000`, one 4-byte register per pad, `bits[5:0]`
= SEL function

---

## PWM Channels

`pwmchip0` exposes multiple channels via the standard Linux PWM sysfs
interface (`/sys/class/pwm/pwmchip0/pwmN/{period,duty_cycle,enable,polarity}`).

| Channel | IOMUX pad | Real function | Confirmed how |
|---------|-----------|----------------|----------------|
| pwm2 | -- | Buzzer (`pwm-beeper`, DT `beep` node, 200Hz) | DT node, distinct from vendor's board-file assumption of PWM2=fan |
| **pwm3** | GPIO10 (IOMUX SEL=0x0E) | **Real fan speed control** -- genuinely proportional to duty cycle at a fixed 25kHz period: 25%→~1620rpm, 50%→~3390rpm, 75%→~5010rpm, 90%→~5970rpm. Confirmed by a controlled live sweep, non-inverted duty, with the previously-assumed fan channel (pwm4) held fixed throughout to rule out cross-talk. | **Empirically confirmed**, live tach measurement (see Fan Tachometer below) |
| pwm4 | -- | Was assumed to be the fan channel based on an earlier functional test. A rigorous re-test (full 0-100% duty sweep at fixed 25kHz, register-readback-verified writes) showed **zero RPM response at any duty**. Not the fan. | Empirically ruled out |
| pwm5 | -- | Unclaimed/unconnected. A duty sweep at fixed period produced no RPM change while the real fan channel (pwm3) was running. | Empirically ruled out as an alternative fan channel |

**Open discrepancy, noted honestly:** GPIO10's IOMUX SEL (0x0E) was
originally assumed to mean "LCD backlight PWM" purely from register
inspection, and a device bring-up script (`backlight_bringup.sh`) was
built around that assumption using values (`period=40000`,
`duty=10400`, i.e. 26%) that turned out to closely match this board's
real *fan* startup duty from disassembling the stock firmware
(`FAN_DUTY_INIT=25`). It's likely the two got conflated during initial
bring-up rather than genuinely being the same physical pin. Disassembly
of the byte-identical stock `mm_miner` binary shows its own real
`fan_init()` targeting `pwmchip0/pwm3` (chip=0, channel=3) directly --
consistent with the live pwm3 fan measurements above, but not yet
reconciled with what actually drives the LCD backlight on this board.
If you're working on display code here, verify backlight control
empirically rather than trusting the old GPIO10=backlight assumption.

### Fan Tachometer

Real RPM readback is via `/dev/timer5`, a raw pulse-counting timer
capture device -- **not** a sysfs RPM node (none exists on this
board). `timer4` was tested and reads nothing.

```
TMIOC_SET_TIMEOUT = _IOW('T', 0x20, int)   // arms a capture window, in seconds
```

Write the window length via `TMIOC_SET_TIMEOUT`, then `read()` after
sleeping that long returns the accumulated pulse count. Two pulses per
revolution is standard for a 4-pin PC fan:

```
rpm = pulses * 60 / 2   // for a 1-second window
```

---

## GPIO Pins with Explicit Device-Tree Nodes

| GPIO | Bank/Bit | Direction | Function |
|------|----------|-----------|----------|
| GPIO0 | A/0 | IN | Button A (`func_key0`), active-low |
| GPIO28 | A/28 | IN | Button B (`func_key1`), active-low |
| GPIO34 | B/2 | -- | UART3_TX to the ASIC chain (IOMUX SEL=0x2E overrides GPIO mode) |
| GPIO41 | B/9 | OUT | LCD DC (Data/Command for the ST7789V SPI display) |

GPIO31 and GPIO63 have no DT entry -- they're written directly by the
big-core RTOS firmware, not exposed to Linux userspace as GPIOs.

| GPIO | Function | Notes |
|------|----------|-------|
| GPIO31 | ASIC chain RST, active-HIGH | Held LOW by the RTOS while the chain is running |
| GPIO34 | ASIC chain power enable (this fork's usage) | Confirmed live: real power draw jumps when set HIGH, drops to idle when LOW |
| GPIO63 | UART3_RX from the ASIC chain | IOMUX SEL=0x2F, adjacent to GPIO34's TX SEL |

---

## IOMUX Pad Assignments (Peripheral Bus)

Base `0x91105000`, `bits[5:0]` = SEL.

| Pads | SEL | Peripheral |
|------|-----|------------|
| GPIO0, GPIO1 | 0x04 | UART1 TX/RX -- `/dev/ttyS1`, 115200 baud, Linux console |
| GPIO3, GPIO4 | 0x08 | I2C2 SCL/SDA -- `/dev/i2c-2`, 100kHz |
| GPIO5, GPIO6 | 0x08 | I2C3 SCL/SDA -- `/dev/i2c-3`, 100kHz |
| GPIO14-22 | 0x1F | SPI0 / NAND flash, 4-bit QSPI, 50MHz |
| GPIO34 | 0x2E | UART3_TX to the ASIC chain |
| GPIO36-40 | 0x0F (GPIO mode) | SPI1 / LCD, 25MHz |
| GPIO63 | 0x2F | UART3_RX from the ASIC chain |

## I2C Device Map

| Bus | Node | Addr | Chip | Function |
|-----|------|------|------|----------|
| i2c@91407000 | `/dev/i2c-2` | 0x40 | INA226 | Power monitor on the USB-C PD input rail |
| i2c@91407000 | `/dev/i2c-2` | 0x42 | HUSB238A | USB-C PD/PPS sink controller |
| i2c@91408000 | `/dev/i2c-3` | 0x48 | DC-DC | ASIC core voltage control (write-only) |
| i2c@91408000 | `/dev/i2c-3` | 0x50 | EEPROM (AT24C) | Board serial number |

## SPI Device Map

| Controller | Device | Function | Speed |
|------------|--------|----------|-------|
| SPI0 (`spi@91584000`) | Winbond SPI NAND | 256MiB flash -- rootfs/app/data partitions | 50MHz, 4-bit |
| SPI1 (`spi@91583000`) | ST7789V | 240x240 RGB565 LCD | 25MHz, 8-bit |

---

## ASIC Communication Path

```
Linux (big core)                    RTOS (little core, this fork's rtos_core.elf)
─────────────────                   ────────────────────────────────────────────
mujina-minerd                       rtos_core.elf
  │ IPC (shared mem)  ◄────────────►  │
  │                                   │ UART3 @ 0x91403000
  │                                   │   TX: GPIO34 (IOMUX SEL=0x2E)
  │                                   │   RX: GPIO63 (IOMUX SEL=0x2F)
  │                                   │   RST: GPIO31 (active-HIGH, held LOW while running)
  │                                   │
  │                                   └──► 12x ASIC chips
  │                                        8N2 UART, CRC8 framing
```

12x ASIC chips, running at real per-mode PLL/voltage points calibrated
per-device in `/data/factory/hashrate_cali.ini` on the target hardware
(format: `max_power_W-temp_limit_C-volt_mV-pll_start_MHz-pll_step_MHz`).
