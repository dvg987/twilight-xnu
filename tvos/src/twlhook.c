/*
 * libtwl.bbd.dylib: inside tvOS's backboardd, what the window server decides about each frame.
 * Virtual table entries of CA::WindowServer's display and server classes are replaced by loggers that
 * call the originals. Addresses are tvOS 16.5 (20L563) AppleTV6,2's, unslid; each is checked first.
 */
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

/* one set of loggers per display class: which class the display is, is part of the answer */
#define DISPLAY_HOOKS(cls)                                                                                      \
static int (*cls##_is_ready_o)(void *);                                                                         \
static int (*cls##_ignore_o)(void *, void *);                                                                   \
static uintptr_t (*cls##_begin_o)(void *, void *);                                                              \
static uintptr_t (*cls##_finish_o)(void *, void *, unsigned);                                                   \
static unsigned cls##_n[4];                                                                                     \
static int cls##_last[4] = { -2, -2, -2, -2 };                                                                  \
static int cls##_is_ready(void *d) {                                                                            \
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

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = render_update_o(server, update, flags);

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

static void
copy_refs(IOSurfaceRef src, IOSurfaceRef dst)
{
	size_t rows, bytes;

	if (IOSurfaceGetWidth(src) != IOSurfaceGetWidth(dst) || IOSurfaceGetHeight(src) != IOSurfaceGetHeight(dst)) {
		return;
	}
	IOSurfaceLock(src, 1 /* read only */, NULL);
	IOSurfaceLock(dst, 0, NULL);
	rows = IOSurfaceGetHeight(src);
	bytes = IOSurfaceGetBytesPerRow(src) < IOSurfaceGetBytesPerRow(dst) ? IOSurfaceGetBytesPerRow(src) : IOSurfaceGetBytesPerRow(dst);
	for (size_t y = 0; y < rows; y++) {
		memcpy((char *)IOSurfaceGetBaseAddress(dst) + y * IOSurfaceGetBytesPerRow(dst),
		    (const char *)IOSurfaceGetBaseAddress(src) + y * IOSurfaceGetBytesPerRow(src), bytes);
	}
	IOSurfaceUnlock(dst, 0, NULL);
	IOSurfaceUnlock(src, 1, NULL);
	if (copies++ < 8 || copies % 100 == 0) {
		say("twl: copy surface %u -> %u (%zu rows, copy %u)\n", IOSurfaceGetID(src), IOSurfaceGetID(dst), rows, copies);
	}
}

static void
copy_back(void *from, void *to)
{
	IOSurfaceRef src = surface_ref(from), dst = surface_ref(to);

	if (src != NULL && dst != NULL) {
		copy_refs(src, dst);
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

static void
transfer_to_scanout(void)
{
	IOSurfaceRef src = last_surface ? surface_ref(last_surface) : NULL;

	if (src == NULL) {
		return;
	}
	for (uint32_t id = 1; id <= 8; id++) {
		IOSurfaceRef dst;

		if (id == IOSurfaceGetID(src) || is_drawn_into(id) || (dst = IOSurfaceLookup(id)) == NULL) {
			continue;
		}
		if (IOSurfaceGetWidth(dst) == IOSurfaceGetWidth(src) && IOSurfaceGetHeight(dst) == IOSurfaceGetHeight(src)) {
			copy_refs(src, dst);
		}
		CFRelease(dst);
	}
}


static void *
my_update_surface(void *display, int a, int b, uint64_t c, int d)
{
	void *r = update_surface_o(display, a, b, c, d);

	if (r != NULL && last_surface != NULL && r != last_surface) {
		copy_back(last_surface, r);
	}
	if (r != NULL) {
		IOSurfaceRef ref = surface_ref(r);

		last_surface = r;
		if (ref != NULL) {
			note_drawn(ref);
		}
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
#include "twlinput.inc"

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
	base = (uintptr_t)_dyld_get_shared_cache_range(&length);
	slide = base - 0x180000000UL;
	say("twl: in %s, shared cache slide %#lx\n", path, slide);
	AppleDisplay_install(0x1cf195240UL);
	AppleCLCDDisplay_install(0x1cf196a20UL);
	AppleRGBOutDisplay_install(0x1cf195a60UL);
	update_surface_o = replace_virtual("AppleCLCDDisplay update_surface", VSLOT(0x1cf196a20UL, 0x1c0), 0x189be9f54UL,
	    my_update_surface);
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
