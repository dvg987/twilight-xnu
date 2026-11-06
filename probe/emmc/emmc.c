/* eMMC probe: drive the SD_EMMC controller as U-Boot left it, read a few sectors, record what happened. */
#include <stdint.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
u64 cntpct(void);
u64 smc_call(u64 fn, u64 a1, u64 a2, u64 a3);

#define REC 0x6f000000UL
#define BUF 0x6f100000UL
#define MMC 0xfe08c000UL
#define R(o) (*(volatile u32 *)(MMC + (o)))
#define START 0x40
#define CFG 0x44
#define STATUS 0x48
#define IRQ_EN 0x4c
#define CMD_CFG 0x50
#define CMD_ARG 0x54
#define CMD_DAT 0x58
#define CMD_RSP 0x5c
#define OWNER (1u << 31)
#define EOC (1u << 11)
#define DATA_IO (1u << 18)
#define BLOCK_MODE (1u << 9)
#define RESP_NUM (1u << 22)
#define R1B (1u << 10)
#define NO_RESP (1u << 16)
#define ST_EOC (1u << 13)
#define ST_ERR 0x1fffu

struct rec { u64 magic, n; u32 clock, cfg, status0, irq_en, delay, adjust; u32 res[64][4]; };

static u32 cmd(int idx, u32 arg, u32 flags, u32 blocks, u64 buf, u32 *resp)
{
	R(START) = 0;
	R(STATUS) = 0xffff;
	u32 c = OWNER | EOC | ((u32)idx << 24) | flags | (12u << 12);
	if (blocks) c |= DATA_IO | BLOCK_MODE | blocks;
	R(CMD_CFG) = c;
	R(CMD_DAT) = (u32)buf;
	R(CMD_RSP) = 0;
	__asm__ volatile("dsb sy");
	R(CMD_ARG) = arg;
	u64 t0 = cntpct();
	u32 st;
	do {
		st = R(STATUS);
	} while (!(st & (ST_EOC | ST_ERR)) && cntpct() - t0 < 24000000ULL);
	if (resp) *resp = R(CMD_RSP);
	R(STATUS) = 0xffff;
	return st;
}

void emmc_main(u64 dtb, u64 base)
{
	struct rec *r = (struct rec *)REC;
	r->magic = 0x434d4d45504c5754ULL; /* "TWLPEMMC" */
	r->clock = R(0); r->cfg = R(CFG); r->status0 = R(STATUS); r->irq_en = R(IRQ_EN);
	r->delay = R(4); r->adjust = R(8);
	R(IRQ_EN) = 0;
	int n = 0;
	u32 resp;
	/* 0: CMD13 with RCA 1 (status), 1: the same with RCA 0x2 */
	for (u32 rca = 1; rca <= 2; rca++) {
		u32 st = cmd(13, rca << 16, RESP_NUM, 0, 0, &resp);
		r->res[n][0] = 13; r->res[n][1] = rca; r->res[n][2] = st; r->res[n][3] = resp; n++;
	}
	/* single-block reads: LBA 0, 1, darwin start; then 8 blocks with CMD23+CMD18 */
	u32 lbas[3] = { 0, 1, 40095744 };
	for (int i = 0; i < 3; i++) {
		for (int k = 0; k < 512; k += 4) *(volatile u32 *)(BUF + i * 0x1000 + k) = 0xdeadbeef;
		u32 st = cmd(17, lbas[i], RESP_NUM, 1, BUF + i * 0x1000, &resp);
		r->res[n][0] = 17; r->res[n][1] = lbas[i]; r->res[n][2] = st; r->res[n][3] = resp; n++;
	}
	u32 st = cmd(23, 8, RESP_NUM, 0, 0, &resp);
	r->res[n][0] = 23; r->res[n][1] = 8; r->res[n][2] = st; r->res[n][3] = resp; n++;
	st = cmd(18, 2, RESP_NUM, 8, BUF + 0x4000, &resp);
	r->res[n][0] = 18; r->res[n][1] = 2; r->res[n][2] = st; r->res[n][3] = resp; n++;
	r->n = n;
	smc_call(0x84000009, 7, 0, 0);
}
