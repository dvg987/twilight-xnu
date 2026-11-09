/*
 * HEVC bring-up on the box (boot-arg vdec_debug=1): drives the HEVC core from user space through the probe node's
 * debug ioctls, double write only (8-bit linear NV12-ish, no compressed frames), and checks each picture against
 * FFmpeg's MD5s. The sequence follows Amlogic's vh265.c (single-instance flow, S7D settings); it is a development
 * tool, not a decoder.
 *
 *   hevcdev PACKETS MD5S [FRAMES] [OUT.yuv]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/hakios.vdec.h>
#include "../video/hevc/hevc_regs.h"

/* ---- the debug backend ---- */

static int fd = -1;
static uint64_t pool_pa;
static uint32_t pool_size;
static unsigned verbose;

static void
die(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	exit(1);
}

static uint32_t
rd(uint32_t reg)
{
	struct twl_vdec_dbg_reg r = { reg, 0, 0, 0 };
	if (verbose > 2) fprintf(stderr, "  r %04x\n", reg);
	if (ioctl(fd, TWL_VDEC_DBG_REG, &r) != 0) die("read %#x: %s\n", reg, strerror(errno));
	if (verbose > 2) fprintf(stderr, "    = %08x\n", r.value);
	return r.value;
}

static void
wr(uint32_t reg, uint32_t value)
{
	struct twl_vdec_dbg_reg r = { reg, value, 1, 0 };
	if (verbose > 1) fprintf(stderr, "  w %04x = %08x\n", reg, value);
	if (ioctl(fd, TWL_VDEC_DBG_REG, &r) != 0) die("write %#x: %s\n", reg, strerror(errno));
}

#define READ_VREG(r) rd(r)
#define WRITE_VREG(r, v) wr(r, v)
#define SET_VREG_MASK(r, m) wr(r, rd(r) | (m))
#define CLEAR_VREG_MASK(r, m) wr(r, rd(r) & ~(m))

static void
mem(uint32_t offset, void *buffer, uint32_t size, int write)
{
	struct twl_vdec_dbg_mem m = { (uint64_t)(uintptr_t)buffer, offset, size, (uint32_t)write, 0 };
	if (verbose > 1) fprintf(stderr, "  mem %s %#x+%#x\n", write ? "write" : "read", offset, size);
	if (ioctl(fd, TWL_VDEC_DBG_MEM, &m) != 0) die("pool %s %#x+%#x: %s\n", write ? "write" : "read", offset, size,
	    strerror(errno));
}

static double
now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- the pool: work space, stream, MV and picture buffers ---- */

#define ALIGN(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define PAGE 0x1000u

struct buf { uint32_t start, size; };
static struct {
	struct buf ipp, sao_abv, sao_vb, short_term_rps, vps, sps, pps, sao_up, swap_buf, swap_buf2, scalelut,
	    dblk_para, dblk_data, dblk_data2, mmu_vbh, mmu_vbh_dw, cm_header_dw, mpred_above, rpm, lmem;
} ws;
#define STREAM_OFF   0x00800000u
#define STREAM_SIZE  0x007f0000u   /* up to the MMU map */
#define MMU_MAP_OFF  0x00ff0000u    /* the current picture's page map: one 32-bit page number per 4 KiB */
#define MMU_MAP_SIZE 0x00010000u
#define FRAMES_OFF   0x01000000u

static uint32_t
pa(uint32_t offset)
{
	return (uint32_t)(pool_pa + offset);
}

/* amvh265_workbuff_spec[5] (S7D: newer than TM2, 4K capable), laid out as init_buff_spec does */
static void
layout_workspace(void)
{
	uint32_t at = 0;
	struct { struct buf *b; uint32_t size; } spec[] = {
		{ &ws.ipp, 0x4000 * 2 }, { &ws.sao_abv, 0 }, { &ws.sao_vb, 0 }, { &ws.short_term_rps, 0x800 },
		{ &ws.vps, 0x800 }, { &ws.sps, 0x800 }, { &ws.pps, 0x2000 }, { &ws.sao_up, 0 }, { &ws.swap_buf, 0x800 },
		{ &ws.swap_buf2, 0x800 }, { &ws.scalelut, 0x8000 }, { &ws.dblk_para, 0x40100 },
		{ &ws.dblk_data, 0x140800 }, { &ws.dblk_data2, 0x100800 }, { &ws.mmu_vbh, 0xa000 },
		{ &ws.mmu_vbh_dw, 0xa000 << 1 }, { &ws.cm_header_dw, 0 }, { &ws.mpred_above, 0x8000 },
		{ &ws.rpm, 0x80 * 2 }, { &ws.lmem, 0x500 * 2 },
	};
	for (unsigned i = 0; i < sizeof(spec) / sizeof(spec[0]); i++) {
		at = (uint32_t)ALIGN(at, PAGE);
		spec[i].b->start = pa(at);
		spec[i].b->size = spec[i].size;
		at += spec[i].size;
	}
	if (at > STREAM_OFF) die("work space %#x too big\n", at);
}

/* ---- the RPM: the firmware's parse of the slice header (vh265.c union param_u, from ucode lmem) ---- */

#define RPM_BEGIN 0x100
#define RPM_END   0x180
union param {
	uint16_t data[RPM_END - RPM_BEGIN];
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
		uint16_t vui_num_units_in_tick_hi, vui_num_units_in_tick_lo, vui_time_scale_hi, vui_time_scale_lo;
		uint16_t bit_depth, profile_etc, sei_frame_field_info, video_signal_type;
		uint16_t modification_list[0x20];
		uint16_t conformance_window_flag, conf_win_left_offset, conf_win_right_offset, conf_win_top_offset;
		uint16_t conf_win_bottom_offset, chroma_format_idc, color_description, aspect_ratio_idc;
		uint16_t sar_width, sar_height, sps_max_dec_pic_buffering_minus1_0;
	} p;
};

enum { B_SLICE, P_SLICE, I_SLICE };
enum {
	NAL_TRAIL_N, NAL_TRAIL_R, NAL_TSA_N, NAL_TSA_R, NAL_STSA_N, NAL_STSA_R, NAL_RADL_N, NAL_RADL_R, NAL_RASL_N,
	NAL_RASL_R, NAL_BLA_W_LP = 16, NAL_BLA_W_RADL, NAL_BLA_N_LP, NAL_IDR_W_RADL, NAL_IDR_N_LP, NAL_CRA,
};
#define RPS_END      0x8000
#define RPS_LT_BIT   14
#define RPS_USED_BIT 13
#define INVALID_POC  ((int)0x80000000)
#define MAX_REF_ACTIVE 16

#define PCM_LOOP_FILTER_DISABLED_FLAG_BIT 0
#define PCM_ENABLE_FLAG_BIT 1
#define LOOP_FILER_ACROSS_TILES_ENABLED_FLAG_BIT 2
#define PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT 3
#define DEBLOCKING_FILTER_OVERRIDE_ENABLED_FLAG_BIT 4
#define PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT 5
#define DEBLOCKING_FILTER_OVERRIDE_FLAG_BIT 6
#define SLICE_DEBLOCKING_FILTER_DISABLED_FLAG_BIT 7
#define SLICE_SAO_LUMA_FLAG_BIT 8
#define SLICE_SAO_CHROMA_FLAG_BIT 9
#define SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT 10

/* ---- pictures ---- */

#define MAX_PICS 16
#define MAX_SLICES 64
struct pic {
	int used;               /* holds a picture (referenced or not yet output) */
	int referenced, long_term, output_pending;
	int poc, idr_period, decode_index;
	int slice_type, slice_idx;
	int ref_num[2];
	int ref_poc[2][MAX_SLICES][16];
	uint32_t y, uv, mv, mv_size;    /* physical: the linear copy (luma, chroma), co-located MVs */
	uint32_t cm;                    /* physical: the compressed picture (body, then header), if any */
	uint32_t y_off;                 /* pool offset of the linear copy */
};

static struct {
	union param param;
	int pic_w, pic_h, lcu_size, lcu_size_log2, lcu_x_num, lcu_y_num, lcu_total;
	int curr_poc, prev_tid0_poc, poc_random_access, idr_period;
	int new_pic, slice_addr, slice_segment_addr, is_next_slice_segment;
	int slice_type, tmvp, ldc, col_from_l0, plevel, max_merge, ref_num[2], col_poc;
	int misc_flag0;
	int tile_enabled, num_tile_col, num_tile_row, new_tile, tile_x, tile_y;
	int tile_start_lcu_x, tile_start_lcu_y, tile_width_lcu, tile_height_lcu;
	struct { int width, height, start_cu_x, start_cu_y; } tile[20][10];
	struct pic pics[MAX_PICS];
	struct pic *cur, *col;
	int pics_ready, n_pics;
	uint32_t frame_bytes, uv_offset, stride, rows, mv_bytes;
	int compressed;                 /* compressed references (needed for 10 bits) + the 8-bit linear copy */
	int mmu;                        /* ... their body in pages listed in the map (h265_mmu firmware) */
	uint32_t cm_body, cm_header, cm_bytes, dw_offset;
	int decode_index;
	uint32_t endian;
	uint32_t start_decoding_flag, rps_set_id;   /* what the firmware has seen (VPS/SPS/PPS/IDR), kept across runs */
	uint32_t swap_addr;     /* where the TEE put the firmware's overlay code: back into its register every run */
} h;

struct rps { int num_neg, num_pos, num_lt; int st0[16], st1[16], lt[16]; };

#define HEVC_CONFIG_LITTLE_ENDIAN ((0xff0 << 8) | 0xf)

static uint32_t
uv_pages(int w, int hh)
{
	/* calc_buffer_u_v_h_size(w, h, 1, 64): chroma bytes in 4K pages for the 64-aligned picture */
	unsigned lw = (w + 63) / 64, lh = (hh + 63) / 64;
	return ((lw * lh * 64 * 64 / 2) + 0xfff) >> 12;
}

/* init_pic_list_hw + init_decode_head_hw: the MC's table of reference addresses and the compressed frames' sizes
 * (both lost with the core's reset: every run) */
static void
init_pic_list_hw(void)
{
	WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, (1 << 1) | (1 << 2));
	for (int i = 0; i < h.n_pics; i++) {
		if (h.mmu) {
			WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_DATA, h.pics[i].cm >> 5);           /* the header */
		} else if (h.compressed) {
			WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_DATA, h.pics[i].cm >> 5);
		} else {
			WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_DATA, h.pics[i].y >> 5);
			WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_DATA, h.pics[i].uv >> 5);
		}
	}
	WRITE_VREG(HEVCD_MPP_ANC2AXI_TBL_CONF_ADDR, 1);
	WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, (0 << 8) | (0 << 1) | 1);
	for (int i = 0; i < 32; i++) WRITE_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR, 0);
	if (h.compressed) {
		WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, h.mmu ? 1u << 4 : 0);
		WRITE_VREG(HEVCD_MPP_DECOMP_CTL2, h.mmu ? 0 : h.cm_body >> 5);
		WRITE_VREG(HEVC_CM_BODY_LENGTH, h.cm_body);
		WRITE_VREG(HEVC_CM_HEADER_OFFSET, h.cm_body);
		WRITE_VREG(HEVC_CM_HEADER_LENGTH, h.mmu ? 32 * ((h.pic_w + 127) >> 7) * ((h.pic_h + 63) >> 6) : h.cm_header);
		uint32_t ctrl5 = READ_VREG(HEVC_SAO_CTRL5) & ~(1u << 15);
		if (h.mmu) {
			WRITE_VREG(HEVC_SAO_MMU_VH0_ADDR, ws.mmu_vbh.start);
			WRITE_VREG(HEVC_SAO_MMU_VH1_ADDR, ws.mmu_vbh.start + (ws.mmu_vbh.size >> 1));
			WRITE_VREG(HEVC_SAO_CTRL9, READ_VREG(HEVC_SAO_CTRL9) | 1);
			ctrl5 |= 1u << 10;              /* the header at HEVC_CM_HEADER_START_ADDR */
		}
		WRITE_VREG(HEVC_SAO_CTRL5, ctrl5);
	}
}

static void
init_pics(void)
{
	uint32_t uvp = uv_pages(h.pic_w, h.pic_h);
	h.stride = (uint32_t)ALIGN(h.pic_w, 64);
	h.rows = (uint32_t)ALIGN(h.pic_h, 64);
	h.uv_offset = uvp << 13;            /* luma: twice the chroma */
	/* compute_losless_comp_body_size / _header_size: 4 KiB per 64x32 (10 bits), 32 bytes per 128x64; with the MMU
	 * the header is hevc_get_header_size's and comes first */
	h.cm_body = 4096 * ((h.pic_w + 63) >> 6) * ((h.pic_h + 31) >> 5);
	h.cm_header = 32 * ((h.pic_w + 127) >> 7) * ((h.pic_h + 63) >> 6);
	uint32_t mmu_header = (uint32_t)(ALIGN(h.pic_w, 64) * ALIGN(h.pic_h, 64) > 1920 * 1088 ? 0x48000 : 0x10000);
	if (h.mmu) h.cm_header = mmu_header;
	h.cm_bytes = h.compressed ? (uint32_t)ALIGN(h.cm_body + h.cm_header, 0x10000) : 0;
	if (h.mmu && (h.cm_body >> 12) * 4 > MMU_MAP_SIZE) die("MMU map too small\n");
	h.dw_offset = h.cm_bytes;
	h.frame_bytes = h.cm_bytes + (uint32_t)ALIGN(h.uv_offset + (uvp << 12), 0x10000);
	unsigned mv_unit = h.lcu_size_log2 == 6 ? 0x200 : h.lcu_size_log2 == 5 ? 0x80 : 0x20;
	unsigned ex_w = (unsigned)ALIGN(h.pic_w, 64), ex_h = (unsigned)ALIGN(h.pic_h, 64);
	h.mv_bytes = (uint32_t)ALIGN((ex_w >> h.lcu_size_log2) * (ex_h >> h.lcu_size_log2) * mv_unit + 0x40000, 0x10000);
	uint32_t at = FRAMES_OFF;
	h.n_pics = (int)((pool_size - FRAMES_OFF) / (h.frame_bytes + h.mv_bytes));
	if (h.n_pics > MAX_PICS) h.n_pics = MAX_PICS;
	if (h.n_pics < 8) die("pool holds only %d pictures of %dx%d (vdec_pool_mb)\n", h.n_pics, h.pic_w, h.pic_h);
	for (int i = 0; i < h.n_pics; i++) {
		struct pic *p = &h.pics[i];
		memset(p, 0, sizeof(*p));
		p->cm = pa(at);
		p->y_off = at + h.dw_offset;
		p->y = pa(at + h.dw_offset);
		p->uv = p->y + h.uv_offset;
		at += h.frame_bytes;
		p->mv = pa(at);
		p->mv_size = h.mv_bytes;
		at += h.mv_bytes;
	}
	fprintf(stderr, "%d pictures: %dx%d %d-bit, stride %u rows %u, frame %#x (%s; chroma at %#x), MV %#x, %#x of pool used\n",
	    h.n_pics, h.pic_w, h.pic_h, (h.param.p.bit_depth & 0xf) + 8, h.stride, h.rows, h.frame_bytes,
	    h.compressed ? "compressed + linear copy" : "linear only", h.uv_offset, h.mv_bytes, at);

	init_pic_list_hw();
	/* stale pictures from an earlier run would pass for decoded ones */
	uint8_t *z = calloc(1, 0x100000);
	for (uint32_t off = FRAMES_OFF; off < at; off += 0x100000) mem(off, z, at - off < 0x100000 ? at - off : 0x100000, 1);
	free(z);
	h.pics_ready = 1;
}

static struct pic *
pic_by_poc(int poc)
{
	for (int i = 0; i < h.n_pics; i++)
		if (h.pics[i].used && h.pics[i].poc == poc && h.pics[i].idr_period == h.idr_period) return &h.pics[i];
	return NULL;
}

static int
read_ref_pic_set(int poc, struct rps *r)
{
	memset(r, 0, sizeof(*r));
	for (int i = 0; i < 16; i++) {
		uint16_t v = h.param.p.CUR_RPS[i];
		if (v == RPS_END) break;
		if (!((v >> RPS_USED_BIT) & 1)) continue;
		int delta = v & ((1 << (RPS_USED_BIT - 1)) - 1);
		int lt = (v >> RPS_LT_BIT) & 1;
		if ((v >> (RPS_USED_BIT - 1)) & 1) {
			int ref = poc - ((1 << (RPS_USED_BIT - 1)) - delta);
			if (lt) r->lt[r->num_lt++] = ref;
			else r->st0[r->num_neg++] = ref;
		} else {
			if (lt) r->lt[r->num_lt++] = poc + delta;
			else r->st1[r->num_pos++] = poc + delta;
		}
	}
	return r->num_neg + r->num_pos + r->num_lt;
}

static void
apply_ref_pic_set(int poc)
{
	for (int k = 0; k < h.n_pics; k++) {
		struct pic *p = &h.pics[k];
		if (!p->used) continue;
		p->long_term = 0;
		if (!p->referenced || p->poc == poc) continue;
		int kept = 0;
		if (p->idr_period == h.idr_period) {
			for (int i = 0; i < 16; i++) {
				uint16_t v = h.param.p.CUR_RPS[i];
				if (v == RPS_END) break;
				int delta = v & ((1 << (RPS_USED_BIT - 1)) - 1);
				int ref = (v & (1 << (RPS_USED_BIT - 1))) ? poc - ((1 << (RPS_USED_BIT - 1)) - delta) : poc + delta;
				if (ref == p->poc) {
					p->long_term = (v >> RPS_LT_BIT) & 1;
					kept = 1;
					break;
				}
			}
		}
		if (!kept) p->referenced = 0;
	}
}

static int
set_ref_pic_list(const struct rps *r)
{
	struct pic *p = h.cur;
	int n0 = h.param.p.num_ref_idx_l0_active > MAX_REF_ACTIVE ? MAX_REF_ACTIVE : h.param.p.num_ref_idx_l0_active;
	int n1 = h.param.p.num_ref_idx_l1_active > MAX_REF_ACTIVE ? MAX_REF_ACTIVE : h.param.p.num_ref_idx_l1_active;
	int neg = r->num_neg, pos = r->num_pos, total = neg + pos + r->num_lt;
	int *l0 = p->ref_poc[0][p->slice_idx], *l1 = p->ref_poc[1][p->slice_idx];
	for (int i = 0; i < 16; i++) l0[i] = l1[i] = 0;
	if (total > 0) {
		for (int i = 0; i < n0; i++) {
			int c = (h.param.p.modification_flag & 1) ? h.param.p.modification_list[i] : i % total;
			if (c >= total) return -1;
			l0[i] = c >= neg + pos ? r->lt[c - neg - pos] : c >= neg ? r->st1[c - neg] : r->st0[c];
		}
		if (h.param.p.slice_type == B_SLICE) {
			for (int i = 0; i < n1; i++) {
				int c = (h.param.p.modification_flag & 2) ?
				    h.param.p.modification_list[(h.param.p.modification_flag & 1) ? n0 + i : i] : i % total;
				if (c >= total) return -1;
				l1[i] = c >= neg + pos ? r->lt[c - neg - pos] : c >= pos ? r->st0[c - pos] : r->st1[c];
			}
		}
	}
	p->slice_type = h.param.p.slice_type == I_SLICE ? 2 : h.param.p.slice_type == P_SLICE ? 1 : 0;
	p->ref_num[0] = n0;
	p->ref_num[1] = n1;
	return 0;
}

static int
is_ref_long_term(int poc)
{
	struct pic *p = pic_by_poc(poc);
	return p && p->referenced && p->long_term;
}

static void
update_tile_info(int pic_width_cu, int pic_height_cu)
{
	h.tile_enabled = h.param.p.tiles_enabled_flag & 1;
	if (h.tile_enabled) {
		h.num_tile_col = h.param.p.num_tile_columns_minus1 + 1;
		h.num_tile_row = h.param.p.num_tile_rows_minus1 + 1;
		if (h.num_tile_row > 20 || h.num_tile_row <= 0) h.num_tile_row = 1;
		if (h.num_tile_col > 10 || h.num_tile_col <= 0) h.num_tile_col = 1;
		int start_y = 0;
		for (int i = 0; i < h.num_tile_row; i++) {
			int start_x = 0;
			for (int j = 0; j < h.num_tile_col; j++) {
				if (h.param.p.tiles_enabled_flag & 2) {
					h.tile[i][j].width = (j + 1) * pic_width_cu / h.num_tile_col - j * pic_width_cu / h.num_tile_col;
					h.tile[i][j].height = (i + 1) * pic_height_cu / h.num_tile_row - i * pic_height_cu / h.num_tile_row;
				} else {
					h.tile[i][j].width = j == h.num_tile_col - 1 ? pic_width_cu - start_x : h.param.p.tile_width[j];
					h.tile[i][j].height = i == h.num_tile_row - 1 ? pic_height_cu - start_y :
					    i < 8 ? h.param.p.tile_height[i] : 0;
				}
				h.tile[i][j].start_cu_x = start_x;
				h.tile[i][j].start_cu_y = start_y;
				start_x += h.tile[i][j].width;
			}
			start_y += h.tile[i][0].height;
		}
	} else {
		h.num_tile_col = h.num_tile_row = 1;
		h.tile[0][0].width = pic_width_cu;
		h.tile[0][0].height = pic_height_cu;
		h.tile[0][0].start_cu_x = h.tile[0][0].start_cu_y = 0;
	}
}

static int
get_tile_index(int cu_adr, int pic_width_lcu)
{
	int cu_x = cu_adr % pic_width_lcu, cu_y = cu_adr / pic_width_lcu, tx = 0, ty = 0;
	if (h.tile_enabled) {
		for (int i = 0; i < h.num_tile_col && cu_x >= h.tile[0][i].start_cu_x; i++) tx = i;
		for (int i = 0; i < h.num_tile_row && cu_y >= h.tile[i][0].start_cu_y; i++) ty = i;
	}
	return tx | ty << 8;
}

/* ---- per-slice hardware set-up (vh265.c config_*) ---- */

static void
config_title_hw(int sao_vb_size, int sao_mem_unit)
{
	WRITE_VREG(HEVC_sao_mem_unit, sao_mem_unit);
	WRITE_VREG(HEVC_SAO_ABV, ws.sao_abv.start);
	WRITE_VREG(HEVC_sao_vb_size, sao_vb_size);
	WRITE_VREG(HEVC_SAO_VB, ws.sao_vb.start);
}

static void
config_mc_buffer(void)
{
	struct pic *c = h.cur;
	for (int list = 0; list < 2; list++) {
		if (list == 0 && c->slice_type == 2) continue;
		if (list == 1 && c->slice_type != 0) continue;
		WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, ((list ? 16 : 0) << 8) | (0 << 1) | 1);
		for (int i = 0; i < c->ref_num[list]; i++) {
			struct pic *r = pic_by_poc(c->ref_poc[list][c->slice_idx][i]);
			if (!r) {
				fprintf(stderr, "POC %d: no reference POC %d in list %d\n", c->poc, c->ref_poc[list][c->slice_idx][i], list);
				r = c;
			}
			int index = (int)(r - h.pics), cy = h.compressed ? index : index << 1, cuv = h.compressed ? index : (index << 1) + 1;
			WRITE_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR, (cuv << 16) | (cuv << 8) | cy);
		}
	}
}

static void
config_mcrcc_axi_hw(int slice_type)
{
	uint32_t a, b;
	int l0 = h.cur->ref_num[0], l1 = h.cur->ref_num[1];
	WRITE_VREG(HEVCD_MCRCC_CTL1, 0x2);
	if (slice_type == 2) {
		WRITE_VREG(HEVCD_MCRCC_CTL1, 0x0);
		return;
	}
	if (slice_type == 0) {
		WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, (0 << 8) | (0 << 1) | 0);
		a = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
		a |= a << 16;
		WRITE_VREG(HEVCD_MCRCC_CTL2, a);
		WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, (16 << 8) | (1 << 1) | 0);
		b = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
		b |= b << 16;
		if (a == b && l1 > 1) {
			b = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
			b |= b << 16;
		}
		WRITE_VREG(HEVCD_MCRCC_CTL3, b);
	} else {
		WRITE_VREG(HEVCD_MPP_ANC_CANVAS_ACCCONFIG_ADDR, (0 << 8) | (1 << 1) | 0);
		a = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
		a |= a << 16;
		WRITE_VREG(HEVCD_MCRCC_CTL2, a);
		if (l0 == 1) {
			WRITE_VREG(HEVCD_MCRCC_CTL3, a);
		} else {
			a = READ_VREG(HEVCD_MPP_ANC_CANVAS_DATA_ADDR) & 0xffff;
			a |= a << 16;
			WRITE_VREG(HEVCD_MCRCC_CTL3, a);
		}
	}
	WRITE_VREG(HEVCD_MCRCC_CTL1, 0xff0);
}

static void
config_mpred_hw(void)
{
	struct pic *c = h.cur, *col = h.col;
	int above_en = 1, mv_wr_en = 1, mv_rd_en = h.slice_type != 2, col_intra = 0;
	uint32_t rd_start;
	if (!col) {
		rd_start = c->mv;
		col_intra = 1;
	} else {
		rd_start = col->mv;
	}
	uint32_t unit = h.lcu_size_log2 == 6 ? 0x200 : h.lcu_size_log2 == 5 ? 0x80 : 0x20;
	uint32_t rd_ptr = rd_start + h.slice_addr * unit;
	uint32_t rd_end = rd_start + (col ? col->mv_size : c->mv_size);
	uint32_t wr_ptr = c->mv + h.slice_addr * unit;
	WRITE_VREG(HEVC_MPRED_MV_WR_START_ADDR, c->mv);
	WRITE_VREG(HEVC_MPRED_MV_RD_START_ADDR, rd_start);
	uint32_t d = (h.lcu_x_num - h.tile_width_lcu) * unit;
	WRITE_VREG(HEVC_MPRED_MV_WR_ROW_JUMP, d);
	WRITE_VREG(HEVC_MPRED_MV_RD_ROW_JUMP, d);
	d = (h.slice_type & 3) | (h.new_pic & 1) << 2 | (h.new_tile & 1) << 3 | (h.is_next_slice_segment & 1) << 4 |
	    (h.tmvp & 1) << 5 | (h.ldc & 1) << 6 | (h.col_from_l0 & 1) << 7 | 0 << 8 | (above_en & 1) << 9 |
	    (mv_wr_en & 1) << 10 | (mv_rd_en & 1) << 11 | (col_intra & 1) << 12 | 1 << 13 | 0 << 14 | 0 << 15 |
	    (h.lcu_size_log2 & 0xf) << 16 | 3 << 20 | (h.plevel & 7) << 24;
	WRITE_VREG(HEVC_MPRED_CTRL0, d & ~(1u << 28));
	WRITE_VREG(HEVC_MPRED_CTRL1, h.max_merge | 2 << 4 | 3 << 8 | 5 << 12 | 36 << 16);
	WRITE_VREG(HEVC_MPRED_PIC_SIZE, h.pic_w | h.pic_h << 16);
	WRITE_VREG(HEVC_MPRED_PIC_SIZE_LCU, (h.lcu_x_num - 1) | (h.lcu_y_num - 1) << 16);
	WRITE_VREG(HEVC_MPRED_TILE_START, h.tile_start_lcu_x | h.tile_start_lcu_y << 16);
	WRITE_VREG(HEVC_MPRED_TILE_SIZE_LCU, h.tile_width_lcu | h.tile_height_lcu << 16);
	WRITE_VREG(HEVC_MPRED_REF_NUM, h.ref_num[0] | h.ref_num[1] << 8);
	d = 0;
	for (int i = 0; i < h.ref_num[0]; i++) if (is_ref_long_term(c->ref_poc[0][c->slice_idx][i])) d |= 1u << i;
	for (int i = 0; i < h.ref_num[1]; i++) if (is_ref_long_term(c->ref_poc[1][c->slice_idx][i])) d |= 1u << (i + 16);
	WRITE_VREG(HEVC_MPRED_LT_REF, d);
	WRITE_VREG(HEVC_MPRED_REF_EN_L0, (1u << h.ref_num[0]) - 1);
	WRITE_VREG(HEVC_MPRED_REF_EN_L1, (1u << h.ref_num[1]) - 1);
	WRITE_VREG(HEVC_MPRED_CUR_POC, h.curr_poc);
	if (col) WRITE_VREG(HEVC_MPRED_COL_POC, h.col_poc);
	/* in pairs, L0 then L1 */
	for (int i = 0; i < 16; i++) {
		WRITE_VREG(HEVC_MPRED_L0_REF00_POC + i, c->ref_poc[0][c->slice_idx][i]);
		WRITE_VREG(HEVC_MPRED_L1_REF00_POC + i, c->ref_poc[1][c->slice_idx][i]);
	}
	if (h.new_pic) {
		WRITE_VREG(HEVC_MPRED_ABV_START_ADDR, ws.mpred_above.start);
		WRITE_VREG(HEVC_MPRED_MV_WPTR, wr_ptr);
		WRITE_VREG(HEVC_MPRED_MV_RPTR, rd_start);
	} else if (!h.is_next_slice_segment) {
		WRITE_VREG(HEVC_MPRED_MV_RPTR, rd_ptr);
	}
	WRITE_VREG(HEVC_MPRED_MV_RD_END_ADDR, rd_end);
}

static void
config_sao_hw(void)
{
	uint32_t d, d2;
	int misc = h.misc_flag0;
	struct pic *c = h.cur;
	uint32_t uvp = uv_pages(h.pic_w, h.pic_h);
	union param *params = &h.param;

	d = READ_VREG(HEVC_SAO_CTRL0);
	WRITE_VREG(HEVC_SAO_CTRL0, (d & ~0xfu) | h.lcu_size_log2);
	WRITE_VREG(HEVC_SAO_PIC_SIZE, h.pic_w | h.pic_h << 16);
	WRITE_VREG(HEVC_SAO_PIC_SIZE_LCU, (h.lcu_x_num - 1) | (h.lcu_y_num - 1) << 16);
	if (h.new_pic) WRITE_VREG(HEVC_SAO_Y_START_ADDR, 0xffffffff);
	if (h.compressed) WRITE_VREG(HEVC_SAO_CTRL26, 0);
	/* the linear copy at full size; no memory saving */
	d = READ_VREG(HEVC_SAO_CTRL5);
	WRITE_VREG(HEVC_SAO_CTRL5, d & ~((0xffu << 16) | (1u << 9)));
	WRITE_VREG(HEVC_SAO_Y_START_ADDR, c->y);
	if (h.mmu) {
		WRITE_VREG(HEVC_CM_BODY_START_ADDR, 0);
		WRITE_VREG(HEVC_CM_HEADER_START_ADDR, c->cm);
	} else if (h.compressed) {
		WRITE_VREG(HEVC_CM_BODY_START_ADDR, c->cm);
	}
	WRITE_VREG(HEVC_SAO_Y_LENGTH, uvp << (12 + 1));
	WRITE_VREG(HEVC_SAO_C_START_ADDR, c->uv);
	WRITE_VREG(HEVC_SAO_C_LENGTH, uvp << 12);
	WRITE_VREG(HEVC_SAO_Y_WPTR, c->y);
	WRITE_VREG(HEVC_SAO_C_WPTR, c->uv);
	if (h.new_pic) {
		d = (0xff << 8) | 0;                    /* SM1 and later */
		if (h.pic_w >= 1280) d |= 1 << 4;       /* dblk pipeline mode */
		d &= ~0x300u;
		d |= h.compressed ? (1 << 8) | (1 << 9) : 1 << 9;     /* compressed (first) write, linear (double) write */
		WRITE_VREG(HEVC_DBLK_CFGB, d);
		WRITE_VREG(HEVC_DBLK_CFG2, h.pic_w | h.pic_h << 16);
		d = ((misc >> PCM_ENABLE_FLAG_BIT) & 1) ? ((misc >> PCM_LOOP_FILTER_DISABLED_FLAG_BIT) & 1) << 3 : 0;
		d |= ((params->p.pps_cb_qp_offset & 0x1f) << 4) | ((params->p.pps_cr_qp_offset & 0x1f) << 9);
		d |= h.lcu_size == 64 ? 0 : h.lcu_size == 32 ? 1 : 2;
		d |= h.pic_w <= 64 ? 1 << 20 : 0;
		WRITE_VREG(HEVC_DBLK_CFG1, d);
		WRITE_VREG(HEVC_DBLK_STS1 + 4, 1u << 28);
	}
	d = READ_VREG(HEVC_SAO_CTRL1);
	d &= ~0x3000u;                          /* linear */
	d &= ~0xff0u;
	d |= (h.endian >> 8) & 0xfff;
	d &= ~0x3u;
	if (!h.compressed) d |= 0x1;            /* no compressed output */
	d &= ~(3u << 14);
	d |= 2u << 14;                          /* lines 64-byte aligned */
	WRITE_VREG(HEVC_SAO_CTRL1, d);
	d = READ_VREG(HEVCD_IPP_AXIIF_CONFIG);
	d &= ~0x30u;
	d &= ~0xfu;
	d |= h.endian & 0xf;
	d &= ~(3u << 8);
	d |= 2u << 8;
	WRITE_VREG(HEVCD_IPP_AXIIF_CONFIG, d);

	d = 0;
	d2 = READ_VREG(HEVC_SAO_CTRL0) & ~0x300u;
	if (h.tile_enabled) {
		d |= ((misc >> LOOP_FILER_ACROSS_TILES_ENABLED_FLAG_BIT) & 1) << 0;
		d2 |= ((misc >> LOOP_FILER_ACROSS_TILES_ENABLED_FLAG_BIT) & 1) << 8;
	}
	int slice_dbk_disabled = (misc >> SLICE_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1;
	if ((misc & (1 << DEBLOCKING_FILTER_OVERRIDE_ENABLED_FLAG_BIT)) && (misc & (1 << DEBLOCKING_FILTER_OVERRIDE_FLAG_BIT))) {
		d |= slice_dbk_disabled << 2;
		if (!slice_dbk_disabled) {
			d |= (params->p.slice_beta_offset_div2 & 0xf) << 3;
			d |= (params->p.slice_tc_offset_div2 & 0xf) << 7;
		}
	} else {
		d |= ((misc >> PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1) << 2;
		if (!((misc >> PPS_DEBLOCKING_FILTER_DISABLED_FLAG_BIT) & 1)) {
			d |= (params->p.pps_beta_offset_div2 & 0xf) << 3;
			d |= (params->p.pps_tc_offset_div2 & 0xf) << 7;
		}
	}
	if ((misc & (1 << PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT)) &&
	    ((misc & (1 << SLICE_SAO_LUMA_FLAG_BIT)) || (misc & (1 << SLICE_SAO_CHROMA_FLAG_BIT)) || !slice_dbk_disabled)) {
		d |= ((misc >> SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT) & 1) << 1;
		d2 |= ((misc >> SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT) & 1) << 9;
	} else {
		d |= ((misc >> PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT) & 1) << 1;
		d2 |= ((misc >> PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED_FLAG_BIT) & 1) << 9;
	}
	WRITE_VREG(HEVC_DBLK_CFG9, d);
	WRITE_VREG(HEVC_SAO_CTRL0, d2);
}

/* ---- core start-up (vh265_prot_init, hevc_init_decoder_hw, hevc_config_work_space_hw) ---- */

static void
config_work_space(void)
{
	WRITE_VREG(HEVCD_IPP_LINEBUFF_BASE, ws.ipp.start);
	WRITE_VREG(HEVC_RPM_BUFFER, ws.rpm.start);
	WRITE_VREG(HEVC_SHORT_TERM_RPS, ws.short_term_rps.start);
	WRITE_VREG(HEVC_VPS_BUFFER, ws.vps.start);
	WRITE_VREG(HEVC_SPS_BUFFER, ws.sps.start);
	WRITE_VREG(HEVC_PPS_BUFFER, ws.pps.start);
	WRITE_VREG(HEVC_SAO_UP, ws.sao_up.start);
	if (h.mmu) WRITE_VREG(HEVC_ASSIST_MMU_MAP_ADDR, pa(MMU_MAP_OFF));
	WRITE_VREG(HEVC_SCALELUT, ws.scalelut.start);
	WRITE_VREG(HEVC_DBLK_CFG3, 0x8020);     /* HEVC_8K_LFTOFFSET_FIX, an 8K work space */
	WRITE_VREG(HEVC_DBLK_CFG4, ws.dblk_para.start);
	WRITE_VREG(HEVC_DBLK_CFG5, ws.dblk_data.start);
	WRITE_VREG(HEVC_DBLK_CFGE, ws.dblk_data2.start);
	WRITE_VREG(LMEM_DUMP_ADR, ws.lmem.start);
}

static const uint16_t parser_cmd[37] = {
	0x0401, 0x8401, 0x0800, 0x0402, 0x9002, 0x1423, 0x8CC3, 0x1423, 0x8804, 0x9825, 0x0800, 0x04FE,
	0x8406, 0x8411, 0x1800, 0x8408, 0x8409, 0x8C2A, 0x9C2B, 0x1C00, 0x840F, 0x8407, 0x8000, 0x8408,
	0x2000, 0xA800, 0x8410, 0x04DE, 0x840C, 0x840D, 0xAC00, 0xA000, 0x08C0, 0x08E0, 0xA40E, 0xFC00,
	0x7C00
};

static void
init_decoder_hw(void)
{
	WRITE_VREG(DOS_SW_RESET3, 1 << 14);
	CLEAR_VREG_MASK(HEVC_CABAC_CONTROL, 1);
	CLEAR_VREG_MASK(HEVC_PARSER_CORE_CONTROL, 1);
	WRITE_VREG(HEVC_SHIFT_STARTCODE, 0x00000100);
	WRITE_VREG(HEVC_SHIFT_EMULATECODE, 0x00000300);
	uint32_t d = READ_VREG(HEVC_PARSER_INT_CONTROL) & 0x03ffffff;
	d |= (3u << 29) | (2 << 26) | (1 << 24) | (1 << 22) | (1 << 7) | (1 << 4) | (0 << 3) | (1 << 0);
	WRITE_VREG(HEVC_PARSER_INT_CONTROL, d);
	WRITE_VREG(HEVC_SHIFT_STATUS, READ_VREG(HEVC_SHIFT_STATUS) | (1 << 1) | (1 << 0));
	WRITE_VREG(HEVC_SHIFT_CONTROL, (3 << 6) | (2 << 4) | (2 << 1) | (1 << 0));
	WRITE_VREG(HEVC_CABAC_CONTROL, 1 << 0);
	WRITE_VREG(HEVC_PARSER_IF_CONTROL, (1 << 5) | (1 << 2) | (1 << 0));
	WRITE_VREG(HEVC_PARSER_CORE_CONTROL, 1 << 0);
	WRITE_VREG(HEVC_DEC_STATUS_REG, 0);
	WRITE_VREG(HEVC_IQIT_SCALELUT_WR_ADDR, 0);
	for (int i = 0; i < 1024; i++) WRITE_VREG(HEVC_IQIT_SCALELUT_DATA, 0);
	WRITE_VREG(HEVC_DECODE_SIZE, 0);
	WRITE_VREG(HEVC_DECODE_COUNT, 0);
	WRITE_VREG(HEVC_PARSER_CMD_WRITE, (1 << 16) | (0 << 0));
	for (int i = 0; i < 37; i++) WRITE_VREG(HEVC_PARSER_CMD_WRITE, parser_cmd[i]);
	WRITE_VREG(HEVC_PARSER_CMD_SKIP_0, 0x0000090b);
	WRITE_VREG(HEVC_PARSER_CMD_SKIP_1, 0x1b14140f);
	WRITE_VREG(HEVC_PARSER_CMD_SKIP_2, 0x001b1910);
	WRITE_VREG(HEVCD_IPP_TOP_CNTL, (0 << 1) | (1 << 0));
	WRITE_VREG(HEVCD_IPP_TOP_CNTL, (1 << 1) | (0 << 0));
	if (!h.compressed) WRITE_VREG(HEVCD_MPP_DECOMP_CTL1, 1u << 31);   /* NV21 reference reads (double write only) */
}

static void
prot_init(void)
{
	config_work_space();
	init_decoder_hw();
	WRITE_VREG(HEVC_ASSIST_MBOX0_CLR_REG, 1);
	WRITE_VREG(HEVC_ASSIST_MBOX0_MASK, 1);
	WRITE_VREG(HEVC_PSCALE_CTRL, 0);
	WRITE_VREG(DEBUG_REG1, 0);
	WRITE_VREG(NAL_SEARCH_CTL, 0x8);        /* vps/sps/pps/i-slice checked in the firmware */
	WRITE_VREG(NAL_SEARCH_CTL, READ_VREG(NAL_SEARCH_CTL) | (1 << 17));     /* parser_sei_enable 1 */
	WRITE_VREG(DECODE_STOP_POS, 0);
	/* config_decode_mode: frame based, start_decoding_flag, mailbox 0 interrupt */
	WRITE_VREG(HEVC_DECODE_MODE, 0x1 | (h.start_decoding_flag << 16) | (0x80u << 24));
	WRITE_VREG(HEVC_DECODE_MODE2, h.rps_set_id);
	WRITE_VREG(NAL_SEARCH_CTL, READ_VREG(NAL_SEARCH_CTL) & ~(1u << 22));
}

static void
reset_core(void)
{
	if (verbose > 1) fprintf(stderr, "  reset\n");
	if (ioctl(fd, TWL_VDEC_DBG_HEVC_RESET) != 0) die("HEVC reset: %s\n", strerror(errno));
	if (verbose > 1) fprintf(stderr, "  reset done\n");
}

static void
core_stop(void)
{
	WRITE_VREG(HEVC_MPSR, 0);
	WRITE_VREG(HEVC_CPSR, 0);
	for (int i = 0; i < 1000 && (READ_VREG(HEVC_IMEM_DMA_CTRL) & 0x8000); i++) usleep(10);
	for (int i = 0; i < 1000 && (READ_VREG(HEVC_LMEM_DMA_CTRL) & 0x8000); i++) usleep(10);
	for (int i = 0; i < 1000 && (READ_VREG(HEVC_WRRSP_LMEM) & 0xfff); i++) usleep(10);
}

static void
core_start(void)
{
	WRITE_VREG(DOS_SW_RESET3, (1 << 12) | (1 << 11));
	WRITE_VREG(DOS_SW_RESET3, 0);
	SET_VREG_MASK(HEVC_WRRSP_LMEM, 7u << 25);
	WRITE_VREG(HEVC_MPSR, 1);
}

/* ---- one slice header from the firmware ---- */

static struct pic *
new_pic(void)
{
	/* any picture neither referenced nor waiting to be shown (shown at once here: decode order, checked by POC) */
	for (int i = 0; i < h.n_pics; i++) {
		struct pic *p = &h.pics[i];
		if (!p->used || (!p->referenced && !p->output_pending)) {
			memset(p->ref_poc, 0, sizeof(p->ref_poc));
			p->used = 1;
			p->referenced = 1;
			p->output_pending = 1;
			p->poc = h.curr_poc;
			p->idr_period = h.idr_period;
			p->decode_index = h.decode_index;
			p->slice_idx = 0;
			return p;
		}
	}
	return NULL;
}

/* 0: decode the slice; 1: skip it; <0: error */
static int
slice_header(void)
{
	union param *rp = &h.param;
	struct rps rps;
	int nal = rp->p.m_nalUnitType;
	if (rp->p.first_slice_segment_in_pic_flag == 0) {
		h.slice_segment_addr = rp->p.slice_segment_address;
		if (!rp->p.dependent_slice_segment_flag) h.slice_addr = h.slice_segment_addr;
	} else {
		h.slice_segment_addr = h.slice_addr = 0;
	}
	int prev_poc = h.curr_poc;
	h.misc_flag0 = rp->p.misc_flag0;
	h.slice_type = rp->p.slice_type == I_SLICE ? 2 : rp->p.slice_type == P_SLICE ? 1 : rp->p.slice_type == B_SLICE ? 0 : 3;
	h.tmvp = rp->p.slice_temporal_mvp_enable_flag;
	h.is_next_slice_segment = rp->p.dependent_slice_segment_flag ? 1 : 0;
	int w = rp->p.pic_width_in_luma_samples, hh = rp->p.pic_height_in_luma_samples;
	if (!w || !hh || (w > hh ? w : hh) > 4096 || (w > hh ? hh : w) > 2304) return -1;
	int lcu_log2 = rp->p.log2_min_coding_block_size_minus3 + 3 + rp->p.log2_diff_max_min_coding_block_size;
	if (lcu_log2 < 4 || lcu_log2 > 6) return -1;
	if (!h.pics_ready) {
		h.pic_w = w;
		h.pic_h = hh;
		h.lcu_size_log2 = lcu_log2;
		h.lcu_size = 1 << lcu_log2;
		init_pics();
	}
	if (w != h.pic_w || hh != h.pic_h) die("size change %dx%d to %dx%d not handled\n", h.pic_w, h.pic_h, w, hh);
	h.lcu_size_log2 = lcu_log2;
	h.lcu_size = 1 << lcu_log2;
	h.lcu_x_num = (h.pic_w + h.lcu_size - 1) >> lcu_log2;
	h.lcu_y_num = (h.pic_h + h.lcu_size - 1) >> lcu_log2;
	h.lcu_total = h.lcu_x_num * h.lcu_y_num;

	if (nal == NAL_IDR_W_RADL || nal == NAL_IDR_N_LP) {
		if (rp->p.first_slice_segment_in_pic_flag) h.idr_period++;
		h.curr_poc = 0;
		if (rp->p.m_temporalId - 1 == 0) h.prev_tid0_poc = 0;
	} else {
		int max_lsb = 1 << (rp->p.log2_max_pic_order_cnt_lsb_minus4 + 4), lsb = rp->p.POClsb;
		int prev_lsb = h.prev_tid0_poc % max_lsb, prev_msb = h.prev_tid0_poc - prev_lsb, msb;
		if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
		else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
		else msb = prev_msb;
		if (nal == NAL_BLA_W_LP || nal == NAL_BLA_W_RADL || nal == NAL_BLA_N_LP) msb = 0;
		h.curr_poc = msb + lsb;
		if (rp->p.m_temporalId - 1 == 0) h.prev_tid0_poc = h.curr_poc;
	}
	read_ref_pic_set(h.curr_poc, &rps);
	h.ref_num[0] = rp->p.num_ref_idx_l0_active > MAX_REF_ACTIVE ? MAX_REF_ACTIVE : rp->p.num_ref_idx_l0_active;
	h.ref_num[1] = rp->p.num_ref_idx_l1_active > MAX_REF_ACTIVE ? MAX_REF_ACTIVE : rp->p.num_ref_idx_l1_active;
	/* RASL pictures after the first CRA have no references: skipped */
	if (h.poc_random_access == INT32_MAX) {
		h.poc_random_access = nal == NAL_CRA || nal == NAL_BLA_W_LP || nal == NAL_BLA_W_RADL || nal == NAL_BLA_N_LP ?
		    h.curr_poc : -INT32_MAX;
	} else if (nal == NAL_BLA_W_LP || nal == NAL_BLA_W_RADL || nal == NAL_BLA_N_LP) {
		h.poc_random_access = h.curr_poc;
	} else if (h.curr_poc < h.poc_random_access && (nal == NAL_RASL_N || nal == NAL_RASL_R)) {
		return 1;
	}
	WRITE_VREG(HEVC_WAIT_FLAG, READ_VREG(HEVC_WAIT_FLAG) | 0x2);
	if (rp->p.slice_segment_address == 0) {
		h.new_pic = 1;
		apply_ref_pic_set(h.curr_poc);
		h.cur = new_pic();
		if (!h.cur) die("no free picture\n");
		if (h.mmu) {
			/* H265_alloc_mmu: this picture's body pages, contiguous here, after its header */
			static uint32_t map[MMU_MAP_SIZE / 4];
			uint32_t pages = h.cm_body >> 12, body = h.cur->cm + h.cm_header;
			for (uint32_t i = 0; i < pages; i++) map[i] = (body >> 12) + i;
			mem(MMU_MAP_OFF, map, pages * 4, 1);
		}
	} else {
		if (!h.cur) return 1;
		h.cur->slice_idx++;
		h.new_pic = 0;
	}
	if (h.new_pic) {
		int sao_mem_unit = (h.lcu_size == 16 ? 9 : h.lcu_size == 32 ? 14 : 24) << 4;
		int pic_height_cu = (h.pic_h + h.lcu_size - 1) >> h.lcu_size_log2;
		int pic_width_cu = (h.pic_w + h.lcu_size - 1) >> h.lcu_size_log2;
		int sao_vb_size = (sao_mem_unit + (2 << 4)) * pic_height_cu;
		h.decode_index++;
		update_tile_info(pic_width_cu, pic_height_cu);
		config_title_hw(sao_vb_size, sao_mem_unit);
	}
	if (prev_poc != h.curr_poc) {
		h.new_tile = 1;
		h.tile_x = h.tile_y = 0;
	} else if (h.tile_enabled) {
		int t = get_tile_index(rp->p.slice_segment_address, (h.pic_w + h.lcu_size - 1) >> h.lcu_size_log2);
		if (t != (h.tile_x | h.tile_y << 8) && t != -1) {
			h.new_tile = 1;
			h.tile_x = t & 0xff;
			h.tile_y = (t >> 8) & 0xff;
		} else {
			h.new_tile = 0;
		}
	} else {
		h.new_tile = 0;
	}
	if (h.new_tile) {
		h.tile_start_lcu_x = h.tile[h.tile_y][h.tile_x].start_cu_x;
		h.tile_start_lcu_y = h.tile[h.tile_y][h.tile_x].start_cu_y;
		h.tile_width_lcu = h.tile[h.tile_y][h.tile_x].width;
		h.tile_height_lcu = h.tile[h.tile_y][h.tile_x].height;
	}
	if (h.cur->slice_idx >= MAX_SLICES) return -1;
	if (set_ref_pic_list(&rps) < 0) return -1;
	int col_ref = rp->p.collocated_ref_idx;
	struct pic *c = h.cur;
	h.ldc = 0;
	if (rp->p.slice_type != I_SLICE) {
		h.ldc = 1;
		for (int i = 0; i < h.ref_num[0] && h.ldc; i++) if (c->ref_poc[0][c->slice_idx][i] > h.curr_poc) h.ldc = 0;
		if (rp->p.slice_type == B_SLICE)
			for (int i = 0; i < h.ref_num[1] && h.ldc; i++) if (c->ref_poc[1][c->slice_idx][i] > h.curr_poc) h.ldc = 0;
	}
	h.col_from_l0 = rp->p.collocated_from_l0_flag;
	h.plevel = rp->p.log2_parallel_merge_level;
	h.max_merge = 5 - rp->p.five_minus_max_num_merge_cand;
	if (h.max_merge > 5 || h.max_merge < 1) h.max_merge = 5;
	int list = rp->p.slice_type == B_SLICE ? 1 - h.col_from_l0 : 0;
	h.col_poc = col_ref < h.ref_num[list] ? c->ref_poc[list][c->slice_idx][col_ref] : INVALID_POC;
	h.col = NULL;
	if (h.slice_type != 2 && h.col_poc != INVALID_POC) {
		h.col = pic_by_poc(h.col_poc);
		if (!h.col) fprintf(stderr, "POC %d: no co-located POC %d\n", h.curr_poc, h.col_poc);
	}
	config_mc_buffer();
	config_mcrcc_axi_hw(c->slice_type);
	if (!h.tile_width_lcu || !h.tile_height_lcu) return -1;
	config_mpred_hw();
	config_sao_hw();
	return 0;
}

/* ---- the run of one packet ---- */

/* read_decode_info: HEVC_DECODE_INFO is the RPM buffer's scratch register, overwritten by the firmware */
static void
read_decode_info(void)
{
	uint32_t info = READ_VREG(HEVC_RPM_BUFFER);
	h.start_decoding_flag |= info & 0xff;
	h.rps_set_id = (info >> 8) & 0xff;
}

static const char *
status_name(uint32_t s)
{
	switch (s) {
	case 0x8: return "slice segment done";
	case 0xa: return "picture done";
	case 0xb: return "picture error";
	case 0xc: return "SEI";
	case 0xe: return "NAL decode done";
	case 0x20: return "decode buffer empty";
	case 0x21: return "decode timeout";
	case 0x22: return "search buffer empty";
	case 0x23: return "over size";
	case 0x24: return "decode buffer empty 2";
	case 0x25: return "parameter error";
	case 0x50: return "next picture NAL";
	default: return "?";
	}
}

static uint8_t *zero_pad;

/* decodes the packet's picture; returns it or NULL */
static struct pic *
run_packet(const uint8_t *data, uint32_t size)
{
	if (size + 0x4000 > STREAM_SIZE) die("packet of %u bytes too big\n", size);
	mem(STREAM_OFF, (void *)data, size, 1);
	mem(STREAM_OFF + size, zero_pad, 0x4000, 1);
	reset_core();
	if (verbose) fprintf(stderr, "  swap register before the run: %#x\n", READ_VREG(HEVC_STREAM_SWAP_BUFFER2));
	WRITE_VREG(HEVC_STREAM_SWAP_BUFFER2, h.swap_addr);
	prot_init();
	uint32_t start = pa(STREAM_OFF);
	WRITE_VREG(HEVC_STREAM_START_ADDR, start);
	WRITE_VREG(HEVC_STREAM_END_ADDR, start + STREAM_SIZE);
	WRITE_VREG(HEVC_STREAM_RD_PTR, start);
	uint32_t pad = getenv("HEVCDEV_PAD") ? (uint32_t)strtoul(getenv("HEVCDEV_PAD"), NULL, 0) : 0x3000;   /* the parser reads ahead: 1 KiB of padding loses the last bytes */
	WRITE_VREG(HEVC_STREAM_WR_PTR, (start + size + pad) & ~7u);
	SET_VREG_MASK(HEVC_STREAM_CONTROL, 7 << 4);
	SET_VREG_MASK(HEVC_STREAM_FIFO_CTL, 1u << 29);
	SET_VREG_MASK(HEVC_STREAM_CONTROL, 1);
	WRITE_VREG(NAL_SEARCH_CTL, READ_VREG(NAL_SEARCH_CTL) & ~(1u << 21));
	WRITE_VREG(HEVC_DEC_STATUS_REG, 0xff);   /* HEVC_ACTION_DONE */
	WRITE_VREG(HEVC_SHIFT_BYTE_COUNT, 0);
	WRITE_VREG(HEVC_DECODE_SIZE, size + (getenv("HEVCDEV_DSIZE") ? (uint32_t)atoi(getenv("HEVCDEV_DSIZE")) : 0));
	WRITE_VREG(HEVC_DECODE_COUNT, h.decode_index);
	if (h.pics_ready) init_pic_list_hw();
	core_start();
	struct pic *done = NULL;
	double t0 = now();
	uint32_t last = 0xffffffff;
	for (;;) {
		uint32_t s = READ_VREG(HEVC_DEC_STATUS_REG);
		if (s != last && verbose) fprintf(stderr, "  status %#x (%s) at %.1f ms, shift %u\n", s, status_name(s),
		    (now() - t0) * 1e3, READ_VREG(HEVC_SHIFT_BYTE_COUNT));
		last = s;
		if (s == 0x8) {
			read_decode_info();
			uint16_t raw[RPM_END - RPM_BEGIN];
			mem(ws.rpm.start - (uint32_t)pool_pa, raw, sizeof(raw), 0);
			for (int i = 0; i < RPM_END - RPM_BEGIN; i += 4)
				for (int k = 0; k < 4; k++) h.param.data[i + k] = raw[i + 3 - k];
			int r = slice_header();
			if (verbose) fprintf(stderr, "  slice: NAL %d type %d POC %d addr %d -> %d\n", h.param.p.m_nalUnitType,
			    h.param.p.slice_type, h.curr_poc, h.param.p.slice_segment_address, r);
			if (r < 0) die("slice header failed\n");
			if (r == 1) {
				WRITE_VREG(HEVC_WAIT_FLAG, READ_VREG(HEVC_WAIT_FLAG) & ~0x2u);
				WRITE_VREG(HEVC_DEC_STATUS_REG, 0xff);
			} else {
				WRITE_VREG(HEVC_DEC_STATUS_REG, 0x5);       /* HEVC_CODED_SLICE_SEGMENT_DAT */
			}
			WRITE_VREG(HEVC_MCPU_INTR_REQ, 0x04);
			last = 0x5;
			t0 = now();
			continue;
		}
		if (s == 0xc) {
			WRITE_VREG(HEVC_DEC_STATUS_REG, 0xd);
			WRITE_VREG(HEVC_MCPU_INTR_REQ, 0x04);
			continue;
		}
		if (s == 0xa || s == 0x50) {
			read_decode_info();
			done = h.cur;
			break;
		}
		if (s == 0x20 || s == 0x24 || s == 0x22 || s == 0xe) {
			read_decode_info();
			done = h.cur;
			if (verbose) fprintf(stderr, "  packet ended: %s, LCU %#x of %d, shift %u\n", status_name(s),
			    READ_VREG(HEVC_PARSER_LCU_START), h.lcu_total, READ_VREG(HEVC_SHIFT_BYTE_COUNT));
			if (getenv("HEVCDEV_SETTLE")) usleep(atoi(getenv("HEVCDEV_SETTLE")) * 1000);
			if (verbose) fprintf(stderr, "  then LCU %#x, status %#x\n", READ_VREG(HEVC_PARSER_LCU_START),
			    READ_VREG(HEVC_DEC_STATUS_REG));
			break;
		}
		if (s == 0x21 || s == 0x23 || s == 0x25 || s == 0xb) {
			fprintf(stderr, "  decoder error %#x (%s), LCU %#x, shift %u\n", s, status_name(s),
			    READ_VREG(HEVC_PARSER_LCU_START), READ_VREG(HEVC_SHIFT_BYTE_COUNT));
			break;
		}
		if (now() - t0 > 2.0) {
			fprintf(stderr, "  timeout in status %#x: MPC %#x, LCU %#x, shift %u, stream rp %#x wp %#x\n", s,
			    READ_VREG(HEVC_MPC_E), READ_VREG(HEVC_PARSER_LCU_START), READ_VREG(HEVC_SHIFT_BYTE_COUNT),
			    READ_VREG(HEVC_STREAM_RD_PTR), READ_VREG(HEVC_STREAM_WR_PTR));
			break;
		}
	}
	if (getenv("HEVCDEV_WATCH")) {
		/* what the firmware does after the picture: status, PC, scratch 1 (RPM buffer / decode info), DMA */
		double t1 = now();
		uint32_t ls = 0, lp = 0, l1 = 0;
		while (now() - t1 < atoi(getenv("HEVCDEV_WATCH")) / 1000.0) {
			uint32_t st = READ_VREG(HEVC_DEC_STATUS_REG), pc = READ_VREG(HEVC_MPC_E), s1 = READ_VREG(HEVC_RPM_BUFFER);
			if (st != ls || s1 != l1 || (pc != lp && verbose > 1)) {
				fprintf(stderr, "  watch %.2f ms: status %#x pc %#x scratch1 %#x shift %u lcu %#x\n", (now() - t1) * 1e3, st,
				    pc, s1, READ_VREG(HEVC_SHIFT_BYTE_COUNT), READ_VREG(HEVC_PARSER_LCU_START));
				ls = st; lp = pc; l1 = s1;
			}
		}
	}
	core_stop();
	if (verbose) {
		fprintf(stderr, "  stopped; alive\n");
		usleep(200000);
		fprintf(stderr, "  still alive 200 ms later\n");
	}
	return done;
}

/* ---- main ---- */

int
main(int argc, char **argv)
{
	if (argc < 3) die("usage: hevcdev PACKETS MD5S [FRAMES] [OUT.yuv]\n");
	verbose = getenv("HEVCDEV_VERBOSE") ? (unsigned)atoi(getenv("HEVCDEV_VERBOSE")) : 0;
	FILE *f = fopen(argv[1], "rb");
	if (!f) die("%s: %s\n", argv[1], strerror(errno));
	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *file = malloc((size_t)file_size);
	if (fread(file, 1, (size_t)file_size, f) != (size_t)file_size) die("short read\n");
	fclose(f);
	if (memcmp(file, "TWH1", 4)) die("not a TWH1 packet file\n");
	uint32_t count, timescale, depth;
	memcpy(&count, file + 4, 4);
	memcpy(&timescale, file + 8, 4);
	memcpy(&depth, file + 12, 4);
	unsigned frames = argc > 3 ? (unsigned)atoi(argv[3]) : count;
	if (frames > count) frames = count;

	/* reference MD5s, presentation order */
	char (*md5s)[33] = calloc(count, 33);
	long long *pts_sorted = calloc(count, sizeof(long long));
	f = fopen(argv[2], "r");
	if (!f) die("%s: %s\n", argv[2], strerror(errno));
	unsigned n_md5 = 0;
	while (n_md5 < count && fscanf(f, "%lld %32s", &pts_sorted[n_md5], md5s[n_md5]) == 2) n_md5++;
	fclose(f);
	FILE *out = argc > 4 ? fopen(argv[4], "wb") : NULL;

	fd = open("/dev/twl-vdec-probe", O_RDWR);
	if (fd < 0) die("/dev/twl-vdec-probe: %s\n", strerror(errno));
	struct twl_vdec_dbg_info info;
	if (ioctl(fd, TWL_VDEC_DBG_INFO, &info) != 0) die("no debug access (vdec_debug=1?): %s\n", strerror(errno));
	pool_pa = info.pool_pa;
	pool_size = info.pool_size;
	fprintf(stderr, "pool %#llx + %#x, HEVC powered %u; %u packets, %u-bit\n", (unsigned long long)pool_pa, pool_size,
	    info.hevc_powered, count, depth);
	layout_workspace();
	zero_pad = calloc(1, 0x4000);
	{
		uint8_t *z = calloc(1, 0x10000);
		for (uint32_t at = 0; at < STREAM_OFF; at += 0x10000) mem(at, z, 0x10000, 1);
		free(z);
	}
	/* the mode before the first run: the MMU's map address is set with the work space, and a stale one makes the
	 * decoder write its pictures wherever that map's garbage points (it took the kernel's page tables once) */
	const char *mode = getenv("HEVCDEV_MODE") ? getenv("HEVCDEV_MODE") : "mmu";
	h.compressed = strcmp(mode, "dw") != 0;
	h.mmu = !strcmp(mode, "mmu");
	if (h.mmu) {
		uint8_t *z = calloc(1, MMU_MAP_SIZE);
		mem(MMU_MAP_OFF, z, MMU_MAP_SIZE, 1);
		free(z);
	}
	/* hevc_mmu_swap ('265S') for compressed frames; the plain h265 (16) for the linear-only mode */
	int mmu_fw = h.mmu;
	uint32_t fw_id = getenv("HEVCDEV_FW") ? (uint32_t)strtoul(getenv("HEVCDEV_FW"), NULL, 0) : mmu_fw ? 0x32363553u : 16u;
	struct twl_vdec_dbg_fw fw = { fw_id, 2, getenv("HEVCDEV_SWAP") ? (uint32_t)atoi(getenv("HEVCDEV_SWAP")) : 1, 0 };
	if (ioctl(fd, TWL_VDEC_DBG_FW, &fw) != 0 || fw.result != 0) die("firmware load: %d\n", fw.result);
	h.swap_addr = rd(HEVC_STREAM_SWAP_BUFFER2);
	fprintf(stderr, "firmware loaded; swap buffer %#x\n", h.swap_addr);
	h.endian = HEVC_CONFIG_LITTLE_ENDIAN;
	h.poc_random_access = INT32_MAX;
	h.curr_poc = INVALID_POC;

	uint32_t at = 16;
	unsigned exact = 0, decoded = 0;
	uint8_t *frame = NULL;
	double t_all = now();
	for (unsigned i = 0; i < frames; i++) {
		uint32_t size;
		int64_t pts;
		memcpy(&size, file + at, 4);
		memcpy(&pts, file + at + 4, 8);
		const uint8_t *data = file + at + 12;
		at += 12 + size;
		double t = now();
		struct pic *p = run_packet(data, size);
		double took = now() - t;
		if (!p) {
			fprintf(stderr, "packet %u (pts %lld, %u bytes): no picture\n", i, (long long)pts, size);
			continue;
		}
		decoded++;
		p->output_pending = 0;
		if (!frame) frame = malloc(h.frame_bytes);
		mem(p->y_off, frame, h.frame_bytes, 0);
		/* cropped NV12: luma rows, then interleaved chroma rows */
		CC_MD5_CTX ctx;
		CC_MD5_Init(&ctx);
		for (int y = 0; y < h.pic_h; y++) CC_MD5_Update(&ctx, frame + (size_t)y * h.stride, (CC_LONG)h.pic_w);
		for (int y = 0; y < h.pic_h / 2; y++)
			CC_MD5_Update(&ctx, frame + h.uv_offset + (size_t)y * h.stride, (CC_LONG)h.pic_w);
		unsigned char digest[16];
		CC_MD5_Final(digest, &ctx);
		char hex[33];
		for (int k = 0; k < 16; k++) sprintf(hex + 2 * k, "%02x", digest[k]);
		const char *want = NULL;
		for (unsigned k = 0; k < n_md5; k++) if (pts_sorted[k] == pts) want = md5s[k];
		int ok = want && !strcmp(want, hex);
		exact += ok;
		fprintf(stderr, "packet %u: POC %d slot %d, %.1f ms, %s %s\n", i, p->poc, (int)(p - h.pics), took * 1e3, hex,
		    ok ? "exact" : want ? "MISMATCH" : "(no reference)");
		if (out) {
			for (int y = 0; y < h.pic_h; y++) fwrite(frame + (size_t)y * h.stride, 1, (size_t)h.pic_w, out);
			for (int y = 0; y < h.pic_h / 2; y++) fwrite(frame + h.uv_offset + (size_t)y * h.stride, 1, (size_t)h.pic_w, out);
		}
	}
	fprintf(stderr, "%u/%u packets decoded, %u exact, %.2f s\n", decoded, frames, exact, now() - t_all);
	if (out) fclose(out);
	return exact == frames ? 0 : 2;
}
