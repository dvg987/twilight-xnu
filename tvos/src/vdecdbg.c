/*
 * Bring-up (vdec_debug=1): the decoder's registers and pool from the shell.
 *   vdecdbg r REG [COUNT]     read DOS registers (hex word indexes)
 *   vdecdbg w REG VALUE       write one
 *   vdecdbg m OFFSET SIZE     hex dump of the pool at OFFSET
 *   vdecdbg i                 the pool's address and size
 *   vdecdbg f ID CORE SWAP    load a firmware through OP-TEE (CORE 2: HEVC)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/hakios.vdec.h>

int
main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: vdecdbg r REG [N] | w REG VALUE | m OFFSET SIZE | i\n"); return 1; }
	int fd = open("/dev/twl-vdec-probe", O_RDWR);
	if (fd < 0) { perror("/dev/twl-vdec-probe"); return 1; }
	if (argv[1][0] == 'r' && argc > 2) {
		uint32_t reg = (uint32_t)strtoul(argv[2], NULL, 16), n = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 1;
		for (uint32_t i = 0; i < n; i++) {
			struct twl_vdec_dbg_reg r = { reg + i, 0, 0, 0 };
			if (ioctl(fd, TWL_VDEC_DBG_REG, &r)) { perror("read"); return 1; }
			printf("%04x: %08x\n", reg + i, r.value);
		}
	} else if (argv[1][0] == 'w' && argc > 3) {
		struct twl_vdec_dbg_reg r = { (uint32_t)strtoul(argv[2], NULL, 16), (uint32_t)strtoul(argv[3], NULL, 16), 1, 0 };
		if (ioctl(fd, TWL_VDEC_DBG_REG, &r)) { perror("write"); return 1; }
	} else if (argv[1][0] == 'm' && argc > 3) {
		uint32_t size = (uint32_t)strtoul(argv[3], NULL, 0);
		uint8_t *buf = calloc(1, size);
		struct twl_vdec_dbg_mem m = { (uintptr_t)buf, (uint32_t)strtoul(argv[2], NULL, 16), size, 0, 0 };
		if (ioctl(fd, TWL_VDEC_DBG_MEM, &m)) { perror("mem"); return 1; }
		for (uint32_t i = 0; i < size; i += 16) {
			printf("%08x:", m.offset + i);
			for (uint32_t k = i; k < i + 16 && k < size; k++) printf(" %02x", buf[k]);
			printf("\n");
		}
	} else if (argv[1][0] == 'f' && argc > 4) {
		struct twl_vdec_dbg_fw fw = { (uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0),
			(uint32_t)strtoul(argv[4], NULL, 0), 0 };
		if (ioctl(fd, TWL_VDEC_DBG_FW, &fw)) { perror("fw"); return 1; }
		printf("firmware %#x: %d\n", fw.id, fw.result);
	} else if (argv[1][0] == 'i') {
		struct twl_vdec_dbg_info info;
		if (ioctl(fd, TWL_VDEC_DBG_INFO, &info)) { perror("info"); return 1; }
		printf("pool %#llx size %#x hevc %u\n", (unsigned long long)info.pool_pa, info.pool_size, info.hevc_powered);
	}
	return 0;
}
