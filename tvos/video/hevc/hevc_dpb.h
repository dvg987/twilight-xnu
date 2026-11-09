/* The HEVC decoder's picture management in user space (the kernel side is hakios.hevc.c): POC, reference sets
 * and lists, and which of the kernel's slots each picture is decoded into. Pictures are handed out in decode
 * order, as the H.264 decoder does; the player puts them in presentation order. */
#ifndef TWL_HEVC_DPB_H
#define TWL_HEVC_DPB_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/hakios.vdec.h>

struct twl_hevc_dpb;

struct twl_hevc_dpb *twl_hevc_dpb_create(void);
void twl_hevc_dpb_destroy(struct twl_hevc_dpb *dpb);
/* the kernel's slot count for the stream (TWL_VDEC_STREAM after the first PREPARE) */
int twl_hevc_dpb_set_slots(struct twl_hevc_dpb *dpb, unsigned slots);
/* slots the video plane shows or may still be asked to: decoded into only when nothing else is free */
void twl_hevc_dpb_set_held(struct twl_hevc_dpb *dpb, uint32_t held, uint32_t showing);
void twl_hevc_dpb_stats(struct twl_hevc_dpb *dpb, unsigned *picks, unsigned *forced, unsigned *used, unsigned *held);
/* a slice header (TWL_HEVC_RPM_WORDS, from PREPARE or a SLICE's EINPROGRESS): the answer for TWL_HEVC_SLICE.
 * 0, or -1 for a stream it cannot follow (a reference it does not have is not one: decoding goes on). */
int twl_hevc_dpb_slice(struct twl_hevc_dpb *dpb, const uint16_t *rpm, struct twl_hevc_slice *slice);
/* the picture of the last slices: decoded (or not: its slot is given back) */
void twl_hevc_dpb_done(struct twl_hevc_dpb *dpb, bool decoded);
int twl_hevc_dpb_poc(const struct twl_hevc_dpb *dpb);
#endif
