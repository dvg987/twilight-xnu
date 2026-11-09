/* SPDX-License-Identifier: GPL-2.0-or-later
 * Diagnostic only: run a complete small Annex-B GOP without resetting firmware between pictures.
 * References are packed NV12 in decode order, not display order.
 */
#include <sys/hakios.vdec.h>
#include "../video/h264/dpb.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static unsigned char *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    rewind(f);
    if (length <= 0 || length > 16*1024*1024) { fclose(f); return NULL; }
    unsigned char *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data); fclose(f); return NULL;
    }
    fclose(f);
    *size = (size_t)length;
    return data;
}
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: vdecstreamprobe GOP.h264 DECODE_ORDER.nv12\n"); return 2; }
    setbuf(stderr, NULL);
    for (unsigned i=0; i<180 && access("/dev/twl-vdec-probe", R_OK|W_OK); i++) sleep(1);
    size_t size=0, reference_size=0;
    unsigned char *input = load(argv[1], &size), *reference = load(argv[2], &reference_size);
    if (!input || !reference || size >= 1024*1024-1024 || reference_size % 6144) {
        free(input); free(reference); return 3;
    }
    int fd = open("/dev/twl-vdec-probe", O_RDWR);
    struct twl_vdec_caps caps;
    if (fd < 0 || ioctl(fd, TWL_VDEC_GET_CAPS, &caps) < 0 || !(caps.flags & TWL_VDEC_STATEFUL_PROBE)) {
        fprintf(stderr, "vdecstreamprobe: transport unavailable: %d\n", errno);
        if (fd >= 0) close(fd);
        free(input); free(reference); return 4;
    }
    struct twl_h264_dpb *dpb = twl_h264_dpb_create();
    if (!dpb) { close(fd); free(input); free(reference); return 5; }
    uint16_t parameters[TWL_H264_RPM_WORDS];
    unsigned char output[6144];
    struct twl_h264_prepare request = { (uintptr_t)input, (uintptr_t)parameters, (uint32_t)size, sizeof(parameters) };
    unsigned failures=0, completed=0, frames=(unsigned)(reference_size/6144);
    if (ioctl(fd, TWL_VDEC_PREPARE, &request) < 0) {
        fprintf(stderr, "vdecstreamprobe: initial header failed: %d\n", errno);
        failures++;
    } else for (unsigned i=0; i<frames; i++) {
        struct twl_h264_config config;
        if (twl_h264_dpb_prepare(dpb, parameters, &config)) {
            fprintf(stderr, "vdecstreamprobe: DPB header %u rejected (slice %u)\n", i, parameters[0x82]);
            failures++; break;
        }
        config.output = (uintptr_t)output;
        config.output_size = sizeof(output);
        config.rpm = (uintptr_t)parameters;
        if (ioctl(fd, TWL_VDEC_FINISH, &config) < 0) {
            fprintf(stderr, "vdecstreamprobe: picture %u failed: %d\n", i, errno);
            failures++; break;
        }
        unsigned bad=0;
        for (unsigned j=0; j<sizeof(output); j++) bad += output[j] != reference[i*6144+j];
        fprintf(stderr, "vdecstreamprobe: picture %u slice %u POC %d refs %u,%u slot %u: %u mismatches\n",
            i, parameters[0x82], config.frame_poc, config.reference_count[0], config.reference_count[1],
            config.current_slot, bad);
        failures += bad != 0;
        completed++;
        if (twl_h264_dpb_complete(dpb)) { failures++; break; }
        if (i+1 < frames) {
            request.input = request.input_size = 0;
            if (ioctl(fd, TWL_VDEC_NEXT, &request) < 0) {
                fprintf(stderr, "vdecstreamprobe: next header %u failed: %d\n", i+1, errno);
                failures++; break;
            }
        }
    }
    ioctl(fd, TWL_VDEC_CANCEL);
    close(fd);
    twl_h264_dpb_destroy(dpb);
    free(input); free(reference);
    fprintf(stderr, "vdecstreamprobe: %u/%u pictures, %u failures: %s\n", completed, frames, failures,
        completed == frames && !failures ? "PASS" : "FAIL");
    return completed == frames && !failures ? 0 : 1;
}
