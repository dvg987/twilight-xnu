/* Public VideoToolbox API test: decode an AVCC sample sequence and compare every output byte.
 * TWV1 packets: 64x64, compared with a packed NV12 file in decode order. TWV2 packets (tools/vdec-real-fixture.py):
 * any size, timestamped, compared by presentation time with a "PTS MD5" list. TWV3 (tools/hevc-fixture.py): the same
 * for HEVC, with a VPS before the SPS and PPS. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <IOSurface/IOSurfaceRef.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct result {
    const unsigned char *reference;
    size_t reference_size;
    unsigned frames, failures, width, height;
    unsigned count;                 /* TWV2: frames with an MD5 */
    int64_t *pts;
    unsigned char (*md5)[16];
    bool verbose;
};
static int
hexdigit(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static void
output_md5(struct result *r, size_t index, CVImageBufferRef image, CMTime pts)
{
    unsigned w = r->width, h = r->height, cw = (w + 1) & ~1u, ch = (h + 1) / 2;
    if (CVPixelBufferGetWidth(image) != w || CVPixelBufferGetHeight(image) != h) {
        fprintf(stderr, "vtprobe: frame %zu is %zux%zu\n", index, CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image));
        r->failures++; return;
    }
    unsigned at = 0;
    while (at < r->count && r->pts[at] != pts.value) at++;
    if (at == r->count) { fprintf(stderr, "vtprobe: frame %zu: no reference for pts %lld\n", index, pts.value); r->failures++; return; }
    if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly)) { r->failures++; return; }
    CC_MD5_CTX md5;
    CC_MD5_Init(&md5);
    for (unsigned p = 0; p < 2; p++) {
        const unsigned char *base = CVPixelBufferGetBaseAddressOfPlane(image, p);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, p);
        for (unsigned y = 0; y < (p ? ch : h); y++) CC_MD5_Update(&md5, base + y * stride, p ? cw : w);
    }
    /* VTPROBE_DUMP=N: output frame N, cropped NV12, to /var/tmp/vtprobe.nv12 */
    const char *dump = getenv("VTPROBE_DUMP");
    if (dump && (size_t)atoi(dump) == index) {
        FILE *o = fopen("/var/tmp/vtprobe.nv12", "wb");
        for (unsigned p = 0; o && p < 2; p++) {
            const unsigned char *base = CVPixelBufferGetBaseAddressOfPlane(image, p);
            size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, p);
            for (unsigned y = 0; y < (p ? ch : h); y++) fwrite(base + y * stride, 1, p ? cw : w, o);
        }
        if (o) fclose(o);
    }
    if (index < 10) {
        uint32_t sum = 0;
        const unsigned char *luma = CVPixelBufferGetBaseAddressOfPlane(image, 0);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
        for (unsigned y = 0; y < h; y += 7) for (unsigned x = 0; x < w; x += 13) sum += luma[y * stride + x];
        IOSurfaceRef io = CVPixelBufferGetIOSurface(image);
        fprintf(stderr, "vtprobe: callback %zu: surface %u sum %u\n", index, io ? IOSurfaceGetID(io) : 0, sum);
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    unsigned char digest[16];
    CC_MD5_Final(digest, &md5);
    bool same = !memcmp(digest, r->md5[at], 16);
    if (r->verbose || !same) fprintf(stderr, "vtprobe: frame %zu (pts %lld): %s\n", index, pts.value, same ? "match" : "DIFFERS");
    r->frames++;
    r->failures += !same;
}
static void
output(void *context, void *tag, OSStatus status, VTDecodeInfoFlags info,
       CVImageBufferRef image, CMTime pts, CMTime duration)
{
    (void)info; (void)duration;
    struct result *r = context;
    size_t index = (uintptr_t)tag;
    unsigned bad = 0;
    if (r->md5) {
        if (status || !image || CVPixelBufferGetPlaneCount(image) != 2) {
            fprintf(stderr, "vtprobe: callback %zu failed: %d image %p\n", index, status, image);
            r->failures++; return;
        }
        output_md5(r, index, image, pts);
        return;
    }
    if (status || !image || CVPixelBufferGetWidth(image) != 64 || CVPixelBufferGetHeight(image) != 64 ||
        CVPixelBufferGetPlaneCount(image) != 2 || index >= r->reference_size / 6144) {
        fprintf(stderr, "vtprobe: callback %zu failed: %d image %p\n", index, status, image);
        r->failures++; return;
    }
    if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly)) { r->failures++; return; }
    for (unsigned p = 0; p < 2; p++) {
        const unsigned char *base = CVPixelBufferGetBaseAddressOfPlane(image, p);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, p);
        const unsigned char *expected = r->reference + index * 6144 + (p ? 4096 : 0);
        for (unsigned y = 0; y < (p ? 32u : 64u); y++)
            for (unsigned x = 0; x < 64; x++) bad += base[y * stride + x] != expected[y * 64 + x];
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    fprintf(stderr, "vtprobe: callback %zu: %u mismatched bytes, format %08x\n", index, bad,
            (unsigned)CVPixelBufferGetPixelFormatType(image));
    r->frames++;
    r->failures += bad != 0;
}
static unsigned char *
load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    rewind(f);
    if (length <= 0 || length > 256 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *size = (size_t)length;
    return data;
}
static uint32_t
word(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
int
main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) { fprintf(stderr, "usage: vtprobe PLUGIN PACKETS REFERENCE_NV12|REFERENCE.md5 [-v]\n"); return 2; }
    setbuf(stderr, NULL);
    unsigned waited;
    for (waited = 0; waited < 180 && access("/dev/twl-vdec-probe", R_OK | W_OK); waited++) sleep(1);
    fprintf(stderr, "vtprobe: transport wait %u seconds\n", waited);
    /* PLUGIN "-": whatever the system registered (AVD.videodecoder), decoded wherever VideoToolbox chooses */
    bool system = strcmp(argv[1], "-") == 0;
    void *plugin = system ? NULL : dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!system && !plugin) { fprintf(stderr, "vtprobe: dlopen: %s\n", dlerror()); return 3; }
    void (*register_probe)(void) = system ? NULL : dlsym(plugin, "TwlVDECRegisterProbe");
    Boolean (*hardware_supported)(CMVideoCodecType) = dlsym(RTLD_DEFAULT, "VTIsHardwareDecodeSupported");
    if (!system && !register_probe) return 4;
    /* The explicitly registered probe exists in this process, not the system decode XPC service. */
    OSStatus (*apply_restrictions)(uint32_t) = dlsym(RTLD_DEFAULT, "VTApplyRestrictions");
    if (apply_restrictions && !system) fprintf(stderr, "vtprobe: in-process decode: %d\n", apply_restrictions(1));
    Boolean (*avoid_hardware)(void) = dlsym(RTLD_DEFAULT, "VTAvoidHardwareDecoders");
    CFArrayRef (*allowed_decoders)(void) = dlsym(RTLD_DEFAULT, "VTGetAllowedDecoderList");
    fprintf(stderr, "vtprobe: avoid hardware: %d\n", avoid_hardware ? avoid_hardware() : -1);
    if (allowed_decoders && allowed_decoders()) CFShow(allowed_decoders());
    if (hardware_supported)
        fprintf(stderr, "vtprobe: hardware H.264 supported: %d\n", hardware_supported(kCMVideoCodecType_H264));
    if (register_probe) register_probe();
    size_t size = 0, reference_size = 0;
    unsigned char *data = load(argv[2], &size), *reference = load(argv[3], &reference_size);
    bool hevc = data && size >= 24 && !memcmp(data, "TWV3", 4);
    bool timed = hevc || (data && size >= 20 && !memcmp(data, "TWV2", 4));
    if (!data || !reference || size < 16 || (memcmp(data, "TWV1", 4) && !timed)) return 5;
    uint32_t vps_size = hevc ? word(data + 4) : 0;
    const unsigned char *d0 = data + (hevc ? 4 : 0);    /* TWV3: the fields after the VPS's size are TWV2's */
    uint32_t sps_size = word(d0 + 4), pps_size = word(d0 + 8), count = word(d0 + 12);
    uint32_t timescale = timed ? word(d0 + 16) : 5, header = timed ? 20 + (hevc ? 4 + vps_size : 0) : 16;
    if (hevc && vps_size > size - 24) return 6;
    if (!sps_size || !pps_size || sps_size > size - header || pps_size > size - header - sps_size ||
        !count || count > 10000 || (!timed && reference_size != (size_t)count * 6144) || !timescale) return 6;
    struct result result = { reference, reference_size, 0, 0, 64, 64, 0, NULL, NULL, argc == 5 };
    if (timed) {
        /* the reference: "PTS MD5" lines */
        result.pts = calloc(count, sizeof(*result.pts));
        result.md5 = calloc(count, sizeof(*result.md5));
        if (!result.pts || !result.md5) return 6;
        const char *p = (const char *)reference, *end = p + reference_size;
        while (p < end && result.count < count) {
            char *after;
            long long t = strtoll(p, &after, 10);
            if (after == p || after + 33 > end || *after != ' ') break;
            for (unsigned i = 0; i < 16; i++) {
                int hi = hexdigit(after[1 + 2 * i]), lo = hexdigit(after[2 + 2 * i]);
                if (hi < 0 || lo < 0) return 6;
                result.md5[result.count][i] = (unsigned char)(hi << 4 | lo);
            }
            result.pts[result.count++] = t;
            p = after + 33;
            while (p < end && *p != '\n') p++;
            p++;
        }
        if (result.count != count) { fprintf(stderr, "vtprobe: %u references for %u packets\n", result.count, count); return 6; }
    }
    const uint8_t *sets[] = { data + header, data + header + sps_size };
    const size_t sizes[] = { sps_size, pps_size };
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status;
    if (hevc) {
        const uint8_t *hsets[] = { data + 24, data + header, data + header + sps_size };
        const size_t hsizes[] = { vps_size, sps_size, pps_size };
        status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, hsets, hsizes, 4, NULL, &format);
    } else {
        status = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &format);
    }
    fprintf(stderr, "vtprobe: format: %d\n", status);
    if (status) return 7;
    CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format);
    result.width = (unsigned)dimensions.width;
    result.height = (unsigned)dimensions.height;
    fprintf(stderr, "vtprobe: %ux%u, %u packets\n", result.width, result.height, count);
    VTDecompressionOutputCallbackRecord callback = { output, &result };
    const void *key = CFSTR("RequireHardwareAcceleratedVideoDecoder"), *value = kCFBooleanTrue;
    CFDictionaryRef spec = CFDictionaryCreate(NULL, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTDecompressionSessionRef session = NULL;
    status = VTDecompressionSessionCreate(NULL, format, spec, NULL, &callback, &session);
    CFRelease(spec);
    fprintf(stderr, "vtprobe: require-hardware session: %d\n", status);
    if (status) {
        const void *keys[] = { CFSTR("RequireHardwareAcceleratedVideoDecoder"), CFSTR("DecoderID") };
        const void *values[] = { kCFBooleanTrue, CFSTR("com.twilight.vdec.h264.probe.avc1") };
        spec = CFDictionaryCreate(NULL, keys, values, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        status = VTDecompressionSessionCreate(NULL, format, spec, NULL, &callback, &session);
        CFRelease(spec);
        fprintf(stderr, "vtprobe: explicit decoder session: %d\n", status);
    }
    if (status) return 8;
    CFTypeRef hardware = NULL;
    status = VTSessionCopyProperty(session, CFSTR("UsingHardwareAcceleratedVideoDecoder"), NULL, &hardware);
    bool using_hardware = !status && hardware == kCFBooleanTrue;
    fprintf(stderr, "vtprobe: using hardware: %d (property status %d)\n", using_hardware, status);
    if (hardware) CFRelease(hardware);
    size_t pos = header + sps_size + pps_size;
    uint64_t started = mach_absolute_time();
    for (uint32_t i = 0; i < count; i++) {
        if (size - pos < (timed ? 12u : 4u)) { result.failures++; break; }
        uint32_t n = word(data + pos); pos += 4;
        int64_t pts = i;
        if (timed) { pts = (int64_t)((uint64_t)word(data + pos) | (uint64_t)word(data + pos + 4) << 32); pos += 8; }
        if (!n || n > size - pos) { result.failures++; break; }
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        unsigned char *bytes = malloc(n);
        if (!bytes) { result.failures++; break; }
        memcpy(bytes, data + pos, n); pos += n;
        status = CMBlockBufferCreateWithMemoryBlock(NULL, bytes, n, kCFAllocatorMalloc, NULL, 0, n, 0, &block);
        if (status) { free(bytes); result.failures++; break; }
        CMSampleTimingInfo timing = { CMTimeMake(1, (int32_t)timescale), CMTimeMake(pts, (int32_t)timescale), kCMTimeInvalid };
        size_t sample_size = n;
        status = CMSampleBufferCreateReady(NULL, block, format, 1, 1, &timing, 1, &sample_size, &sample);
        if (!status) status = VTDecompressionSessionDecodeFrame(session, sample, 0, (void *)(uintptr_t)i, NULL);
        if (!timed || status || result.verbose) fprintf(stderr, "vtprobe: submit %u: %d\n", i, status);
        if (status) result.failures++;
        if (sample) CFRelease(sample);
        CFRelease(block);
        if (status) break;
    }
    OSStatus delayed = VTDecompressionSessionFinishDelayedFrames(session);
    OSStatus waited_status = VTDecompressionSessionWaitForAsynchronousFrames(session);
    fprintf(stderr, "vtprobe: finish %d wait %d\n", delayed, waited_status);
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double seconds = (double)(mach_absolute_time() - started) * tb.numer / tb.denom / 1e9;
    fprintf(stderr, "vtprobe: %u pictures in %.2f s: %.1f a second\n", count, seconds, seconds > 0 ? count / seconds : 0);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(format);
    bool pass = using_hardware && !delayed && !waited_status && !result.failures && result.frames == count;
    fprintf(stderr, "vtprobe: %u/%u frames, %u failures: %s\n", result.frames, count, result.failures, pass ? "PASS" : "FAIL");
    free(data); free(reference);
    return pass ? 0 : 1;
}
