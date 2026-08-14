#!/bin/sh
# Keeps the SoC's hardware watchdog(s) petted while mm_miner is down.
# mm_miner normally holds /dev/watchdog open and pets it as a side effect
# of its own main loop -- with mm_miner killed for testing (e.g. to get a
# clean Stage 5 IPCM topology), nothing else does, and the watchdog
# hardware-resets the whole device after its timeout (empirically ~150s;
# see docs/BUILD_NOTES.md's Stage 5 section for how this was diagnosed).
#
# EXTENDED (2026-07-26): confirmed live (2026-07-26, `ls -la /dev/
# watchdog*`) that /dev/watchdog (misc, major 10) and /dev/watchdog0
# (major 251, driver `dw_wdt`, physical `wdt0@0x91106000`) are two
# genuinely separate hardware watchdog timer instances -- different
# major numbers, different kernel driver. Only /dev/watchdog was ever
# petted by this script (or by mm_miner) before now; nothing has ever
# touched /dev/watchdog0. A reproducible ~130-160s reboot was observed
# during --stage6replay testing this same day with /dev/watchdog
# confirmed petted throughout (via `ps`) and the RTOS-side /dev/
# watchdog1 confirmed disabled every boot (`wdog.log`, rc=0) -- ruling
# out both previously-known watchdogs as the cause. /dev/watchdog0 is
# the one remaining, previously-untouched candidate (a `dmesg` message
# "watchdog0: watchdog did not stop!" was flagged once earlier this
# project but never conclusively tied to a reboot -- see BUILD_NOTES.md).
# Not proven as the cause, but petting it defensively costs nothing and
# may fix it outright if it is.
#
# Usage: run in the background right after killing mm_miner, kill it (or
# just let the next reboot clean it up) when done testing.
#   /sharefs/watchdog_pet.sh &
# then later:
#   kill %1        # from the same shell, or `killall watchdog_pet.sh`
#
# Interval is well under the observed ~150s timeout for real margin.
PET_INTERVAL_SECS=20

# /dev/watchdog is the known-critical one -- this exec must succeed or
# there's no point continuing. /dev/watchdog0 is a defensive extra: opening
# it can fail (observed live: "Device or resource busy" if something else
# already holds it), and a failed `exec` on a plain fd is FATAL to a POSIX
# shell script -- do it in a way that can't take down the fd 3 loop below.
exec 3>/dev/watchdog

HAVE_WD0=0
if [ -e /dev/watchdog0 ]; then
	if ( exec 4>/dev/watchdog0 ) 2>/dev/null; then
		exec 4>/dev/watchdog0
		HAVE_WD0=1
	fi
fi

while true; do
	printf '\0' >&3
	if [ "$HAVE_WD0" = "1" ]; then
		printf '\0' >&4 2>/dev/null
	fi
	# DIAGNOSTIC (2026-07-26): timestamped pet log, to verify pets are
	# genuinely landing on schedule right up to a reboot, not silently
	# stopping earlier while the process itself stays alive (which would
	# look identical in a plain `ps` check). Cheap, always-on -- remove
	# once the recurring reboot is root-caused.
	echo "pet: $(date +%s) uptime=$(cut -d' ' -f1 /proc/uptime) wd0=$HAVE_WD0"
	sleep "$PET_INTERVAL_SECS"
done
