/* Host-side reference management for the experimental H.264 transport. */
#ifndef TWL_H264_DPB_ADAPTER_H
#define TWL_H264_DPB_ADAPTER_H
#include <stdbool.h>
#include <stdint.h>
struct twl_h264_config;
struct twl_h264_dpb;
struct twl_h264_dpb *twl_h264_dpb_create(void);
int twl_h264_dpb_prepare(struct twl_h264_dpb *, const uint16_t *, struct twl_h264_config *);
int twl_h264_dpb_complete(struct twl_h264_dpb *);
void twl_h264_dpb_destroy(struct twl_h264_dpb *);
/* before the first picture: the slots the kernel gives the stream (TWL_VDEC_STREAM) */
int twl_h264_dpb_set_slots(struct twl_h264_dpb *, unsigned);
void twl_h264_dpb_set_held(struct twl_h264_dpb *, uint32_t, uint32_t);
bool twl_h264_dpb_started(const struct twl_h264_dpb *);
#endif
