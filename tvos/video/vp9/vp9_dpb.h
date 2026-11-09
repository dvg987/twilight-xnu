/* The VP9 decoder's reference map in user space (the kernel side is hakios.vp9.c): which slot each of the eight
 * references is, which slot a frame goes into, what refresh_frame_flags replace. Frames are handed out as they
 * are shown; VP9 does not reorder. */
#ifndef TWL_VP9_DPB_H
#define TWL_VP9_DPB_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/hakios.vdec.h>

struct twl_vp9_dpb;

enum twl_vp9_action {
	TWL_VP9_DECODE,         /* FRAME as filled in */
	TWL_VP9_EXISTING,       /* show_existing_frame: FRAME with SKIP | SHOW */
	TWL_VP9_DROP,           /* not decoded (before the first keyframe, or a reference missing): FRAME with SKIP */
};

struct twl_vp9_dpb *twl_vp9_dpb_create(void);
void twl_vp9_dpb_destroy(struct twl_vp9_dpb *dpb);
/* the kernel's slot count (TWL_VDEC_STREAM after the first frame; 10 until then) */
int twl_vp9_dpb_set_slots(struct twl_vp9_dpb *dpb, unsigned slots);
/* slots the player may still ask for (decoded into only when nothing else is free, the longest shown first) and the
 * ones on the video plane (never) */
void twl_vp9_dpb_set_held(struct twl_vp9_dpb *dpb, uint32_t held, uint32_t showing);
/* a frame's header (TWL_VP9_RPM_WORDS, from PREPARE): what to do, the request filled in, and whether a picture is
 * shown (show_frame, or show_existing_frame) */
enum twl_vp9_action twl_vp9_dpb_frame(struct twl_vp9_dpb *dpb, const uint16_t *rpm, struct twl_vp9_frame *frame,
    bool *shown);
/* the frame of the last DECODE: decoded (its slot replaces the references it refreshes) or not */
void twl_vp9_dpb_done(struct twl_vp9_dpb *dpb, bool decoded);
/* a packet's frames: a superframe's (its index at the end left out), or the packet as one; 0: a bad index */
unsigned twl_vp9_split(const uint8_t *data, uint32_t size, uint32_t offsets[8], uint32_t sizes[8]);
/* the stream starts again (a keyframe is needed) */
void twl_vp9_dpb_reset(struct twl_vp9_dpb *dpb);
#endif
