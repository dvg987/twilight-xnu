/*
 * Adapter to Amlogic's GPL-2.0-or-later reference-picture manager.
 * All calls are serialized by the decoder. Reconstructed pictures are copied out
 * before completion, so display holds do not extend the hardware buffer lifetime.
 */
#include "compat.h"
#include "h264_dpb.h"
#include "dpb.h"
#include <sys/hakios.vdec.h>

struct twl_h264_dpb {
    struct vdec_s vdec;
    struct h264_dpb_stru vendor;
    bool used[TWL_H264_SLOTS], failed;
    unsigned limit;     /* the slots the kernel gives the stream's pictures (TWL_VDEC_STREAM) */
    uint32_t held;      /* slots the display still shows: not to be decoded into */
    uint32_t showing;   /* the plane's slots (it reads them): never decoded into */
    struct twl_h264_slot slots[TWL_H264_SLOTS];
};
unsigned int h264_debug_flag, h264_debug_mask;
_Static_assert(sizeof(union param) == TWL_H264_RPM_WORDS * sizeof(uint16_t), "firmware parameter ABI");

int get_free_buf_idx(struct vdec_s *vdec)
{
    struct twl_h264_dpb *d = vdec->priv;
    /* a slot the display holds only when nothing else is free: a picture shown late beats a decoder stopped */
    for (unsigned pass=0; pass<2; pass++)
        for (unsigned i=0; i<d->limit; i++) if (!d->used[i] && !(d->showing & (1u << i)) && (pass || !(d->held & (1u << i)))) {
            d->used[i] = true;
            return (int)i;
        }
    d->failed = true;
    return -1;
}
int release_buf_spec_num(struct vdec_s *vdec, int slot)
{
    struct twl_h264_dpb *d = vdec->priv;
    if (slot < 0 || slot >= (int)TWL_H264_SLOTS) { d->failed = true; return -1; }
    d->used[slot] = false;
    return 0;
}
int prepare_display_buf(struct vdec_s *vdec, struct FrameStore *frame)
{
    (void)vdec;
    frame->is_output = 1;
    return 0;
}
int post_picture_early(struct vdec_s *vdec, int slot) { (void)vdec; (void)slot; return 0; }
void pic_mutex_lock(struct h264_dpb_stru *d) { (void)d; }
void pic_mutex_unlock(struct h264_dpb_stru *d) { (void)d; }
void *vh264_get_bufspec_lock(struct vdec_s *vdec) { return vdec; }
void bufmgr_force_recover(struct h264_dpb_stru *d) { ((struct twl_h264_dpb *)d->vdec->priv)->failed = true; }
u32 get_error_proc_policy(struct h264_dpb_stru *d) { (void)d; return 0; }
u32 is_save_buffer_mode(void) { return 0; }

struct twl_h264_dpb *twl_h264_dpb_create(void)
{
    struct twl_h264_dpb *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->vdec.priv = d;
    d->vendor.vdec = &d->vdec;
    dpb_init_global(&d->vendor, 0, TWL_H264_SLOTS, 16);
    d->vendor.dec_dpb_size = TWL_H264_SLOTS;
    init_colocate_buf(&d->vendor, TWL_H264_SLOTS);
    d->limit = TWL_H264_SLOTS;
    return d;
}
int twl_h264_dpb_set_slots(struct twl_h264_dpb *d, unsigned slots)
{
    /* before the stream's first picture only: the vendor's buffer counts follow */
    if (!d || slots < 2 || slots > TWL_H264_SLOTS || d->vendor.mDPB.first_pic_done) return -1;
    d->limit = slots;
    d->vendor.dec_dpb_size = (int)slots;
    init_colocate_buf(&d->vendor, (int)slots);
    return 0;
}
void twl_h264_dpb_set_held(struct twl_h264_dpb *d, uint32_t held, uint32_t showing)
{
    if (d) { d->held = held; d->showing = showing; }
}
bool twl_h264_dpb_started(const struct twl_h264_dpb *d)
{
    return d && d->vendor.mDPB.first_pic_done;
}
static bool valid_reordering(const uint16_t *commands)
{
    for (unsigned i=0; i<66;) {
        unsigned command = commands[i++];
        if (command == 3) return true;
        if (command > 2 || i >= 66) return false;
        if (command == 2 && commands[i] > 15) return false;
        i++;
    }
    return false;
}
static bool valid_parameters(const union param *p, bool continuation)
{
    const uint16_t *v = p->l.data;
    unsigned slice = v[SLICE_TYPE], poc_type = v[PIC_ORDER_CNT_TYPE];
    if ((slice != I_Slice && slice != P_Slice && slice != B_Slice) ||
        v[FRAME_MBS_ONLY_FLAG] != 1 || (v[NEW_PICTURE_STRUCTURE] != 0 && v[NEW_PICTURE_STRUCTURE] != 3) ||
        v[LOG2_MAX_FRAME_NUM] < 4 || v[LOG2_MAX_FRAME_NUM] > 16 || poc_type > 2 ||
        (poc_type == 0 && (v[LOG2_MAX_PIC_ORDER_CNT_LSB] < 4 || v[LOG2_MAX_PIC_ORDER_CNT_LSB] > 16)) ||
        v[NUM_REF_FRAMES_IN_PIC_ORDER_CNT_CYCLE] > 128 || v[MAX_REFERENCE_FRAME_NUM] > 16 ||
        (v[FIRST_MB_IN_SLICE] != 0) != continuation || v[NUM_SLICE_GROUPS_MINUS1] ||
        p->dpb.num_ref_idx_l0_active_minus1 >= 32 || p->dpb.num_ref_idx_l1_active_minus1 >= 32)
        return false;
    if (slice != I_Slice && !valid_reordering(p->mmco.l0_reorder_cmd)) return false;
    if (slice == B_Slice && !valid_reordering(p->mmco.l1_reorder_cmd)) return false;
    if ((p->dpb.NAL_info_mmco & 31) != 5 && ((p->dpb.NAL_info_mmco >> 5) & 3)) {
        for (unsigned i=0; i<44;) {
            unsigned command = p->mmco.mmco_cmd[i++];
            if (!command) return true;
            if (command > 6) return false;
            unsigned arguments = command == 3 ? 2 : command == 5 ? 0 : 1;
            if (arguments > 44-i) return false;
            if ((command == 3 || command == 6) && p->mmco.mmco_cmd[i+arguments-1] > 15) return false;
            if (command == 4 && p->mmco.mmco_cmd[i] > 16) return false;
            i += arguments;
        }
        return false;
    }
    return true;
}
int twl_h264_dpb_prepare(struct twl_h264_dpb *d, const uint16_t *parameters, struct twl_h264_config *config)
{
    if (!d || !parameters || !config || d->failed) return -1;
    /* With a picture under way this is its next slice (h264_slice_header_process returns 0, not 1). */
    bool continuation = d->vendor.mVideo.dec_picture != NULL;
    memcpy(&d->vendor.dpb_param, parameters, sizeof(d->vendor.dpb_param));
    if (!valid_parameters(&d->vendor.dpb_param, continuation)) { d->failed = true; return -1; }
    if (!d->vendor.mDPB.first_pic_done && (d->vendor.dpb_param.dpb.NAL_info_mmco & 31) != 5) {
        d->failed = true;
        return -1;
    }
    int gap = 0, status = h264_slice_header_process(&d->vendor, &gap);
    if (status < 0 || status != !continuation || d->failed ||
        d->vendor.dpb_error_flag || !d->vendor.mVideo.dec_picture) { d->failed = true; return -1; }
    struct StorablePicture *picture = d->vendor.mVideo.dec_picture;
    struct Slice *slice = &d->vendor.mSlice;
    if (picture->buf_spec_num < 0 || picture->buf_spec_num >= (int)d->limit ||
        picture->structure != FRAME || picture->mb_aff_frame_flag) { d->failed = true; return -1; }
    memset(config, 0, sizeof(*config));
    config->flags = continuation ? TWL_H264_SLICE : 0;
    unsigned slot = (unsigned)picture->buf_spec_num;
    config->current_slot = slot;
    config->frame_poc = picture->frame_poc;
    config->top_poc = picture->top_poc;
    config->bottom_poc = picture->bottom_poc;
    config->mv_write_slot = picture->colocated_buf_index;
    config->mv_read_slot = -1;
    config->direct_8x8 = (slice->mode_8x8_flags & 6) == 6;
    config->first_mb = (uint32_t)slice->first_mb_in_slice;
    d->slots[slot].top_poc = picture->top_poc;
    d->slots[slot].bottom_poc = picture->bottom_poc;
    for (unsigned i=0; i<TWL_H264_SLOTS; i++) {
        int flags = get_long_term_flag_by_buf_spec_num(&d->vendor, (int)i);
        d->slots[i].long_term = flags > 0 ? (uint32_t)flags & 3 : 0;
        config->slots[i] = d->slots[i];
    }
    for (unsigned list=0; list<2; list++) {
        if (slice->listXsize[list] < 0 || slice->listXsize[list] > 32) { d->failed = true; return -1; }
        config->reference_count[list] = (uint32_t)slice->listXsize[list];
        for (int i=0; i<slice->listXsize[list]; i++) {
            struct StorablePicture *ref = slice->listX[list][i];
            if (!ref || ref->buf_spec_num < 0 || ref->buf_spec_num >= (int)TWL_H264_SLOTS ||
                !d->used[ref->buf_spec_num] || ref->structure != FRAME) { d->failed = true; return -1; }
            config->references[list][i] = (uint32_t)ref->buf_spec_num;
            if (list == 1 && i == 0) {
                config->mv_read_slot = ref->colocated_buf_index;
                int64_t top = (int64_t)picture->poc - ref->top_poc;
                int64_t bottom = (int64_t)picture->poc - ref->bottom_poc;
                config->mv_read_bottom = llabs(top) >= llabs(bottom);
            }
        }
    }
    return 0;
}
int twl_h264_dpb_complete(struct twl_h264_dpb *d)
{
    if (!d || d->failed || !d->vendor.mVideo.dec_picture) return -1;
    struct StorablePicture *picture = d->vendor.mVideo.dec_picture;
    int result = store_picture_in_dpb(&d->vendor, picture, 0);
    d->vendor.mVideo.dec_picture = NULL;
    bufmgr_post(&d->vendor);
    bufmgr_h264_remove_unused_frame(&d->vendor, 0);
    if (result < 0 || d->failed || d->vendor.dpb_error_flag) { d->failed = true; return -1; }
    return 0;
}
void twl_h264_dpb_destroy(struct twl_h264_dpb *d)
{
    if (!d) return;
    if (d->vendor.mVideo.dec_picture) {
        release_picture(&d->vendor, d->vendor.mVideo.dec_picture);
        d->vendor.mVideo.dec_picture = NULL;
    }
    flush_dpb(&d->vendor);
    bufmgr_h264_remove_unused_frame(&d->vendor, 0);
    free(d);
}
