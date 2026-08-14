/* Genuine hardware watchdog disable, held for the process lifetime.
 * Real mm_miner's SIGTERM handler calls watchdog_disable(), which is
 * ioctl(fd, WDIOC_SETOPTIONS, &WDIOS_DISABLECARD) -- confirmed from the
 * vendor binary's disassembly (ghidra_output.txt, VA 0x4e9ec). SIGKILL
 * skips that entirely, leaving the watchdog armed with nothing able to
 * turn it off (see rtos_core/docs/BUILD_NOTES.md, "DEFINITIVE ROOT CAUSE
 * FOUND" section). This helper does the same ioctl directly so `mm_miner`
 * can be killed with -9 (required for ASIC chain health -- SIGTERM
 * poisons the chain) without the device rebooting on any run over ~100s.
 *
 * Usage: run in the background AFTER killing mm_miner, kill it (or just
 * let the next reboot clean it up) when done testing.
 *   /sharefs/wdt_disable_hold &
 *
 * Deliberately never closes the fd: on nowayout-style watchdog drivers,
 * closing without writing the magic 'V' character first re-arms the
 * watchdog instead of leaving it disabled.
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/watchdog.h>

int main(void)
{
	int fd = open("/dev/watchdog", O_RDWR);
	if (fd < 0) {
		perror("open /dev/watchdog");
		return 1;
	}
	int opt = WDIOS_DISABLECARD;
	if (ioctl(fd, WDIOC_SETOPTIONS, &opt) < 0) {
		perror("ioctl WDIOC_SETOPTIONS /dev/watchdog");
		return 1;
	}
	printf("watchdog: disabled /dev/watchdog\n");
	fflush(stdout);

	int fd0 = open("/dev/watchdog0", O_RDWR);
	if (fd0 >= 0) {
		int opt0 = WDIOS_DISABLECARD;
		if (ioctl(fd0, WDIOC_SETOPTIONS, &opt0) == 0)
			printf("watchdog: disabled /dev/watchdog0\n");
		else
			perror("ioctl WDIOC_SETOPTIONS /dev/watchdog0");
		fflush(stdout);
	}

	/* Hold both fds open forever; never call close() on either. */
	for (;;)
		pause();

	return 0;
}
