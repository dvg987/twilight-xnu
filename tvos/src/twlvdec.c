/* SPDX-License-Identifier: GPL-2.0-or-later
 * VideoToolbox decoder bridge for the Amlogic H.264 transport (/dev/twl-vdec-probe, vdec=7).
 * Installed as /System/Library/VideoDecoders/AVD.videodecoder (tvos/build.sh VDEC=1), AVDRegister registers it in
 * every process that loads VideoToolbox's decoder list, when the kernel publishes the transport. The kernel takes one
 * decoder at a time: a second session gets kVTVideoDecoderNotAvailableNowErr and VideoToolbox's software decoder.
 * CMBase/VTVideoDecoder ABI: the tvOS 16.5 H264H8 vtables in work/vdecre.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <arm_neon.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/hakios.vdec.h>
#include "../video/h264/dpb.h"
#include "../video/hevc/hevc_dpb.h"
#include "../video/vp9/vp9_dpb.h"

#define PACKET_MAX (4u * 1024u * 1024u - 1024)
struct job {
    struct job *next;
    void *frame;
    CMSampleBufferRef sample;
    VTDecodeFrameFlags flags;
};

struct decoder {
    pthread_mutex_t mutex;
    bool mutex_ready;
    /* frames decoded on a thread of their own (the caller's thread is the audio server's too: blocked 10-15 ms a 4K
     * frame, the player's audio stopped being taken and YouTube paused every ~20 s); a few queued at most */
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    pthread_t worker;
    bool worker_running, worker_stop;
    struct job *queue_head, *queue_tail;
    unsigned queued;
    int fd;
    void *session;
    uint8_t *headers;
    size_t headers_size;
    int nal_length;
    bool real_time;
    bool stateful;
    bool headers_sent;
    /* The picture: what is shown, and the kernel's copy of the coded frame (luma stride x rows, then chroma). */
    unsigned width, height, stride, rows, max_width, max_height;
    unsigned out_width, out_height;   /* the kernel's copy of each picture: HEVC and VP9 above 1080p, half each way */
    unsigned pix_width, pix_height;   /* the pixel buffers handed out: the copy's size, or the picture's (doubled) */
    uint8_t *output;
    uint64_t hardware_us, copy_us, timed, timed_since, last_call_us;
    /* the last picture: its slot and generation (the video plane's token), and whether only the plane has it */
    uint32_t token_slot, token_generation;
    bool token, plane_only;   /* debug log: the kernel's decode, and the copy into the pixel buffer */
    bool quarter;             /* VP9 above 1440p for a player that names no size: copies a quarter each way (TWL_VP9_QUARTER) */
    bool sized;               /* the player names a picture size (Kodi) */
    unsigned skipped_nals;    /* HEVC NAL units of the unspecified types left out (Dolby Vision's) */
    OSType packed10;          /* 'p420'/'pf20' when the client takes only Apple's packed 10-bit 4:2:0 (tvOS's screensaver);
                               * its copies are always made: an empty one (left so while the plane showed the pictures)
                               * was a fullscreen green frame as the screensaver went away */
    struct twl_h264_dpb *dpb;
    /* HEVC sessions (the kernel's TWL_VDEC_HEVC): their DPB, and whether the kernel has their pictures' layout */
    bool hevc, hevc_capable, hevc_stream;
    struct twl_hevc_dpb *hdpb;
    /* VP9 sessions (TWL_VDEC_VP9): their reference map */
    bool vp9, vp9_capable, vp9_stream;
    struct twl_vp9_dpb *vdpb;
    CMFormatDescriptionRef format;
    /* the last pictures handed to VideoToolbox, still held here (KEPT_PICTURES) */
    CVPixelBufferRef kept[8];
    bool pq;    /* the stream is PQ (HDR10): the video plane's token says so */
    unsigned kept_at;
};
/*
 * VideoToolbox may deliver a picture to the player some frames after it was emitted (the first of a session above
 * all), and the pixel buffer pool meanwhile gave the same buffer out again: the next picture was copied over it and the
 * player got that one twice (a 1080p HEVC stream's first frame showed its third). So the last few emitted pictures
 * stay retained and counted in use here until as many more have been emitted.
 */
#define KEPT_PICTURES 6u


/* a copy row doubled across: luma pixels each twice, or chroma's interleaved Cb Cr pairs each twice; `bytes` out */
static void
double_row(uint8_t *to, const uint8_t *from, unsigned bytes, bool chroma)
{
    unsigned at = 0;
    if (chroma) {
        const uint16_t *src = (const uint16_t *)from;
        uint16_t *dst = (uint16_t *)to;
        unsigned pairs = bytes / 2;
        for (; at + 16 <= pairs; at += 16) {
            uint16x8_t v = vld1q_u16(src + at / 2);
            uint16x8x2_t two = { { v, v } };
            vst2q_u16(dst + at, two);
        }
        for (; at < pairs; at++) dst[at] = src[at / 2];
    } else {
        for (; at + 32 <= bytes; at += 32) {
            uint8x16_t v = vld1q_u8(from + at / 2);
            uint8x16x2_t two = { { v, v } };
            vst2q_u8(to + at, two);
        }
        for (; at < bytes; at++) to[at] = from[at / 2];
    }
}

static void
keep_picture(struct decoder *d, CVPixelBufferRef pixel)
{
    unsigned at = d->kept_at++ % KEPT_PICTURES;
    CVPixelBufferRef old = d->kept[at];
    d->kept[at] = pixel ? (CVPixelBufferRef)CFRetain(pixel) : NULL;
    if (pixel && CVPixelBufferGetIOSurface(pixel)) IOSurfaceIncrementUseCount(CVPixelBufferGetIOSurface(pixel));
    if (old) {
        if (CVPixelBufferGetIOSurface(old)) IOSurfaceDecrementUseCount(CVPixelBufferGetIOSurface(old));
        CFRelease(old);
    }
}

static void
release_kept(struct decoder *d)
{
    for (unsigned i = 0; i < KEPT_PICTURES; i++) {
        CVPixelBufferRef old = d->kept[i];
        d->kept[i] = NULL;
        if (old) {
            if (CVPixelBufferGetIOSurface(old)) IOSurfaceDecrementUseCount(CVPixelBufferGetIOSurface(old));
            CFRelease(old);
        }
    }
}
typedef OSStatus (*factory_fn)(FourCharCode, CFAllocatorRef, void **);
static OSStatus (*derived_create)(CFAllocatorRef, const void *, const void *, void **);
static void *(*derived_storage)(void *);
static const void *(*decoder_class_id)(void);
static OSStatus (*register_decoder)(FourCharCode, CFDictionaryRef, factory_fn);
static OSStatus (*set_attributes)(void *, CFDictionaryRef);
static CVPixelBufferPoolRef (*get_pool)(void *);
static CFDictionaryRef (*get_destination)(void *);       /* what the client asked VideoToolbox for (may be absent) */
static OSStatus (*emit_frame)(void *, void *, OSStatus, VTDecodeInfoFlags, CVImageBufferRef);
static bool symbols_ready;

static uint64_t
now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* touch /var/tmp/twlvdec-debug to have sessions logged in /var/tmp/twlvdec.log (the decode service has no console) */
static void
debug(const char *format, ...)
{
    if (access("/var/tmp/twlvdec-debug", F_OK)) return;
    FILE *f = fopen("/var/tmp/twlvdec.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, format);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(f, "%d %ld.%03ld: ", getpid(), (long)t.tv_sec, t.tv_nsec / 1000000);
    vfprintf(f, format, args);
    va_end(args);
    fclose(f);
}

/* a failure: to standard error, and to the session log when there is one */
#define report(...) do { fprintf(stderr, "twlvdec: " __VA_ARGS__); debug(__VA_ARGS__); } while (0)

static bool
load_symbols(void)
{
    if (symbols_ready) return true;
    derived_create = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
    derived_storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
    decoder_class_id = dlsym(RTLD_DEFAULT, "VTVideoDecoderGetClassID");
    register_decoder = dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoderWithInfo");
    set_attributes = dlsym(RTLD_DEFAULT, "VTDecoderSessionSetPixelBufferAttributes");
    get_pool = dlsym(RTLD_DEFAULT, "VTDecoderSessionGetPixelBufferPool");
    get_destination = dlsym(RTLD_DEFAULT, "VTDecoderSessionGetDestinationPixelBufferAttributes");
    emit_frame = dlsym(RTLD_DEFAULT, "VTDecoderSessionEmitDecodedFrame");
    symbols_ready = derived_create && derived_storage && decoder_class_id && register_decoder &&
                    set_attributes && get_pool && emit_frame;
    return symbols_ready;
}

static void worker_stop(struct decoder *d);

static OSStatus
invalidate(void *object)
{
    struct decoder *d = derived_storage(object);
    debug("invalidate\n");
    worker_stop(d);
    if (d->mutex_ready) pthread_mutex_lock(&d->mutex);
    twl_h264_dpb_destroy(d->dpb);
    twl_hevc_dpb_destroy(d->hdpb);
    twl_vp9_dpb_destroy(d->vdpb);
    d->hdpb = NULL;
    d->vdpb = NULL;
    d->dpb = NULL;
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
    d->session = NULL;
    release_kept(d);
    if (d->mutex_ready) pthread_mutex_unlock(&d->mutex);
    return noErr;
}

static void
finalize(void *object)
{
    struct decoder *d = derived_storage(object);
    invalidate(object);
    free(d->headers);
    d->headers = NULL;
    free(d->output);
    d->output = NULL;
    if (d->format) CFRelease(d->format);
    d->format = NULL;
    if (d->mutex_ready) {
        pthread_mutex_destroy(&d->mutex);
        d->mutex_ready = false;
    }
}

static CFStringRef
copy_description(void *object)
{
    (void)object;
    return CFStringCreateCopy(NULL, CFSTR("Twilight Amlogic H.264 decoder"));
}

static OSStatus
copy_property(void *object, CFStringRef key, CFAllocatorRef allocator, CFTypeRef *out)
{
    (void)object; (void)allocator;
    if (!out) return kVTParameterErr;
    *out = NULL;
    if (CFEqual(key, CFSTR("UsingHardwareAcceleratedVideoDecoder"))) {
        *out = CFRetain(kCFBooleanTrue);
        return noErr;
    }
    if (CFEqual(key, kVTDecompressionPropertyKey_RealTime)) {
        struct decoder *d = derived_storage(object);
        pthread_mutex_lock(&d->mutex);
        bool real_time = d->real_time;
        pthread_mutex_unlock(&d->mutex);
        *out = CFRetain(real_time ? kCFBooleanTrue : kCFBooleanFalse);
        return noErr;
    }
    return kVTPropertyNotSupportedErr;
}

static OSStatus
set_property(void *object, CFStringRef key, CFTypeRef value)
{
    if (CFEqual(key, kVTDecompressionPropertyKey_RealTime)) {
        if (!value || CFGetTypeID(value) != CFBooleanGetTypeID()) return kVTParameterErr;
        struct decoder *d = derived_storage(object);
        pthread_mutex_lock(&d->mutex);
        d->real_time = CFBooleanGetValue(value);
        pthread_mutex_unlock(&d->mutex);
        return noErr;
    }
    return kVTPropertyNotSupportedErr;
}

static void
number(CFMutableDictionaryRef dictionary, CFStringRef key, int value)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &value);
    if (n) { CFDictionarySetValue(dictionary, key, n); CFRelease(n); }
}

static OSStatus
start_session_locked(void *object, void *session, CMVideoFormatDescriptionRef format)
{
    struct decoder *d = derived_storage(object);
    CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
    FourCharCode codec = CMFormatDescriptionGetMediaSubType(format);
    bool hevc = codec == kCMVideoCodecType_HEVC, vp9 = codec == kCMVideoCodecType_VP9;
    if ((codec != kCMVideoCodecType_H264 && !(hevc && d->hevc_capable && d->stateful) &&
         !(vp9 && d->vp9_capable && d->stateful)) || dimensions.width < 16 ||
        dimensions.height < 16 || (!d->stateful && (dimensions.width != 64 || dimensions.height != 64)) ||
        (unsigned)dimensions.width * (unsigned)dimensions.height > d->max_width * d->max_height ||
        (unsigned)(dimensions.width > dimensions.height ? dimensions.width : dimensions.height) > d->max_width ||
        (unsigned)(dimensions.width > dimensions.height ? dimensions.height : dimensions.width) > d->max_height)
        return kVTVideoDecoderUnsupportedDataFormatErr;
    /* (diagnosis: touch /var/tmp/twlvdec-max1080 to refuse sessions above 1080p) */
    if ((dimensions.width > 1920 || dimensions.height > 1088) && !access("/var/tmp/twlvdec-max1080", F_OK)) {
        debug("session %dx%d refused (twlvdec-max1080)\n", dimensions.width, dimensions.height);
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    d->width = (unsigned)dimensions.width;
    d->height = (unsigned)dimensions.height;
    d->hevc = hevc;
    d->vp9 = vp9;
    /* the kernel's frame: H.264 in macroblocks, HEVC and VP9 in 64-pixel units */
    /* above 1080p the kernel keeps HEVC's and VP9's copies at half the size each way (the plane shows the compressed
     * picture, hakios.hevc.c, hakios.vp9.c): the pixel buffers are that size, a quarter of the memory (unless the
     * kernel's dw_full test switch asks for full-size copies) */
    int full = 0;
    size_t full_size = sizeof(full);
    if ((hevc || vp9) && sysctlbyname(hevc ? "kern.hakios_hevc_dw_full" : "kern.hakios_vp9_dw_full", &full, &full_size, NULL, 0))
        full = 0;
    unsigned asked_w = 0, asked_h = 0;
    CFDictionaryRef destination = get_destination ? get_destination(session) : NULL;
    if (destination) {
        CFNumberRef n = CFDictionaryGetValue(destination, kCVPixelBufferWidthKey);
        if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberIntType, &asked_w);
        n = CFDictionaryGetValue(destination, kCVPixelBufferHeightKey);
        if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberIntType, &asked_h);
    }
    unsigned scale = (hevc || vp9) && full != 1 && (d->width > 1920 || d->height > 1088) ? 2 : 1;   /* (2: at most half) */
    /* a player that names no size shows the plane's picture; its own copy (seen only while the window server draws the
     * video under the interface) a quarter each way: 24 slots of 4K at half were 75 MiB the compressed bodies needed */
    d->sized = asked_w || asked_h;
    d->quarter = vp9 && !full && !asked_w && !asked_h && (d->width > 2560 || d->height > 1440);
    if (d->quarter) scale = 4;
    d->out_width = d->width / scale;
    d->out_height = d->height / scale;
    /* A client that asks for pictures of the stream's size (Kodi, through FFmpeg) had VideoToolbox scale each half-size
     * one itself, on the GPU (VTMetalTransferSession, no M2 scaler here): over a second a 4K picture. For such a client
     * the copy is doubled into a full-size buffer here instead (NEON, a few ms); players that name no size
     * (AVSampleBufferDisplayLayer's, which show the plane's full picture) keep the small buffers. */
    bool doubled = scale == 2 && (asked_w > d->out_width || asked_h > d->out_height);
    unsigned asked_format = 0;
    d->packed10 = 0;
    if (destination) {
        CFTypeRef f = CFDictionaryGetValue(destination, kCVPixelBufferPixelFormatTypeKey);
        if (f && CFGetTypeID(f) == CFNumberGetTypeID()) {
            CFNumberGetValue(f, kCFNumberIntType, &asked_format);
            d->packed10 = asked_format == 'p420' || asked_format == 'pf20' ? asked_format : 0;
        } else if (f && CFGetTypeID(f) == CFArrayGetTypeID() && CFArrayGetCount(f) > 0) {
            CFNumberRef n = CFArrayGetValueAtIndex(f, 0);
            if (CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue(n, kCFNumberIntType, &asked_format);
            /* (diagnosis: every format the client would take, in its order) */
            char list[160] = "";
            for (CFIndex k = 0; k < CFArrayGetCount(f) && k < 24; k++) {
                unsigned v = 0;
                CFNumberRef e = CFArrayGetValueAtIndex(f, k);
                if (CFGetTypeID(e) == CFNumberGetTypeID()) CFNumberGetValue(e, kCFNumberIntType, &v);
                size_t at = strlen(list);
                snprintf(list + at, sizeof(list) - at, v > 0xffffff ? " %c%c%c%c" : " %u", v >> 24, v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff);
            }
            debug("the client takes:%s\n", list);
            /* a client that takes no 8-bit 4:2:0 and does take the packed 10-bit one gets that (VideoToolbox's own
             * conversion to it gave the screensaver nothing: every picture was dropped and the screen stayed black) */
            bool eight = false;
            OSType packed = 0;
            for (CFIndex k = 0; k < CFArrayGetCount(f); k++) {
                unsigned v = 0;
                CFNumberRef e = CFArrayGetValueAtIndex(f, k);
                if (CFGetTypeID(e) == CFNumberGetTypeID()) CFNumberGetValue(e, kCFNumberIntType, &v);
                if (v == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || v == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) eight = true;
                if (!packed && (v == 'p420' || v == 'pf20')) packed = v;
            }
            d->packed10 = eight ? 0 : packed;
        }
    }
    CFTypeRef transfer = CMFormatDescriptionGetExtension(format, kCVImageBufferTransferFunctionKey);
    d->pq = transfer && CFEqual(transfer, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
    debug("the client asks for format '%c%c%c%c'; the stream is %s\n", asked_format >> 24, asked_format >> 16 & 0xff,
        asked_format >> 8 & 0xff, asked_format & 0xff, d->pq ? "PQ (HDR10)" : "not PQ");
    debug("the client asks for %ux%u: pictures %s\n", asked_w, asked_h, doubled ? "doubled to full size" :
        scale == 4 ? "a quarter size" : scale == 2 ? "half size" : "full size");
    d->pix_width = doubled ? d->width : d->out_width;
    d->pix_height = doubled ? d->height : d->out_height;
    d->rows = hevc || vp9 ? (d->out_height + 63) & ~63u : (d->height + 15) & ~15u;
    d->stride = hevc || vp9 ? (d->out_width + 63) & ~63u : (((d->width + 15) & ~15u) + 63) & ~63u;
    free(d->output);
    d->output = malloc((size_t)d->stride * d->rows * 3 / 2);
    CFDictionaryRef aperture = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_CleanAperture);
    debug("session %ux%u (stride %u, rows %u), clean aperture %s\n", d->width, d->height, d->stride, d->rows,
        aperture ? "given" : "none");
    if (!d->output) return kVTAllocationFailedErr;
    twl_h264_dpb_destroy(d->dpb);
    twl_hevc_dpb_destroy(d->hdpb);
    twl_vp9_dpb_destroy(d->vdpb);
    d->dpb = d->stateful && !hevc && !vp9 ? twl_h264_dpb_create() : NULL;
    d->hdpb = hevc ? twl_hevc_dpb_create() : NULL;
    d->vdpb = vp9 ? twl_vp9_dpb_create() : NULL;
    d->hevc_stream = d->vp9_stream = false;
    d->headers_sent = false;
    if (d->stateful && !d->dpb && !d->hdpb && !d->vdpb) return kVTAllocationFailedErr;
    if (d->stateful && ioctl(d->fd, TWL_VDEC_CANCEL) < 0) return kVTVideoDecoderMalfunctionErr;
    if (vp9) {
        /* no parameter sets: every frame carries its header */
        free(d->headers);
        d->headers = calloc(1, 1);
        d->headers_size = 0;
        d->nal_length = 0;
        if (!d->headers) return kVTAllocationFailedErr;
        goto described;
    }
    const uint8_t *parameter;
    size_t size = 0, count = 0, total = 0;
    int length = 0;
    OSStatus status = hevc ?
        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, &parameter, &size, &count, &length) :
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &parameter, &size, &count, &length);
    if (status || !count || count > 64 || length < 1 || length > 4) return kVTVideoDecoderBadDataErr;
    uint8_t *headers = malloc(PACKET_MAX);
    if (!headers) return kVTAllocationFailedErr;
    for (size_t i = 0; i < count; i++) {
        status = hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &parameter, &size, NULL, NULL) :
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &parameter, &size, NULL, NULL);
        if (status || !size || total > PACKET_MAX - 4 || size > PACKET_MAX - 4 - total) {
            free(headers);
            return kVTVideoDecoderBadDataErr;
        }
        memcpy(headers + total, "\0\0\0\1", 4); total += 4;
        memcpy(headers + total, parameter, size); total += size;
    }
    free(d->headers);
    d->headers = headers;
    d->headers_size = total;
    d->nal_length = length;
described:
    if (d->format) CFRelease(d->format);
    d->format = (CMFormatDescriptionRef)CFRetain(format);
    d->session = session;
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(NULL, NULL, NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!attributes || !surface) {
        if (attributes) CFRelease(attributes);
        if (surface) CFRelease(surface);
        return kVTAllocationFailedErr;
    }
    CFTypeRef full_range = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FullRangeVideo);
    if (d->pix_width != d->out_width) d->packed10 = 0;      /* (the doubled copy is 8-bit only) */
    OSType pixel_format = d->packed10 ? d->packed10 : full_range == kCFBooleanTrue ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange :
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if (d->packed10) debug("pictures as '%c%c%c%c' (packed 10-bit)\n", d->packed10 >> 24, d->packed10 >> 16 & 0xff,
        d->packed10 >> 8 & 0xff, d->packed10 & 0xff);
    number(attributes, kCVPixelBufferPixelFormatTypeKey, pixel_format);
    number(attributes, kCVPixelBufferWidthKey, (int)d->pix_width);
    number(attributes, kCVPixelBufferHeightKey, (int)d->pix_height);
    number(attributes, kCVPixelBufferBytesPerRowAlignmentKey, 64);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    status = set_attributes(session, attributes);
    CFRelease(surface);
    CFRelease(attributes);
    fprintf(stderr, "twlvdec: start %ux%u %s, %zu header bytes, NAL length %d: %d\n", d->width, d->height,
            vp9 ? "VP9" : hevc ? "HEVC" : "H.264", d->headers_size, d->nal_length, status);
    return status;
}

static OSStatus
start_session(void *object, void *session, CMVideoFormatDescriptionRef format)
{
    struct decoder *d = derived_storage(object);
    if (!d->mutex_ready) return kVTInvalidSessionErr;
    pthread_mutex_lock(&d->mutex);
    OSStatus status = start_session_locked(object, session, format);
    pthread_mutex_unlock(&d->mutex);
    return status;
}

static void
copy_color_attachments(struct decoder *d, CVPixelBufferRef pixel)
{
    /* Preserve metadata supplied by the format description; do not invent color/HDR values. */
    CFDictionaryRef extensions = CMFormatDescriptionGetExtensions(d->format);
    const CFStringRef keys[] = { kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey,
        kCVImageBufferYCbCrMatrixKey, CFSTR("MasteringDisplayColorVolume"), CFSTR("ContentLightLevelInfo") };
    for (unsigned i = 0; extensions && i < sizeof(keys) / sizeof(keys[0]); i++) {
        CFTypeRef value = CFDictionaryGetValue(extensions, keys[i]);
        if (value) CVBufferSetAttachment(pixel, keys[i], value, kCVAttachmentMode_ShouldPropagate);
    }
}

static OSStatus
decode_reference_picture(struct decoder *d, const uint8_t *packet, size_t size, uint8_t *output)
{
    uint16_t parameters[TWL_H264_RPM_WORDS];
    struct twl_h264_prepare request = { (uintptr_t)packet, (uintptr_t)parameters, (uint32_t)size, sizeof(parameters) };
    struct twl_h264_config config;
    if (ioctl(d->fd, TWL_VDEC_PREPARE, &request) < 0) {
        fprintf(stderr, "twlvdec: prepare ioctl failed: %d\n", errno); goto failed;
    }
    struct twl_vdec_stream stream;
    if (ioctl(d->fd, TWL_VDEC_STREAM, &stream) < 0) {
        fprintf(stderr, "twlvdec: stream ioctl failed: %d\n", errno); goto failed;
    }
    if (!twl_h264_dpb_started(d->dpb) && twl_h264_dpb_set_slots(d->dpb, stream.slots)) {
        fprintf(stderr, "twlvdec: %u slots for the stream\n", stream.slots); goto failed;
    }
    twl_h264_dpb_set_held(d->dpb, stream.held, stream.showing);
    /* while the video plane shows this decoder's pictures, nobody needs their pixels (twlcvgl.c) */
    d->plane_only = stream.plane != 0 && !d->packed10;
    if (stream.stride != d->stride || stream.rows != d->rows) {
        debug("kernel frame %ux%u (stride %u, rows %u) is not the session's (stride %u, rows %u)\n", stream.width,
            stream.height, stream.stride, stream.rows, d->stride, d->rows);
        goto failed;
    }
    if (twl_h264_dpb_prepare(d->dpb, parameters, &config)) {
        fprintf(stderr, "twlvdec: reference-header validation failed\n"); goto failed;
    }
    for (unsigned slices = 1;; slices++) {
        config.output = (uintptr_t)output;
        config.output_size = d->stride * d->rows * 3 / 2;
        config.rpm = (uintptr_t)parameters;
        if (d->plane_only) config.flags |= TWL_H264_NOCOPY;
        if (ioctl(d->fd, TWL_VDEC_FINISH, &config) == 0) break;
        if (errno != EINPROGRESS || slices >= 1024) {
            fprintf(stderr, "twlvdec: finish ioctl failed: %d (slice %u)\n", errno, slices); goto failed;
        }
        /* the picture's next slice */
        if (twl_h264_dpb_prepare(d->dpb, parameters, &config) || !(config.flags & TWL_H264_SLICE)) {
            fprintf(stderr, "twlvdec: slice %u header validation failed\n", slices + 1); goto failed;
        }
    }
    if (twl_h264_dpb_complete(d->dpb)) {
        fprintf(stderr, "twlvdec: reference-picture completion failed\n"); goto failed;
    }
    d->token = true;
    d->token_slot = config.current_slot;
    d->token_generation = config.generation;
    return noErr;
failed:
    d->token = false;
    ioctl(d->fd, TWL_VDEC_CANCEL);
    twl_h264_dpb_destroy(d->dpb);
    d->dpb = twl_h264_dpb_create();
    d->headers_sent = false;
    return d->dpb ? kVTVideoDecoderBadDataErr : kVTAllocationFailedErr;
}

/* one HEVC access unit through the kernel (hakios.hevc.c) and the DPB (tvos/video/hevc); *shown false: nothing to
 * show (parameter sets only, or a leading picture whose references the stream does not have) */
static OSStatus
decode_hevc_picture(struct decoder *d, const uint8_t *packet, size_t size, uint8_t *output, bool *shown)
{
    uint16_t rpm[TWL_HEVC_RPM_WORDS];
    struct twl_h264_prepare request = { (uintptr_t)packet, (uintptr_t)rpm, (uint32_t)size, sizeof(rpm) };
    struct twl_hevc_slice slice;
    *shown = false;
    if (ioctl(d->fd, TWL_HEVC_PREPARE, &request) < 0) {
        if (errno == ENODATA) return noErr;
        fprintf(stderr, "twlvdec: HEVC prepare failed: %d\n", errno); goto failed;
    }
    struct twl_vdec_stream stream;
    if (ioctl(d->fd, TWL_VDEC_STREAM, &stream) < 0) {
        fprintf(stderr, "twlvdec: stream ioctl failed: %d\n", errno); goto failed;
    }
    if (!d->hevc_stream) {
        if (twl_hevc_dpb_set_slots(d->hdpb, stream.slots)) goto failed;
        debug("HEVC stream %ux%u, %u slots (stride %u, rows %u)\n", stream.width, stream.height, stream.slots,
            stream.stride, stream.rows);
        d->hevc_stream = true;
    }
    twl_hevc_dpb_set_held(d->hdpb, stream.held, stream.showing);
    d->plane_only = stream.plane != 0 && !d->packed10;
    /* a player that says nothing of the transfer function (Kodi) gets the bitstream's: PQ in the VUI is HDR10 (and Dolby
     * Vision 8.1's base layer), and the plane sends it as such */
    if (!d->pq && stream.transfer == 16) {
        d->pq = true;
        debug("the bitstream says PQ (HDR10)\n");
    }
    if (stream.stride != d->stride || stream.rows != d->rows) {
        debug("kernel frame %ux%u (stride %u, rows %u) is not the session's (stride %u, rows %u)\n", stream.width,
            stream.height, stream.stride, stream.rows, d->stride, d->rows);
        goto failed;
    }
    memset(&slice, 0, sizeof(slice));
    for (unsigned slices = 1;; slices++) {
        if (twl_hevc_dpb_slice(d->hdpb, rpm, &slice)) {
            fprintf(stderr, "twlvdec: HEVC slice %u refused by the DPB\n", slices); goto failed;
        }
        slice.output = (uintptr_t)output;
        slice.output_size = d->stride * d->rows * 3 / 2;
        slice.rpm = (uintptr_t)rpm;
        slice.flags = (slice.flags & TWL_HEVC_SKIP) | (d->plane_only ? TWL_HEVC_NOCOPY : 0);
        if (ioctl(d->fd, TWL_HEVC_SLICE, &slice) == 0) break;
        if (errno == ENODATA) {     /* skipped */
            twl_hevc_dpb_done(d->hdpb, false);
            return noErr;
        }
        if (errno != EINPROGRESS || slices >= 1024) {
            fprintf(stderr, "twlvdec: HEVC slice ioctl failed: %d (slice %u)\n", errno, slices); goto failed;
        }
    }
    twl_hevc_dpb_done(d->hdpb, true);
    d->token = true;
    d->token_slot = slice.slot;
    d->token_generation = slice.generation;
    *shown = true;
    return noErr;
failed:
    d->token = false;
    ioctl(d->fd, TWL_VDEC_CANCEL);
    twl_hevc_dpb_destroy(d->hdpb);
    d->hdpb = twl_hevc_dpb_create();
    d->hevc_stream = false;
    d->headers_sent = false;
    return d->hdpb ? kVTVideoDecoderBadDataErr : kVTAllocationFailedErr;
}

/* one VP9 packet (a superframe's frames in turn) through the kernel (hakios.vp9.c) and the reference map
 * (tvos/video/vp9); *shown false: nothing to show (a hidden frame alone, or one before the first keyframe) */
static OSStatus
decode_vp9_packet(struct decoder *d, const uint8_t *packet, size_t size, uint8_t *output, bool *shown)
{
    uint32_t offsets[8], sizes[8];
    unsigned frames = twl_vp9_split(packet, (uint32_t)size, offsets, sizes);
    *shown = false;
    if (!frames) { report("VP9 superframe index damaged\n"); goto failed; }
    for (unsigned k = 0; k < frames; k++) {
        uint16_t rpm[TWL_VP9_RPM_WORDS];
        struct twl_h264_prepare request = { (uintptr_t)(packet + offsets[k]), (uintptr_t)rpm, sizes[k], sizeof(rpm) };
        if (ioctl(d->fd, TWL_VP9_PREPARE, &request) < 0) {
            if (errno == ENODATA) continue;
            report("VP9 prepare failed: %d\n", errno); goto failed;
        }
        struct twl_vdec_stream stream;
        if (d->vp9_stream && ioctl(d->fd, TWL_VDEC_STREAM, &stream) == 0) {
            twl_vp9_dpb_set_held(d->vdpb, stream.held, stream.showing);
            d->plane_only = stream.plane != 0 && !d->packed10;
        }
        struct twl_vp9_frame f;
        bool frame_shown;
        memset(&f, 0, sizeof(f));
        enum twl_vp9_action action = twl_vp9_dpb_frame(d->vdpb, rpm, &f, &frame_shown);
        f.output = (uintptr_t)output;
        f.output_size = d->stride * d->rows * 3 / 2;
        if (d->plane_only && action != TWL_VP9_DROP) f.flags |= TWL_VP9_NOCOPY;
        if (d->quarter) f.flags |= TWL_VP9_QUARTER;
        int r = ioctl(d->fd, TWL_VP9_FRAME, &f) < 0 ? errno : 0;
        if (action == TWL_VP9_DECODE) twl_vp9_dpb_done(d->vdpb, r == 0);
        if (action == TWL_VP9_DROP) continue;
        if (r) { report("VP9 frame failed: %d\n", r); goto failed; }
        if (!d->vp9_stream && ioctl(d->fd, TWL_VDEC_STREAM, &stream) == 0) {
            if (twl_vp9_dpb_set_slots(d->vdpb, stream.slots)) goto failed;
            debug("VP9 stream %ux%u, %u slots (stride %u, rows %u)\n", stream.width, stream.height, stream.slots,
                stream.stride, stream.rows);
            if (stream.stride != d->stride || stream.rows != d->rows) {
                debug("kernel frame (stride %u, rows %u) is not the session's (stride %u, rows %u)\n", stream.stride,
                    stream.rows, d->stride, d->rows);
                goto failed;
            }
            d->vp9_stream = true;
        }
        if (frame_shown) {
            d->token = true;
            d->token_slot = f.slot;
            d->token_generation = f.generation;
            *shown = true;
        }
    }
    return noErr;
failed:
    d->token = false;
    ioctl(d->fd, TWL_VDEC_CANCEL);
    twl_vp9_dpb_reset(d->vdpb);
    d->vp9_stream = false;
    return kVTVideoDecoderBadDataErr;
}

static OSStatus
decode_frame_now(void *object, void *frame, CMSampleBufferRef sample, VTDecodeFrameFlags flags,
             VTDecodeInfoFlags *info)
{
    struct decoder *d = derived_storage(object);
    uint64_t entered = now_us();
    if (info) *info = 0;
    if (!d->mutex_ready) return kVTInvalidSessionErr;
    /* (a gap in the player's frames, or a call that takes long: when YouTube stalls, which side waits) */
    if (d->last_call_us && entered - d->last_call_us > 500000)
        debug("no frame for %llu ms\n", (entered - d->last_call_us) / 1000);
    pthread_mutex_lock(&d->mutex);
    if (now_us() - entered > 50000) debug("decode_frame waited %llu ms for the session's lock\n", (now_us() - entered) / 1000);
    if (!d->session || !d->headers || d->fd < 0) {
        pthread_mutex_unlock(&d->mutex);
        return kVTInvalidSessionErr;
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t size = block ? CMBlockBufferGetDataLength(block) : 0;
    uint8_t *source = NULL, *packet = NULL;
    CVPixelBufferRef pixel = NULL;
    uint8_t *output = d->output;
    OSStatus status = kVTVideoDecoderBadDataErr;
    if (!size || d->headers_size > PACKET_MAX - 6 || size > PACKET_MAX - d->headers_size - 6) goto done;
    source = malloc(size);
    packet = malloc(PACKET_MAX);
    if (!source || !packet) { status = kVTAllocationFailedErr; goto done; }
    status = CMBlockBufferCopyDataBytes(block, 0, size, source);
    if (status) goto done;
    uint64_t started = now_us();
    if (d->vp9) {
        bool shown;
        status = decode_vp9_packet(d, source, size, output, &shown);
        if (status) goto done;
        if (!shown) {
            flags |= kVTDecodeFrame_DoNotOutputFrame;
            goto done;
        }
        goto decoded_picture;
    }
    /* Make the submitted access-unit boundary explicit for frame-mode input. */
    static const uint8_t aud[] = { 0, 0, 0, 1, 9, 0x50 }; /* primary_pic_type 2: I/P/B */
    size_t aud_size = d->hevc ? 0 : sizeof(aud);    /* HEVC units are framed by the kernel's stream setup */
    memcpy(packet, aud, aud_size);
    size_t header_size = d->stateful && d->headers_sent ? 0 : d->headers_size;
    memcpy(packet + aud_size, d->headers, header_size);
    size_t at = 0, used = aud_size + header_size;
    while (at < size) {
        if (size - at < (size_t)d->nal_length) { status = kVTVideoDecoderBadDataErr; goto done; }
        uint32_t n = 0;
        for (int i = 0; i < d->nal_length; i++) n = (n << 8) | source[at++];
        if (!n || n > size - at || used > PACKET_MAX - 4 || n > PACKET_MAX - used - 4) {
            status = kVTVideoDecoderBadDataErr; goto done;
        }
        /* HEVC's unspecified NAL types (48-63: Dolby Vision's RPU 62 and enhancement layer 63) are not for the core: fed to
         * it with a DV profile 8.1 file, the tops of pictures came out as coloured lines now and then (Kodi) */
        if (d->hevc && ((source[at] >> 1) & 0x3f) >= 48) {
            d->skipped_nals++;
            at += n;
            continue;
        }
        memcpy(packet + used, "\0\0\0\1", 4); used += 4;
        memcpy(packet + used, source + at, n); used += n; at += n;
    }
    started = now_us();
    if (d->hevc) {
        bool shown;
        status = decode_hevc_picture(d, packet, used, output, &shown);
        if (status) goto done;
        d->headers_sent = true;
        if (!shown) {
            flags |= kVTDecodeFrame_DoNotOutputFrame;      /* dropped, not failed */
            goto done;
        }
    } else if (d->stateful) {
        status = decode_reference_picture(d, packet, used, output);
        if (status) goto done;
        d->headers_sent = true;
    } else {
        struct twl_vdec_packet request = { (uintptr_t)packet, (uintptr_t)output, (uint32_t)used, 6144 };
        if (ioctl(d->fd, TWL_VDEC_DECODE, &request) < 0) { status = kVTVideoDecoderBadDataErr; goto done; }
    }
decoded_picture:;
    uint64_t decoded = now_us();
    CVPixelBufferPoolRef pool = get_pool(d->session);
    status = pool ? CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pixel) : kVTAllocationFailedErr;
    if (status) goto done;
    unsigned row_bytes = (d->pix_width + 1) & ~1u, chroma_rows = (d->pix_height + 1) / 2;
    /* packed 10-bit: three samples to a 32-bit word, luma and the interleaved Cb Cr alike */
    size_t luma_need = d->packed10 ? (d->pix_width + 2) / 3 * 4 : d->pix_width;
    size_t chroma_need = d->packed10 ? (row_bytes + 2) / 3 * 4 : row_bytes;
    if (CVPixelBufferGetPlaneCount(pixel) != 2 || CVPixelBufferGetWidthOfPlane(pixel, 0) != d->pix_width ||
        CVPixelBufferGetHeightOfPlane(pixel, 0) != d->pix_height || CVPixelBufferGetHeightOfPlane(pixel, 1) != chroma_rows ||
        CVPixelBufferGetBytesPerRowOfPlane(pixel, 0) < luma_need ||
        CVPixelBufferGetBytesPerRowOfPlane(pixel, 1) < chroma_need) {
        status = kVTVideoDecoderMalfunctionErr; goto done;
    }
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixel);
    if (surface) {
        /* the pool hands buffers out again: a token of an earlier picture must not stay on this one */
        if (d->stateful && d->token) {
            struct twl_vplane_token token = { TWL_VPLANE_MAGIC, d->token_slot, d->token_generation, d->width, d->height,
                d->pq ? TWL_VPLANE_PQ : 0 };
            CFDataRef value = CFDataCreate(NULL, (const UInt8 *)&token, sizeof(token));
            if (value) {
                if (d->timed == 0) debug("token slot %u generation %u on surface %u, plane only %d\n", d->token_slot,
                    d->token_generation, IOSurfaceGetID(surface), d->plane_only);
                IOSurfaceSetValue(surface, CFSTR(TWL_VPLANE_TOKEN_KEY), value);
                CFRelease(value);
            }
        } else {
            IOSurfaceRemoveValue(surface, CFSTR(TWL_VPLANE_TOKEN_KEY));
        }
    }
    if (d->stateful && d->plane_only && surface) goto copied;
    status = CVPixelBufferLockBaseAddress(pixel, 0);
    if (status) goto done;
    for (unsigned plane = 0; plane < 2; plane++) {
        uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pixel, plane);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, plane);
        /* the kernel's frame: luma stride x rows, then interleaved chroma, cropped to what is shown */
        unsigned rows = plane ? chroma_rows : d->pix_height, bytes_per_row = plane ? row_bytes : d->pix_width;
        const uint8_t *bytes = output + (plane ? (size_t)d->stride * d->rows : 0);
        if (d->packed10) {
            /* each 8-bit sample (the kernel's copy is the 10-bit picture rounded) to 10 bits, three to a little-endian
             * word: bits 0-9, 10-19, 20-29 */
            for (unsigned row = 0; row < rows; row++) {
                const uint8_t *from = bytes + (size_t)row * d->stride;
                uint32_t *to = (uint32_t *)(base + row * stride);
                unsigned k = 0;
                for (; k + 3 <= bytes_per_row; k += 3)
                    *to++ = (uint32_t)from[k] << 2 | (uint32_t)from[k + 1] << 12 | (uint32_t)from[k + 2] << 22;
                if (k < bytes_per_row)
                    *to = (uint32_t)from[k] << 2 | (k + 1 < bytes_per_row ? (uint32_t)from[k + 1] << 12 : 0);
            }
        } else if (d->pix_width == d->out_width) {
            for (unsigned row = 0; row < rows; row++)
                memcpy(base + row * stride, bytes + (size_t)row * d->stride, bytes_per_row);
        } else {
            unsigned src_rows = plane ? (d->out_height + 1) / 2 : d->out_height;
            for (unsigned row = 0; row < rows; row++) {
                uint8_t *to = base + row * stride;
                if (row & 1) {      /* each copy row twice */
                    memcpy(to, to - stride, bytes_per_row);
                    continue;
                }
                unsigned from_row = row / 2 < src_rows ? row / 2 : src_rows - 1;
                double_row(to, bytes + (size_t)from_row * d->stride, bytes_per_row, plane != 0);
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel, 0);
copied:
    copy_color_attachments(d, pixel);
    d->hardware_us += decoded - started;
    d->copy_us += now_us() - decoded;
    if (d->timed == 0) d->timed_since = started;
    if (++d->timed == 120) {
        uint64_t wall = now_us() - d->timed_since;
        debug("120 pictures %ux%u: %llu us each decoding, %llu us each copying; %.1f a second, %llu%% busy\n", d->width,
            d->height, d->hardware_us / 120, d->copy_us / 120, wall ? 120e6 / wall : 0.0,
            wall ? (d->hardware_us + d->copy_us) * 100 / wall : 0);
        d->timed = d->hardware_us = d->copy_us = 0;
        if (d->hevc && d->hdpb) {
            unsigned picks, forced, used, held;
            twl_hevc_dpb_stats(d->hdpb, &picks, &forced, &used, &held);
            debug("HEVC slots: %u of %u pictures into a held slot (then on average %u slots the DPB's, %u held); %u NAL units "
                "of types 48-63 left out\n", forced, picks, used, held, d->skipped_nals);
        }
    }
done:
    free(source);
    free(packet);
    if (status) debug("frame of %zu bytes failed: %d\n", size, (int)status);
    VTDecodeInfoFlags emitted_info = status || (flags & kVTDecodeFrame_DoNotOutputFrame) ? kVTDecodeInfo_FrameDropped : 0;
    void *session = d->session;
    /* Client callbacks can call back into VideoToolbox; do not hold the DPB lock during delivery. */
    pthread_mutex_unlock(&d->mutex);
    uint64_t emitting = now_us();
    emit_frame(session, frame, status, emitted_info, emitted_info ? NULL : pixel);
    uint64_t emitted = now_us();
    if (emitted - emitting > 50000) debug("the player's output callback took %llu ms\n", (emitted - emitting) / 1000);
    if (emitting - entered > 100000) debug("decode_frame took %llu ms (%zu bytes, status %d)\n", (emitting - entered) / 1000,
        size, (int)status);
    d->last_call_us = emitted;
    if (pixel) {
        if (!emitted_info) {
            pthread_mutex_lock(&d->mutex);
            keep_picture(d, pixel);
            pthread_mutex_unlock(&d->mutex);
        }
        CVPixelBufferRelease(pixel);
    }
    if (info) *info = emitted_info;
    fprintf(stderr, "twlvdec: hardware frame: %d\n", status);
    return noErr; /* EmitDecodedFrame owns delivery of the status for this frame. */
}

#define QUEUE_MOST 4

static void *
worker_main(void *object)
{
    struct decoder *d = derived_storage(object);
    pthread_mutex_lock(&d->queue_mutex);
    for (;;) {
        while (!d->queue_head && !d->worker_stop) pthread_cond_wait(&d->queue_cond, &d->queue_mutex);
        if (!d->queue_head) break;
        struct job *j = d->queue_head;
        d->queue_head = j->next;
        if (!d->queue_head) d->queue_tail = NULL;
        pthread_mutex_unlock(&d->queue_mutex);
        VTDecodeInfoFlags info = 0;
        decode_frame_now(object, j->frame, j->sample, j->flags, &info);
        CFRelease(j->sample);
        free(j);
        pthread_mutex_lock(&d->queue_mutex);
        d->queued--;
        pthread_cond_broadcast(&d->queue_cond);
    }
    pthread_mutex_unlock(&d->queue_mutex);
    return NULL;
}

/* stop the worker once the frames given to it are done (invalidate) */
static void
worker_stop(struct decoder *d)
{
    if (!d->worker_running) return;
    pthread_mutex_lock(&d->queue_mutex);
    d->worker_stop = true;
    pthread_cond_broadcast(&d->queue_cond);
    pthread_mutex_unlock(&d->queue_mutex);
    pthread_join(d->worker, NULL);
    d->worker_running = d->worker_stop = false;
}

static OSStatus
decode_frame(void *object, void *frame, CMSampleBufferRef sample, VTDecodeFrameFlags flags, VTDecodeInfoFlags *info)
{
    struct decoder *d = derived_storage(object);
    if (!d->stateful || !d->mutex_ready) return decode_frame_now(object, frame, sample, flags, info);
    if (!d->worker_running) {
        pthread_attr_t a;
        struct sched_param p = { .sched_priority = 47 };
        pthread_attr_init(&a);
        pthread_attr_setschedparam(&a, &p);
        d->worker_stop = false;
        d->worker_running = pthread_create(&d->worker, &a, worker_main, object) == 0;
        pthread_attr_destroy(&a);
        if (!d->worker_running) return decode_frame_now(object, frame, sample, flags, info);
    }
    struct job *j = calloc(1, sizeof(*j));
    if (!j) return decode_frame_now(object, frame, sample, flags, info);
    j->frame = frame;
    j->sample = (CMSampleBufferRef)CFRetain(sample);
    j->flags = flags;
    pthread_mutex_lock(&d->queue_mutex);
    while (d->queued >= QUEUE_MOST) pthread_cond_wait(&d->queue_cond, &d->queue_mutex);
    if (d->queue_tail) d->queue_tail->next = j; else d->queue_head = j;
    d->queue_tail = j;
    d->queued++;
    pthread_cond_broadcast(&d->queue_cond);
    pthread_mutex_unlock(&d->queue_mutex);
    if (info) *info = kVTDecodeInfo_Asynchronous;
    return noErr;
}

static OSStatus
supported_properties(void *object, CFDictionaryRef *out)
{
    (void)object;
    if (!out) return kVTParameterErr;
    const void *keys[] = { kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey };
    const void *values[] = { kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly };
    CFDictionaryRef description = CFDictionaryCreate(NULL, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!description) return kVTAllocationFailedErr;
    CFMutableDictionaryRef realtime = CFDictionaryCreateMutableCopy(NULL, 0, description);
    if (!realtime) { CFRelease(description); return kVTAllocationFailedErr; }
    CFDictionarySetValue(realtime, kVTPropertyReadWriteStatusKey, kVTPropertyReadWriteStatus_ReadWrite);
    const void *property_keys[] = { CFSTR("UsingHardwareAcceleratedVideoDecoder"), kVTDecompressionPropertyKey_RealTime };
    const void *property_values[] = { description, realtime };
    *out = CFDictionaryCreate(NULL, property_keys, property_values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(realtime);
    CFRelease(description);
    return *out ? noErr : kVTAllocationFailedErr;
}

struct base_class {
    uint64_t version, storage_size;
    void *equal;
    OSStatus (*invalidate)(void *);
    void (*finalize)(void *);
    CFStringRef (*description)(void *);
    OSStatus (*copy_property)(void *, CFStringRef, CFAllocatorRef, CFTypeRef *);
    OSStatus (*set_property)(void *, CFStringRef, CFTypeRef);
    void *reserved[3];
};
struct decoder_class {
    uint64_t version;
    OSStatus (*start)(void *, void *, CMVideoFormatDescriptionRef);
    OSStatus (*decode)(void *, void *, CMSampleBufferRef, VTDecodeFrameFlags, VTDecodeInfoFlags *);
    OSStatus (*supported)(void *, CFDictionaryRef *);
    void *reserved[6];
};
static const struct base_class base = { 1, sizeof(struct decoder), NULL, invalidate, finalize,
    copy_description, copy_property, set_property, { NULL, NULL, NULL } };
static const struct decoder_class codec = { 3, start_session, decode_frame, supported_properties, { NULL } };
static const void *vtable[] = { NULL, &base, &codec };

static OSStatus
create_decoder(FourCharCode type, CFAllocatorRef allocator, void **out)
{
    if (!out || (type != kCMVideoCodecType_H264 && type != kCMVideoCodecType_HEVC && type != kCMVideoCodecType_VP9))
        return kVTCouldNotFindVideoDecoderErr;
    *out = NULL;
    int fd = open("/dev/twl-vdec-probe", O_RDWR);
    struct twl_vdec_caps caps;
    if (fd < 0) { fprintf(stderr, "twlvdec: open failed: %d\n", errno); return kVTVideoDecoderNotAvailableNowErr; }
    if (ioctl(fd, TWL_VDEC_GET_CAPS, &caps) < 0 || caps.abi != TWL_VDEC_ABI || caps.width < 64 || caps.height < 64 ||
        (type == kCMVideoCodecType_HEVC && !(caps.flags & TWL_VDEC_HEVC)) ||
        (type == kCMVideoCodecType_VP9 && !(caps.flags & TWL_VDEC_VP9))) {
        fprintf(stderr, "twlvdec: caps check failed: %d\n", errno);
        close(fd); return kVTVideoDecoderNotAvailableNowErr;
    }
    OSStatus status = derived_create(allocator, vtable, decoder_class_id(), out);
    struct decoder *d = *out ? derived_storage(*out) : NULL;
    if (d) { memset(d, 0, sizeof(*d)); d->fd = -1; }
    if (status || !*out) {
        if (*out) CFRelease(*out);
        *out = NULL;
        close(fd);
        return status ? status : kVTAllocationFailedErr;
    }
    pthread_mutexattr_t attributes;
    int error = pthread_mutexattr_init(&attributes);
    if (!error) {
        error = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
        if (!error) error = pthread_mutex_init(&d->mutex, &attributes);
        pthread_mutexattr_destroy(&attributes);
    }
    if (!error) error = pthread_mutex_init(&d->queue_mutex, NULL);
    if (!error) error = pthread_cond_init(&d->queue_cond, NULL);
    if (error) {
        CFRelease(*out);
        *out = NULL;
        close(fd);
        return kVTAllocationFailedErr;
    }
    d->mutex_ready = true;
    d->fd = fd;
    d->stateful = (caps.flags & TWL_VDEC_STATEFUL_PROBE) != 0;
    d->hevc_capable = (caps.flags & TWL_VDEC_HEVC) != 0;
    d->vp9_capable = (caps.flags & TWL_VDEC_VP9) != 0;
    d->max_width = caps.width;
    d->max_height = caps.height;
    if ((type == kCMVideoCodecType_HEVC || type == kCMVideoCodecType_VP9) && d->max_width < 4096) {
        /* HEVC and VP9 pictures' size is the kernel's to check (it lays the pool out per stream) */
        d->max_width = 4096;
        d->max_height = 2304;
    }
    return noErr;
}

static OSStatus register_codec(FourCharCode codec, CFStringRef id);

static OSStatus
register_h264(void)
{
    return register_codec(kCMVideoCodecType_H264, CFSTR("com.twilight.vdec.h264"));
}

static OSStatus
register_codec(FourCharCode codec, CFStringRef id)
{
    if (!load_symbols()) { fprintf(stderr, "twlvdec: missing tvOS decoder ABI symbols\n"); return kVTCouldNotFindVideoDecoderErr; }
    CFMutableDictionaryRef info = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!info) return kVTAllocationFailedErr;
    CFDictionarySetValue(info, CFSTR("CMClassImplementationID"), id);
    const CFStringRef *hardware_key = dlsym(RTLD_DEFAULT, "kVTVideoDecoder_IsHardwareAccelerated");
    const CFStringRef *rating_key = dlsym(RTLD_DEFAULT, "kVTVideoDecoderInfo_Rating");
    CFDictionarySetValue(info, hardware_key ? *hardware_key : CFSTR("VTIsHardwareAccelerated"), kCFBooleanTrue);
    number(info, rating_key ? *rating_key : CFSTR("VTRating"), 1000);
    OSStatus status = register_decoder(codec, info, create_decoder);
    CFRelease(info);
    return status;
}

void
TwlVDECRegisterProbe(void)
{
    fprintf(stderr, "twlvdec: explicitly registered the H.264 decoder: %d\n", (int)register_h264());
    void *object = NULL;
    OSStatus factory_status = create_decoder(kCMVideoCodecType_H264, NULL, &object);
    fprintf(stderr, "twlvdec: factory self-test: %d\n", factory_status);
    if (object) CFRelease(object);
}

void
AVDRegister(void)
{
    /* only with the kernel's reference-picture transport; without it VideoToolbox keeps its software decoder.
     * The node is only looked at: opening it would take the one decoder from whoever has it. */
    if (access("/dev/twl-vdec-probe", F_OK) != 0) return;
    OSStatus status = register_h264();
    if (status) fprintf(stderr, "twlvdec: registering the H.264 decoder failed: %d\n", (int)status);
    /* a kernel without HEVC makes creating one fail, and VideoToolbox falls back to its software decoder */
    status = register_codec(kCMVideoCodecType_HEVC, CFSTR("com.twilight.vdec.hevc"));
    if (status) fprintf(stderr, "twlvdec: registering the HEVC decoder failed: %d\n", (int)status);
    status = register_codec(kCMVideoCodecType_VP9, CFSTR("com.twilight.vdec.vp9"));
    if (status) fprintf(stderr, "twlvdec: registering the VP9 decoder failed: %d\n", (int)status);
}

#ifdef TWL_VDEC_PLAYER_PROBE
/* Built separately and injected only into the explicitly selected test player. */
__attribute__((constructor)) static void
player_probe(void)
{
    OSStatus (*restrictions)(uint32_t) = dlsym(RTLD_DEFAULT, "VTApplyRestrictions");
    if (!restrictions || restrictions(1)) {
        fprintf(stderr, "twlvdec: could not select in-process test decoding\n");
        return;
    }
    TwlVDECRegisterProbe();
}
#endif
