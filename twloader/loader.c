/*
 * twloader: plays iBoot for HakiOS's XNU on the Xiaomi Mi Box S Gen 3 (twilight, Amlogic S905X5M)
 * and on QEMU's virt machine with a GICv2, both of which start it the way they start Linux.
 *
 *   - finds the XNUP payload (kernel, RAM disk, command line) where the firmware put the initrd
 *   - takes the largest stretch of RAM nobody else needs as XNU's [physBase, physBase + memSize)
 *   - loads the Mach-O kernel there, builds the Apple device tree and boot_args behind it
 *   - on the box: starts its last core as a monitor, which takes the log of a kernel that has
 *     stopped making progress and restarts into fastboot (RAM survives that, the rescue Linux reads it)
 *   - leaves EL2 for EL1 and jumps into the kernel
 *
 * The MMU stays off on the boot core: memory is Device memory to us, so every access is aligned.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "identity.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

u64 smc_call(u64 fn, u64 a1, u64 a2, u64 a3);
u64 hvc_call(u64 fn, u64 a1, u64 a2, u64 a3);
u64 read_cntpct(void);
u64 read_cntfrq(void);
u64 current_el(void);
u64 read_mpidr(void);
void dcache_clean(u64 start, u64 end);
void dcache_inval(u64 start, u64 end);
void enter_kernel(u64 entry, u64 boot_args) __attribute__((noreturn));
void el2_mmu_on(u64 ttbr, u64 tcr, u64 mair);
void loader_return_stub(void);
void monitor_entry(void);
void secondary_trampoline(void);
extern char _start[], _image_end[];

/* ------------------------------------------------------------------------ */
/* Conventions shared with HakiOS's kernel and payload tools                */

#define PAYLOAD_MAGIC       0x50554E58u                 /* "XNUP" */
#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL
#define KERNEL_LINK_OFFSET  0x03000000ULL
#define BASE_ALIGN          0x02000000ULL
#define MAX_MEM_SIZE        0x180000000ULL
#define PAGE_SIZE_MAX       0x4000ULL
#define KERNEL_LIVE_MAGIC   0x214556494c554e58ULL       /* "XNULIVE!" */
#define RETURN_SLOT_MAGIC   0x545352444C554E58ULL       /* "XNULDRST" */
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_SIZE      0x80000u
#define BOOT_LINE_LENGTH    608

#define PSCI_CPU_ON         0xC4000003u
#define PSCI_AFFINITY_INFO  0xC4000004u
#define PSCI_SYSTEM_OFF     0x84000008u
#define PSCI_SYSTEM_RESET   0x84000009u
#define AML_REASON_BOOTLOADER 7                         /* back into U-Boot's fastboot, RAM kept */

struct boot_video { u64 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; };
struct boot_args {
	u16 revision, version;
	u64 virtBase, physBase, memSize, topOfKernelData;
	struct boot_video video;
	u32 machineType;
	u64 deviceTreeP;
	u32 deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	u64 bootFlags, memSizeActual;
};

#define MH_MAGIC_64   0xFEEDFACFu
#define LC_SEGMENT_64 0x19
#define LC_UNIXTHREAD 0x5
struct mach_header_64 { u32 magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { u32 cmd, cmdsize; };
struct segment_command_64 {
	u32 cmd, cmdsize; char segname[16];
	u64 vmaddr, vmsize, fileoff, filesize;
	u32 maxprot, initprot, nsects, flags;
};

struct kernel_live {
	u64 magic, heartbeat;
	u64 log, log_len, log_ring_at, log_wrapped;     /* kernel virtual addresses */
	u64 log_head, log_ring;
	u64 recording, stop, magic2, sound;
};

/*
 * The mailbox: 16 MB of RAM outside the kernel's window that the rescue Linux reads back
 * (tools/twl-log.py). Header, then our own log, then the kernel's.
 */
#define MBOX_MAGIC      0x31584f424d4c5754ULL           /* "TWLMBOX1" */
#define MBOX_SIZE       0x01000000ULL
#define MBOX_LLOG       0x00001000ULL
#define MBOX_LLOG_SIZE  0x000ff000ULL
#define MBOX_KLOG       0x00100000ULL
#define MBOX_KLOG_SIZE  0x00f00000ULL
enum { ST_NONE, ST_LOADER, ST_KERNEL, ST_RETURNED, ST_HUNG, ST_FAILED, ST_EL2_TRAP };
struct mbox {
	u64 magic, boots, state, reason;
	u64 regs[8];                    /* what the kernel or a trap said: reason codes, ESR, ELR, FAR ... */
	u64 llog_len, klog_len;
	u64 phys_base, mem_size, slide, kernel_live_pa;
	u64 heartbeat, monitor_seconds, hang_seconds;
	u64 t_loader, t_kernel;         /* counter values at loader start and kernel entry */
	u64 shots, shot_every, shot_from;       /* the monitor's time-lapse: slots taken, seconds apart, from second */
	u64 shot_at[32];                /* seconds since the kernel's entry, per slot (a ring) */
};
#define SHOT_RING_PA    0x70800000ULL   /* slot n at SHOT_RING_PA + n * 8 MB, up to 0x7f000000 */
#define SHOT_SLOTS      10              /* (the rescue Linux uses the RAM above 0x75800000) */
/* kept by the monitor while the kernel runs (never in the mailbox then), put there on the way back */
static volatile u64 shot_times[32] __attribute__((aligned(64)));
static volatile u64 shot_count __attribute__((aligned(64)));
static volatile u64 monitor_ticks __attribute__((aligned(64)));

/* ------------------------------------------------------------------------ */
/* Platforms                                                                */

struct range { u64 base, size; };
struct platform {
	const char *name, *match;       /* the FDT root compatible starts with match */
	u64 io_base, io_size;
	u64 gicd, gicd_size, gicc, gicc_size;
	u64 uart;                       /* PL011, 0: none */
	u64 fb, fb_stride;              /* the firmware's framebuffer, 0: none */
	u32 fb_width, fb_height;
	int cores, core_shift, monitor;
	struct range fixed[6];          /* RAM that is never the kernel's */
	u64 mbox;                       /* 0: the top 16 MB of RAM */
};

static const struct platform platforms[] = {
	{
		.name = "twilight", .match = "s7d",
		.io_base = 0xf0000000, .io_size = 0x10000000,
		.gicd = 0xfff01000, .gicd_size = 0x1000, .gicc = 0xfff02000, .gicc_size = 0x2000,
		.fb = 0x1f800000, .fb_stride = 1920 * 4, .fb_width = 1920, .fb_height = 1080,
		.cores = 4, .core_shift = 8, .monitor = 1,
		.fixed = {
			{ 0x00000000, 0x07500000 },     /* U-Boot's own, the secure monitor and TEE, ramoops */
			{ 0x1f800000, 0x00800000 },     /* the framebuffer the display scans out */
			{ 0x6f000000, 0x01000000 },     /* the mailbox */
			{ 0x70000000, 0x0f000000 },     /* the last frame, saved on the way back, and the monitor's time-lapse */
			{ 0x7f000000, 0x01000000 },     /* the secure world's at the top */
		},
		.mbox = 0x6f000000,
	},
	{
		.name = "qemu-virt", .match = "linux,dummy-virt",
		.io_base = 0x08000000, .io_size = 0x08000000,
		.gicd = 0x08000000, .gicd_size = 0x10000, .gicc = 0x08010000, .gicc_size = 0x10000,
		.uart = 0x09000000,
		.cores = 8, .core_shift = 0, .monitor = 0,
	},
};

static const struct platform *P;
static u64 fb_base, fb_stride, fb_width, fb_height;     /* the platform's, or QEMU's ramfb */
static struct mbox *MB;
static int psci_hvc;
static u64 ram_base, ram_size;

/* ------------------------------------------------------------------------ */
/* Memory helpers: aligned accesses only (Device memory with the MMU off)   */

void *
memset(void *dst, int c, size_t n)
{
	u8 *d = dst;
	u64 v = (u8)c * 0x0101010101010101ULL;

	while (n && ((uintptr_t)d & 7)) {
		*d++ = (u8)c; n--;
	}
	for (; n >= 8; n -= 8, d += 8) {
		*(volatile u64 *)d = v;
	}
	while (n--) {
		*d++ = (u8)c;
	}
	return dst;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	u8 *d = dst;
	const u8 *s = src;

	if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
		while (n && ((uintptr_t)d & 7)) {
			*d++ = *s++; n--;
		}
		for (; n >= 8; n -= 8, d += 8, s += 8) {
			*(volatile u64 *)d = *(const volatile u64 *)s;
		}
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	u8 *d = dst;
	const u8 *s = src;

	if (d <= s || d >= s + n) {
		return memcpy(dst, src, n);
	}
	while (n--) {
		d[n] = s[n];
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const u8 *x = a, *y = b;

	for (; n; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++; b++;
	}
	return *a == *b;
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

static u32
be32(const void *p)
{
	const u8 *b = p;

	return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

/* ------------------------------------------------------------------------ */
/* Log: the mailbox, and the UART where there is one                        */

static void
uart_putc(char c)
{
	volatile u32 *u = (volatile u32 *)P->uart;

	if (c == '\n') {
		uart_putc('\r');
	}
	while (u[0x18 / 4] & (1u << 5)) {
		/* TX FIFO full */
	}
	u[0] = (u8)c;
}

static void
put_char(char c)
{
	if (P != NULL && P->uart != 0) {
		uart_putc(c);
	}
	if (MB != NULL && MB->llog_len < MBOX_LLOG_SIZE) {
		((volatile char *)MB)[MBOX_LLOG + MB->llog_len] = c;
		MB->llog_len++;
	}
}

static void
put_str(const char *s)
{
	while (*s) {
		put_char(*s++);
	}
}

static void
put_hex(u64 v)
{
	static const char hex[] = "0123456789abcdef";
	char buf[19];
	int n = 0;

	buf[n++] = '0';
	buf[n++] = 'x';
	int started = 0;
	for (int shift = 60; shift >= 0; shift -= 4) {
		int d = (v >> shift) & 0xf;
		if (d || started || shift == 0) {
			buf[n++] = hex[d];
			started = 1;
		}
	}
	buf[n] = 0;
	put_str(buf);
}

static void
put_dec(u64 v)
{
	char buf[21];
	int n = 20;

	buf[n] = 0;
	do {
		buf[--n] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	put_str(buf + n);
}

/* %x (hex, 64-bit), %d (decimal, 64-bit), %s */
static void
print(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 'x') {
			put_hex(va_arg(ap, u64));
		} else if (*fmt == 'd') {
			put_dec(va_arg(ap, u64));
		} else if (*fmt == 's') {
			const char *s = va_arg(ap, const char *);
			put_str(s ? s : "(null)");
		} else {
			put_char(*fmt);
		}
	}
	va_end(ap);
}

static u64
psci(u64 fn, u64 a1, u64 a2, u64 a3)
{
	return psci_hvc ? hvc_call(fn, a1, a2, a3) : smc_call(fn, a1, a2, a3);
}

/* The last frame on the screen, where the rescue Linux reads it back (tools/twl-shot.py): outside the kernel's window. */
#define SHOT_PA 0x70000000ULL

static void
save_screen(void)
{
	if (MB != NULL) {
		dcache_inval((u64)shot_times, (u64)shot_times + sizeof(shot_times) + 64);
		MB->shots = shot_count;
		dcache_inval((u64)&monitor_ticks, (u64)&monitor_ticks + 8);
		MB->monitor_seconds = monitor_ticks;
		for (int i = 0; i < 32; i++) {
			MB->shot_at[i] = shot_times[i];
		}
	}
	if (P != NULL && fb_base != 0 && P->mbox != 0) {
		memcpy((void *)SHOT_PA, (const void *)fb_base, fb_stride * fb_height);
		dcache_clean(SHOT_PA, SHOT_PA + fb_stride * fb_height);
	}
}

/* Back to the firmware: on the box into fastboot with RAM kept, under QEMU off. */
static void __attribute__((noreturn))
finish(void)
{
	save_screen();
	dcache_clean((u64)MB, (u64)MB + MBOX_SIZE);
	if (P->monitor) {
		psci(PSCI_SYSTEM_RESET, AML_REASON_BOOTLOADER, 0, 0);
	} else {
		psci(PSCI_SYSTEM_OFF, 0, 0, 0);
	}
	for (;;) {
	}
}

/* ------------------------------------------------------------------------ */
/* FDT: just what we need of the firmware's tree                            */

struct fdt {
	const u8 *base, *structs, *strings;
	u32 size;
};

static int
fdt_open(struct fdt *f, u64 addr)
{
	const u8 *b = (const u8 *)addr;

	if (addr == 0 || be32(b) != 0xd00dfeed) {
		return 0;
	}
	f->base = b;
	f->size = be32(b + 4);
	f->structs = b + be32(b + 8);
	f->strings = b + be32(b + 12);
	return 1;
}

/*
 * Find a property: path is "/" (the root), "/chosen" or the like, matched by node name up to
 * '@' at each level. Returns its value and length, or NULL.
 */
static const u8 *
fdt_prop(const struct fdt *f, const char *path, const char *prop, u32 *len)
{
	const u8 *p = f->structs;
	int depth = -1, matched = -1;       /* matched: the depth down to which the path is matched */
	const char *want[8];
	int nwant = 0;
	char comp[8][32];

	/* split the path */
	for (const char *c = path; *c && nwant < 8;) {
		while (*c == '/') {
			c++;
		}
		if (!*c) {
			break;
		}
		int n = 0;
		while (*c && *c != '/' && n < 31) {
			comp[nwant][n++] = *c++;
		}
		comp[nwant][n] = 0;
		want[nwant] = comp[nwant];
		nwant++;
	}
	for (;;) {
		u32 tag = be32(p);
		p += 4;
		if (tag == 1) {                         /* FDT_BEGIN_NODE */
			const char *name = (const char *)p;
			size_t n = str_len(name);
			p += (n + 4) & ~3UL;
			depth++;
			if (depth == 0) {
				matched = 0;
			} else if (matched == depth - 1 && depth <= nwant) {
				const char *w = want[depth - 1];
				size_t i = 0;
				while (w[i] && w[i] == name[i]) {
					i++;
				}
				if (w[i] == 0 && (name[i] == 0 || name[i] == '@')) {
					matched = depth;
				}
			}
		} else if (tag == 2) {                  /* FDT_END_NODE */
			if (matched == depth) {
				matched--;
			}
			depth--;
			if (depth < 0) {
				return NULL;
			}
		} else if (tag == 3) {                  /* FDT_PROP */
			u32 plen = be32(p), nameoff = be32(p + 4);
			const u8 *val = p + 8;
			p += 8 + ((plen + 3) & ~3u);
			if (matched == depth && depth == nwant && str_eq((const char *)f->strings + nameoff, prop)) {
				*len = plen;
				return val;
			}
		} else if (tag == 4) {                  /* FDT_NOP */
			continue;
		} else {
			return NULL;
		}
	}
}

static u64
fdt_cells(const u8 *v, u32 cells)
{
	return cells == 2 ? ((u64)be32(v) << 32 | be32(v + 4)) : be32(v);
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload {
	const u8 *kernel, *ramdisk, *ramdisk1;
	u64 kernel_size, ramdisk_size, ramdisk1_size;
	const char *cmdline, *version;
};

static int
parse_payload(struct payload *pl, u64 addr, u64 size)
{
	const u8 *b = (const u8 *)addr;
	u32 total;

	memset(pl, 0, sizeof(*pl));
	pl->cmdline = "";
	if (size < 8 || *(const u32 *)b != PAYLOAD_MAGIC) {
		print("no XNUP payload at %x (size %x)\n", addr, size);
		return 0;
	}
	total = ((const u32 *)b)[1];
	if (total > size - 8) {
		print("payload says %x bytes, the initrd has %x\n", (u64)total, size - 8);
		return 0;
	}
	for (u64 off = 8; off + 24 <= 8 + (u64)total;) {
		const char *name = (const char *)(b + off);
		u32 len = *(const u32 *)(b + off + 16);
		const u8 *data = b + off + 24;

		if (name[0] == 0) {
			break;
		}
		if (off + 24 + len > 8 + (u64)total) {
			print("payload entry %s runs past the end\n", name);
			return 0;
		}
		if (str_eq(name, "kernel")) {
			pl->kernel = data; pl->kernel_size = len;
		} else if (str_eq(name, "ramdisk")) {
			pl->ramdisk = data; pl->ramdisk_size = len;
		} else if (str_eq(name, "ramdisk1")) {
			pl->ramdisk1 = data; pl->ramdisk1_size = len;
		} else if (str_eq(name, "cmdline")) {
			pl->cmdline = (const char *)data;
		} else if (str_eq(name, "kernel.ver")) {
			pl->version = (const char *)data;
		}
		print("payload: %s %x bytes\n", name, (u64)len);
		off += 24 + ((len + 15) & ~15u);
	}
	return pl->kernel != NULL;
}

/* ------------------------------------------------------------------------ */
/* The kernel's memory window: the largest free stretch of RAM              */

static struct range taken[16];
static int ntaken;

static void
take(u64 base, u64 size)
{
	if (size != 0 && ntaken < 16) {
		taken[ntaken].base = base;
		taken[ntaken].size = size;
		ntaken++;
	}
}

static int
choose_window(u64 *win_base, u64 *win_size)
{
	u64 best_base = 0, best_size = 0, at = ram_base, end = ram_base + ram_size;

	/* the taken ranges in order of their base, then the gaps between them */
	for (int i = 1; i < ntaken; i++) {
		for (int j = i; j > 0 && taken[j].base < taken[j - 1].base; j--) {
			struct range t = taken[j];
			taken[j] = taken[j - 1];
			taken[j - 1] = t;
		}
	}
	for (int i = 0; i <= ntaken; i++) {
		u64 gap_end = i < ntaken ? taken[i].base : end;
		if (gap_end > end) {
			gap_end = end;
		}
		if (gap_end > at) {
			u64 b = (at + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
			u64 e = gap_end & ~(PAGE_SIZE_MAX - 1);
			if (e > b && e - b > best_size) {
				best_base = b;
				best_size = e - b;
			}
		}
		if (i < ntaken && taken[i].base + taken[i].size > at) {
			at = taken[i].base + taken[i].size;
		}
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	*win_base = best_base;
	*win_size = best_size;
	return best_size >= 0x10000000ULL;
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer (as HakiOS's xnuloader writes it)               */

struct dt_writer {
	u8 *base, *cur, *end;
	int overflow;
	u32 scratch[2];
	u32 *counts[16];
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, u32 len)
{
	u64 padded = ((u64)len + 3) & ~3ULL;

	if (w->overflow || str_len(name) > 32 || (u64)(w->end - w->cur) < 36 + padded) {
		w->overflow = 1;
		return;
	}
	memset(w->cur, 0, 36);
	memcpy(w->cur, name, str_len(name));
	*(u32 *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (size_t)padded);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + padded;
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (u32)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, u32 value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, u64 value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, u32 size)
{
	u8 field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, u32 count, const unsigned char *data)
{
	for (u32 i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut")) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	if (w->overflow || (u64)(w->end - w->cur) < 8) {
		w->overflow = 1;
		w->counts[w->depth++] = w->scratch;
		return;
	}
	w->counts[w->depth++] = (u32 *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* iBoot hands the kernel its NVRAM in CHRP format; there is none here: a valid, empty one. */
static void
nvram_chrp_header(u8 *h, u8 sig, u32 bytes, const char *name)
{
	u16 blocks = (u16)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (u8)blocks;
	h[3] = (u8)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (u8)sum;
}

static void
build_nvram_image(u8 *image)
{
	u32 lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (u32 i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	u32 adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Command line                                                             */

struct line {
	char *buf;
	u32 n;
};

static void
line_add(struct line *l, const char *s)
{
	while (*s && l->n < BOOT_LINE_LENGTH - 1) {
		l->buf[l->n++] = *s++;
	}
	l->buf[l->n] = 0;
}

static int
cmdline_has(const char *cmdline, const char *word)
{
	for (const char *c = cmdline; *c; c++) {
		if ((c == cmdline || c[-1] == ' ') && starts_with(c, word)) {
			return 1;
		}
	}
	return 0;
}

static u64
cmdline_number(const char *cmdline, const char *key, u64 dflt)
{
	for (const char *c = cmdline; *c; c++) {
		if ((c == cmdline || c[-1] == ' ') && starts_with(c, key)) {
			u64 v = 0;
			for (c += str_len(key); *c >= '0' && *c <= '9'; c++) {
				v = v * 10 + (u64)(*c - '0');
			}
			return v;
		}
	}
	return dflt;
}

/* ------------------------------------------------------------------------ */
/* The monitor core                                                         */

static volatile u64 monitor_started;
static u64 monitor_stack[0x1000] __attribute__((aligned(16)));
static u64 monitor_l1[512] __attribute__((aligned(4096)));
static struct kernel_live *live_pa;
static u64 live_slide;

/* the kernel's log in order (its head, then its ring from the oldest), into the mailbox */
static void
copy_kernel_log(void)
{
	struct kernel_live *live = live_pa;
	const char *log;
	u64 n = 0, len, ring_at, head, ring;
	int wrapped;
	char *out = (char *)MB + MBOX_KLOG;

	if (live == NULL) {
		MB->klog_len = 0;
		return;
	}
	log = (const char *)(live->log + live_slide);
	len = *(volatile u32 *)(live->log_len + live_slide);
	ring_at = *(volatile u32 *)(live->log_ring_at + live_slide);
	wrapped = *(volatile u8 *)(live->log_wrapped + live_slide);
	head = live->log_head;
	ring = live->log_ring;
	if (wrapped && ring_at < ring && head + ring <= MBOX_KLOG_SIZE) {
		memcpy(out, log, head);
		memcpy(out + head, log + head + ring_at, ring - ring_at);
		memcpy(out + head + ring - ring_at, log + head, ring_at);
		n = head + ring;
	} else {
		n = len < MBOX_KLOG_SIZE ? len : MBOX_KLOG_SIZE;
		memcpy(out, log, n);
	}
	MB->klog_len = n;
}

/*
 * Runs on the last core, at EL2 with an identity map and the caches on, so that it sees what the
 * kernel's cores have in their caches. The kernel must be making progress: its heartbeat (every
 * ten seconds from early boot) or its log. When neither moves for hang_seconds, the log goes into
 * the mailbox and the box restarts into fastboot.
 */
void
twl_monitor(void)
{
	if (current_el() == 2) {
		for (u64 i = 0; i < 4; i++) {
			u64 pa = i << 30;
			int ram = pa < ram_base + ram_size && pa + (1ULL << 30) > ram_base;
			/* block descriptor: AF, inner shareable, AttrIndx 1 (normal WB) or 0 (device) */
			monitor_l1[i] = pa | 1 | (1u << 10) | (ram ? (3u << 8) | (1u << 2) : 0);
		}
		/* TCR_EL2: T0SZ 32 (4 GB), WB WA inner and outer, inner shareable, 4K granule, 40-bit PA */
		u64 tcr = 32 | (1u << 8) | (1u << 10) | (3u << 12) | (2ULL << 16) | (1u << 23) | (1u << 31);
		el2_mmu_on((u64)monitor_l1, tcr, 0xff00ULL /* attr1 normal WB, attr0 device nGnRnE */);
	}
	monitor_started = 1;
	dcache_clean((u64)&monitor_started, (u64)&monitor_started + 8);

	/*
	 * While the kernel runs, the mailbox is only read here (each time from RAM): a line of it dirty in this
	 * core's cache could be written back over what the kernel's way back writes there with its MMU off.
	 */
	u64 freq = read_cntfrq(), last_change = read_cntpct(), last_beat = ~0ULL, last_len = ~0ULL;
	u64 hang_seconds;
	dcache_inval((u64)MB, (u64)MB + 0x1000);
	hang_seconds = MB->hang_seconds;
	u64 shot_every = MB->shot_every, shot_from = MB->shot_from, shots = 0, ticks = 0;
	for (;;) {
		u64 t0 = read_cntpct();
		while (read_cntpct() - t0 < freq) {
		}
		dcache_inval((u64)MB, (u64)MB + 0x1000);
		u64 state = MB->state;
		if (state == ST_RETURNED || state == ST_FAILED || state == ST_EL2_TRAP) {
			for (;;) {
				__asm__ volatile ("wfe");
			}
		}
		if (state != ST_KERNEL) {
			last_change = read_cntpct();
			continue;
		}
		ticks++;
		monitor_ticks = ticks;
		dcache_clean((u64)&monitor_ticks, (u64)&monitor_ticks + 8);
		if (shot_every != 0 && fb_base != 0 && P->mbox != 0 && ticks >= shot_from && ticks % shot_every == 0) {
			/* the frame as the display reads it: from RAM, not from what this core may have cached of it */
			u64 bytes = fb_stride * fb_height, slot = SHOT_RING_PA + (shots % SHOT_SLOTS) * 0x800000ULL;
			dcache_inval(fb_base, fb_base + bytes);
			memcpy((void *)slot, (const void *)fb_base, bytes);
			dcache_clean(slot, slot + bytes);
			shot_times[shots % 32] = ticks;
			shots++;
			shot_count = shots;
			dcache_clean((u64)shot_times, (u64)shot_times + sizeof(shot_times) + 64);
		}
		u64 beat = live_pa ? live_pa->heartbeat : 0;
		u64 len = live_pa ? *(volatile u32 *)(live_pa->log_len + live_slide) : 0;
		if (beat != last_beat || len != last_len) {
			last_beat = beat;
			last_len = len;
			last_change = read_cntpct();
		} else if ((read_cntpct() - last_change) / freq >= hang_seconds) {
			MB->heartbeat = beat;
			copy_kernel_log();
			MB->state = ST_HUNG;
			print("monitor: no progress for %d s (heartbeat %d, log %x bytes): restarting\n", hang_seconds, beat, len);
			finish();
		}
	}
}

static void
start_monitor(void)
{
	u64 self = read_mpidr() & 0xffffff;
	u64 target = (u64)(P->cores - 1) << P->core_shift;

	if (target == self) {
		print("monitor: this is the last core already; none\n");
		return;
	}
	u64 r = psci(PSCI_CPU_ON, target, (u64)monitor_entry, (u64)&monitor_stack[0x1000]);
	if (r != 0) {
		print("monitor: PSCI CPU_ON(%x) failed: %x\n", target, r);
		return;
	}
	u64 t0 = read_cntpct();
	while (!monitor_started && read_cntpct() - t0 < read_cntfrq()) {
		dcache_inval((u64)&monitor_started, (u64)&monitor_started + 8);
	}
	print("monitor: core %x %s\n", target, monitor_started ? "running" : "did not report in");
}

/* ------------------------------------------------------------------------ */
/* Ways back                                                                */

void
twl_kernel_returned(u64 log_pa, u64 log_len, u64 reason, u64 x3, u64 x4, u64 x5, u64 x6, u64 x7)
{
	u64 n = log_len < MBOX_KLOG_SIZE ? log_len : MBOX_KLOG_SIZE;

	MB->regs[0] = reason;
	MB->regs[1] = x3;
	MB->regs[2] = x4;
	MB->regs[3] = x5;
	MB->regs[4] = x6;
	MB->regs[5] = x7;
	MB->reason = reason;
	if (log_pa != 0 && n != 0) {
		memcpy((char *)MB + MBOX_KLOG, (const char *)log_pa, n);
	}
	MB->klog_len = log_pa ? n : 0;
	MB->state = ST_RETURNED;
	print("\nthe kernel came back: reason %x, %x bytes of log, %x %x %x %x %x\n", reason, log_len, x3, x4, x5, x6, x7);
	if (P->uart != 0 && log_pa != 0) {
		put_str("== KERNEL LOG ==\n");
		for (u64 i = 0; i < n; i++) {
			uart_putc(((const char *)log_pa)[i]);
		}
	}
	finish();
}

void
twl_el2_exception(u64 esr, u64 elr, u64 far)
{
	if (MB != NULL) {
		MB->state = ST_EL2_TRAP;
		MB->regs[0] = esr;
		MB->regs[1] = elr;
		MB->regs[2] = far;
	}
	print("EL2 exception: ESR %x ELR %x FAR %x\n", esr, elr, far);
	if (P != NULL) {
		finish();
	}
	for (;;) {
	}
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */

static void __attribute__((noreturn))
fail(const char *why)
{
	print("twloader: %s: not started\n", why);
	if (MB != NULL) {
		MB->state = ST_FAILED;
	}
	finish();
}

/*
 * QEMU's ramfb: a display of any size in memory of ours, set through the fw_cfg file "etc/ramfb" (DMA interface).
 * The same size as the box's screen, 32 bits a pixel.
 */
#define FW_CFG_BASE     0x09020000ULL
static int cmdline_ramfb_debug = 0;

static void
ramfb_setup(u64 at)
{
	static struct { u64 addr; u32 fourcc, flags, width, height, stride; } __attribute__((packed, aligned(8))) cfg;
	static volatile struct { u32 control, length; u64 address; } __attribute__((aligned(8))) dma;
	struct { u32 size; u16 select, reserved; char name[56]; } file;
	u32 count = 0, select = 0;

	*(volatile u16 *)(FW_CFG_BASE + 8) = __builtin_bswap16(0x19);
	for (u32 i = 0; i < 4; i++) {
		((u8 *)&count)[i] = *(volatile u8 *)FW_CFG_BASE;
	}
	count = __builtin_bswap32(count);
	for (u32 i = 0; i < count && i < 512 && select == 0; i++) {
		for (u32 k = 0; k < sizeof(file); k++) {
			((u8 *)&file)[k] = *(volatile u8 *)FW_CFG_BASE;
		}
		if (cmdline_ramfb_debug) {
			print("fw_cfg: %x %s (%d bytes)\n", (u64)__builtin_bswap16(file.select), file.name, (u64)__builtin_bswap32(file.size));
		}
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0) {
		print("ramfb: none (start QEMU with -device ramfb)\n");
		return;
	}
	memset((void *)at, 0, 1920 * 4 * 1080);
	cfg.addr = __builtin_bswap64(at);
	cfg.fourcc = __builtin_bswap32(0x34325258);     /* 'XR24' */
	cfg.flags = 0;
	cfg.width = __builtin_bswap32(1920);
	cfg.height = __builtin_bswap32(1080);
	cfg.stride = __builtin_bswap32(1920 * 4);
	dma.control = __builtin_bswap32(select << 16 | 0x08 | 0x10);
	dma.length = __builtin_bswap32(28);    /* (sizeof is 32: the struct is aligned to 8) */
	dma.address = __builtin_bswap64((u64)&cfg);
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile u32 *)(FW_CFG_BASE + 16) = __builtin_bswap32((u32)((u64)&dma >> 32));
	*(volatile u32 *)(FW_CFG_BASE + 20) = __builtin_bswap32((u32)(u64)&dma);
	for (u32 spin = 0; dma.control != 0 && spin < 10000000; spin++) {
	}
	if (dma.control != 0) {
		print("ramfb: the request did not complete (control %x, select %x)\n", (u64)__builtin_bswap32(dma.control), (u64)select);
		return;
	}
	fb_base = at;
	fb_stride = 1920 * 4;
	fb_width = 1920;
	fb_height = 1080;
	print("ramfb: 1920x1080 at %x\n", at);
}

void
twl_main(u64 fdt_pa, u64 image_base)
{
	struct fdt f;
	u32 len;
	const u8 *v;
	const char *compat = "";

	if (!fdt_open(&f, fdt_pa)) {
		P = &platforms[0];      /* without a tree it can only be the box (QEMU always gives one) */
	} else {
		if ((v = fdt_prop(&f, "/", "compatible", &len)) != NULL) {
			compat = (const char *)v;
		}
		P = &platforms[0];
		for (u32 i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
			if (starts_with(compat, platforms[i].match)) {
				P = &platforms[i];
			}
		}
	}

	/* RAM from the tree (the box: the U-Boot tree's /memory, which keeps the top 8 MB) */
	ram_base = 0;
	ram_size = 0x7f800000;
	if (fdt_open(&f, fdt_pa) && (v = fdt_prop(&f, "/memory", "reg", &len)) != NULL && len >= 16) {
		ram_base = fdt_cells(v, 2);
		ram_size = fdt_cells(v + 8, 2);
	}
	u64 mbox = P->mbox ? P->mbox : ram_base + ram_size - MBOX_SIZE;
	fb_base = P->fb;
	fb_stride = P->fb_stride;
	fb_width = P->fb_width;
	fb_height = P->fb_height;
	MB = (struct mbox *)mbox;
	u64 boots = MB->magic == MBOX_MAGIC ? MB->boots : 0;
	memset(MB, 0, 0x1000);
	MB->magic = MBOX_MAGIC;
	MB->boots = boots + 1;
	MB->state = ST_LOADER;
	MB->t_loader = read_cntpct();

	print("\ntwloader for HakiOS on %s (FDT compatible \"%s\"), EL%d, core %x, image at %x\n", P->name, compat,
	    current_el(), read_mpidr() & 0xffffff, image_base);
	print("RAM %x..%x, mailbox %x (run %d)\n", ram_base, ram_base + ram_size, mbox, MB->boots);

	if (fdt_open(&f, fdt_pa) && (v = fdt_prop(&f, "/psci", "method", &len)) != NULL) {
		psci_hvc = str_eq((const char *)v, "hvc");
	}

	/* the payload is the initrd */
	u64 rd_start = 0, rd_end = 0;
	if (fdt_open(&f, fdt_pa)) {
		if ((v = fdt_prop(&f, "/chosen", "linux,initrd-start", &len)) != NULL) {
			rd_start = fdt_cells(v, len / 4);
		}
		if ((v = fdt_prop(&f, "/chosen", "linux,initrd-end", &len)) != NULL) {
			rd_end = fdt_cells(v, len / 4);
		}
	}
	print("initrd %x..%x, FDT %x\n", rd_start, rd_end, fdt_pa);
	struct payload pl;
	if (rd_end <= rd_start || !parse_payload(&pl, rd_start, rd_end - rd_start)) {
		fail("no kernel");
	}

	/* everything that is not the kernel's to have */
	for (int i = 0; i < 6; i++) {
		take(P->fixed[i].base, P->fixed[i].size);
	}
	take(mbox, MBOX_SIZE);
	if (P->fb == 0 && P->uart != 0) {
		/* QEMU: ramfb in the 16 MB below the mailbox */
		ramfb_setup(mbox - 0x1000000ULL);
		take(mbox - 0x1000000ULL, 0x1000000ULL);
	}
	take(image_base & ~0xfffULL, (u64)(_image_end - _start) + 0x2000);
	take(rd_start & ~0xfffULL, rd_end - (rd_start & ~0xfffULL) + 0x1000);
	if (fdt_pa) {
		take(fdt_pa & ~0xfffULL, (u64)f.size + 0x2000);
	}
	u64 phys_base, win_size;
	if (!choose_window(&phys_base, &win_size)) {
		fail("no stretch of RAM big enough");
	}
	print("kernel window %x..%x (%d MB)\n", phys_base, phys_base + win_size, win_size >> 20);

	/* --- the kernel ---------------------------------------------------- */
	const struct mach_header_64 *mh = (const struct mach_header_64 *)pl.kernel;
	if (pl.kernel_size < sizeof(*mh) || mh->magic != MH_MAGIC_64 || mh->sizeofcmds > pl.kernel_size - sizeof(*mh)) {
		fail("the kernel is not a 64-bit Mach-O");
	}
	u64 vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (u32 i = 0; i < mh->ncmds; i++, lc = (const void *)((const u8 *)lc + lc->cmdsize)) {
		if (lc->cmdsize < sizeof(*lc)) {
			fail("a load command of size 0");
		}
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->filesize > sg->vmsize || sg->fileoff + sg->filesize > pl.kernel_size) {
				fail("a segment outside the kernel file");
			}
			vm_lo = sg->vmaddr < vm_lo ? sg->vmaddr : vm_lo;
			vm_hi = sg->vmaddr + sg->vmsize > vm_hi ? sg->vmaddr + sg->vmsize : vm_hi;
		} else if (lc->cmd == LC_UNIXTHREAD && lc->cmdsize >= 16 + 33 * 8) {
			entry_va = ((const u64 *)((const u8 *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0 || vm_hi - KERNEL_VIRT_BASE > win_size) {
		fail("unexpected kernel layout");
	}
	u64 slide = phys_base - KERNEL_VIRT_BASE;
	u64 kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	u64 rd_pa = kern_hi, rd_size = (pl.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	u64 rd1_pa = rd_pa + rd_size, rd1_size = (pl.ramdisk1_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	u64 dt_pa = rd1_pa + rd1_size, dt_max = 0x40000, ba_pa = dt_pa + dt_max;
	u64 pram_pa = ba_pa + PAGE_SIZE_MAX, top = pram_pa + PANIC_LOG_SIZE;
	if (top - phys_base + 0x4000000ULL > win_size) {
		fail("the kernel and its RAM disk do not fit");
	}
	lc = (const struct load_command *)(mh + 1);
	for (u32 i = 0; i < mh->ncmds; i++, lc = (const void *)((const u8 *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), pl.kernel + sg->fileoff, sg->filesize);
	}
	print("kernel at %x..%x\n", kern_lo, kern_hi);
	if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pl.ramdisk, pl.ramdisk_size);
		print("RAM disk at %x, %x bytes\n", rd_pa, pl.ramdisk_size);
	}
	if (rd1_size != 0) {
		memset((void *)rd1_pa, 0, rd1_size);
		memcpy((void *)rd1_pa, pl.ramdisk1, pl.ramdisk1_size);
		print("second RAM disk at %x, %x bytes\n", rd1_pa, pl.ramdisk1_size);
	}
	memset((void *)pram_pa, 0, PANIC_LOG_SIZE);

	for (u64 *q = (u64 *)kern_lo; q < (u64 *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC && live_pa == NULL) {
			live_pa = (struct kernel_live *)q;
		}
		if (*q == RETURN_SLOT_MAGIC) {
			q[1] = (u64)loader_return_stub;
			q[2] = 0;
			q[3] = slide;
			print("return slot at %x\n", (u64)q);
		}
	}
	live_slide = slide;
	print("kernel's signs of life at %x\n", (u64)live_pa);

	/* --- boot_args ----------------------------------------------------- */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	if (fb_base != 0) {
		ba->video.v_baseAddr = fb_base;
		ba->video.v_display = cmdline_has(pl.cmdline, "-v") ? 0 : 1;
		ba->video.v_rowBytes = fb_stride;
		ba->video.v_width = fb_width;
		ba->video.v_height = fb_height;
		ba->video.v_depth = 32;
	}
	struct line l = { ba->commandLine, 0 };
	line_add(&l, P->uart != 0 && !cmdline_has(pl.cmdline, "serial=") ? "debug=0x8 serial=3 " : "debug=0x8 ");
	if (!cmdline_has(pl.cmdline, "cpus=") && !cmdline_has(pl.cmdline, "cpumask=")) {
		line_add(&l, "cpus=1 ");
	}
	if (rd_size != 0 && !cmdline_has(pl.cmdline, "rd=")) {
		line_add(&l, "rd=md0 ");
	}
	if (pl.version != NULL && str_len(pl.version) < 64) {
		line_add(&l, "hakios_kver=");
		line_add(&l, pl.version);
		line_add(&l, " ");
	}
	line_add(&l, pl.cmdline);
	print("cmdline: %s\n", ba->commandLine);

	/* --- device tree --------------------------------------------------- */
	struct dt_writer w = { .base = (u8 *)dt_pa, .cur = (u8 *)dt_pa, .end = (u8 *)(dt_pa + dt_max) };
	u64 cntfrq = read_cntfrq();
	u8 seed[64];
	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (u8)(read_cntpct() >> (i & 7)) ^ (u8)(i * 37);
	}
	dt_begin(&w, "device-tree");
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8011");
	dt_prop_padded(&w, "serial-number", "TWL00MIBOXS3", 32);
	dt_prop_padded(&w, "mlb-serial-number", "TWL00MIBOXS3MLB00", 32);
	dt_prop_padded(&w, "model-number", "MP7P2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1842", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	dt_prop_u32(&w, "display-scale", 1);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", psci_hvc ? "hvc" : "smc");
	dt_prop_u64(&w, "psci-el2-trampoline", (u64)secondary_trampoline);
	static const u8 wifi_address[6] = { 0x02, 0x54, 0x57, 0x4c, 0x00, 0x01 };
	static const u8 bluetooth_address[6] = { 0x02, 0x54, 0x57, 0x4c, 0x00, 0x02 };
	static const u8 ethernet_address[6] = { 0x02, 0x54, 0x57, 0x4c, 0x00, 0x03 };
	static const u8 manifest_hash[48] = { 0x74, 0x77, 0x69, 0x6c, 0x69, 0x67, 0x68, 0x74 };
	static const u8 no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x0000054574C0001ULL);
	dt_prop_u32(&w, "chip-id", 0x8011);
	dt_prop_u32(&w, "board-id", 0x02);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "mac-address-ethernet0", ethernet_address, sizeof(ethernet_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.120.15");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	static u8 nvram_image[NVRAM_BANK_SIZE] __attribute__((aligned(8)));
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	if (rd_size != 0) {
		u64 disk[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk", disk, sizeof(disk));
	}
	if (rd1_size != 0) {
		u64 disk1[2] = { rd1_pa, rd1_size };
		dt_prop(&w, "RAMDisk1", disk1, sizeof(disk1));
	}
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_end(&w);

	dt_begin(&w, "options");
	dt_end(&w);

	u64 pram_reg[2] = { pram_pa, PANIC_LOG_SIZE };
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/* the cores: this one first, then the others but the monitor's */
	u64 self = read_mpidr() & 0xffffff;
	int ncpus = 0;
	dt_begin(&w, "cpus");
	for (int pass = 0; pass < 2; pass++) {
		for (int core = 0; core < P->cores; core++) {
			u64 mpidr = (u64)core << P->core_shift;
			char name[5] = { 'c', 'p', 'u', (char)('0' + ncpus), 0 };

			if ((pass == 0) != (mpidr == self) || (P->monitor && core == P->cores - 1)) {
				continue;
			}
			dt_begin(&w, name);
			dt_prop_str(&w, "device_type", "cpu");
			dt_prop_str(&w, "state", ncpus == 0 ? "running" : "waiting");
			dt_prop_u32(&w, "reg", (u32)mpidr);
			dt_prop_u32(&w, "cpu-id", (u32)ncpus);
			dt_prop_u64(&w, "timebase-frequency", cntfrq);
			dt_prop_u64(&w, "fixed-frequency", cntfrq);
			dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
			dt_prop_u64(&w, "bus-frequency", 100000000ULL);
			dt_prop_u64(&w, "memory-frequency", 100000000ULL);
			dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
			dt_end(&w);
			ncpus++;
		}
	}
	dt_end(&w);

	u64 ranges[3] = { 0, P->io_base, P->io_size };
	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", P->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-400");
	u64 gic_reg[4] = { P->gicd - P->io_base, P->gicd_size, P->gicc - P->io_base, P->gicc_size };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);
	if (P->uart != 0) {
		u64 reg2[2] = { P->uart - P->io_base, 0x1000 };
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);
	dt_end(&w);
	if (w.overflow) {
		fail("the device tree does not fit");
	}
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = (u32)(w.cur - w.base);
	print("device tree %x (%x bytes), boot_args %x, top %x, %d core(s) for the kernel\n", dt_pa,
	    (u64)ba->deviceTreeLength, ba_pa, top, (u64)ncpus);

	MB->phys_base = phys_base;
	MB->mem_size = win_size;
	MB->slide = slide;
	MB->kernel_live_pa = (u64)live_pa;
	MB->hang_seconds = cmdline_number(pl.cmdline, "twl_hang=", 60);
	MB->shot_every = cmdline_number(pl.cmdline, "twl_shot=", 15);
	MB->shot_from = cmdline_number(pl.cmdline, "twl_shot_from=", 0);
	if (P->monitor && !cmdline_has(pl.cmdline, "twl_monitor=0")) {
		start_monitor();
	}
	print("jumping to %x\n", entry_va + slide);
	MB->t_kernel = read_cntpct();
	MB->state = ST_KERNEL;
	enter_kernel(entry_va + slide, ba_pa);
}
