/*
 * audioprobe (diagnosis): feed AVSampleBufferAudioRenderer through a synchronizer the way YouTube's player does, from a
 * file, and report each second how far the enqueued audio is ahead of the clock and how often the renderer refused more.
 *   audioprobe ec3 FILE CHANNELS [SECONDS]    raw E-AC-3 syncframes (ffmpeg -c:a eac3 -f eac3), 1536 frames each
 *   audioprobe pcm FILE CHANNELS [SECONDS]    raw signed 16-bit little-endian PCM at 48 kHz
 * A renderer that plays in real time keeps the lead steady; one that drains slower refuses more and more while the
 * lead shrinks, and the clock stops when it reaches the enqueued audio.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <AudioToolbox/AudioToolbox.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef void *obj_t;
#define MSG(ret, ...) ((ret (*)(obj_t, SEL, ##__VA_ARGS__))objc_msgSend)

static double
now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static obj_t
make(const char *cls)
{
	obj_t o = MSG(obj_t)((obj_t)objc_getClass(cls), sel_registerName("alloc"));
	return MSG(obj_t)(o, sel_registerName("init"));
}

int
main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: audioprobe ec3|pcm FILE CHANNELS [SECONDS]\n");
		return 1;
	}
	int ec3 = !strcmp(argv[1], "ec3"), channels = atoi(argv[3]);
	double seconds = argc > 4 ? atof(argv[4]) : 40;
	int fd = open(argv[2], O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) {
		perror(argv[2]);
		return 1;
	}
	uint8_t *data = malloc(st.st_size);
	if (read(fd, data, st.st_size) != st.st_size) {
		perror("read");
		return 1;
	}
	AudioStreamBasicDescription asbd = { .mSampleRate = 48000, .mChannelsPerFrame = channels };
	if (ec3) {
		asbd.mFormatID = kAudioFormatEnhancedAC3;
		asbd.mFramesPerPacket = 1536;
	} else {
		asbd.mFormatID = kAudioFormatLinearPCM;
		asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
		asbd.mBitsPerChannel = 16;
		asbd.mFramesPerPacket = 1;
		asbd.mBytesPerFrame = asbd.mBytesPerPacket = 2 * channels;
	}
	/* AUDIOPROBE_LAYOUT=tag (hex), AUDIOPROBE_FLAGS=n: as YouTube's player describes E-AC-3 (0x9b0006, 1) */
	const char *tag = getenv("AUDIOPROBE_LAYOUT"), *fl = getenv("AUDIOPROBE_FLAGS");
	AudioChannelLayout acl = { .mChannelLayoutTag = tag ? (AudioChannelLayoutTag)strtoul(tag, NULL, 16) : 0 };
	if (fl) asbd.mFormatFlags = (UInt32)atoi(fl);
	CMAudioFormatDescriptionRef format;
	OSStatus s = CMAudioFormatDescriptionCreate(NULL, &asbd, tag ? sizeof(acl) : 0, tag ? &acl : NULL, 0, NULL, NULL, &format);
	if (s) {
		fprintf(stderr, "format: %d\n", (int)s);
		return 1;
	}
	dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW);
	/* AUDIOPROBE_CHANNELS=n: a playback audio session asking for n output channels (YouTube asks for the most there are,
	 * which puts the HDMI output into multichannel PCM) */
	if (getenv("AUDIOPROBE_CHANNELS")) {
		obj_t session = MSG(obj_t)((obj_t)objc_getClass("AVAudioSession"), sel_registerName("sharedInstance"));
		CFStringRef *playback = dlsym(RTLD_DEFAULT, "AVAudioSessionCategoryPlayback");
		obj_t err = NULL;
		MSG(BOOL, obj_t, obj_t *)(session, sel_registerName("setCategory:error:"), playback ? (obj_t)*playback : NULL, &err);
		MSG(BOOL, BOOL, obj_t *)(session, sel_registerName("setActive:error:"), YES, &err);
		if (getenv("AUDIOPROBE_IOBUF")) {
			BOOL k = MSG(BOOL, double, obj_t *)(session, sel_registerName("setPreferredIOBufferDuration:error:"), atof(getenv("AUDIOPROBE_IOBUF")), &err);
			printf("io buffer: asked %s s: %d, now %.4f s\n", getenv("AUDIOPROBE_IOBUF"), k, MSG(double)(session, sel_registerName("IOBufferDuration")));
		}
		long want = atol(getenv("AUDIOPROBE_CHANNELS"));
		BOOL ok = MSG(BOOL, long, obj_t *)(session, sel_registerName("setPreferredOutputNumberOfChannels:error:"), want, &err);
		printf("session: asked %ld channels: %d; output %ld, maximum %ld\n", want, ok,
		    MSG(long)(session, sel_registerName("outputNumberOfChannels")),
		    MSG(long)(session, sel_registerName("maximumOutputNumberOfChannels")));
	}
	obj_t renderer = make("AVSampleBufferAudioRenderer"), sync = make("AVSampleBufferRenderSynchronizer");
	if (!renderer || !sync) {
		fprintf(stderr, "no AVFoundation classes\n");
		return 1;
	}
	/* AUDIOPROBE_PITCH=name: the renderer's time-pitch algorithm (AVAudioTimePitchAlgorithm<name> symbol suffix) */
	if (getenv("AUDIOPROBE_PITCH")) {
		char sym[96];
		snprintf(sym, sizeof sym, "AVAudioTimePitchAlgorithm%s", getenv("AUDIOPROBE_PITCH"));
		CFStringRef *algo = dlsym(RTLD_DEFAULT, sym);
		if (algo) MSG(void, obj_t)(renderer, sel_registerName("setAudioTimePitchAlgorithm:"), (obj_t)*algo);
		obj_t now = MSG(obj_t)(renderer, sel_registerName("audioTimePitchAlgorithm"));
		printf("time pitch: %s (asked %s)\n", now ? CFStringGetCStringPtr((CFStringRef)now, kCFStringEncodingUTF8) : "?", sym);
	}
	MSG(void, obj_t)(sync, sel_registerName("addRenderer:"), renderer);

	size_t pos = 0;
	int64_t pts = 0;                        /* in frames */
	unsigned refused = 0, enqueued = 0;
	int started = 0;
	double start = now(), mark = start;
	while (now() - start < seconds) {
		double max_lead = getenv("AUDIOPROBE_LEAD") ? atof(getenv("AUDIOPROBE_LEAD")) : 1e9;
		CMTime c0 = MSG(CMTime)(sync, sel_registerName("currentTime"));
		while (pos < (size_t)st.st_size && (!started || pts / 48000.0 - CMTimeGetSeconds(c0) < max_lead) &&
		    MSG(BOOL)(renderer, sel_registerName("isReadyForMoreMediaData"))) {
			size_t len;
			int frames;
			if (ec3) {
				if (pos + 4 > (size_t)st.st_size || data[pos] != 0x0b || data[pos + 1] != 0x77) {
					fprintf(stderr, "lost E-AC-3 sync at %zu\n", pos);
					pos = st.st_size;
					break;
				}
				len = (((data[pos + 2] & 7) << 8 | data[pos + 3]) + 1) * 2;
				frames = 1536;
			} else {
				frames = 1024;
				len = (size_t)frames * 2 * channels;
			}
			if (pos + len > (size_t)st.st_size) {
				pos = st.st_size;
				break;
			}
			CMBlockBufferRef block;
			CMBlockBufferCreateWithMemoryBlock(NULL, data + pos, len, kCFAllocatorNull, NULL, 0, len, 0, &block);
			CMSampleBufferRef sample;
			CMTime t = CMTimeMake(pts, 48000);
			if (ec3) {
				AudioStreamPacketDescription pd = { 0, 1536, (UInt32)len };
				s = CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, block, format, 1, t, &pd, &sample);
			} else {
				s = CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, block, format, frames, t, NULL, &sample);
			}
			CFRelease(block);
			if (s) {
				fprintf(stderr, "sample: %d\n", (int)s);
				return 1;
			}
			MSG(void, obj_t)(renderer, sel_registerName("enqueueSampleBuffer:"), sample);
			CFRelease(sample);
			pos += len;
			pts += frames;
			enqueued++;
		}
		if (!started && pts >= 48000) {
			MSG(void, float)(sync, sel_registerName("setRate:"), 1.0f);
			started = 1;
		}
		if (pos < (size_t)st.st_size) {
			refused++;
		}
		usleep(10000);
		double t = now();
		if (t - mark >= 1) {
			CMTime c = MSG(CMTime)(sync, sel_registerName("currentTime"));
			double clock = CMTimeGetSeconds(c), audio = pts / 48000.0;
			long status = MSG(long)(renderer, sel_registerName("status"));
			printf("%6.2f: clock %7.3f enqueued %7.3f lead %+.3f; %u buffers, refused %u, status %ld\n", t - start, clock, audio,
			    audio - clock, enqueued, refused, status);
			fflush(stdout);
			mark = t;
			refused = enqueued = 0;
		}
	}
	return 0;
}
