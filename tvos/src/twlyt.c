/*
 * libtwlyt.dylib (injected into YouTube): what it asks VideoToolbox about hardware decoders, written to its standard
 * error (twlcrash's file). YouTube streams VP9 (and AV1) when VTIsHardwareDecodeSupported says so. The box decodes
 * H.264, HEVC and VP9 itself (tvos/src/twlvdec.c), not AV1, and VideoToolbox's software AV1 on four small cores cannot
 * keep up, so for AV1 the answer here is no.
 */
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <fcntl.h>

#define DYLD_INTERPOSE(_r, _o) \
	__attribute__((used)) static struct { const void *r; const void *o; } _interpose_##_o \
	__attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&_r, (const void *)(unsigned long)&_o };

static void
fourcc(CMVideoCodecType c, char out[5])
{
	out[0] = (char)(c >> 24); out[1] = (char)(c >> 16); out[2] = (char)(c >> 8); out[3] = (char)c; out[4] = 0;
}

static Boolean
my_VTIsHardwareDecodeSupported(CMVideoCodecType codec)
{
	char name[5];
	Boolean real = VTIsHardwareDecodeSupported(codec);
	Boolean said = codec == 'av01' ? false : real;
	fourcc(codec, name);
	dprintf(2, "twlyt: VTIsHardwareDecodeSupported('%s'): %d, told %d\n", name, real, said);
	if (codec == 'vp09') {
		/* what the box's decoder needs here: its node visible (twlvdec's AVDRegister) and the bundle loaded */
		int a = access("/dev/twl-vdec-probe", F_OK), e = a ? errno : 0, avd = 0;
		for (uint32_t i = 0; i < _dyld_image_count(); i++) {
			const char *n = _dyld_get_image_name(i);
			if (n && strstr(n, "AVD.videodecoder")) avd = 1;
		}
		dprintf(2, "twlyt: node %s (errno %d), AVD.videodecoder %s\n", a ? "hidden" : "visible", e, avd ? "loaded" : "not loaded");
		int fd = open("/dev/twl-vdec-probe", O_RDWR), oe = fd < 0 ? errno : 0;
		if (fd >= 0) close(fd);
		/* what VTIsHardwareDecodeSupported does for VP9: an instance made with hardware required and VP9 allowed */
		OSStatus (*select_create)(CMVideoCodecType, CFAllocatorRef, CFDictionaryRef, void **) =
		    dlsym(RTLD_DEFAULT, "VTSelectAndCreateVideoDecoderInstance");
		Boolean (*entitled)(void *) = dlsym(RTLD_DEFAULT, "VTIsSecTaskEntitledForVP9Decode");
		void *(*task_self)(CFAllocatorRef) = dlsym(RTLD_DEFAULT, "SecTaskCreateFromSelf");
		void *task = task_self ? task_self(NULL) : NULL;
		int ent = task && entitled ? entitled(task) : -1;
		const CFStringRef *allow = dlsym(RTLD_DEFAULT, "kVTVideoDecoderSpecification_AllowVP9Decode");
		const void *keys[] = { CFSTR("RequireHardwareAcceleratedVideoDecoder"), allow ? *allow : CFSTR("AllowVP9Decode") };
		const void *values[] = { kCFBooleanTrue, kCFBooleanTrue };
		CFDictionaryRef spec = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		void *instance = NULL;
		OSStatus st = select_create ? select_create(codec, NULL, spec, &instance) : -1;
		dprintf(2, "twlyt: open O_RDWR errno %d; VP9 entitled %d; an instance: %d\n", oe, ent, (int)st);
		if (instance) CFRelease(instance);
		CFRelease(spec);
	}
	return said;
}
DYLD_INTERPOSE(my_VTIsHardwareDecodeSupported, VTIsHardwareDecodeSupported)

static OSStatus
my_VTDecompressionSessionCreate(CFAllocatorRef a, CMVideoFormatDescriptionRef format, CFDictionaryRef spec,
    CFDictionaryRef attrs, const VTDecompressionOutputCallbackRecord *cb, VTDecompressionSessionRef *out)
{
	char name[5];
	OSStatus r = VTDecompressionSessionCreate(a, format, spec, attrs, cb, out);
	CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(format);
	fourcc(CMFormatDescriptionGetMediaSubType(format), name);
	dprintf(2, "twlyt: VTDecompressionSessionCreate '%s' %dx%d: %d\n", name, d.width, d.height, (int)r);
	return r;
}
DYLD_INTERPOSE(my_VTDecompressionSessionCreate, VTDecompressionSessionCreate)

/* how its video frames reach the screen: CoreVideo's GL texture cache, asked for what */
#include <CoreVideo/CoreVideo.h>
static CVReturn
my_CVOpenGLESTextureCacheCreate(CFAllocatorRef a, CFDictionaryRef cattrs, CVEAGLContext ctx, CFDictionaryRef tattrs,
    CVOpenGLESTextureCacheRef *out)
{
	CVReturn r = CVOpenGLESTextureCacheCreate(a, cattrs, ctx, tattrs, out);
	dprintf(2, "twlyt: CVOpenGLESTextureCacheCreate: %d\n", r);
	return r;
}
DYLD_INTERPOSE(my_CVOpenGLESTextureCacheCreate, CVOpenGLESTextureCacheCreate)

static CVReturn
my_CVOpenGLESTextureCacheCreateTextureFromImage(CFAllocatorRef a, CVOpenGLESTextureCacheRef cache, CVImageBufferRef src,
    CFDictionaryRef attrs, GLenum target, GLint internal, GLsizei w, GLsizei h, GLenum format, GLenum type, size_t plane,
    CVOpenGLESTextureRef *out)
{
	static unsigned logged;
	CVReturn r = CVOpenGLESTextureCacheCreateTextureFromImage(a, cache, src, attrs, target, internal, w, h, format, type,
	    plane, out);
	if (logged < 40) {
		char name[5];
		fourcc(CVPixelBufferGetPixelFormatType(src), name);
		logged++;
		dprintf(2, "twlyt: texture of '%s' (%zu planes, iosurface %d) plane %zu: target %#x internal %#x %dx%d format %#x "
		    "type %#x: %d, name %u\n", name, CVPixelBufferGetPlaneCount(src), CVPixelBufferGetIOSurface(src) != NULL, plane,
		    target, internal, w, h, format, type, r, r == 0 && out && *out ? CVOpenGLESTextureGetName(*out) : 0);
	}
	return r;
}
DYLD_INTERPOSE(my_CVOpenGLESTextureCacheCreateTextureFromImage, CVOpenGLESTextureCacheCreateTextureFromImage)

/* where each image is (a code page that fails its signature check makes the kernel drop the process's CS_VALID, and
 * with it the entitlements VideoToolbox asks for before allowing VP9) */
__attribute__((constructor)) static void
images(void)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *n = _dyld_get_image_name(i);
		const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i);
		uint64_t lo = ~0ull, hi = 0;
		const struct load_command *lc = (const void *)(h + 1);
		for (uint32_t k = 0; k < h->ncmds; k++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
			if (lc->cmd != LC_SEGMENT_64) continue;
			const struct segment_command_64 *sg = (const void *)lc;
			if (!strcmp(sg->segname, "__PAGEZERO") || !strcmp(sg->segname, "__LINKEDIT")) continue;
			uint64_t a = sg->vmaddr + _dyld_get_image_vmaddr_slide(i);
			if (a < lo) lo = a;
			if (a + sg->vmsize > hi) hi = a + sg->vmsize;
		}
		if (lo < 0x180000000ull) dprintf(2, "twlyt: image %#llx-%#llx %s\n", lo, hi, n ? n : "?");
	}
}
