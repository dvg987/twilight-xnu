/* See hevc_dpb.h. The rules are vh265.c's (hevc_slice_segment_header_process, read_ref_pic_set,
 * apply_ref_pic_set, set_ref_pic_list), on the firmware's parse of the slice header. */
#include "hevc_dpb.h"
#include <stdlib.h>
#include <string.h>

#define RPM_END      0x8000
#define RPS_LT_BIT   14
#define RPS_USED_BIT 13
#define INVALID_POC  ((int32_t)0x80000000)

enum { B_SLICE, P_SLICE, I_SLICE };
enum {
	NAL_RASL_N = 8, NAL_RASL_R = 9, NAL_BLA_W_LP = 16, NAL_BLA_W_RADL, NAL_BLA_N_LP, NAL_IDR_W_RADL, NAL_IDR_N_LP,
	NAL_CRA,
};

/* the firmware's parse (vh265.c union param_u; the kernel's copy is hakios.hevc.c's) */
union param {
	uint16_t data[TWL_HEVC_RPM_WORDS];
	struct {
		uint16_t CUR_RPS[0x10];
		uint16_t num_ref_idx_l0_active, num_ref_idx_l1_active, slice_type, slice_temporal_mvp_enable_flag;
		uint16_t dependent_slice_segment_flag, slice_segment_address, num_title_rows_minus1;
		uint16_t pic_width_in_luma_samples, pic_height_in_luma_samples;
		uint16_t log2_min_coding_block_size_minus3, log2_diff_max_min_coding_block_size;
		uint16_t log2_max_pic_order_cnt_lsb_minus4, POClsb, collocated_from_l0_flag, collocated_ref_idx;
		uint16_t log2_parallel_merge_level, five_minus_max_num_merge_cand, sps_num_reorder_pics_0;
		uint16_t modification_flag, tiles_enabled_flag, num_tile_columns_minus1, num_tile_rows_minus1;
		uint16_t tile_width[12], tile_height[8];
		uint16_t misc_flag0, pps_beta_offset_div2, pps_tc_offset_div2, slice_beta_offset_div2;
		uint16_t slice_tc_offset_div2, pps_cb_qp_offset, pps_cr_qp_offset, first_slice_segment_in_pic_flag;
		uint16_t m_temporalId, m_nalUnitType;
		uint16_t vui[4], bit_depth, profile_etc, sei_frame_field_info, video_signal_type;
		uint16_t modification_list[0x20];
	} p;
};

struct pic {
	bool used, referenced, long_term;
	int poc, period;
};

struct twl_hevc_dpb {
	unsigned slots;
	uint32_t held, showing;     /* slots shown lately; the plane's now (never decoded into) */
	uint32_t picks, forced, forced_used, forced_held;   /* diagnosis: held slots decoded into, and what filled the rest */
	struct pic pics[TWL_HEVC_SLOTS];
	int cur;                /* the slot of the picture whose slices are being decoded, or -1 */
	int poc, prev_tid0_poc, poc_random_access, period;
	bool started;
};

struct rps { int num_neg, num_pos, num_lt; int st0[16], st1[16], lt[16]; };

struct twl_hevc_dpb *
twl_hevc_dpb_create(void)
{
	struct twl_hevc_dpb *d = calloc(1, sizeof(*d));
	if (!d) return NULL;
	d->slots = 8;
	d->cur = -1;
	d->poc = INVALID_POC;
	d->poc_random_access = INT32_MAX;
	return d;
}

void
twl_hevc_dpb_destroy(struct twl_hevc_dpb *d)
{
	free(d);
}

int
twl_hevc_dpb_set_slots(struct twl_hevc_dpb *d, unsigned slots)
{
	if (slots < 2 || slots > TWL_HEVC_SLOTS) return -1;
	d->slots = slots;
	return 0;
}

void
twl_hevc_dpb_set_held(struct twl_hevc_dpb *d, uint32_t held, uint32_t showing)
{
	d->held = held;
	d->showing = showing;
}

int
twl_hevc_dpb_poc(const struct twl_hevc_dpb *d)
{
	return d->poc;
}

static int
find(const struct twl_hevc_dpb *d, int poc)
{
	for (unsigned i = 0; i < d->slots; i++)
		if (d->pics[i].used && d->pics[i].referenced && d->pics[i].poc == poc && d->pics[i].period == d->period) return (int)i;
	return -1;
}

static int
rps_poc(uint16_t v, int poc)
{
	int delta = v & ((1 << (RPS_USED_BIT - 1)) - 1);
	return (v & (1 << (RPS_USED_BIT - 1))) ? poc - ((1 << (RPS_USED_BIT - 1)) - delta) : poc + delta;
}

static void
read_ref_pic_set(const union param *rp, int poc, struct rps *r)
{
	memset(r, 0, sizeof(*r));
	for (int i = 0; i < 16; i++) {
		uint16_t v = rp->p.CUR_RPS[i];
		if (v == RPM_END) break;
		if (!((v >> RPS_USED_BIT) & 1)) continue;
		int ref = rps_poc(v, poc), lt = (v >> RPS_LT_BIT) & 1;
		if ((v >> (RPS_USED_BIT - 1)) & 1) {
			if (lt) r->lt[r->num_lt++] = ref; else r->st0[r->num_neg++] = ref;
		} else {
			if (lt) r->lt[r->num_lt++] = ref; else r->st1[r->num_pos++] = ref;
		}
	}
}

/* every picture the set does not name is no longer a reference (an IDR's set is empty) */
static void
apply_ref_pic_set(struct twl_hevc_dpb *d, const union param *rp, int poc)
{
	for (unsigned k = 0; k < d->slots; k++) {
		struct pic *p = &d->pics[k];
		if (!p->used) continue;
		p->long_term = false;
		bool kept = false;
		if (p->referenced && p->period == d->period) {
			for (int i = 0; i < 16 && rp->p.CUR_RPS[i] != RPM_END; i++) {
				if (rps_poc(rp->p.CUR_RPS[i], poc) == p->poc) {
					p->long_term = (rp->p.CUR_RPS[i] >> RPS_LT_BIT) & 1;
					kept = true;
					break;
				}
			}
		}
		if (!kept) p->referenced = false;
		if (!p->referenced) p->used = false;    /* shown at once: nothing waits to be output */
	}
}

static int
free_slot(struct twl_hevc_dpb *d)
{
	d->picks++;
	for (int pass = 0; pass < 2; pass++)
		for (unsigned i = 0; i < d->slots; i++)
			if (!d->pics[i].used && !(d->showing & (1u << i)) && (pass || !(d->held & (1u << i)))) {
				if (pass) {
					d->forced++;
					for (unsigned k = 0; k < d->slots; k++) {
						d->forced_used += d->pics[k].used;
						d->forced_held += !d->pics[k].used && (d->held & (1u << k)) != 0;
					}
				}
				return (int)i;
			}
	return -1;
}

/* diagnosis: of the pictures since the last call, how many went into a held slot, and on average how many slots the DPB
 * itself kept (references, pictures to output) and how many the holds kept when that happened */
void
twl_hevc_dpb_stats(struct twl_hevc_dpb *d, unsigned *picks, unsigned *forced, unsigned *used, unsigned *held)
{
	*picks = d->picks;
	*forced = d->forced;
	*used = d->forced ? d->forced_used / d->forced : 0;
	*held = d->forced ? d->forced_held / d->forced : 0;
	d->picks = d->forced = d->forced_used = d->forced_held = 0;
}

int
twl_hevc_dpb_slice(struct twl_hevc_dpb *d, const uint16_t *rpm, struct twl_hevc_slice *s)
{
	union param rp;
	struct rps rps;
	memcpy(rp.data, rpm, sizeof(rp.data));
	int nal = rp.p.m_nalUnitType;
	bool new_pic = rp.p.slice_segment_address == 0;
	memset(s->ref_poc, 0, sizeof(s->ref_poc));
	memset(s->ref_slot, 0, sizeof(s->ref_slot));
	s->ref_num[0] = s->ref_num[1] = 0;
	s->long_term = 0;
	s->col_slot = -1;
	s->col_poc = 0;
	s->flags &= ~TWL_HEVC_SKIP;

	if (new_pic) {
		if (nal == NAL_IDR_W_RADL || nal == NAL_IDR_N_LP) {
			d->period++;
			d->poc = 0;
		} else {
			int max_lsb = 1 << (rp.p.log2_max_pic_order_cnt_lsb_minus4 + 4), lsb = rp.p.POClsb;
			int prev_lsb = d->prev_tid0_poc & (max_lsb - 1), prev_msb = d->prev_tid0_poc - prev_lsb, msb;
			if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
			else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
			else msb = prev_msb;
			if (nal == NAL_BLA_W_LP || nal == NAL_BLA_W_RADL || nal == NAL_BLA_N_LP) {
				msb = 0;
				d->period++;
			}
			d->poc = msb + lsb;
		}
		if (rp.p.m_temporalId - 1 == 0) d->prev_tid0_poc = d->poc;
		/* RASL pictures after the first CRA (or a BLA) refer to pictures before it: skipped */
		bool rap = nal == NAL_CRA || nal == NAL_BLA_W_LP || nal == NAL_BLA_W_RADL || nal == NAL_BLA_N_LP;
		if (d->poc_random_access == INT32_MAX) {
			d->poc_random_access = rap ? d->poc : -INT32_MAX;
		} else if (nal == NAL_BLA_W_LP || nal == NAL_BLA_W_RADL || nal == NAL_BLA_N_LP) {
			d->poc_random_access = d->poc;
		} else if (d->poc < d->poc_random_access && (nal == NAL_RASL_N || nal == NAL_RASL_R)) {
			d->cur = -1;
			s->flags |= TWL_HEVC_SKIP;
			s->slot = 0;
			s->poc = d->poc;
			return 0;
		}
		apply_ref_pic_set(d, &rp, d->poc);
		d->cur = free_slot(d);
		if (d->cur < 0) return -1;
		struct pic *p = &d->pics[d->cur];
		p->used = p->referenced = true;
		p->long_term = false;
		p->poc = d->poc;
		p->period = d->period;
		d->started = true;
	} else if (d->cur < 0) {
		/* a further slice of a skipped (or failed) picture */
		s->flags |= TWL_HEVC_SKIP;
		s->slot = 0;
		return 0;
	}
	s->slot = (uint32_t)d->cur;
	s->poc = d->poc;
	/* the slots a reference names (the kernel keeps their compressed bodies; the others' can go) */
	s->referenced = 1u << 31;
	for (unsigned i = 0; i < d->slots && i < 31; i++)
		if (d->pics[i].used && d->pics[i].referenced) s->referenced |= 1u << i;

	read_ref_pic_set(&rp, d->poc, &rps);
	int n0 = rp.p.num_ref_idx_l0_active > 16 ? 16 : rp.p.num_ref_idx_l0_active;
	int n1 = rp.p.num_ref_idx_l1_active > 16 ? 16 : rp.p.num_ref_idx_l1_active;
	int neg = rps.num_neg, pos = rps.num_pos, total = neg + pos + rps.num_lt;
	if (rp.p.slice_type != I_SLICE && total > 0) {
		for (int list = 0; list < (rp.p.slice_type == B_SLICE ? 2 : 1); list++) {
			int n = list ? n1 : n0;
			for (int i = 0; i < n; i++) {
				int c;
				if (list == 0) c = (rp.p.modification_flag & 1) ? rp.p.modification_list[i] : i % total;
				else c = (rp.p.modification_flag & 2) ?
				    rp.p.modification_list[(rp.p.modification_flag & 1) ? n0 + i : i] : i % total;
				if (c >= total) return -1;
				int poc = c >= neg + pos ? rps.lt[c - neg - pos] :
				    list == 0 ? (c >= neg ? rps.st1[c - neg] : rps.st0[c]) : (c >= pos ? rps.st0[c - pos] : rps.st1[c]);
				int slot = find(d, poc);
				s->ref_poc[list][i] = poc;
				/* a reference the stream lost (a cut, a seek): the picture itself stands in, as vh265 does */
				s->ref_slot[list][i] = (uint32_t)(slot >= 0 ? slot : d->cur);
				if (slot >= 0 && d->pics[slot].long_term) s->long_term |= 1u << (list * 16 + i);
			}
			s->ref_num[list] = (uint32_t)n;
		}
		int list = rp.p.slice_type == B_SLICE ? 1 - rp.p.collocated_from_l0_flag : 0;
		int col = rp.p.collocated_ref_idx;
		if (col < (int)s->ref_num[list]) {
			s->col_poc = s->ref_poc[list][col];
			s->col_slot = find(d, s->col_poc);
		}
	}
	return 0;
}

void
twl_hevc_dpb_done(struct twl_hevc_dpb *d, bool decoded)
{
	if (!decoded && d->cur >= 0) d->pics[d->cur].used = d->pics[d->cur].referenced = false;
	d->cur = -1;
}
