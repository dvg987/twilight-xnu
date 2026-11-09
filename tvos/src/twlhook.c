/*
 * libtwl.bbd.dylib: inside tvOS's backboardd, what the window server decides about each frame.
 * Virtual table entries of CA::WindowServer's display and server classes are replaced by loggers that
 * call the originals. Addresses are tvOS 16.5 (20L563) AppleTV6,2's, unslid; each is checked first.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <libkern/OSCacheControl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

extern intptr_t _dyld_get_image_slide(const struct mach_header *mh);
extern const void *_dyld_get_shared_cache_range(size_t *length);
extern int _dyld_get_shared_cache_uuid(unsigned char uuid[16]);

static uintptr_t slide;

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
	{
		/* (stderr reaches the kernel log, which the GPU's messages soon fill: a copy for ssh) */
		static int fd = -2;

		if (fd == -2) {
			fd = open("/var/tmp/twl-bbd.log", O_WRONLY | O_APPEND | O_CREAT, 0644);
		}
		if (fd >= 0) {
			write(fd, line, (size_t)n);
		}
	}
}

static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	void *original;

	if (*entry != expected + slide) {
		say("twl: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("twl: %s: table not writable\n", what);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/*
 * Whether a layer goes to the display as one of its own (a video layer to the video plane) or into the frame by the
 * GPU: AppleRGBOutDisplay::detachable_layer_impl's answer (0: it may go; else a kDisplayDetachReasonCode), logged
 * when it changes and for layers wider than the frame (a 1440p or 4K video).
 */
static int (*detachable_o)(void *, unsigned *, double *, void *, const double *, const double *, unsigned);
typedef struct __IOSurface *IOSurfaceRef;
static IOSurfaceRef surface_ref(void *surface);
extern size_t IOSurfaceGetPlaneCount(IOSurfaceRef);

/*
 * Reason bit 0x80: the display cannot scale the layer itself, so the window server is to scale a copy first with the
 * memory-to-memory scaler (the M2 scaler, IOSurfaceAccelerator), which this machine has not got: the copy failed
 * (kServerDetachReasonCodeSurfaceCopyFailed), the video went to the GPU and showed black in YouTube's small settings
 * window. A video surface (planar YUV) goes to the video plane, whose scaler takes any of these sizes: never asked.
 */
#define DETACH_NEEDS_SCALER     0x80u
/*
 * detachable_layer_impl's answers 0xb, "invalid pixel format" (a PQ video in an 8-bit '420v' surface, which Apple's
 * hardware would get in a 10-bit one), and 0xf, "color matrix not supported" (BT.2020's). Our decoder's pictures carry
 * the video plane's token, and the plane shows the decoder's own compressed picture, not the surface, converting the
 * matrix itself: such a layer may go. Refused, it was drawn on the GPU (green, then a GPU fault that took the window
 * server down).
 */
#define DETACH_INVALID_FORMAT   0xb
#define DETACH_COLOR_MATRIX     0xf     /* kDisplayDetachReasonCodeColorMatrixNotSupported: BT.2020's, the plane converts it */
extern CFTypeRef IOSurfaceCopyValue(IOSurfaceRef, CFStringRef);
extern uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef);
extern uint32_t IOSurfaceGetID(IOSurfaceRef);
static void say(const char *format, ...);

static int
my_detachable(void *d, unsigned *reason, double *scale, void *surface, const double *src, const double *dst, unsigned flags)
{
	static int last = -1;
	static unsigned told;
	int r = detachable_o(d, reason, scale, surface, src, dst, flags);
	IOSurfaceRef io;

	if ((r == DETACH_INVALID_FORMAT || r == DETACH_COLOR_MATRIX) && surface) {
		static unsigned told_format;
		CFTypeRef token = NULL;
		/* the answer per surface (pointer and ID), remembered: asking the kernel was 30% of the display thread at 4K60 */
		static struct { IOSurfaceRef io; uint32_t id; bool token; } known[32];
		static unsigned next_known;
		bool cached = false, has = false;
		io = surface_ref(surface);
		if (io != NULL && IOSurfaceGetPlaneCount(io) >= 2) {
			uint32_t id = IOSurfaceGetID(io);
			for (unsigned k = 0; k < 32; k++)
				if (known[k].io == io && known[k].id == id) { cached = true; has = known[k].token; break; }
			if (!cached) {
				token = IOSurfaceCopyValue(io, CFSTR("TwlVideoPlanePicture"));
				has = token != NULL;
				known[next_known % 32].io = io;
				known[next_known % 32].id = id;
				known[next_known % 32].token = has;
				next_known++;
			}
		}
		if (told_format++ < 8) {
			say("twl: detachable: refused (%d): surface %p, IOSurface %u '%c%c%c%c' planes %zu, token %s\n", r, surface,
			    io ? IOSurfaceGetID(io) : 0, io ? (int)(IOSurfaceGetPixelFormat(io) >> 24) : '?',
			    io ? (int)(IOSurfaceGetPixelFormat(io) >> 16 & 0xff) : '?', io ? (int)(IOSurfaceGetPixelFormat(io) >> 8 & 0xff) : '?',
			    io ? (int)(IOSurfaceGetPixelFormat(io) & 0xff) : '?', io ? IOSurfaceGetPlaneCount(io) : 0, has ? "yes" : "no");
		}
		if (token != NULL) CFRelease(token);
		if (has) r = 0;
	}

	if (r == 0 && reason && (*reason & DETACH_NEEDS_SCALER) && surface && (io = surface_ref(surface)) != NULL &&
	    IOSurfaceGetPlaneCount(io) >= 2) {
		*reason &= ~DETACH_NEEDS_SCALER;
	}

	if ((r != last || (src && src[2] > 1920)) && told < 200) {
		told++;
		say("twl: detachable_layer_impl -> %d (reason %u, scale %.3f x %.3f) src %.0f,%.0f %.0fx%.0f dst %.0f,%.0f %.0fx%.0f "
		    "flags %#x\n", r, reason ? *reason : 0, scale ? scale[0] : 0, scale ? scale[1] : 0, src ? src[0] : 0,
		    src ? src[1] : 0, src ? src[2] : 0, src ? src[3] : 0, dst ? dst[0] : 0, dst ? dst[1] : 0, dst ? dst[2] : 0,
		    dst ? dst[3] : 0, flags);
		last = r;
	}
	return r;
}


/*
 * new_metal_context sets MetalContext flag 0x800 (framebuffer fetch) without asking the device. Boot-arg ca_fetch=0 clears it, for
 * measurements only: CA then never draws the textures its backdrop blurs read, and they show as unwritten (magenta).
 */
static void
fetch_off(void)
{
	static const char key[] = "ca_fetch=0";
	char args[1024] = "";
	size_t length = sizeof(args) - 1;
	uint32_t *insn = (uint32_t *)(0x189afd240UL + slide);
	vm_address_t page = (vm_address_t)insn & ~(vm_address_t)(vm_page_size - 1);

	if (sysctlbyname("kern.bootargs", args, &length, NULL, 0) != 0 || strstr(args, key) == NULL) {
		return;
	}
	if (*insn != 0x52850009) {      /* mov w9, #0x2800 */
		say("twl: new_metal_context: found %#x; framebuffer fetch left on\n", *insn);
		return;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("twl: new_metal_context: text not writable\n");
		return;
	}
	*insn = 0x52840009;             /* mov w9, #0x2000 */
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(insn, sizeof(*insn));
	say("twl: framebuffer fetch off\n");
}

/*
 * fb_flip=1: the display scans the surface a frame was drawn in, so IOMFBDisplay::finish_update's copy of it to the surface the swap names (the
 * M2 scaler's job, a full-screen pass of the GPU here) is for nothing: both its calls of copy_surface answer "done" instead.
 */
static void
scanout_copy_off(void)
{
	static const uintptr_t calls[] = { 0x189be6da4UL, 0x189be7734UL };
	char args[1024] = "";
	size_t length = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &length, NULL, 0) != 0 || strstr(args, "fb_flip=1") == NULL) {
		return;
	}
	for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
		uint32_t *insn = (uint32_t *)(calls[i] + slide);
		uint32_t bl = 0x94000000u | (uint32_t)(((0x189bf6ef8UL - calls[i]) >> 2) & 0x3ffffff);
		vm_address_t page = (vm_address_t)insn & ~(vm_address_t)(vm_page_size - 1);

		if (*insn != bl) {
			say("twl: finish_update: found %#x at %#lx, expected %#x; copy left on\n", *insn, calls[i], bl);
			continue;
		}
		if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
			say("twl: finish_update: text not writable\n");
			continue;
		}
		*insn = 0x52800020;             /* mov w0, #1 */
		vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
		sys_icache_invalidate(insn, sizeof(*insn));
		say("twl: finish_update: copy %u to the scanout surface off\n", i);
	}
}

/*
 * CA::Render::ImageQueue::forward_dm turns a PQ or HLG video's pictures into HDRProcessing's 'rg3h' surfaces on the GPU
 * (display management for Apple's display pipe): green and slow on the Mali, never detachable, and the window server's
 * GPU faulted drawing them. The video plane shows the decoder's own picture and the HDMI output carries PQ itself, so
 * the queue's pictures are used as they are: the branch taken when HDRProcessing is absent, always. (Its flag is not
 * cleared instead: it also says the display can show HDR, and tvOS then left HDR modes out.)
 */
static void
forward_dm_off(void)
{
	uint32_t *insn = (uint32_t *)(0x1899e1638UL + slide);
	vm_address_t page = (vm_address_t)insn & ~(vm_address_t)(vm_page_size - 1);

	if (*insn != 0x34005b48u) {             /* cbz w8, the return */
		say("twl: forward_dm: found %#x, left on\n", *insn);
		return;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("twl: forward_dm: text not writable\n");
		return;
	}
	*insn = 0x140002dau;                    /* b, the return */
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(insn, sizeof(*insn));
	say("twl: HDR video's display management (forward_dm) off\n");
}

#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

/*
 * A TV out's display (boot-arg fb_external): CoreAnimation sets its canvas, the size it draws, to the mode's
 * (Display::set_size(mode, mode) in update_framebuffer_locked), 3840x2160 in 2160p: four times the pixels and frames
 * too big for the display to scan. Here the canvas is 1920x1080 in every mode, the physical size stays the mode's,
 * so the swap names the 1920x1080 frame onto the whole timing and the VPP scales it (the kernel takes that as a
 * frame to scan). Its RGB layers may then be scaled down too (720p): the smallest scale, 1.0 by default, is 0.5.
 * set_size posts the change; the next mode change sets the mode's size again, so it is looked at every frame.
 */
#define DISPLAY_PHYSICAL        0x50    /* Vec2<int> */
#define DISPLAY_CANVAS          0x58    /* Vec2<int> */
#define DISPLAY_SCALE           0x68    /* Vec2<double> */
#define DISPLAY_RGB_MIN_SCALE   0x8e8   /* double (AppleDisplay::supports_scale_p) */

static int tv_out;

/* the kernel's display is a TV out (kern.fb_external, MiniIOMobileFramebuffer: the default) */
static int
tv_out_display(void)
{
	uint32_t external = 0;
	size_t size = sizeof(external);

	return sysctlbyname("kern.fb_external", &external, &size, NULL, 0) == 0 && external != 0;
}
static void *modes_again;
static void (*display_update_geometry)(void *);
static void my_set_size(void *d, const int *physical, const int *canvas, const double *scale);

/*
 * -[CADisplayMode preferredScale] is 2 for a mode taller than 1080 lines: with the 1920x1080 canvas the UI would be laid
 * out in 960x540 points. A TV out's modes are scale 1 here (and in PineBoard, tvos/src/twlpb.c, which gives the apps theirs).
 */
static unsigned long (*mode_scale_o)(void *, SEL);

static unsigned long
mode_scale(void *self, SEL sel)
{
	unsigned long r = mode_scale_o(self, sel);

	if (tv_out && r > 1 && ((unsigned long (*)(void *, SEL))objc_msgSend)(self, sel_registerName("height")) > 1080) {
		return 1;
	}
	return r;
}

static void
canvas_init(void)
{
	char args[1024] = "";
	size_t length = sizeof(args) - 1;
	const char *e;
	uint32_t *set_size = (uint32_t *)(0x189a664acUL + slide);
	vm_address_t page = (vm_address_t)set_size & ~(vm_address_t)(vm_page_size - 1);
	Class cls = objc_getClass("CADisplayMode");
	Method m = cls == NULL ? NULL : class_getInstanceMethod(cls, sel_registerName("preferredScale"));

	(void)args, (void)length, (void)e;
	if (!tv_out_display() || access("/var/mobile/twl/nocanvas", F_OK) == 0) {
		return;
	}
	if (set_size[0] != 0xb9405008 || set_size[1] != 0xb9400029) {        /* ldr w8, [x0, #0x50]; ldr w9, [x1] */
		say("twl: Display::set_size: found %#x %#x; canvas left to CoreAnimation\n", set_size[0], set_size[1]);
		return;
	}
	if (m == NULL || (uintptr_t)method_getImplementation(m) != 0x1899ce110UL + slide) {
		say("twl: -[CADisplayMode preferredScale] not where expected; canvas left to CoreAnimation\n");
		return;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("twl: Display::set_size: text not writable; canvas left to CoreAnimation\n");
		return;
	}
	display_update_geometry = (void (*)(void *))(0x189a6652cUL + slide);
	set_size[0] = 0x58000050;       /* ldr x16, #8 */
	set_size[1] = 0xd61f0200;       /* br x16 */
	*(uint64_t *)(void *)&set_size[2] = (uint64_t)(uintptr_t)my_set_size;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(set_size, 16);
	mode_scale_o = (unsigned long (*)(void *, SEL))method_setImplementation(m, (IMP)mode_scale);
	/*
	 * Once Home is up, the window server takes the display's size from the root layer of the home screen's context (its
	 * "logical bounds"): the mode's pixels, 3840x2160, and the frames were 4K again. The canvas is the size here.
	 */
	setenv("CA_DISABLE_LOGICAL_BOUNDS", "1", 1);
	/*
	 * A TV out's frames are wide colour, 'b3a8' (BGR10A2) and the like, which the Metal plug-in does not draw into: the
	 * screen was solid red. IOMFBDisplay::framebuffer_pixel_format answers 'BGRA' here, as for the panel.
	 */
	{
		uint32_t *fpf = (uint32_t *)(0x189bf7578UL + slide);
		vm_address_t fpage = (vm_address_t)fpf & ~(vm_address_t)(vm_page_size - 1);

		if (fpf[0] == 0xa9bd57f6 && fpf[1] == 0xa9014ff4 && fpf[2] == 0xa9027bfd &&
		    vm_protect(mach_task_self(), fpage, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS) {
			fpf[0] = 0x528a4820;    /* mov w0, #0x5241 */
			fpf[1] = 0x72a848e0;    /* movk w0, #0x4247, lsl #16: 'BGRA' */
			fpf[2] = 0xd65f03c0;    /* ret */
			vm_protect(mach_task_self(), fpage, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
			sys_icache_invalidate(fpf, 12);
			say("twl: TV out: framebuffer format BGRA\n");
		} else {
			say("twl: framebuffer_pixel_format: found %#x %#x %#x; format left to CoreAnimation\n", fpf[0], fpf[1], fpf[2]);
		}
	}
	tv_out = 1;
	say("twl: TV out: canvas 1920x1080, modes at scale 1\n");
}

static void
keep_canvas(void *d)
{
	double *rgb_min = (double *)((char *)d + DISPLAY_RGB_MIN_SCALE);

	if (tv_out && *rgb_min > 0.5) {
		*rgb_min = 0.5;
	}
}

/*
 * Display::set_size(physical, canvas, scale), CoreAnimation's own, with the canvas 1920x1080 for the TV out's display (an
 * AppleRGBOutDisplay); set_size's first instructions jump here. (Forcing the canvas from outside, between frames, stopped
 * the window server drawing.)
 */
#define RGBOUT_VTABLE           0x1cf195a70UL   /* (its object's first word) */

static void
my_set_size(void *d, const int *physical, const int *canvas, const double *scale)
{
	int *p = (int *)((char *)d + DISPLAY_PHYSICAL), *c = (int *)((char *)d + DISPLAY_CANVAS);
	double *k = (double *)((char *)d + DISPLAY_SCALE);
	int want[2] = { canvas[0], canvas[1] }, phys[2] = { physical[0], physical[1] };
	static unsigned told;

	/*
	 * Physical size 1920x1080 too: with a canvas smaller than the display, the window server draws into an intermediate and
	 * scales it into a scan-out surface of the timing's size, a 4K pass of the GPU every frame. At 1920x1080 it draws
	 * straight into frames the display scans, as on the panel, and the VPP scales them to the timing.
	 */
	if (*(uintptr_t *)d == RGBOUT_VTABLE + slide && physical[0] > 0 && physical[1] > 0) {
		want[0] = phys[0] = 1920;
		want[1] = phys[1] = 1080;
	}
	/*
	 * Flag bit 37 (IOMFBDisplay +0x802): without it update_digital_modes drops the HDR10 4:2:2 and 4:4:4 colour elements and
	 * keeps only 4:2:0 for HDR, which this box's 4:2:0 output shows blocky. (Its other use, a deep framebuffer format, is
	 * answered 'BGRA' by the patch in canvas_init.)
	 */
	if (*(uintptr_t *)d == RGBOUT_VTABLE + slide) {
		uint64_t flags;

		memcpy(&flags, (char *)d + 0x802, sizeof(flags));
		if (!(flags & (1ULL << 37))) {
			flags |= 1ULL << 37;
			memcpy((char *)d + 0x802, &flags, sizeof(flags));
			say("twl: TV out: HDR 4:2:2 / 4:4:4 modes allowed (flags %#llx)\n", (unsigned long long)flags);
		}
		/* the modes were read when the display opened, before the bit: read again once, off this thread (it takes the display's lock) */
		if (physical[0] > 0 && !modes_again) {
			modes_again = d;
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{
				void (**vt)(void *, int) = *(void (***)(void *, int))modes_again;
				uint64_t f;

				memcpy(&f, (char *)modes_again + 0x802, sizeof(f));
				if (!(f & (1ULL << 37))) {
					f |= 1ULL << 37;
					memcpy((char *)modes_again + 0x802, &f, sizeof(f));
				}
				if ((uintptr_t)vt[0x318 / 8] == 0x189bee7b0UL + slide) {
					say("twl: TV out: display modes read again\n");
					vt[0x318 / 8](modes_again, 1);
				}
			});
		}
	}
	physical = phys;
	if (told < 24) {
		told++;
		say("twl: Display::set_size(%dx%d, %dx%d, %g) of %#lx: canvas %dx%d\n", physical[0], physical[1], canvas[0], canvas[1],
		    scale[0], *(uintptr_t *)d - slide, want[0], want[1]);
	}
	if (p[0] == physical[0] && p[1] == physical[1] && c[0] == want[0] && c[1] == want[1] && k[0] == scale[0] && k[1] == scale[1]) {
		return;
	}
	p[0] = physical[0];
	p[1] = physical[1];
	c[0] = want[0];
	c[1] = want[1];
	k[0] = scale[0];
	k[1] = scale[1];
	display_update_geometry(d);
}

static void note_damage(void *update);

/* one set of loggers per display class: which class the display is, is part of the answer */
#define DISPLAY_HOOKS(cls)                                                                                      \
static int (*cls##_is_ready_o)(void *);                                                                         \
static int (*cls##_ignore_o)(void *, void *);                                                                   \
static uintptr_t (*cls##_begin_o)(void *, void *);                                                              \
static uintptr_t (*cls##_finish_o)(void *, void *, unsigned);                                                   \
static unsigned cls##_n[4];                                                                                     \
static int cls##_last[4] = { -2, -2, -2, -2 };                                                                  \
static int cls##_is_ready(void *d) {                                                                            \
	keep_canvas(d);                                                                                         \
	int r = cls##_is_ready_o(d);                                                                            \
	if (r != cls##_last[0] || cls##_n[0] % 500 == 0)                                                        \
		say("twl: " #cls "::is_ready -> %d (call %u, flags %#llx)\n", r, cls##_n[0],                    \
		    (unsigned long long)*(uint64_t *)((char *)d + 0x802));                                      \
	cls##_last[0] = r; cls##_n[0]++; return r; }                                                            \
static int cls##_ignore(void *d, void *u) {                                                                     \
	int r = cls##_ignore_o(d, u) & 1;                                                                       \
	if (r != cls##_last[1] || cls##_n[1] % 500 == 0)                                                        \
		say("twl: " #cls "::ignore_update_p -> %d (call %u, flags %#llx, +25b %#x, update +e8 %#x)\n", r,   \
		    cls##_n[1], (unsigned long long)*(uint64_t *)((char *)d + 0x802), *((uint8_t *)d + 0x25b),  \
		    *(uint32_t *)((char *)u + 0xe8));                                                           \
	cls##_last[1] = r; cls##_n[1]++; return r; }                                                            \
static uintptr_t cls##_begin(void *d, void *u) {                                                                \
	uintptr_t r = cls##_begin_o(d, u);                                                                      \
	if ((int)(r != 0) != cls##_last[2] || cls##_n[2] % 500 == 0)                                            \
		say("twl: " #cls "::begin_update -> %#lx (call %u)\n", r, cls##_n[2]);                          \
	cls##_last[2] = r != 0; cls##_n[2]++; return r; }                                                       \
static void transfer_to_scanout(void);                                                                          \
static uintptr_t cls##_finish(void *d, void *u, unsigned f) {                                                   \
	note_damage(u);                                                                                         \
	transfer_to_scanout();                                                                                  \
	uintptr_t r = cls##_finish_o(d, u, f);                                                                  \
	if ((int)f != cls##_last[3] || cls##_n[3] % 500 == 0)                                                   \
		say("twl: " #cls "::finish_update(%#x) -> %#lx (call %u)\n", f, r, cls##_n[3]);                 \
	cls##_last[3] = (int)f; cls##_n[3]++; return r; }                                                       \
static void cls##_install(uintptr_t vt) {                                                                       \
	cls##_is_ready_o = replace_virtual(#cls " is_ready", VSLOT(vt, 0xb0), 0x189be4774UL, cls##_is_ready);   \
	cls##_ignore_o = replace_virtual(#cls " ignore_update_p", VSLOT(vt, 0x138), 0x189be562cUL, cls##_ignore); \
	cls##_begin_o = replace_virtual(#cls " begin_update", VSLOT(vt, 0x170), 0x189be5a68UL, cls##_begin);    \
	cls##_finish_o = replace_virtual(#cls " finish_update", VSLOT(vt, 0x178), 0x189be614cUL, cls##_finish); }

DISPLAY_HOOKS(AppleDisplay)
DISPLAY_HOOKS(AppleCLCDDisplay)
DISPLAY_HOOKS(AppleRGBOutDisplay)

static uintptr_t (*render_update_o)(void *, void *, unsigned *);
static void *(*renderer_o)(void *);
static uintptr_t (*render_surface_o)(void *, void *, void *, void *, int);
static unsigned n_render, n_surface, n_renderer;
static uint64_t pace_wait, pace_copy;  /* this window's time in copy_refs, ns */

/*
 * Frame pacing: every 60 frames that follow another within 250 ms (something is moving), how far apart they were and where the
 * time went: render_update in all, and of that the wait for the GPU and the copies.
 */
static void
pace(uint64_t start, uint64_t end)
{
	static const unsigned edge[] = { 20, 36, 53, 70, 105, 250 };
	static uint64_t last, render, gaps;
	static unsigned bucket[6], frames;
	uint64_t gap = (start - last) / 1000000;

	last = start;
	if (gap >= 250) {
		pace_wait = pace_copy = 0;
		return;
	}
	for (int i = 0; i < 6; i++) {
		if (gap < edge[i]) {
			bucket[i]++;
			break;
		}
	}
	gaps += gap;
	render += end - start;
	if (++frames == 60) {
		say("twl: pace n=60 gap %llu ms <20:%u <36:%u <53:%u <70:%u <105:%u <250:%u render %llu us wait %llu us copy %llu us\n", gaps / 60,
		    bucket[0], bucket[1], bucket[2], bucket[3], bucket[4], bucket[5], render / 60000, pace_wait / 60000, pace_copy / 60000);
		memset(bucket, 0, sizeof(bucket));
		frames = 0;
		gaps = render = pace_wait = pace_copy = 0;
	}
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	uintptr_t r = render_update_o(server, update, flags);

	pace(start, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
	if (n_render++ % 200 == 0 || n_render < 8) {
		say("twl: Server::render_update -> %#lx flags %#x (call %u)\n", r, flags ? *flags : 0, n_render);
	}
	return r;
}

static void *
my_renderer(void *server)
{
	void *r = renderer_o(server);

	if (n_renderer++ < 4) {
		say("twl: Server::renderer -> %p\n", r);
	}
	return r;
}

static uintptr_t
my_render_surface(void *server, void *update, void *surface, void *bounds, int b)
{
	uintptr_t r = render_surface_o(server, update, surface, bounds, b);

	if (n_surface++ % 200 == 0 || n_surface < 8) {
		say("twl: Server::render_surface(surface %p) -> %#lx (call %u)\n", surface, r, n_surface);
	}
	return r;
}

static void *(*current_surface_o)(void *, int, uint64_t, int);
static unsigned n_current;

static void *
my_current_surface(void *server, int a, uint64_t b, int c)
{
	void *r = current_surface_o(server, a, b, c);

	if (n_current++ % 200 == 0 || n_current < 8) {
		say("twl: IOMFBServer::current_surface(%d, %#llx, %d) -> %p (call %u)\n", a, (unsigned long long)b, c, r, n_current);
	}
	return r;
}

/* the surface a frame is drawn into, and the software context's acceptance of it */
static void *(*update_surface_o)(void *, int, int, uint64_t, int);
static int (*make_buffer_current_o)(void *, void *);
static unsigned n_us, n_mbc;

/*
 * CoreAnimation draws only what changed into the buffer of a frame, and before that has the frame on the
 * screen copied into it -- by the IOSurfaceAccelerator (the M2 scaler), which this machine has not got: the
 * copy fails without a word and a new buffer stays empty. So the copy is done here, by the processor, each
 * time the window server goes on to another buffer: what was shown last, into the one about to be drawn.
 */
typedef struct __IOSurface *IOSurfaceRef;
static void *last_surface;
extern int IOSurfaceLock(IOSurfaceRef, uint32_t, uint32_t *);
extern int IOSurfaceUnlock(IOSurfaceRef, uint32_t, uint32_t *);
extern void *IOSurfaceGetBaseAddress(IOSurfaceRef);
extern size_t IOSurfaceGetBytesPerRow(IOSurfaceRef);
extern size_t IOSurfaceGetHeight(IOSurfaceRef);
extern size_t IOSurfaceGetWidth(IOSurfaceRef);
extern uint32_t IOSurfaceGetID(IOSurfaceRef);

static IOSurfaceRef
surface_ref(void *surface)
{
	/* CA::WindowServer::IOSurface::iosurface(), virtual slot 0x98 */
	void **vt = *(void ***)surface;

	if ((uintptr_t)vt[0x98 / 8] != 0x189b152b8UL + slide) {
		return NULL;
	}
	return ((IOSurfaceRef (*)(void *))vt[0x98 / 8])(surface);
}

static unsigned copies;
extern IOSurfaceRef IOSurfaceLookup(uint32_t);
extern void CFRelease(const void *);

/*
 * With the GPU drawing (the Mali's Metal plug-in), the frame just handed over may still be in flight: the copy waits for the work
 * committed until now (the plug-in's native_bridge_metal_wait_committed; absent, as with the software renderer, nothing to wait for).
 */
static void
wait_for_gpu(void)
{
	static void (*wait)(uint64_t);
	static int looked;

	if (!looked) {
		looked = 1;
		wait = (void (*)(uint64_t))dlsym(RTLD_DEFAULT, "native_bridge_metal_wait_committed");
		say("twl: GPU frames: %s\n", wait ? "the copies wait for the Metal plug-in's work" : "no Metal plug-in, nothing to wait for");
	}
	if (wait != NULL) {
		wait(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000);
	}
}

static int (*flip_wait)(uint32_t);

/* What the processor wrote, out to memory: with fb_flip the display reads the surface, and nothing else may clean it. */
static void
clean_to_memory(char *start, size_t length)
{
	uint64_t ctr;
	size_t line;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	line = 4u << ((ctr >> 16) & 0xf);
	for (uintptr_t p = (uintptr_t)start & ~(uintptr_t)(line - 1); p < (uintptr_t)start + length; p += line) {
		__asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
	}
	__asm__ volatile("dsb sy" : : : "memory");
}

/*
 * The frame's damage (CA::Render::Update's shape, at +0x10; CA::Shape::get_bounds), which CoreAnimation draws again in
 * full: the copy back into the frame's surface leaves it out. Video on the plane damages the whole frame on every one of
 * its pictures, and the copy (8 MB, ~3 ms of backboardd's display thread, a fifth of its time) was for nothing.
 * /var/mobile/twl/copy_damage, looked at now and then: 0 copies everything (as before), 1 (the default) skips the copy when
 * the damage covers the frame, 2 also copies only what lies outside a partial damage.
 */
struct ca_bounds {
	int x, y, w, h;
	const void *shape;      /* the CA::Shape the bounds are of (a shape may be several rectangles) */
};

static struct ca_bounds frame_damage;
static int frame_damage_known, copy_damage = 1;
static bool (*shape_contains)(const void *, const struct ca_bounds *);        /* CA::Shape::contains(Bounds const&) */
static long (*shape_rects)(const void *);                                     /* CA::Shape::rect_count() */

static void
note_damage(void *update)
{
	static void (*get_bounds)(const void *, struct ca_bounds *);
	static unsigned frames;
	void *shape;

	if (get_bounds == NULL) {
		get_bounds = (void (*)(const void *, struct ca_bounds *))(0x189b82b98UL + slide);    /* CA::Shape::get_bounds */
		shape_contains = (bool (*)(const void *, const struct ca_bounds *))(0x189b836a4UL + slide);
		shape_rects = (long (*)(const void *))(0x189b84858UL + slide);
	}
	if (frames++ % 120 == 0) {
		int fd = open("/var/mobile/twl/copy_damage", O_RDONLY);
		char c = '1';

		if (fd >= 0) {
			if (read(fd, &c, 1) != 1) {
				c = '1';
			}
			close(fd);
		}
		copy_damage = c >= '0' && c <= '2' ? c - '0' : 1;
	}
	frame_damage_known = 0;
	if (copy_damage == 0 || update == NULL || (shape = *(void **)((char *)update + 0x10)) == NULL) {
		return;
	}
	get_bounds(shape, &frame_damage);
	frame_damage.shape = shape;
	frame_damage_known = 1;
}

static void
copy_rows(char *to, size_t to_row, const char *from, size_t from_row, size_t first, size_t count, size_t offset, size_t bytes,
    int clean)
{
	for (size_t y = first; y < first + count; y++) {
		memcpy(to + y * to_row + offset, from + y * from_row + offset, bytes);
		if (clean) {
			clean_to_memory(to + y * to_row + offset, bytes);
		}
	}
}

static void
copy_refs(IOSurfaceRef src, IOSurfaceRef dst, const struct ca_bounds *redrawn)
{
	static uint64_t waited, copied;
	static unsigned skipped, partial;
	uint64_t t0, t1, t2;
	size_t rows, bytes, width;
	int clean = flip_wait != NULL;

	if (IOSurfaceGetWidth(src) != IOSurfaceGetWidth(dst) || IOSurfaceGetHeight(src) != IOSurfaceGetHeight(dst)) {
		return;
	}
	rows = IOSurfaceGetHeight(src);
	width = IOSurfaceGetWidth(src);
	struct ca_bounds whole = { 0, 0, (int)width, (int)rows, NULL };

	if (redrawn != NULL && redrawn->shape != NULL && shape_contains(redrawn->shape, &whole)) {
		if (skipped++ < 4 || skipped % 600 == 0) {
			say("twl: copy back: none, the frame is drawn whole (%d,%d %dx%d; %u so)\n", redrawn->x, redrawn->y, redrawn->w,
			    redrawn->h, skipped);
		}
		return;
	}
	t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	wait_for_gpu();
	t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	IOSurfaceLock(src, 1 /* read only */, NULL);
	IOSurfaceLock(dst, 0, NULL);
	bytes = IOSurfaceGetBytesPerRow(src) < IOSurfaceGetBytesPerRow(dst) ? IOSurfaceGetBytesPerRow(src) : IOSurfaceGetBytesPerRow(dst);
	{
		char *to = IOSurfaceGetBaseAddress(dst);
		const char *from = IOSurfaceGetBaseAddress(src);
		size_t to_row = IOSurfaceGetBytesPerRow(dst), from_row = IOSurfaceGetBytesPerRow(src);
		size_t pixel = width ? bytes / width : 4;

		if (redrawn != NULL && copy_damage == 2 && pixel > 0 && redrawn->w > 0 && redrawn->h > 0 && redrawn->shape != NULL &&
		    shape_rects(redrawn->shape) == 1) {
			/* around the damage: the rows above and below it whole, beside it on its rows */
			size_t y0 = redrawn->y < 0 ? 0 : (size_t)redrawn->y, y1 = (size_t)redrawn->y + redrawn->h;
			size_t x0 = redrawn->x < 0 ? 0 : (size_t)redrawn->x, x1 = (size_t)redrawn->x + redrawn->w;

			y0 = y0 > rows ? rows : y0;
			y1 = y1 > rows ? rows : y1 < y0 ? y0 : y1;
			x0 = x0 > width ? width : x0;
			x1 = x1 > width ? width : x1 < x0 ? x0 : x1;
			copy_rows(to, to_row, from, from_row, 0, y0, 0, bytes, clean);
			copy_rows(to, to_row, from, from_row, y1, rows - y1, 0, bytes, clean);
			if (x0 > 0) {
				copy_rows(to, to_row, from, from_row, y0, y1 - y0, 0, x0 * pixel, clean);
			}
			if (x1 < width) {
				copy_rows(to, to_row, from, from_row, y0, y1 - y0, x1 * pixel, bytes - x1 * pixel, clean);
			}
			partial++;
		} else {
			copy_rows(to, to_row, from, from_row, 0, rows, 0, bytes, 0);
			if (clean) {
				clean_to_memory(to, rows * to_row);
			}
		}
	}
	IOSurfaceUnlock(dst, 0, NULL);
	IOSurfaceUnlock(src, 1, NULL);
	t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	waited += t1 - t0;
	copied += t2 - t1;
	pace_wait += t1 - t0;
	pace_copy += t2 - t1;
	if (copies++ < 8 || copies % 100 == 0) {
		say("twl: copy surface %u -> %u (%zu rows, copy %u, mean wait %llu us, copy %llu us; %u around a damage, %u skipped; "
		    "damage %s%d,%d %dx%d)\n", IOSurfaceGetID(src), IOSurfaceGetID(dst), rows, copies, waited / copies / 1000,
		    copied / copies / 1000, partial, skipped, redrawn ? "" : "unknown ", redrawn ? redrawn->x : 0, redrawn ? redrawn->y : 0,
		    redrawn ? redrawn->w : 0, redrawn ? redrawn->h : 0);
	}
}

static void
copy_back(void *from, void *to, const struct ca_bounds *redrawn)
{
	IOSurfaceRef src = surface_ref(from), dst = surface_ref(to);

	if (src != NULL && dst != NULL) {
		copy_refs(src, dst, redrawn);
	}
}

/*
 * The same accelerator also carries a finished frame from the buffer it was drawn in to the one the display
 * shows (another surface of the same size). Which that is: a surface of the frame's size and format that
 * the window server never draws into. Up to 8 surfaces are looked at, by number.
 */
static uint32_t drawn_into[8];

static void
note_drawn(IOSurfaceRef s)
{
	uint32_t id = IOSurfaceGetID(s);

	for (int i = 0; i < 8; i++) {
		if (drawn_into[i] == id) {
			return;
		}
		if (drawn_into[i] == 0) {
			drawn_into[i] = id;
			return;
		}
	}
}

static int
is_drawn_into(uint32_t id)
{
	for (int i = 0; i < 8; i++) {
		if (drawn_into[i] == id) {
			return 1;
		}
	}
	return 0;
}

static uint64_t (*commits)(void);
static int (*flip)(uint32_t), (*flip_target)(uint32_t);
static uint64_t flip_commits;
static int flip_pending;
static void *copy_from, *copy_to;

static void
transfer_to_scanout(void)
{
	static int (*set_source)(IOSurfaceRef, uint64_t);
	static int looked;
	IOSurfaceRef src = last_surface ? surface_ref(last_surface) : NULL;

	if (src == NULL) {
		return;
	}
	if (!looked) {
		char args[1024] = "";
		size_t length = sizeof(args) - 1;

		looked = 1;
		if (sysctlbyname("kern.bootargs", args, &length, NULL, 0) == 0) {
			if (strstr(args, "twl_source=1") != NULL) {
				set_source = (int (*)(IOSurfaceRef, uint64_t))dlsym(RTLD_DEFAULT, "native_gop_set_source");
			}
			if (strstr(args, "fb_flip=1") != NULL) {
				flip = (int (*)(uint32_t))dlsym(RTLD_DEFAULT, "native_gop_flip");
				flip_target = (int (*)(uint32_t))dlsym(RTLD_DEFAULT, "native_gop_flip_target");
				flip_wait = (int (*)(uint32_t))dlsym(RTLD_DEFAULT, "native_gop_flip_wait");
				commits = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "native_bridge_metal_commits");
				say("twl: fb_flip=1: native_gop_flip at %p\n", (void *)flip);
			}
		}
	}
	/*
	 * The display scans the surface it was drawn in; the swap's surfaces stay as they are. The drawing is committed after this: the plug-in
	 * flips when it completes. A frame that commits nothing is flipped by my_update_surface.
	 */
	if (copy_to != NULL && flip_target != NULL) {
		uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

		/* the frame before is on the display by now, and the surface it replaced free: nothing of this frame is committed yet */
		wait_for_gpu();
		if (surface_ref(copy_to) != NULL) {
			flip_wait(IOSurfaceGetID(surface_ref(copy_to)));
		}
		pace_wait += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
		copy_back(copy_from, copy_to, frame_damage_known ? &frame_damage : NULL);
		copy_to = NULL;
	}
	if (flip_target != NULL && flip_target(IOSurfaceGetID(src))) {
		flip_pending = 1;
		flip_commits = commits != NULL ? commits() : 0;
		return;
	}
	/*
	 * twl_source=1, for measurements: the display's presenter copies the frame from here itself. The surfaces the swap names then go
	 * stale, and the kernel's settle and any frame the presenter misses draw whatever they last held.
	 */
	if (set_source != NULL && set_source(src, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000)) {
		return;
	}
	for (uint32_t id = 1; id <= 8; id++) {
		IOSurfaceRef dst;

		if (id == IOSurfaceGetID(src) || is_drawn_into(id) || (dst = IOSurfaceLookup(id)) == NULL) {
			continue;
		}
		if (IOSurfaceGetWidth(dst) == IOSurfaceGetWidth(src) && IOSurfaceGetHeight(dst) == IOSurfaceGetHeight(src)) {
			copy_refs(src, dst, NULL);
		}
		CFRelease(dst);
	}
}


static void *
my_update_surface(void *display, int a, int b, uint64_t c, int d)
{
	void *r = update_surface_o(display, a, b, c, d);

	if (r != NULL && last_surface != NULL && r != last_surface) {
		if (flip_wait != NULL) {
			/* the surface may be on the display: copied at the hand-over, when it has left. A frame that committed nothing is flipped here. */
			if (flip_pending && commits != NULL && commits() == flip_commits && surface_ref(last_surface) != NULL) {
				flip(IOSurfaceGetID(surface_ref(last_surface)));
			}
			flip_pending = 0;
			copy_from = last_surface;
			copy_to = r;
		} else {
			copy_back(last_surface, r, NULL);
		}
	}
	if (r != NULL) {
		IOSurfaceRef ref = surface_ref(r);

		last_surface = r;
		if (ref != NULL) {
			note_drawn(ref);
		}
	}

	if (tv_out && (n_us % 200 == 0 || n_us < 6)) {
		const int *f = (const int *)display;

		say("twl: display sizes: physical %dx%d canvas %dx%d +90 %d,%d %dx%d +a0 %d,%d %dx%d +b0 %d,%d %dx%d +c0 %d,%d %dx%d, +279 %#x\n",
		    f[0x50 / 4], f[0x54 / 4], f[0x58 / 4], f[0x5c / 4], f[0x90 / 4], f[0x94 / 4], f[0x98 / 4], f[0x9c / 4], f[0xa0 / 4],
		    f[0xa4 / 4], f[0xa8 / 4], f[0xac / 4], f[0xb0 / 4], f[0xb4 / 4], f[0xb8 / 4], f[0xbc / 4], f[0xc0 / 4], f[0xc4 / 4],
		    f[0xc8 / 4], f[0xcc / 4], *((const uint8_t *)display + 0x279));
	}
	if (n_us++ % 200 == 0 || n_us < 10) {
		uint32_t fourcc = r ? *(uint32_t *)((char *)r + 0x20) : 0;
		say("twl: IOMFBDisplay::update_surface(%d, %d, %#llx, %d) -> %p '%c%c%c%c' (call %u)\n", a, b,
		    (unsigned long long)c, d, r, fourcc >> 24 & 0xff ? fourcc >> 24 : '?', fourcc >> 16 & 0xff ? fourcc >> 16 & 0xff : '?',
		    fourcc >> 8 & 0xff ? fourcc >> 8 & 0xff : '?', fourcc & 0xff ? fourcc & 0xff : '?', n_us);
	}
	return r;
}

static int
my_make_buffer_current(void *context, void *surface)
{
	int r = make_buffer_current_o(context, surface);

	if (n_mbc++ % 200 == 0 || n_mbc < 10 || !r) {
		say("twl: SWContext::make_buffer_current(%p) -> %d (call %u)\n", surface, r, n_mbc);
	}
	return r;
}

#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/ucontext.h>
#include "twlinput.inc"

/*
 * A crash of the window server, said before it happens: the faulting address, the registers that matter and the
 * frame-pointer chain, each as an image and an offset in it (dladdr), on the console; then the signal goes on as before.
 */
static void
where(const char *what, uintptr_t pc)
{
	Dl_info info;

	if (pc != 0 && dladdr((const void *)pc, &info) != 0 && info.dli_fname != NULL) {
		const char *name = strrchr(info.dli_fname, '/');

		say("twl: crash:   %s %#lx = %s+%#lx (%s)\n", what, (unsigned long)pc, name ? name + 1 : info.dli_fname,
		    (unsigned long)(pc - (uintptr_t)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
	} else {
		say("twl: crash:   %s %#lx\n", what, (unsigned long)pc);
	}
}

static void
crashed(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = (ucontext_t *)context;
	uintptr_t fp;

	say("twl: crash: signal %d, code %d, address %p\n", sig, info->si_code, info->si_addr);
	if (uc != NULL && uc->uc_mcontext != NULL) {
		where("pc", (uintptr_t)uc->uc_mcontext->__ss.__pc);
		where("lr", (uintptr_t)uc->uc_mcontext->__ss.__lr);
		fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
		for (int i = 0; i < 24 && fp != 0 && (fp & 7) == 0; i++) {
			uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

			where("frame", ret & 0x0000ffffffffffffUL);
			if (next <= fp) {
				break;
			}
			fp = next;
		}
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
catch_crashes(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crashed;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
}

__attribute__((constructor)) static void
twl_init(void)
{
	char path[1024];
	uint32_t size = sizeof(path);
	size_t length;
	uintptr_t base;

	if (_NSGetExecutablePath(path, &size) != 0 || strstr(path, "backboardd") == NULL) {
		return;
	}
	catch_crashes();
	base = (uintptr_t)_dyld_get_shared_cache_range(&length);
	slide = base - 0x180000000UL;
	say("twl: in %s, shared cache slide %#lx\n", path, slide);
	fetch_off();
	scanout_copy_off();
	forward_dm_off();
	canvas_init();
	AppleDisplay_install(0x1cf195240UL);
	AppleCLCDDisplay_install(0x1cf196a20UL);
	AppleRGBOutDisplay_install(0x1cf195a60UL);
	update_surface_o = replace_virtual("AppleCLCDDisplay update_surface", VSLOT(0x1cf196a20UL, 0x1c0), 0x189be9f54UL,
	    my_update_surface);
	/* (a TV out, fb_external, is an AppleRGBOutDisplay: the same IOMFBDisplay::update_surface, and its frames are flipped the same way) */
	{
		void *o = replace_virtual("AppleRGBOutDisplay update_surface", VSLOT(0x1cf195a60UL, 0x1c0), 0x189be9f54UL, my_update_surface);

		if (update_surface_o == NULL) {
			update_surface_o = (void *(*)(void *, int, int, uint64_t, int))o;
		}
	}
	detachable_o = replace_virtual("AppleRGBOutDisplay detachable_layer_impl", VSLOT(0x1cf195a60UL, 0x780), 0x1899baf8cUL,
	    my_detachable);
	make_buffer_current_o = replace_virtual("SWContext make_buffer_current", VSLOT(0x1cf1b1f48UL, 0x348), 0x189aa2b34UL,
	    my_make_buffer_current);
	start_keys();
	/* the server is a CA::WindowServer::EAGLServer (an IOMFBServer that renders with OpenGL ES, or without it in software) */
	render_update_o = replace_virtual("EAGLServer render_update", VSLOT(0x1cf199958UL, 0x178), 0x189a07350UL, my_render_update);
	renderer_o = replace_virtual("EAGLServer renderer", VSLOT(0x1cf199958UL, 0x1a0), 0x189a074f8UL, my_renderer);
	render_surface_o = replace_virtual("EAGLServer render_surface", VSLOT(0x1cf199958UL, 0x180), 0x189a07400UL,
	    my_render_surface);
	current_surface_o = replace_virtual("EAGLServer current_surface", VSLOT(0x1cf199958UL, 0x040), 0x189ba45c8UL,
	    my_current_surface);
}
