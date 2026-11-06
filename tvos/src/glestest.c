/*
 * glestest: OpenGL ES on the Mali-G310 checked on its own, without CoreAnimation or the EAGL shim: Mesa's OSMesa
 * (libOSMesa.8.dylib from tvos/gpu/build-mesa-gl.sh: gallium panfrost through darwin-drm to the kernel's MaliService).
 *
 *   glestest [-v 2|3] [-s SIZE] [-n FRAMES] [-l LIBRARY]
 *
 * The library is dlopen()ed: LIBRARY, else libOSMesa.8.dylib next to this program, else /redwood/lib/libOSMesa.8.dylib.
 * Steps, each says PASS or FAIL:
 *   1  an OpenGL ES context (version 3, else 2); GL_VENDOR / GL_RENDERER / GL_VERSION; on the GPU when the renderer says Panfrost
 *   2  the context's own buffer: clear to dark blue, a triangle with a GLSL program (red, green, blue corners), glReadPixels
 *   3  the same into a framebuffer object (a texture), the way the EAGL shim draws
 *   4  an IOSurface as the texture (glEGLImageTargetTexture2DOES with the surface's ID, as gl_osmesa.c does): drawn into by the
 *      GPU, read back from the surface's memory with this processor
 *   5  FRAMES (default 60) full-screen 1920x1080 frames, glFinish each: milliseconds a frame
 * Exit status: 0 everything passed on the GPU, 2 everything passed but on the software rasterizer, 1 a step failed, 100+ no library.
 * Environment: OSMESA_SOFTWARE=1 (softpipe, to compare), OSMESA_DEBUG_GPU=1 (why the GPU was not taken), DRM_DARWIN_TRACE=1
 * (every ioctl of the shim), PAN_MESA_DEBUG=... (panfrost's own).
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38

#define GL_NO_ERROR                     0
#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_SHADING_LANGUAGE_VERSION     0x8b8c
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_TEXTURE_2D                   0x0de1
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_NEAREST                      0x2600
#define GL_ARRAY_BUFFER                 0x8892
#define GL_STATIC_DRAW                  0x88e4
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_FRAMEBUFFER                  0x8d40
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_PACK_ALIGNMENT               0x0d05

typedef struct osmesa_context *OSMesaContext;
static OSMesaContext (*OSMesaCreateContextAttribs)(const int *, OSMesaContext);
static unsigned char (*OSMesaMakeCurrent)(OSMesaContext, void *, unsigned, int, int);
static void (*OSMesaDestroyContext)(OSMesaContext);
static void *(*OSMesaGetProcAddress)(const char *);

#define GL_FUNCTIONS(X) \
	X(const unsigned char *, glGetString, (unsigned)) \
	X(unsigned, glGetError, (void)) \
	X(void, glViewport, (int, int, int, int)) \
	X(void, glClearColor, (float, float, float, float)) \
	X(void, glClear, (unsigned)) \
	X(void, glFinish, (void)) \
	X(void, glFlush, (void)) \
	X(void, glPixelStorei, (unsigned, int)) \
	X(void, glReadPixels, (int, int, int, int, unsigned, unsigned, void *)) \
	X(unsigned, glCreateShader, (unsigned)) \
	X(void, glShaderSource, (unsigned, int, const char *const *, const int *)) \
	X(void, glCompileShader, (unsigned)) \
	X(void, glGetShaderiv, (unsigned, unsigned, int *)) \
	X(void, glGetShaderInfoLog, (unsigned, int, int *, char *)) \
	X(unsigned, glCreateProgram, (void)) \
	X(void, glAttachShader, (unsigned, unsigned)) \
	X(void, glBindAttribLocation, (unsigned, unsigned, const char *)) \
	X(void, glLinkProgram, (unsigned)) \
	X(void, glGetProgramiv, (unsigned, unsigned, int *)) \
	X(void, glGetProgramInfoLog, (unsigned, int, int *, char *)) \
	X(void, glUseProgram, (unsigned)) \
	X(void, glGenBuffers, (int, unsigned *)) \
	X(void, glBindBuffer, (unsigned, unsigned)) \
	X(void, glBufferData, (unsigned, long, const void *, unsigned)) \
	X(void, glVertexAttribPointer, (unsigned, int, unsigned, unsigned char, int, const void *)) \
	X(void, glEnableVertexAttribArray, (unsigned)) \
	X(void, glDrawArrays, (unsigned, int, int)) \
	X(void, glGenTextures, (int, unsigned *)) \
	X(void, glDeleteTextures, (int, const unsigned *)) \
	X(void, glBindTexture, (unsigned, unsigned)) \
	X(void, glTexParameteri, (unsigned, unsigned, int)) \
	X(void, glTexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *)) \
	X(void, glGenFramebuffers, (int, unsigned *)) \
	X(void, glDeleteFramebuffers, (int, const unsigned *)) \
	X(void, glBindFramebuffer, (unsigned, unsigned)) \
	X(void, glFramebufferTexture2D, (unsigned, unsigned, unsigned, unsigned, int)) \
	X(unsigned, glCheckFramebufferStatus, (unsigned)) \
	X(void, glEGLImageTargetTexture2DOES, (unsigned, void *))

#define DECLARE(ret, name, args) static ret(*name) args;
GL_FUNCTIONS(DECLARE)

static int failures;

/* clean and invalidate to the point of coherency (libSystem's sys_dcache_flush only cleans here) */
static void
dcache_civac(void *start, size_t length)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	const size_t line = 4u << ((ctr >> 16) & 0xf);
	for (uintptr_t p = (uintptr_t)start & ~(uintptr_t)(line - 1); p < (uintptr_t)start + length; p += line)
		__asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
	__asm__ volatile("dsb sy" : : : "memory");
}

static void
result(const char *step, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void
result(const char *step, int ok, const char *fmt, ...)
{
	va_list ap;

	printf("glestest: %-10s %s  ", step, ok ? "PASS" : "FAIL");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	failures += !ok;
}

static double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void *
open_library(const char *asked)
{
	char path[1024], here[1024];
	uint32_t size = sizeof here;
	void *library;

	if (asked != NULL) {
		library = dlopen(asked, RTLD_NOW | RTLD_LOCAL);
		if (library == NULL) {
			fprintf(stderr, "glestest: %s\n", dlerror());
		}
		return library;
	}
	if (_NSGetExecutablePath(here, &size) == 0 && strrchr(here, '/') != NULL) {
		*strrchr(here, '/') = 0;
		snprintf(path, sizeof path, "%s/libOSMesa.8.dylib", here);
		if ((library = dlopen(path, RTLD_NOW | RTLD_LOCAL)) != NULL) {
			printf("glestest: library %s\n", path);
			return library;
		}
		fprintf(stderr, "glestest: %s\n", dlerror());
	}
	library = dlopen("/redwood/lib/libOSMesa.8.dylib", RTLD_NOW | RTLD_LOCAL);
	if (library == NULL) {
		fprintf(stderr, "glestest: %s\n", dlerror());
	} else {
		printf("glestest: library /redwood/lib/libOSMesa.8.dylib\n");
	}
	return library;
}

static void *
gl_function(void *library, const char *name)
{
	void *f = dlsym(library, name);

	return f != NULL ? f : OSMesaGetProcAddress(name);
}

static unsigned
shader(unsigned type, const char *source)
{
	unsigned s = glCreateShader(type);
	int ok = 0;
	char log[1024] = "";

	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		printf("glestest: %s shader: %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		return 0;
	}
	return s;
}

/* a triangle: red bottom left, green bottom right, blue top, over a dark blue clear */
static unsigned program, buffer;

static int
setup_program(void)
{
	static const char *vs =
		"#version 100\n"
		"attribute vec2 position;\n"
		"attribute vec3 color;\n"
		"varying lowp vec3 v_color;\n"
		"void main() { v_color = color; gl_Position = vec4(position, 0.0, 1.0); }\n";
	static const char *fs =
		"#version 100\n"
		"varying lowp vec3 v_color;\n"
		"void main() { gl_FragColor = vec4(v_color, 1.0); }\n";
	static const float vertices[] = {
		-0.8f, -0.8f, 1, 0, 0,
		 0.8f, -0.8f, 0, 1, 0,
		 0.0f,  0.8f, 0, 0, 1,
	};
	unsigned v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
	int ok = 0;
	char log[1024] = "";

	if (!v || !f) {
		return 0;
	}
	program = glCreateProgram();
	glAttachShader(program, v);
	glAttachShader(program, f);
	glBindAttribLocation(program, 0, "position");
	glBindAttribLocation(program, 1, "color");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(program, sizeof log, NULL, log);
		printf("glestest: link: %s\n", log);
		return 0;
	}
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 5 * sizeof(float), (const void *)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, 0, 5 * sizeof(float), (const void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	return 1;
}

static void
draw(int width, int height)
{
	glViewport(0, 0, width, height);
	glClearColor(0.0f, 0.0f, 0.5f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(program);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

/*
 * Check a picture of draw(): pixels as R, G, B, A bytes (rgba) or B, G, R, A (bgra), rows `stride` bytes apart, bottom row first.
 * The corners are the clear colour, the points near the triangle's corners are mostly that corner's colour, and about 32% of the
 * picture is triangle.
 */
static int
check(const char *step, const uint8_t *pixels, int width, int height, size_t stride, int bgra, double ms)
{
	struct { const char *name; double x, y; int r, g, b; } probes[] = {
		{ "corner", 0.02, 0.02, 0, 0, 128 },
		{ "red", -0.7, -0.75, 255, 0, 0 },
		{ "green", 0.7, -0.75, 0, 255, 0 },
		{ "blue", 0.0, 0.7, 0, 0, 255 },
	};
	char detail[512];
	size_t used = 0;
	long triangle = 0;
	int ok = 1;
	uint32_t sum = 0;

	for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
		int px = i == 0 ? 1 : (int)((probes[i].x + 1) / 2 * width), py = i == 0 ? 1 : (int)((probes[i].y + 1) / 2 * height);
		const uint8_t *p = pixels + (size_t)py * stride + (size_t)px * 4;
		int r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2];
		int close = abs(r - probes[i].r) < 80 && abs(g - probes[i].g) < 80 && abs(b - probes[i].b) < 80;

		ok &= close;
		used += (size_t)snprintf(detail + used, sizeof detail - used, "%s(%d,%d)=%02x%02x%02x%s ", probes[i].name, px, py, r, g, b,
		    close ? "" : "!");
	}
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const uint8_t *p = pixels + (size_t)y * stride + (size_t)x * 4;
			int r = bgra ? p[2] : p[0], b = bgra ? p[0] : p[2];

			triangle += !(r == 0 && p[1] == 0 && b > 120 && b < 136);
			sum = sum * 31 + (uint32_t)(p[0] | p[1] << 8 | p[2] << 16);
		}
	}
	double share = 100.0 * triangle / ((double)width * height);
	ok &= share > 28 && share < 36;
	snprintf(detail + used, sizeof detail - used, "triangle %.1f%% of %dx%d, checksum %08x, %.2f ms", share, width, height, sum, ms);
	result(step, ok, "%s", detail);
	return ok;
}

int
main(int argc, char **argv)
{
	int version = 3, size = 256, frames = 60;
	const char *asked = NULL;
	void *library;
	OSMesaContext context = NULL;
	int on_gpu;

	for (int i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "-v") == 0) version = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "-s") == 0) size = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "-n") == 0) frames = atoi(argv[i + 1]);
		else if (strcmp(argv[i], "-l") == 0) asked = argv[i + 1];
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if ((library = open_library(asked)) == NULL) {
		return 100;
	}
	OSMesaCreateContextAttribs = dlsym(library, "OSMesaCreateContextAttribs");
	OSMesaMakeCurrent = dlsym(library, "OSMesaMakeCurrent");
	OSMesaDestroyContext = dlsym(library, "OSMesaDestroyContext");
	OSMesaGetProcAddress = dlsym(library, "OSMesaGetProcAddress");
	if (!OSMesaCreateContextAttribs || !OSMesaMakeCurrent || !OSMesaDestroyContext || !OSMesaGetProcAddress) {
		fprintf(stderr, "glestest: the library has no OSMesa interface\n");
		return 101;
	}
#define RESOLVE(ret, name, args) \
	if ((name = (ret(*) args)gl_function(library, #name)) == NULL) { fprintf(stderr, "glestest: no %s\n", #name); return 102; }
	GL_FUNCTIONS(RESOLVE)

	/* 1: the context */
	double t0 = now_ms();
	uint8_t *window = calloc((size_t)size * size, 4);
	for (int v = version; v >= 2 && context == NULL; v--) {
		const int attributes[] = {
			OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
			OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, v, OSMESA_CONTEXT_MINOR_VERSION, 0, 0
		};
		context = OSMesaCreateContextAttribs(attributes, NULL);
		version = v;
	}
	if (context == NULL || !OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, size, size)) {
		result("context", 0, "no OpenGL ES context (%s)", context == NULL ? "OSMesaCreateContextAttribs" : "OSMesaMakeCurrent");
		return 1;
	}
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	on_gpu = renderer != NULL && strstr(renderer, "Panfrost") != NULL;
	printf("glestest: GL_VENDOR   %s\n", glGetString(GL_VENDOR));
	printf("glestest: GL_RENDERER %s\n", renderer);
	printf("glestest: GL_VERSION  %s\n", glGetString(GL_VERSION));
	printf("glestest: GLSL        %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	result("context", 1, "OpenGL ES %d context in %.1f ms, %s", version, now_ms() - t0,
	    on_gpu ? "on the GPU (panfrost)" : "NOT on the GPU (software rasterizer; OSMESA_DEBUG_GPU=1 says why)");
	if (!setup_program()) {
		result("program", 0, "the GLSL program did not build");
		return 1;
	}

	/* 2: the context's own buffer */
	uint8_t *pixels = malloc((size_t)1920 * 1080 * 4);
	t0 = now_ms();
	draw(size, size);
	glFinish();
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	check("window", pixels, size, size, (size_t)size * 4, 0, now_ms() - t0);

	/* 3: a framebuffer object of a texture */
	unsigned texture, framebuffer;
	int fw = 512, fh = 384;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		result("fbo", 0, "framebuffer incomplete");
	} else {
		t0 = now_ms();
		draw(fw, fh);
		glFinish();
		glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		check("fbo", pixels, fw, fh, (size_t)fw * 4, 0, now_ms() - t0);
	}

	/* 4: an IOSurface's memory as the texture (what hakios_gl_surface_texture() does for CoreAnimation) */
	{
		int32_t sw = 320, sh = 240, bpe = 4, row = (320 * 4 + 63) & ~63, format = 'BGRA';
		CFNumberRef values[5] = {
			CFNumberCreate(NULL, kCFNumberSInt32Type, &sw), CFNumberCreate(NULL, kCFNumberSInt32Type, &sh),
			CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe), CFNumberCreate(NULL, kCFNumberSInt32Type, &row),
			CFNumberCreate(NULL, kCFNumberSInt32Type, &format),
		};
		const void *keys[5] = { CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
			CFSTR("IOSurfaceBytesPerRow"), CFSTR("IOSurfacePixelFormat") };
		CFDictionaryRef properties = CFDictionaryCreate(NULL, keys, (const void **)values, 5, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		IOSurfaceRef surface = IOSurfaceCreate(properties);
		unsigned surface_texture, surface_framebuffer;

		CFRelease(properties);
		for (unsigned i = 0; i < 5; i++) {
			CFRelease(values[i]);
		}
		if (surface == NULL) {
			result("iosurface", 0, "IOSurfaceCreate failed");
		} else {
			uint8_t *base = IOSurfaceGetBaseAddress(surface);
			size_t stride = IOSurfaceGetBytesPerRow(surface), bytes = IOSurfaceGetAllocSize(surface);

			printf("glestest: IOSurface %u: %dx%d, %zu bytes a row, %zu bytes at %p\n", IOSurfaceGetID(surface), sw, sh, stride,
			    bytes, base);
			IOSurfaceLock(surface, 0, NULL);
			memset(base, 0x5a, bytes);              /* what the GPU must overwrite */
			dcache_civac(base, bytes);              /* (the library does this too, at import) */
			IOSurfaceUnlock(surface, 0, NULL);
			glGenTextures(1, &surface_texture);
			glBindTexture(GL_TEXTURE_2D, surface_texture);
			while (glGetError() != GL_NO_ERROR) {
			}
			glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (void *)(uintptr_t)IOSurfaceGetID(surface));
			unsigned error = glGetError();
			if (error != GL_NO_ERROR && !on_gpu) {
				printf("glestest: iosurface  SKIP  refused (%#x), as it is without the GPU\n", error);
			} else if (error != GL_NO_ERROR) {
				result("iosurface", 0, "glEGLImageTargetTexture2DOES refused the surface (%#x)", error);
			} else {
				glGenFramebuffers(1, &surface_framebuffer);
				glBindFramebuffer(GL_FRAMEBUFFER, surface_framebuffer);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface_texture, 0);
				if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
					result("iosurface", 0, "framebuffer of the surface incomplete");
				} else {
					t0 = now_ms();
					draw(sw, sh);
					glFinish();
					double ms = now_ms() - t0;
					/* the GPU wrote the pages behind this processor's caches: drop whatever lines of them it holds */
					IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
					dcache_civac(base, bytes);
					/* (texture row 0, the bottom row in GL's terms, is the surface's first row: the same order as
					 * glReadPixels's; copied out while the surface is locked) */
					uint8_t *copy = malloc((size_t)sh * stride);
					for (int y = 0; y < sh; y++) {
						memcpy(copy + (size_t)y * stride, base + (size_t)y * stride, stride);
					}
					IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
					check("iosurface", copy, sw, sh, stride, 1, ms);
					free(copy);
					glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
					check("iosurf-rd", pixels, sw, sh, (size_t)sw * 4, 0, 0);
				}
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDeleteFramebuffers(1, &surface_framebuffer);
			}
			glDeleteTextures(1, &surface_texture);
			CFRelease(surface);
		}
	}

	/* 5: full-screen frames */
	if (frames > 0) {
		unsigned big, big_framebuffer;
		glGenTextures(1, &big);
		glBindTexture(GL_TEXTURE_2D, big);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1920, 1080, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &big_framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, big_framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, big, 0);
		draw(1920, 1080);
		glFinish();
		t0 = now_ms();
		for (int i = 0; i < frames; i++) {
			draw(1920, 1080);
			glFinish();
		}
		double per = (now_ms() - t0) / frames;
		glReadPixels(0, 0, 1920, 1080, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		check("1080p", pixels, 1920, 1080, 1920 * 4, 0, per);
		printf("glestest: 1080p: %d frames, %.2f ms a frame (clear + triangle + glFinish)\n", frames, per);
	}

	OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
	OSMesaDestroyContext(context);
	printf("glestest: %s: %d step(s) failed, %s\n", failures ? "FAILED" : "all passed", failures, on_gpu ? "GPU" : "software");
	return failures ? 1 : on_gpu ? 0 : 2;
}
