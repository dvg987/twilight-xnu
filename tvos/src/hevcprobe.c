/*
 * The kernel's HEVC transport (TWL_HEVC_PREPARE/SLICE) with the user-space DPB (tvos/video/hevc), checked against
 * FFmpeg's MD5s (tools/hevc-fixture.py): hevcprobe PACKETS MD5S [FRAMES]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/hakios.vdec.h>
#include "../video/hevc/hevc_dpb.h"

static double
now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int
main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: hevcprobe PACKETS MD5S [FRAMES]\n"); return 1; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *file = malloc((size_t)file_size);
	if (fread(file, 1, (size_t)file_size, f) != (size_t)file_size || memcmp(file, "TWH1", 4)) { fprintf(stderr, "bad packets\n"); return 1; }
	fclose(f);
	uint32_t count;
	memcpy(&count, file + 4, 4);
	unsigned frames = argc > 3 ? (unsigned)atoi(argv[3]) : count;
	if (frames > count) frames = count;
	char (*md5s)[33] = calloc(count, 33);
	long long *pts = calloc(count, sizeof(long long));
	unsigned n_md5 = 0;
	f = fopen(argv[2], "r");
	while (f && n_md5 < count && fscanf(f, "%lld %32s", &pts[n_md5], md5s[n_md5]) == 2) n_md5++;
	if (f) fclose(f);

	int fd = open("/dev/twl-vdec-probe", O_RDWR);
	if (fd < 0) { perror("/dev/twl-vdec-probe"); return 1; }
	struct twl_vdec_caps caps;
	if (ioctl(fd, TWL_VDEC_GET_CAPS, &caps) || !(caps.flags & TWL_VDEC_HEVC)) { fprintf(stderr, "no HEVC transport\n"); return 1; }
	struct twl_hevc_dpb *dpb = twl_hevc_dpb_create();
	uint16_t rpm[TWL_HEVC_RPM_WORDS];
	uint8_t *frame = NULL;
	size_t frame_size = 0;
	struct twl_vdec_stream stream = { 0 };
	unsigned exact = 0, decoded = 0;
	double t_all = now(), t_hw = 0;
	uint32_t at = 16;
	for (unsigned i = 0; i < frames; i++) {
		uint32_t size;
		int64_t t;
		memcpy(&size, file + at, 4);
		memcpy(&t, file + at + 4, 8);
		const uint8_t *data = file + at + 12;
		at += 12 + size;
		double t0 = now();
		struct twl_h264_prepare prep = { (uintptr_t)data, (uintptr_t)rpm, size, sizeof(rpm) };
		if (ioctl(fd, TWL_HEVC_PREPARE, &prep)) {
			fprintf(stderr, "packet %u: prepare: %s\n", i, strerror(errno));
			continue;
		}
		if (!stream.slots) {
			if (ioctl(fd, TWL_VDEC_STREAM, &stream) || twl_hevc_dpb_set_slots(dpb, stream.slots)) {
				fprintf(stderr, "stream: %s\n", strerror(errno));
				return 1;
			}
			frame_size = (size_t)stream.stride * stream.rows * 3 / 2;
			frame = malloc(frame_size);
			fprintf(stderr, "stream %ux%u, %u slots, stride %u rows %u\n", stream.width, stream.height, stream.slots,
			    stream.stride, stream.rows);
		}
		struct twl_hevc_slice slice = { 0 };
		int r;
		for (;;) {
			if (twl_hevc_dpb_slice(dpb, rpm, &slice)) { fprintf(stderr, "packet %u: DPB refused the slice\n", i); return 1; }
			slice.output = (uintptr_t)frame;
			slice.output_size = (uint32_t)frame_size;
			slice.rpm = (uintptr_t)rpm;
			r = ioctl(fd, TWL_HEVC_SLICE, &slice) ? errno : 0;
			if (r != EINPROGRESS) break;
		}
		twl_hevc_dpb_done(dpb, r == 0);
		t_hw += now() - t0;
		if (r) {
			fprintf(stderr, "packet %u: %s\n", i, r == ENODATA ? "skipped" : strerror(r));
			if (r != ENODATA) return 1;
			continue;
		}
		decoded++;
		CC_MD5_CTX ctx;
		CC_MD5_Init(&ctx);
		/* HEVCPROBE_HEIGHT=N: the cropped height the references have (the kernel names the coded one) */
		unsigned rows = getenv("HEVCPROBE_HEIGHT") ? (unsigned)atoi(getenv("HEVCPROBE_HEIGHT")) : stream.height;
		if (rows == 0 || rows > stream.height) rows = stream.height;
		for (unsigned y = 0; y < rows; y++) CC_MD5_Update(&ctx, frame + (size_t)y * stream.stride, stream.width);
		for (unsigned y = 0; y < rows / 2; y++)
			CC_MD5_Update(&ctx, frame + (size_t)stream.stride * stream.rows + (size_t)y * stream.stride, stream.width);
		unsigned char digest[16];
		char hex[33];
		CC_MD5_Final(digest, &ctx);
		for (int k = 0; k < 16; k++) sprintf(hex + 2 * k, "%02x", digest[k]);
		const char *want = NULL;
		for (unsigned k = 0; k < n_md5; k++) if (pts[k] == t) want = md5s[k];
		bool ok = want && !strcmp(want, hex);
		/* HEVCPROBE_DUMP=N: packet N's picture, cropped NV12, to /var/tmp/hevcprobe.nv12 */
		const char *dump = getenv("HEVCPROBE_DUMP");
		if (dump && (unsigned)atoi(dump) == i) {
			FILE *o = fopen("/var/tmp/hevcprobe.nv12", "wb");
			for (unsigned y = 0; o && y < stream.height; y++) fwrite(frame + (size_t)y * stream.stride, 1, stream.width, o);
			for (unsigned y = 0; o && y < stream.height / 2; y++)
				fwrite(frame + (size_t)stream.stride * stream.rows + (size_t)y * stream.stride, 1, stream.width, o);
			if (o) fclose(o);
		}
		exact += ok;
		if (!ok) fprintf(stderr, "packet %u: POC %d slot %u gen %u: %s\n", i, slice.poc, slice.slot, slice.generation,
		    want ? "MISMATCH" : "no reference");
	}
	fprintf(stderr, "%u/%u decoded, %u exact; %.1f ms each in the kernel (with the copy out), %.2f s\n", decoded, frames,
	    exact, decoded ? t_hw * 1e3 / decoded : 0, now() - t_all);
	return exact == frames ? 0 : 2;
}
