/* See vp9_dpb.h. The rules are vvp9.c's vp9_bufmgr_process and refresh_ref_frames, on the firmware's parse of the
 * frame's header. */
#include "vp9_dpb.h"
#include <stdlib.h>
#include <string.h>

/* the firmware's parse (vvp9.c union param_u), the words read here */
enum {
	P_PROFILE, P_SHOW_EXISTING, P_FRAME_TO_SHOW, P_FRAME_TYPE, P_SHOW_FRAME, P_ERROR_RESILIENT, P_INTRA_ONLY,
	P_DISPLAY_SIZE, P_RESET_CONTEXT, P_REFRESH, P_WIDTH, P_HEIGHT, P_DISPLAY_W, P_DISPLAY_H, P_REF_INFO,
};
#define KEY_FRAME 0

struct twl_vp9_dpb {
	unsigned slots;
	uint32_t held, showing;
	uint32_t shown_at[TWL_VP9_SLOTS], clock;      /* when each slot's picture was last given out to show */
	bool cur_shown;
	int map[8];             /* the eight references' slots, -1: none */
	bool has_key;
	int cur;                /* the slot of the frame being decoded, -1: none */
	uint8_t refresh;
};

struct twl_vp9_dpb *
twl_vp9_dpb_create(void)
{
	struct twl_vp9_dpb *d = calloc(1, sizeof(*d));
	if (!d) return NULL;
	d->slots = 10;
	twl_vp9_dpb_reset(d);
	return d;
}

void
twl_vp9_dpb_destroy(struct twl_vp9_dpb *d)
{
	free(d);
}

void
twl_vp9_dpb_reset(struct twl_vp9_dpb *d)
{
	for (int i = 0; i < 8; i++) d->map[i] = -1;
	d->has_key = false;
	d->cur = -1;
}

int
twl_vp9_dpb_set_slots(struct twl_vp9_dpb *d, unsigned slots)
{
	if (slots < 9 || slots > TWL_VP9_SLOTS) return -1;
	d->slots = slots;
	return 0;
}

void
twl_vp9_dpb_set_held(struct twl_vp9_dpb *d, uint32_t held, uint32_t showing)
{
	d->held = held;
	d->showing = showing;
}

static int
free_slot(const struct twl_vp9_dpb *d)
{
	int oldest = -1;
	for (unsigned i = 0; i < d->slots; i++) {
		bool referenced = false;
		for (int r = 0; r < 8; r++) referenced |= d->map[r] == (int)i;
		if (referenced || (d->showing & (1u << i))) continue;
		if (!(d->held & (1u << i))) return (int)i;
		if (oldest < 0 || d->shown_at[i] < d->shown_at[oldest]) oldest = (int)i;
	}
	return oldest;
}

enum twl_vp9_action
twl_vp9_dpb_frame(struct twl_vp9_dpb *d, const uint16_t *p, struct twl_vp9_frame *f, bool *shown)
{
	f->flags &= ~(uint32_t)(TWL_VP9_SKIP | TWL_VP9_SHOW);
	d->cur = -1;
	*shown = false;
	if (p[P_SHOW_EXISTING]) {
		int slot = d->map[p[P_FRAME_TO_SHOW] & 7];
		f->flags |= TWL_VP9_SKIP;
		if (slot < 0) return TWL_VP9_DROP;
		f->flags |= TWL_VP9_SHOW;
		f->slot = (uint32_t)slot;
		d->shown_at[slot] = ++d->clock;
		*shown = true;
		return TWL_VP9_EXISTING;
	}
	bool key = p[P_FRAME_TYPE] == KEY_FRAME;
	bool intra_only = !key && !p[P_SHOW_FRAME] && p[P_INTRA_ONLY];
	if (!d->has_key && !key && !intra_only) {
		f->flags |= TWL_VP9_SKIP;
		return TWL_VP9_DROP;
	}
	if (!key && !intra_only) {
		for (int i = 0; i < 3; i++) {
			int slot = d->map[(p[P_REF_INFO] >> ((2 - i) * 4 + 1)) & 7];
			if (slot < 0) {
				f->flags |= TWL_VP9_SKIP;
				return TWL_VP9_DROP;
			}
			f->ref_slot[i] = (uint32_t)slot;
		}
	} else {
		f->ref_slot[0] = f->ref_slot[1] = f->ref_slot[2] = 0;
	}
	d->cur = free_slot(d);
	if (d->cur < 0) {
		f->flags |= TWL_VP9_SKIP;
		return TWL_VP9_DROP;
	}
	d->has_key = true;
	d->refresh = key ? 0xff : (uint8_t)p[P_REFRESH];
	f->slot = (uint32_t)d->cur;
	f->referenced = 1u << 31;                       /* the slots the references name: the others' bodies can go */
	for (int r = 0; r < 8; r++) {
		if (d->map[r] >= 0) f->referenced |= 1u << d->map[r];
	}
	*shown = d->cur_shown = p[P_SHOW_FRAME] != 0;
	return TWL_VP9_DECODE;
}

void
twl_vp9_dpb_done(struct twl_vp9_dpb *d, bool decoded)
{
	if (decoded && d->cur >= 0) {
		for (int i = 0; i < 8; i++)
			if (d->refresh & (1u << i)) d->map[i] = d->cur;
		if (d->cur_shown) d->shown_at[d->cur] = ++d->clock;
	}
	d->cur = -1;
}

unsigned
twl_vp9_split(const uint8_t *data, uint32_t size, uint32_t offsets[8], uint32_t sizes[8])
{
	uint8_t marker = size ? data[size - 1] : 0;
	if ((marker & 0xe0) == 0xc0) {
		unsigned bytes = 1 + ((marker >> 3) & 3), frames = 1 + (marker & 7), index = 2 + bytes * frames;
		if (size >= index && data[size - index] == marker) {
			const uint8_t *at = data + size - index + 1;
			uint32_t offset = 0;
			for (unsigned i = 0; i < frames; i++) {
				uint32_t s = 0;
				for (unsigned b = 0; b < bytes; b++) s |= (uint32_t)*at++ << (b * 8);
				if (offset + s > size - index) return 0;
				offsets[i] = offset;
				sizes[i] = s;
				offset += s;
			}
			return frames;
		}
	}
	offsets[0] = 0;
	sizes[0] = size;
	return 1;
}
