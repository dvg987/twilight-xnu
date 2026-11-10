/*
 * twlshot: what the TV shows, as a PNG, from a shell on the box (twlshot OUT.png [scale-divisor]).
 * The window server renders its display into an IOSurface of ours (QuartzCore's CARenderServerRenderDisplay, the
 * call screenshot tools use), which ImageIO writes out. The display's name comes from [CADisplay mainDisplay].
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>
#include <ImageIO/ImageIO.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

extern void CARenderServerRenderDisplay(unsigned int port, CFStringRef display, IOSurfaceRef surface, int x, int y);

static IOSurfaceRef
surface_make(int width, int height)
{
	int bpe = 4, row = (width * 4 + 63) & ~63, format = 'BGRA';
	CFNumberRef v[5] = {
		CFNumberCreate(NULL, kCFNumberIntType, &width), CFNumberCreate(NULL, kCFNumberIntType, &height),
		CFNumberCreate(NULL, kCFNumberIntType, &bpe), CFNumberCreate(NULL, kCFNumberIntType, &row),
		CFNumberCreate(NULL, kCFNumberIntType, &format),
	};
	const void *k[5] = { CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"), CFSTR("IOSurfaceBytesPerElement"),
		             CFSTR("IOSurfaceBytesPerRow"), CFSTR("IOSurfacePixelFormat") };
	CFDictionaryRef d = CFDictionaryCreate(NULL, k, (const void **)v, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef s = IOSurfaceCreate(d);

	CFRelease(d);
	for (int i = 0; i < 5; i++) {
		CFRelease(v[i]);
	}
	return s;
}

int
main(int argc, char **argv)
{
	int width = 1920, height = 1080, div = argc > 2 ? atoi(argv[2]) : 2;
	CFStringRef name = CFSTR("LCD");
	id display;

	if (argc < 2 || div < 1) {
		fprintf(stderr, "usage: twlshot OUT.png [scale-divisor, default 2]\n");
		return 2;
	}
	display = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("CADisplay"), sel_registerName("mainDisplay"));
	if (display != NULL) {
		CGRect b = ((CGRect (*)(id, SEL))objc_msgSend)(display, sel_registerName("bounds"));
		id n = ((id (*)(id, SEL))objc_msgSend)(display, sel_registerName("name"));
		if (b.size.width > 0 && b.size.height > 0) {
			width = (int)b.size.width;
			height = (int)b.size.height;
		}
		if (n != NULL) {
			name = (CFStringRef)n;
		}
	}
	IOSurfaceRef s = surface_make(width, height);
	if (s == NULL) {
		fprintf(stderr, "twlshot: no surface\n");
		return 1;
	}
	CARenderServerRenderDisplay(0, name, s, 0, 0);

	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef src = CGBitmapContextCreate(IOSurfaceGetBaseAddress(s), width, height, 8, IOSurfaceGetBytesPerRow(s), cs,
	    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGImageRef full = src ? CGBitmapContextCreateImage(src) : NULL;
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	if (full == NULL) {
		fprintf(stderr, "twlshot: no image\n");
		return 1;
	}
	CGContextRef small = CGBitmapContextCreate(NULL, width / div, height / div, 8, 0, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
	CGContextSetInterpolationQuality(small, kCGInterpolationMedium);
	CGContextDrawImage(small, CGRectMake(0, 0, width / div, height / div), full);
	CGImageRef out = CGBitmapContextCreateImage(small);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], (CFIndex)strlen(argv[1]), false);
	CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
	if (dst == NULL) {
		fprintf(stderr, "twlshot: cannot write %s\n", argv[1]);
		return 1;
	}
	CGImageDestinationAddImage(dst, out, NULL);
	bool ok = CGImageDestinationFinalize(dst);
	char nm[64] = "?";
	CFStringGetCString(name, nm, sizeof(nm), kCFStringEncodingUTF8);
	printf("twlshot: display \"%s\" %dx%d -> %s (%dx%d): %s\n", nm, width, height,
	    argv[1], width / div, height / div, ok ? "written" : "FAILED");
	return ok ? 0 : 1;
}
