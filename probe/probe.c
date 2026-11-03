#ifndef REASON
#define REASON 7
#endif
#include <stdint.h>
typedef uint64_t u64; typedef uint32_t u32; typedef uint8_t u8;
void read_sysregs(u64 *out);
u64 cntpct(void);
u64 smc_call(u64 fn, u64 a1, u64 a2, u64 a3);
void dcache_clean_range(u64 start, u64 end);

#define REC 0x6f000000UL
struct rec { u64 magic, version, x0_dtb, image_base, sys[16], dtb_copied, fb_word0, boots, wdt[4], stage; };

static u32 be32(const void *p) { const u8 *b = p; return (u32)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

void probe_main(u64 dtb, u64 base)
{
	struct rec *r = (struct rec *)REC;
	u64 prev_boots = (r->magic == 0x31424f5250574c54ULL) ? r->boots : 0;
	r->magic = 0x31424f5250574c54ULL; /* "TLWPROB1" */
	r->version = 1;
	r->x0_dtb = dtb;
	r->image_base = base;
	read_sysregs(r->sys);
	r->boots = prev_boots + 1;
	for (int i = 0; i < 4; i++) r->wdt[i] = *(volatile u32 *)(0xfe002100UL + 4 * i);
	r->stage = 1;
	r->dtb_copied = 0;
	if (dtb && be32((void *)dtb) == 0xd00dfeed) {
		u32 sz = be32((u8 *)dtb + 4);
		if (sz > 0x80000) sz = 0x80000;
		u8 *d = (u8 *)(REC + 0x10000), *s = (u8 *)dtb;
		for (u32 i = 0; i < sz; i++) d[i] = s[i];
		r->dtb_copied = sz;
	}
#ifdef PAINT
	/* paint a band at the top of the U-Boot framebuffer: 1920x1080x4 at 0x1f800000 */
	volatile u32 *fb = (volatile u32 *)0x1f800000UL;
	r->fb_word0 = fb[0];
	for (u32 y = 0; y < 270; y++)
		for (u32 x = 0; x < 1920; x++)
			fb[y * 1920 + x] = (x < 640) ? 0xffff0000 : (x < 1280) ? 0xff00ff00 : 0xff0000ff;
	dcache_clean_range(0x1f800000UL, 0x1f800000UL + 1920 * 270 * 4);
#endif
	dcache_clean_range(REC, REC + 0x10000 + 0x80000);
	r->stage = 2;
#ifdef DELAY
	u64 t0 = cntpct();
	while (cntpct() - t0 < 24000000ULL * DELAY) ;
#endif
	smc_call(0x84000009, REASON, 0, 0); /* PSCI SYSTEM_RESET, Amlogic reason 7 = bootloader */
}
