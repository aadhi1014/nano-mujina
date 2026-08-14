#!/bin/sh
# Autolaunch supervisor for the custom rtos_core/mujina stack (Linux
# side). Runs under rcS, which uses a real busybox ash shell -- while
# loops work here, unlike RT-Smart's msh (see main.c's --ipc6loop
# comment for why the RTOS side has to loop internally in C instead).
#
# rtos_core.elf itself is launched separately by /sharefs/init.sh on
# the RTOS/big-core side (--ipc6loop, loops forever internally). This
# script only supervises the Linux-side processes.
APP=/mntapp/release/linux/app

# Wait for wlan0 to have an address before trying to reach the pool --
# up to 60s, matching this project's other network-dependent bringup
# scripts.
i=0
while [ $i -lt 60 ]; do
    ip=$(ifconfig wlan0 2>/dev/null | grep 'inet addr:' | cut -d: -f2 | cut -d' ' -f1)
    if [ -n "$ip" ]; then
        break
    fi
    i=$((i + 1))
    sleep 1
done

# Give rtos_core.elf a moment to register its IPC service before the
# driver's first connect attempt.
sleep 5

# Each supervised process gets its own restart loop, backgrounded
# independently so one crashing doesn't affect the others. A short
# delay between restarts avoids spinning hot if something is
# persistently broken (no network, pool down, etc).
#
# No MUJINA_LOG override: the built-in default (warn,mujina_miner=info)
# is quiet enough for normal operation. Binary lives in /data (not
# /mntapp -- that partition is only 7.4MB total, far too small for this
# static binary).
(
    while true; do
        MUJINA_NANO3S_ENABLE=1 MUJINA_NANO3S_POWER_TARGET_W=62.0 MUJINA_API_LISTEN=0.0.0.0:80 MUJINA_POOL_URL=stratum+tcp://pool.256foundation.org:3333 MUJINA_POOL_USER=YOUR_WALLET.YOUR_WORKER /data/mujina-minerd > /data/mujina-minerd.log 2>&1
        sleep 3
    done
) &

(
    while true; do
        /data/nano3s_ui > /sharefs/nano3s_ui.log 2>&1
        sleep 3
    done
) &

(
    while true; do
        $APP/fb_button > /sharefs/fb_button.log 2>&1
        sleep 3
    done
) &

# Power/fan idle-control harness (2026-07-29) -- no IPC of its own, just
# power_en (GPIO34) + fan (pwm4) sysfs control driven by pause/resume
# requests via /tmp/harness_control. See mujina_test_harness.c.
(
    while true; do
        $APP/mujina_test_harness > /sharefs/harness.log 2>&1
        sleep 3
    done
) &

# Log size watchdog: mujina-minerd.log has no built-in rotation, so even
# at the quiet default level it grows without bound over a long-running
# session. Checked every 5 minutes; if it exceeds 5MB, kill the process
# (not truncate the file directly -- externally truncating a file a
# process is actively writing to via a fixed fd offset can desync the
# write position and produce a sparse file that jumps right back up,
# same class of bug found in rtos_core's ipc6.log fix). The supervisor
# loop above already restarts mujina-minerd on exit and reopens its log
# with plain shell '>' redirection, which truncates cleanly on process
# start -- so killing it here is what actually resets the file safely.
(
    while true; do
        sleep 300
        if [ -f /data/mujina-minerd.log ]; then
            SIZE=$(wc -c < /data/mujina-minerd.log)
            if [ "$SIZE" -gt 5242880 ]; then
                PID=$(pidof mujina-minerd)
                if [ -n "$PID" ]; then
                    kill "$PID"
                fi
            fi
        fi
    done
) &
