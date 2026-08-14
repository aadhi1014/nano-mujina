#!/bin/sh
# Hardware bring-up for the Nano3s Linux side, replacing what mm_miner
# normally does as a side effect of its own init before it's deleted from
# the boot sequence. Each step is independent -- one failing doesn't stop
# the others, matching how these ran as separate scripts from rcS before.

# --- ASIC core voltage ---
# Sets the factory LOW-mode value (3392mV, hashrate_cali.ini's mode0) via
# the same register mm_miner itself uses (i2c bus 3, addr 0x48, reg 0xF8),
# replicating mujina_test_harness's mv_to_code() formula (base=3600mV,
# step=26mV, code=0x88 -> 3392mV). Needed because nothing else applies
# core voltage before rtos_core.elf tries to enumerate the chain at boot.
i2cset -y 3 0x48 0xF8 0x88

# --- GPIO34 (chip power enable) ---
# Found via RE of mm_miner_riscv64's power_set_onoff(state):
#   if (state == 1) gpio_set_value(34, 1);   // ON path
#   else             gpio_set_value(34, 0);   // OFF path
# Must run before rtos_core.elf's uart_init() reconfigures this pin's IOMUX
# to the UART3_TX function.
[ -d /sys/class/gpio/gpio34 ] || echo 34 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio34/direction
echo 1 > /sys/class/gpio/gpio34/value

# --- WiFi ---
# Replicates the exact bring-up commands mm_miner itself shells out to, so
# wlan0 still comes up when mm_miner is absent.
wpa_supplicant -B -D nl80211 -i wlan0 -c /data/userconfig/wpa_supplicant.conf
wpa_cli -a /etc/wpa_action.sh -B -i wlan0 -P /var/run/wpa_cli.pid
udhcpc -i wlan0 -b -p /var/run/udhcpc.wlan0.pid &

# --- Fan ---
# Real channel on this unit: pwmchip0/pwm4 (not pwm2/pwm3 -- pwm2 is
# kernel-locked to the beeper, pwm3 is the LCD backlight). Duty cycle does
# not give variable speed control on this fan (RPM is roughly constant for
# a given frequency); frequency does: 25kHz settles at ~2020 RPM, 1kHz
# gives ~6300 RPM. Using 1kHz for near-max speed.
FAN_CHIP=/sys/class/pwm/pwmchip0
FAN_PERIOD=1000000
[ -d "$FAN_CHIP/pwm4" ] || echo 4 > "$FAN_CHIP/export"
echo "$FAN_PERIOD" > "$FAN_CHIP/pwm4/period"
echo $((FAN_PERIOD/2)) > "$FAN_CHIP/pwm4/duty_cycle"
echo 1 > "$FAN_CHIP/pwm4/enable"
