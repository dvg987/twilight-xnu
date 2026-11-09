/*
 * The kernel's VP9 transport (TWL_VP9_PREPARE/FRAME) with the user-space reference map (tvos/video/vp9), checked
 * against FFmpeg's MD5s (tools/vp9-fixture.py): vp9probe PACKETS MD5S [FRAMES]
 */
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/hakios.vdec.h>
#include "../video/vp9/vp9_dpb.h"

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
	if (argc < 3) { fprintf(stderr, "usage: vp9probe PACKETS MD5S [FRAMES]\n"); return 1; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *file = malloc((size_t)file_size);
	if (fread(file, 1, (size_t)file_size, f) != (size_t)file_size || memcmp(file, "TWV9", 4)) { fprintf(stderr, "bad packets\n"); return 1; }
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
	if (ioctl(fd, TWL_VDEC_GET_CAPS, &caps) || !(caps.flags & TWL_VDEC_VP9)) { fprintf(stderr, "no VP9 transport\n"); return 1; }
	struct twl_vp9_dpb *dpb = twl_vp9_dpb_create();
	uint16_t rpm[TWL_VP9_RPM_WORDS];
	size_t frame_size = (size_t)4096 * 2304 * 3 / 2;
	uint8_t *frame = malloc(frame_size);
	struct twl_vdec_stream stream = { 0 };
	unsigned exact = 0, shown_count = 0, decoded = 0, existing = 0, dropped = 0, scaled = 0;
	double t_all = now(), t_hw = 0;
	uint32_t at = 16;
	for (unsigned i = 0; i < frames; i++) {
		uint32_t size;
		int64_t t;
		memcpy(&size, file + at, 4);
		memcpy(&t, file + at + 4, 8);
		const uint8_t *data = file + at + 12;
		at += 12 + size;
		if (getenv("VP9PROBE_SKIP") && i < (unsigned)atoi(getenv("VP9PROBE_SKIP"))) continue;
		uint32_t offsets[8], sizes[8];
		unsigned n = twl_vp9_split(data, size, offsets, sizes);
		if (!n) { fprintf(stderr, "packet %u: bad superframe index\n", i); return 1; }
		bool packet_shown = false;
		for (unsigned k = 0; k < n; k++) {
			double t0 = now();
			struct twl_h264_prepare prep = { (uintptr_t)(data + offsets[k]), (uintptr_t)rpm, sizes[k], sizeof(rpm) };
			int pr = ioctl(fd, TWL_VP9_PREPARE, &prep) ? errno : 0;
			if (getenv("VP9PROBE_VERBOSE"))
				fprintf(stderr, "packet %u frame %u: %u bytes, first %02x: %s\n", i, k, sizes[k], data[offsets[k]],
				    pr ? strerror(pr) : "header");
			if (!pr && getenv("VP9PROBE_VERBOSE"))
				fprintf(stderr, "  profile %u existing %u type %u show %u err %u intra %u refresh %#x %ux%u ref_info %#x same %#x "
				    "lf %u sharp %u depth %u\n", rpm[0], rpm[1], rpm[3], rpm[4], rpm[5], rpm[6], rpm[9], rpm[10], rpm[11],
				    rpm[14], rpm[15], rpm[23], rpm[24], rpm[25]);
			if (pr) {
				if (pr != ENODATA) fprintf(stderr, "packet %u frame %u: prepare: %s\n", i, k, strerror(pr));
				continue;
			}
			struct twl_vp9_frame fr = { 0 };
			bool shown;
			enum twl_vp9_action action = twl_vp9_dpb_frame(dpb, rpm, &fr, &shown);
			fr.output = (uintptr_t)frame;
			fr.output_size = (uint32_t)frame_size;
			int r = ioctl(fd, TWL_VP9_FRAME, &fr) ? errno : 0;
			t_hw += now() - t0;
			if (action == TWL_VP9_DECODE) twl_vp9_dpb_done(dpb, r == 0);
			if (action == TWL_VP9_DROP) {
				dropped++;
				continue;
			}
			if (r) {
				fprintf(stderr, "packet %u frame %u (slot %u): %s\n", i, k, fr.slot, strerror(r));
				if (r == ESTALE) twl_vp9_dpb_reset(dpb);
				continue;
			}
			if (action == TWL_VP9_DECODE) decoded++; else existing++;
			if (!stream.slots && !ioctl(fd, TWL_VDEC_STREAM, &stream)) {
				twl_vp9_dpb_set_slots(dpb, stream.slots);
				fprintf(stderr, "stream %ux%u, %u slots, stride %u rows %u\n", stream.width, stream.height, stream.slots,
				    stream.stride, stream.rows);
			}
			if (!shown) continue;
			if (packet_shown) fprintf(stderr, "packet %u: a second shown frame\n", i);
			packet_shown = true;
			shown_count++;
			unsigned out_w = stream.width < fr.width ? stream.width : fr.width;
			unsigned out_h = stream.height < fr.height ? stream.height : fr.height;
			if (getenv("VP9PROBE_DUMP") && i == (unsigned)atoi(getenv("VP9PROBE_DUMP"))) {
				/* the shown frame as cropped NV12, for a diff against FFmpeg's */
				FILE *d = fopen("/var/tmp/vp9probe.nv12", "wb");
				for (unsigned y = 0; d && y < out_h; y++) fwrite(frame + (size_t)y * stream.stride, 1, out_w, d);
				for (unsigned y = 0; d && y < out_h / 2; y++)
					fwrite(frame + (size_t)stream.stride * stream.rows + (size_t)y * stream.stride, 1, out_w, d);
				if (d) fclose(d);
			}
			if (stream.width < fr.width) {
				/* the kernel's copy is downscaled (above 1080p; kern.hakios_vp9_dw_full=1 for full size): not compared */
				scaled++;
				continue;
			}
			CC_MD5_CTX ctx;
			CC_MD5_Init(&ctx);
			for (unsigned y = 0; y < fr.height; y++) CC_MD5_Update(&ctx, frame + (size_t)y * stream.stride, fr.width);
			for (unsigned y = 0; y < fr.height / 2; y++)
				CC_MD5_Update(&ctx, frame + (size_t)stream.stride * stream.rows + (size_t)y * stream.stride, fr.width);
			unsigned char digest[16];
			char hex[33];
			CC_MD5_Final(digest, &ctx);
			for (int b = 0; b < 16; b++) sprintf(hex + 2 * b, "%02x", digest[b]);
			bool ok = i < n_md5 && !strcmp(md5s[i], hex);
			exact += ok;
			if (!ok) fprintf(stderr, "packet %u frame %u: slot %u %ux%u%s: %s\n", i, k, fr.slot, fr.width, fr.height,
			    action == TWL_VP9_EXISTING ? " (existing)" : "", i < n_md5 ? "MISMATCH" : "no reference");
		}
		if (!packet_shown) fprintf(stderr, "packet %u: nothing shown\n", i);
	}
	fprintf(stderr, "%u packets: %u frames decoded, %u shown again, %u dropped; %u shown, %u exact, %u downscaled; %.1f ms "
	    "a frame in the kernel (with the copy out), %.2f s\n", frames, decoded, existing, dropped, shown_count, exact, scaled,
	    decoded + existing ? t_hw * 1e3 / (decoded + existing) : 0, now() - t_all);
	return exact == frames ? 0 : 2;
}
