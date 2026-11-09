/*
 * libtwlytav.dylib (diagnosis, injected into YouTube with libtwlcrash for its standard error): what YouTube's player
 * does with AVFoundation's sample-buffer renderers, a line a second. Each second: video frames enqueued (and their last
 * presentation time), how often the display layer said it was not ready for more, its status; the same for the
 * audio renderer; the synchronizer's rate and time. A stall shows which side stopped.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

typedef void *obj_t;

/* touch /var/tmp/twlyt-novp9: VP9 is said not to be decoded in hardware, so YouTube streams H.264 (a comparison) */
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
static Boolean
my_VTIsHardwareDecodeSupported(CMVideoCodecType codec)
{
	Boolean real = VTIsHardwareDecodeSupported(codec);
	Boolean said = codec == 'av01' || (codec == 'vp09' && access("/var/tmp/twlyt-novp9", F_OK) == 0) ? false : real;
	dprintf(2, "twlytav: VTIsHardwareDecodeSupported('%c%c%c%c'): %d, told %d\n", (char)(codec >> 24), (char)(codec >> 16),
	    (char)(codec >> 8), (char)codec, real, said);
	return said;
}
/* what YouTube's player reads as the media time: calls a second and the last answer, in the summary */
static unsigned tb_calls;
static double tb_last;
static CMTimebaseRef tb_last_base;
static CMTime
my_CMTimebaseGetTime(CMTimebaseRef timebase)
{
	CMTime t = CMTimebaseGetTime(timebase);
	Dl_info info;
	if (!dladdr(__builtin_return_address(0), &info) || info.dli_fbase != (void *)_dyld_get_image_header(0)) return t;
	tb_calls++;
	tb_last = CMTIME_IS_VALID(t) ? CMTimeGetSeconds(t) : -1;
	tb_last_base = timebase;
	return t;
}
__attribute__((used)) static struct { const void *r; const void *o; } interpose_tb __attribute__((section("__DATA,__interpose"))) =
    { (const void *)&my_CMTimebaseGetTime, (const void *)&CMTimebaseGetTime };
__attribute__((used)) static struct { const void *r; const void *o; } interpose_vt __attribute__((section("__DATA,__interpose"))) =
    { (const void *)&my_VTIsHardwareDecodeSupported, (const void *)&VTIsHardwareDecodeSupported };

static struct {
	unsigned video, video_not_ready, audio, audio_not_ready, flushes, requests;
	double video_pts, audio_pts;
	obj_t layer, renderer, sync;
} s;

static IMP enqueue_video_o, ready_video_o, enqueue_audio_o, ready_audio_o, flush_o, flush_remove_o, request_o, set_rate_o,
    set_rate_time_o;

static double
pts_of(obj_t buffer)
{
	CMTime t = CMSampleBufferGetPresentationTimeStamp((CMSampleBufferRef)buffer);
	return CMTIME_IS_VALID(t) ? CMTimeGetSeconds(t) : -1;
}

static void
my_enqueue_video(obj_t self, SEL cmd, obj_t buffer)
{
	s.layer = self;
	static CMFormatDescriptionRef vfd;
	CMFormatDescriptionRef f = CMSampleBufferGetFormatDescription((CMSampleBufferRef)buffer);
	if (f && f != vfd) {
		FourCharCode c = CMFormatDescriptionGetMediaSubType(f);
		CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f);
		dprintf(2, "twlytav: video format '%c%c%c%c' %dx%d at pts %.3f\n", (char)(c >> 24), (char)(c >> 16), (char)(c >> 8), (char)c,
		    d.width, d.height, pts_of(buffer));
		vfd = f;
	}
	s.video++;
	s.video_pts = pts_of(buffer);
	((void (*)(obj_t, SEL, obj_t))enqueue_video_o)(self, cmd, buffer);
}

static BOOL
my_ready_video(obj_t self, SEL cmd)
{
	BOOL r = ((BOOL (*)(obj_t, SEL))ready_video_o)(self, cmd);
	if (!r) s.video_not_ready++;
	return r;
}

static void
my_enqueue_audio(obj_t self, SEL cmd, obj_t buffer)
{
	static FourCharCode last_id;
	static double last_rate, last_end;
	static UInt32 last_ch;
	CMFormatDescriptionRef f = CMSampleBufferGetFormatDescription((CMSampleBufferRef)buffer);
	const AudioStreamBasicDescription *a = f ? CMAudioFormatDescriptionGetStreamBasicDescription(f) : NULL;
	CMTime d = CMSampleBufferGetDuration((CMSampleBufferRef)buffer);
	double pts = pts_of(buffer), dur = CMTIME_IS_VALID(d) ? CMTimeGetSeconds(d) : -1;
	if (a && (a->mFormatID != last_id || a->mSampleRate != last_rate || a->mChannelsPerFrame != last_ch)) {
		dprintf(2, "twlytav: audio format '%c%c%c%c' %.0f Hz %u ch, %u frames a packet\n", (char)(a->mFormatID >> 24),
		    (char)(a->mFormatID >> 16), (char)(a->mFormatID >> 8), (char)a->mFormatID, a->mSampleRate, a->mChannelsPerFrame,
		    a->mFramesPerPacket);
		last_id = a->mFormatID;
		last_rate = a->mSampleRate;
		last_ch = a->mChannelsPerFrame;
	}
	static unsigned dumped;
	if (dumped < 4) {
		CMBlockBufferRef b = CMSampleBufferGetDataBuffer((CMSampleBufferRef)buffer);
		size_t len = b ? CMBlockBufferGetDataLength(b) : 0;
		uint8_t head[16] = { 0 };
		const AudioStreamPacketDescription *pd = NULL;
		size_t npd = 0;
		CMSampleBufferGetAudioStreamPacketDescriptionsPtr((CMSampleBufferRef)buffer, &pd, &npd);
		if (b) CMBlockBufferCopyDataBytes(b, 0, len < 16 ? len : 16, head);
		CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray((CMSampleBufferRef)buffer, false);
		dprintf(2, "twlytav: audio buffer %u: %ld samples, %zu bytes, %zu packet descriptions (first %u frames %u bytes), pts %.4f dur %.4f, "
		    "%ld attachments; %02x%02x %02x%02x %02x%02x %02x%02x\n", dumped, (long)CMSampleBufferGetNumSamples((CMSampleBufferRef)buffer),
		    len, npd, npd ? pd[0].mVariableFramesInPacket : 0, npd ? pd[0].mDataByteSize : 0, pts, dur, att ? (long)CFArrayGetCount(att) : -1,
		    head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
		CFDictionaryRef bufatt = CMCopyDictionaryOfAttachments(NULL, buffer, kCMAttachmentMode_ShouldPropagate);
		if (dumped == 0) {
			CFStringRef d = CFCopyDescription(f);
			char text[2048];
			if (d && CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8)) dprintf(2, "twlytav: audio format description: %s\n", text);
			if (d) CFRelease(d);
		}
		if (bufatt) {
			CFStringRef d = CFCopyDescription(bufatt);
			char text[1024];
			if (d && CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8)) dprintf(2, "twlytav: buffer attachments: %s\n", text);
			if (d) CFRelease(d);
			CFRelease(bufatt);
		}
		dumped++;
	}
	static CMFormatDescriptionRef last_fd;
	static unsigned since_fd;
	since_fd++;
	if (f != last_fd) {
		dprintf(2, "twlytav: audio format description object %p at pts %.3f (%s the last one's contents, after %u buffers)\n", (void *)f, pts,
		    last_fd && CMFormatDescriptionEqual(f, last_fd) ? "same as" : "not", since_fd);
		last_fd = f;
		since_fd = 0;
	}
	if (last_end > 0 && (pts < last_end - 0.002 || pts > last_end + 0.002))
		dprintf(2, "twlytav: audio discontinuity: %.3f after one ending at %.3f\n", pts, last_end);
	last_end = pts + dur;
	s.renderer = self;
	s.audio++;
	s.audio_pts = pts;
	((void (*)(obj_t, SEL, obj_t))enqueue_audio_o)(self, cmd, buffer);
}

static BOOL
my_ready_audio(obj_t self, SEL cmd)
{
	BOOL r = ((BOOL (*)(obj_t, SEL))ready_audio_o)(self, cmd);
	if (!r) s.audio_not_ready++;
	return r;
}

static void
my_flush(obj_t self, SEL cmd)
{
	s.flushes++;
	dprintf(2, "twlytav: display layer flush\n");
	((void (*)(obj_t, SEL))flush_o)(self, cmd);
}

static void
my_flush_remove(obj_t self, SEL cmd)
{
	s.flushes++;
	dprintf(2, "twlytav: display layer flushAndRemoveImage\n");
	((void (*)(obj_t, SEL))flush_remove_o)(self, cmd);
}

static void
my_request(obj_t self, SEL cmd, obj_t queue, obj_t block)
{
	s.requests++;
	((void (*)(obj_t, SEL, obj_t, obj_t))request_o)(self, cmd, queue, block);
}

static void
my_set_rate(obj_t self, SEL cmd, float rate)
{
	s.sync = self;
	dprintf(2, "twlytav: synchronizer rate %.2f\n", rate);
	((void (*)(obj_t, SEL, float))set_rate_o)(self, cmd, rate);
}

static void
my_set_rate_time(obj_t self, SEL cmd, float rate, CMTime time)
{
	s.sync = self;
	dprintf(2, "twlytav: synchronizer rate %.2f at %.3f\n", rate, CMTIME_IS_VALID(time) ? CMTimeGetSeconds(time) : -1);
	((void (*)(obj_t, SEL, float, CMTime))set_rate_time_o)(self, cmd, rate, time);
}

static IMP current_time_o;
static int in_summary;
static CMTime
my_current_time(obj_t self, SEL cmd)
{
	CMTime t = ((CMTime (*)(obj_t, SEL))current_time_o)(self, cmd);
	if (!in_summary) {
		tb_calls++;
		tb_last = CMTIME_IS_VALID(t) ? CMTimeGetSeconds(t) : -1;
	}
	return t;
}

static IMP
swap(const char *cls, const char *sel, IMP replacement)
{
	Class c = objc_getClass(cls);
	Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : NULL;
	if (m == NULL) {
		dprintf(2, "twlytav: no -[%s %s]\n", cls, sel);
		return NULL;
	}
	return method_setImplementation(m, replacement);
}

static void
summary(void)
{
	static unsigned last_video, last_audio;
	double now = -1;
	long status = -1;
	obj_t error = NULL;

	if (s.sync != NULL) {
		in_summary = 1;
		CMTime t = ((CMTime (*)(obj_t, SEL))objc_msgSend)(s.sync, sel_registerName("currentTime"));
		in_summary = 0;
		now = CMTIME_IS_VALID(t) ? CMTimeGetSeconds(t) : -1;
	}
	if (s.renderer != NULL) {
		long rs = ((long (*)(obj_t, SEL))objc_msgSend)(s.renderer, sel_registerName("status"));
		obj_t re = ((obj_t (*)(obj_t, SEL))objc_msgSend)(s.renderer, sel_registerName("error"));
		if (rs != 1 || re) dprintf(2, "twlytav: audio renderer status %ld%s\n", rs, re ? " (error)" : "");
	}
	if (s.layer != NULL) {
		status = ((long (*)(obj_t, SEL))objc_msgSend)(s.layer, sel_registerName("status"));
		error = ((obj_t (*)(obj_t, SEL))objc_msgSend)(s.layer, sel_registerName("error"));
		static obj_t told;
		if (error && error != told) {
			obj_t d = ((obj_t (*)(obj_t, SEL))objc_msgSend)(error, sel_registerName("description"));
			const char *c = d ? ((const char *(*)(obj_t, SEL))objc_msgSend)(d, sel_registerName("UTF8String")) : NULL;
			dprintf(2, "twlytav: display layer error: %s\n", c ? c : "?");
			told = error;
		}
		static BOOL said_rf;
		BOOL rf = ((BOOL (*)(obj_t, SEL))objc_msgSend)(s.layer, sel_registerName("requiresFlushToResumeDecoding"));
		if (rf != said_rf) dprintf(2, "twlytav: display layer requiresFlushToResumeDecoding %d\n", rf);
		said_rf = rf;
	}
	if (s.video == last_video && s.audio == last_audio && !s.video_not_ready && !s.audio_not_ready) return;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	dprintf(2, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	dprintf(2, "[timebase: %u reads, last %.3f, rate %.2f] ", tb_calls, tb_last, tb_last_base ? CMTimebaseGetRate(tb_last_base) : -1);
	tb_calls = 0;
	dprintf(2, "twlytav: video +%u (pts %.3f, not ready %u) audio +%u (pts %.3f, not ready %u) clock %.3f; layer status %ld%s, "
	    "flushes %u, requests %u\n", s.video - last_video, s.video_pts, s.video_not_ready, s.audio - last_audio, s.audio_pts,
	    s.audio_not_ready, now, status, error ? " (error)" : "", s.flushes, s.requests);
	last_video = s.video;
	last_audio = s.audio;
	s.video_not_ready = s.audio_not_ready = 0;
}

/* /var/tmp/twlyt-stereo: the audio session says 2 output channels (YouTube then picks stereo audio, not E-AC-3 5.1) */
static IMP max_ch_o, out_ch_o;
static long
my_max_ch(obj_t self, SEL cmd)
{
	long n = ((long (*)(obj_t, SEL))max_ch_o)(self, cmd);
	return access("/var/tmp/twlyt-stereo", F_OK) == 0 && n > 2 ? 2 : n;
}
static long
my_out_ch(obj_t self, SEL cmd)
{
	long n = ((long (*)(obj_t, SEL))out_ch_o)(self, cmd);
	return access("/var/tmp/twlyt-stereo", F_OK) == 0 && n > 2 ? 2 : n;
}

/* every AV notification (audio session interruptions and route changes, renderers flushed by the system, decode
 * failures): a renderer that stops taking data mid-play is told about in one of these */
static void
on_note(CFNotificationCenterRef center, void *observer, CFNotificationName name, const void *object, CFDictionaryRef info)
{
	char n[160] = "?";
	(void)center; (void)observer;
	if (name) CFStringGetCString(name, n, sizeof n, kCFStringEncodingUTF8);
	if (strncmp(n, "AV", 2) && !strstr(n, "Interruption") && !strstr(n, "MediaServices")) return;
	if (!strcmp(n, "AVSampleBufferDisplayLayerReadyForDisplayDidChangeNotification")) return;
	char d[600] = "";
	if (info) {
		CFStringRef desc = CFCopyDescription(info);
		if (desc) { CFStringGetCString(desc, d, sizeof d, kCFStringEncodingUTF8); CFRelease(desc); }
		for (char *c = d; *c; c++) if (*c == '\n') *c = ' ';
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	dprintf(2, "%ld.%03ld twlytav: NOTE %s (object %p) %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000, n, object, d);
}

__attribute__((constructor)) static void
start(void)
{
	CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(), NULL, on_note, NULL, NULL,
	    CFNotificationSuspensionBehaviorDeliverImmediately);
	if (access("/var/tmp/twlyt-stereo", F_OK) == 0) {
		max_ch_o = swap("AVAudioSession", "maximumOutputNumberOfChannels", (IMP)my_max_ch);
		out_ch_o = swap("AVAudioSession", "outputNumberOfChannels", (IMP)my_out_ch);
	}
	enqueue_video_o = swap("AVSampleBufferDisplayLayer", "enqueueSampleBuffer:", (IMP)my_enqueue_video);
	ready_video_o = swap("AVSampleBufferDisplayLayer", "isReadyForMoreMediaData", (IMP)my_ready_video);
	flush_o = swap("AVSampleBufferDisplayLayer", "flush", (IMP)my_flush);
	flush_remove_o = swap("AVSampleBufferDisplayLayer", "flushAndRemoveImage", (IMP)my_flush_remove);
	request_o = swap("AVSampleBufferDisplayLayer", "requestMediaDataWhenReadyOnQueue:usingBlock:", (IMP)my_request);
	enqueue_audio_o = swap("AVSampleBufferAudioRenderer", "enqueueSampleBuffer:", (IMP)my_enqueue_audio);
	ready_audio_o = swap("AVSampleBufferAudioRenderer", "isReadyForMoreMediaData", (IMP)my_ready_audio);
	set_rate_o = swap("AVSampleBufferRenderSynchronizer", "setRate:", (IMP)my_set_rate);
	current_time_o = swap("AVSampleBufferRenderSynchronizer", "currentTime", (IMP)my_current_time);
	set_rate_time_o = swap("AVSampleBufferRenderSynchronizer", "setRate:time:", (IMP)my_set_rate_time);
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler_f(timer, (dispatch_function_t)summary);
	dispatch_resume(timer);
	dprintf(2, "twlytav: hooked\n");
}
