/* reboot2 [reason]: restart with a reason string (Amlogic: bootloader, fastboot, update, cold_boot ...) */
#include <linux/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	sync();
	return syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2,
		       argc > 1 ? argv[1] : "bootloader");
}
