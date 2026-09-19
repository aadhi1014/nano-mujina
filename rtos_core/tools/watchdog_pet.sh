#!/bin/sh
# Pets /dev/watchdog and (if present) /dev/watchdog0 in a loop, so the
# hardware watchdog doesn't reset the device while mm_miner (which
# normally pets it) is not running.
#
# /dev/watchdog and /dev/watchdog0 are separate watchdog device nodes;
# both are opened and petted if available.
#
# Usage: run in the background, then kill it (or reboot) when done.
#   /sharefs/watchdog_pet.sh &
# then later:
#   kill %1        # from the same shell, or `killall watchdog_pet.sh`
PET_INTERVAL_SECS=20

# fd 3 is /dev/watchdog and is required. fd 4 is /dev/watchdog0 and is
# optional, so it's opened in a subshell first to test availability
# without aborting the script if it fails.
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
	# Logs each pet with a timestamp and current uptime.
	echo "pet: $(date +%s) uptime=$(cut -d' ' -f1 /proc/uptime) wd0=$HAVE_WD0"
	sleep "$PET_INTERVAL_SECS"
done
